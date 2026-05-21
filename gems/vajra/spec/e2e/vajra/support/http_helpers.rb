# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'fileutils'
require 'tmpdir'

module VajraE2EHttpHelpers # rubocop:disable Metrics/ModuleLength
  def parse_http_response(response)
    headers, body = response.split("\r\n\r\n", 2)
    raise ArgumentError, "incomplete HTTP response: #{response.inspect}" if body.nil?

    header_lines = headers.split("\r\n")
    status_line = header_lines.shift
    parsed_headers = header_lines.to_h do |line|
      name, value = line.split(':', 2)
      raise ArgumentError, "invalid HTTP response header line: #{line.inspect} in #{response.inspect}" if value.nil?

      [name.downcase, value.strip]
    end

    { raw: response, status_line:, headers: parsed_headers, body: }
  end

  def read_http_response(
    socket,
    buffered_bytes: String.new(encoding: Encoding::BINARY),
    timeout: VajraE2EHelpers::HTTP_RESPONSE_READ_TIMEOUT_SECONDS,
    wait_thread: nil,
    output: nil,
    request_label: 'request'
  )
    response = String.new(buffered_bytes, encoding: Encoding::BINARY)

    Timeout.timeout(timeout) do
      response << socket.readpartial(4096) until response.include?("\r\n\r\n")

      headers, body = response.split("\r\n\r\n", 2)
      content_length = http_content_length(headers)

      body << socket.readpartial(4096) while body.bytesize < content_length

      [parse_http_response(http_complete_response(headers, body, content_length)), http_trailing_bytes(body, content_length)]
    end
  rescue EOFError, Errno::ECONNRESET => e
    raise e.class, http_read_failure_message(e, request_label, wait_thread, output, buffered_bytes, response), e.backtrace
  end

  def read_raw_http_response(
    socket,
    timeout: VajraE2EHelpers::HTTP_RESPONSE_READ_TIMEOUT_SECONDS,
    wait_thread: nil,
    output: nil,
    request_label: 'request'
  )
    response, = read_http_response(socket, timeout:, wait_thread:, output:, request_label:)
    response[:raw]
  end

  def http_read_failure_message(error, request_label, wait_thread, output, buffered_bytes, response)
    message = "#{request_label} failed while reading HTTP response: #{error.class}: #{error.message}"
    message << " buffered_bytes=#{buffered_bytes.inspect}" unless buffered_bytes.empty?
    message << " response_so_far=#{response.inspect}" if response.bytesize > buffered_bytes.bytesize
    message << " process=#{process_diagnostics(wait_thread, output)}" if wait_thread && output
    message
  end

  def http_content_length(headers)
    content_length_header = headers.lines.find { |line| line.match?(/\Acontent-length:/i) }
    return 0 unless content_length_header

    Integer(content_length_header.split(':', 2).last.strip)
  end

  def http_complete_response(headers, body, content_length)
    complete_response = String.new(encoding: Encoding::BINARY)
    complete_response << headers
    complete_response << "\r\n\r\n".b
    complete_response << body.byteslice(0, content_length)
    complete_response
  end

  def http_trailing_bytes(body, content_length)
    body.byteslice(content_length..) || String.new(encoding: Encoding::BINARY)
  end

  def request_response(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'request_response')
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response: response, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def request_response_from_inline_start(env:, timeout: 15)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'request_response_from_inline_start')
      socket.close

      status = stop_process(wait_thread, timeout:)

      { exitstatus: status.exitstatus, response: response, port: selected_port, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def sequential_request_result(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      buffered_bytes = String.new(encoding: Encoding::BINARY)
      socket.write("GET /first HTTP/1.1\r\nHost: localhost\r\n\r\n")
      first_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'sequential_request_result:first'
      )

      socket.write("GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      second_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'sequential_request_result:second'
      )
      connection_closed = Timeout.timeout(2) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        first_response:,
        second_response:,
        connection_closed:,
        trailing_bytes: buffered_bytes,
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def pipelined_request_result(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      buffered_bytes = String.new(encoding: Encoding::BINARY)
      socket.write(
        "GET /first HTTP/1.1\r\n" \
        "Host: localhost\r\n\r\n" \
        "GET /second HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Connection: close\r\n\r\n"
      )
      first_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'pipelined_request_result:first'
      )
      second_response, buffered_bytes = read_http_response(
        socket,
        buffered_bytes:,
        wait_thread:,
        output:,
        request_label: 'pipelined_request_result:second'
      )
      connection_closed = Timeout.timeout(2) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        first_response:,
        second_response:,
        connection_closed:,
        trailing_bytes: buffered_bytes,
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def rack_env_request_result(request:, port: disposable_listener_port, env: {})
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          captured_env = %w[
            REQUEST_METHOD
            SCRIPT_NAME
            PATH_INFO
            QUERY_STRING
            SERVER_PROTOCOL
            SERVER_NAME
            SERVER_PORT
            REMOTE_ADDR
            REMOTE_PORT
            rack.url_scheme
            HTTP_HOST
            HTTP_X_TRACE_ID
            CONTENT_TYPE
            CONTENT_LENGTH
          ].each_with_object({}) do |key, values|
            values[key] = rack_env[key]
          end

          [200, { "Content-Type" => "application/json" }, [JSON.generate(captured_env)]]
        end
      )

      Vajra.start
    RUBY

    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(request)
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'rack_env_request_result')
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def rack_app_request_result(script:, request:, port: disposable_listener_port, env: {})
    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(request)
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'rack_app_request_result')
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def rack_app_request_chunks_result(script:, chunks:, port: disposable_listener_port, env: {}, pause: nil)
    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      begin
        chunks.each do |chunk|
          socket.write(chunk)
          sleep pause if pause
        end
        socket.close_write
        response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'rack_app_request_chunks_result')
      ensure
        socket.close unless socket.closed?
      end

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  # rubocop:disable Metrics/AbcSize, ThreadSafety/NewThread
  def concurrent_rack_app_request_results(script:, requests:, port: disposable_listener_port, env: {})
    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      responses = Array.new(requests.length)
      ready_mutex = Mutex.new
      ready_condition = ConditionVariable.new
      ready_count = 0
      release_requests = false
      wait_timeout = VajraE2EHelpers::HTTP_RESPONSE_READ_TIMEOUT_SECONDS
      workers = requests.each_with_index.map do |request, index|
        Thread.new do
          socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
          begin
            ready_mutex.synchronize do
              ready_count += 1
              ready_condition.broadcast
              Timeout.timeout(wait_timeout) do
                ready_condition.wait(ready_mutex) until release_requests
              end
            end
            socket.write(request)
            responses[index] = read_raw_http_response(
              socket,
              wait_thread:,
              output:,
              request_label: "concurrent_rack_app_request_results:#{index}"
            )
          ensure
            socket.close unless socket.closed?
          end
        end
      end
      begin
        ready_mutex.synchronize do
          Timeout.timeout(wait_timeout) do
            ready_condition.wait(ready_mutex) until ready_count == requests.length
          end
          release_requests = true
          ready_condition.broadcast
        end
        workers.each(&:value)
      ensure
        ready_mutex.synchronize do
          release_requests = true
          ready_condition.broadcast
        end
      end

      status = stop_process(wait_thread)

      {
        exitstatus: status.exitstatus,
        responses:,
        output: "#{startup_output.join}#{output.read}",
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end
  # rubocop:enable Metrics/AbcSize, ThreadSafety/NewThread

  def packaged_app_request_result(files:, request:, env: {}, args: [])
    packaged_app_command_request_result(
      files:,
      command: packaged_vajra_command(*args),
      request:,
      env: vajra_env(port: 0).merge(env)
    )
  end

  def packaged_app_startup_failure(files:, env: {}, args: [])
    packaged_app_command_startup_failure(
      files:,
      command: packaged_vajra_command(*args),
      env: vajra_env(port: 0).merge(env)
    )
  end

  def packaged_app_command_request_result(files:, command:, request:, env: {})
    with_packaged_app(files:) do |app_root|
      Open3.popen2e(app_root_bundle_env.merge(env), *command, chdir: app_root) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)

        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        socket.write(request)
        response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'packaged_app_command_request_result')
        socket.close

        status = stop_process(wait_thread)

        { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  def packaged_app_command_request_result_on_port(files:, command:, request:, port:, env: {}, timeout: 15)
    with_packaged_app(files:) do |app_root|
      Open3.popen2e(app_root_bundle_env.merge(env), *command, chdir: app_root) do |_stdin, output, wait_thread|
        response = wait_for_http_response(
          port,
          request,
          wait_thread:,
          output:,
          timeout:,
          request_label: 'packaged_app_command_request_result_on_port'
        )
        status = stop_process(wait_thread)

        { exitstatus: status.exitstatus, response:, output: output.read, port: }
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  def packaged_app_command_startup_failure(files:, command:, env: {})
    with_packaged_app(files:) do |app_root|
      Open3.popen2e(app_root_bundle_env.merge(env), *command, chdir: app_root) do |_stdin, output, wait_thread|
        status = Timeout.timeout(15) { wait_thread.value }
        { exitstatus: status.exitstatus, output: output.read }
      ensure
        cleanup_process(wait_thread, output)
      end
    end
  end

  def with_packaged_app(files:)
    Dir.mktmpdir('vajra-app-root') do |app_root|
      files.each do |relative_path, contents|
        absolute_path = File.join(app_root, relative_path)
        FileUtils.mkdir_p(File.dirname(absolute_path))
        File.write(absolute_path, contents)
      end

      yield app_root
    end
  end
  private :with_packaged_app

  def wait_for_http_response(port, request, wait_thread:, output:, timeout:, request_label:)
    Timeout.timeout(timeout) do
      loop do
        raise "#{request_label} exited early: #{process_diagnostics(wait_thread, output)}" unless wait_thread.alive?

        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
        socket.write(request)
        return read_raw_http_response(socket, wait_thread:, output:, request_label:)
      rescue Errno::ECONNREFUSED, Errno::EHOSTUNREACH, Errno::ECONNRESET, EOFError, Timeout::Error
        sleep 0.01
      ensure
        socket&.close unless socket.nil? || socket.closed?
      end
    end
  end
  private :wait_for_http_response
end

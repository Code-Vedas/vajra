# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module VajraE2EStartupHelpers
  def startup_failure(port:)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def startup_failure_with_env(port_value)
    Open3.popen2e(
      vajra_env.merge('VAJRA_PORT' => port_value),
      *vajra_command,
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def startup_failure_with_config_env(env)
    Open3.popen2e(vajra_env.merge(env), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def startup_failure_with_inline_start(env)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def startup_failure_with_inline_script(script, env: {})
    Open3.popen2e(vajra_env.merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def inline_start_command
    inline_ruby_command(<<~RUBY)
      require "vajra"
      options = {}
      options[:host] = ENV["RUBY_HOST"] if ENV.key?("RUBY_HOST")
      options[:port] = Integer(ENV["RUBY_PORT"]) if ENV.key?("RUBY_PORT")
      options[:workers] = Integer(ENV["RUBY_WORKERS"]) if ENV.key?("RUBY_WORKERS")
      options[:log_level] = ENV["RUBY_LOG_LEVEL"] if ENV.key?("RUBY_LOG_LEVEL")
      if ENV.key?("RUBY_THREADS")
        options[:threads] = ENV.fetch("RUBY_THREADS").split(",").map { |value| Integer(value.strip) }
      end
      if ENV.key?("RUBY_QUEUE_CAPACITY")
        options[:queue_capacity] = Integer(ENV.fetch("RUBY_QUEUE_CAPACITY"))
      end
      options[:scheduler_policy] = ENV["RUBY_SCHEDULER_POLICY"] if ENV.key?("RUBY_SCHEDULER_POLICY")
      options[:request_timeout] = Integer(ENV.fetch("RUBY_REQUEST_TIMEOUT")) if ENV.key?("RUBY_REQUEST_TIMEOUT")
      if ENV.key?("RUBY_REQUEST_HEAD_TIMEOUT")
        options[:request_head_timeout] = Integer(ENV.fetch("RUBY_REQUEST_HEAD_TIMEOUT"))
      end
      if ENV.key?("RUBY_FIRST_DATA_TIMEOUT")
        options[:first_data_timeout] = Integer(ENV.fetch("RUBY_FIRST_DATA_TIMEOUT"))
      end
      if ENV.key?("RUBY_PERSISTENT_TIMEOUT")
        options[:persistent_timeout] = Integer(ENV.fetch("RUBY_PERSISTENT_TIMEOUT"))
      end
      options[:worker_timeout] = Integer(ENV.fetch("RUBY_WORKER_TIMEOUT")) if ENV.key?("RUBY_WORKER_TIMEOUT")
      if ENV.key?("RUBY_MAX_REQUEST_HEAD_BYTES")
        options[:max_request_head_bytes] = Integer(ENV.fetch("RUBY_MAX_REQUEST_HEAD_BYTES"))
      end
      Vajra.start(**options)
    RUBY
  end

  def programmatic_shutdown(max_attempts: 3, stop_before_start: false)
    script = <<~RUBY
      require "socket"
      require "timeout"
      require "vajra"

      port = Integer(ENV.fetch("VAJRA_PORT"))
      host = "#{VajraE2EHelpers::LISTENER_HOST}"
      bind_host = "#{VajraE2EHelpers::LISTENER_BIND_HOST}"
      Thread.report_on_exception = false

      def server_ready?(host, port)
        Timeout.timeout(0.1) do
          socket = TCPSocket.new(host, port)
          socket.write("GET / HTTP/1.1\\r\\nHost: localhost\\r\\nConnection: close\\r\\n\\r\\n")
          response = socket.readpartial(1024)
          response.include?("HTTP/1.1 200 OK")
        ensure
          socket&.close
        end
      rescue Timeout::Error, Errno::ECONNREFUSED, Errno::EHOSTUNREACH, Errno::ECONNRESET, EOFError
        false
      end

      Vajra.stop if #{stop_before_start ? 'true' : 'false'}

      stopper_thread = Thread.new do
        Timeout.timeout(5) do
          loop do
            break if server_ready?(host, port)
            sleep 0.01
          end
        end

        Vajra.stop
      end

      Vajra.start
      Timeout.timeout(5) { stopper_thread.join }

      rebound_server = TCPServer.new(bind_host, port)
      rebound_server.close
    RUBY

    max_attempts.times do |attempt|
      selected_port = begin
        server = TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, 0)
        server.addr[1]
      ensure
        server&.close
      end
      result = Open3.popen2e(
        vajra_env(port: selected_port), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        status = Timeout.timeout(15) { wait_thread.value }
        { exitstatus: status.exitstatus, output: output.read, port: selected_port }
      ensure
        cleanup_process(wait_thread, output)
      end

      next if bind_conflict_output?(result[:output], result[:port]) && attempt < max_attempts - 1

      return result
    end
  end

  def oversized_request_result(env:, payload_size:)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(
        "GET / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "X-Oversized: #{'a' * payload_size}\r\n" \
        "Connection: close\r\n\r\n"
      )
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'oversized_request_result')
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def request_with_body_result(env:, body:)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(
        "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
      )
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'request_with_body_result')
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end
end

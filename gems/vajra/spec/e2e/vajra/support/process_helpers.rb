# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module VajraE2EProcessHelpers
  def read_available_output(output)
    captured = +''

    loop do
      captured << output.read_nonblock(4096)
    end
  rescue IO::WaitReadable, IOError, Errno::EIO
    captured
  end

  def process_diagnostics(wait_thread, output)
    fragments = []
    fragments << "pid=#{wait_thread.pid}"
    fragments << "alive=#{wait_thread.alive?}"

    unless wait_thread.alive?
      status = wait_thread.value
      fragments << "exitstatus=#{status.exitstatus.inspect}"
      fragments << "termsig=#{status.termsig.inspect}"
    end

    captured_output = read_available_output(output)
    fragments << "output=#{captured_output.inspect}" unless captured_output.empty?
    fragments.join(' ')
  end

  def wait_for_exit(wait_thread, timeout: 5)
    Timeout.timeout(timeout) { wait_thread.value }
  end

  def stop_process(wait_thread, signal: 'INT', timeout: 5)
    Process.kill(signal, wait_thread.pid)
    wait_for_exit(wait_thread, timeout: timeout)
  rescue Errno::ESRCH
    wait_thread.value
  end

  def cleanup_process(wait_thread, output)
    if wait_thread.alive?
      begin
        stop_process(wait_thread, timeout: 2)
      rescue Timeout::Error
        Process.kill('KILL', wait_thread.pid)
        wait_thread.value
      rescue Errno::ESRCH
        nil
      end
    end
  ensure
    output.read unless output.closed?
    output.close unless output.closed?
  end

  def bind_conflict_output?(output, port)
    output.include?("Unable to start Vajra: listener bind failed for 0.0.0.0:#{port}") &&
      output.include?('Address already in use')
  end

  def disposable_listener_port
    0
  end

  def bind_port(port: disposable_listener_port)
    TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, port)
  end

  def idle_keep_alive_timeout_result(port: disposable_listener_port, env: {})
    Open3.popen2e(vajra_env(port:).merge(env), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      response, trailing_bytes = read_http_response(
        socket.tap { |open_socket| open_socket.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n") },
        wait_thread:,
        output:,
        request_label: 'idle_keep_alive_timeout_result'
      )
      connection_closed = Timeout.timeout(VajraE2EHelpers::IDLE_KEEP_ALIVE_CLOSE_TIMEOUT_SECONDS) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, connection_closed:, trailing_bytes:, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def delayed_request_result(
    request:,
    initial_pause:,
    port: disposable_listener_port,
    env: {},
    timeout: 15
  )
    Open3.popen2e(vajra_env(port:).merge(env), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      begin
        sleep initial_pause
        socket.write(request)
        socket.close_write
        response = Timeout.timeout(timeout) { socket.read }
      ensure
        socket.close unless socket.closed?
      end

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def idle_shutdown(port: disposable_listener_port, signal: 'INT')
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      status = stop_process(wait_thread, signal:)

      { exitstatus: status.exitstatus, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def active_request_shutdown(request:, port: disposable_listener_port, signal: 'INT')
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(request)
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'active_request_shutdown')
      socket.close

      Process.kill(signal, wait_thread.pid)
      immediate_output = wait_for_graceful_shutdown_banner(output, wait_thread)
      status = wait_for_exit(wait_thread)

      {
        exitstatus: status.exitstatus,
        response:,
        immediate_output:,
        output: "#{startup_output.join}#{immediate_output}#{output.read}",
        port: selected_port
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def wait_for_graceful_shutdown_banner(output, wait_thread)
    captured_output = +''

    Timeout.timeout(1) do
      loop do
        captured_output << read_available_output(output)
        break captured_output if captured_output.include?('- Gracefully shutting down workers...')
        raise Timeout::Error if !wait_thread.alive? && captured_output.empty?

        sleep 0.01
      end
    end
  end

  def wait_for_socket_close(socket)
    Timeout.timeout(1) do
      socket.read
      true
    end
  rescue Timeout::Error
    false
  rescue EOFError, Errno::ECONNRESET
    true
  end

  def keep_alive_shutdown_with_open_socket(
    followup_chunks: [],
    port: disposable_listener_port,
    signal: 'INT',
    exit_timeout: 2
  )
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'keep_alive_shutdown_with_open_socket')
      followup_chunks.each { |chunk| socket.write(chunk) }

      Process.kill(signal, wait_thread.pid)
      immediate_output = wait_for_graceful_shutdown_banner(output, wait_thread)
      socket_closed = wait_for_socket_close(socket)
      status = wait_for_exit(wait_thread, timeout: exit_timeout)

      {
        exitstatus: status.exitstatus,
        response:,
        socket_closed:,
        immediate_output:,
        output: "#{startup_output.join}#{immediate_output}#{output.read}",
        port: selected_port
      }
    ensure
      socket.close unless socket.nil? || socket.closed?
      cleanup_process(wait_thread, output)
    end
  end

  def request_chunks_result(chunks:, port: disposable_listener_port, env: {}, timeout: 15, pause: nil)
    script = <<~RUBY
      require "timeout"
      require "vajra"
      Thread.report_on_exception = false

      stopper_thread = Thread.new do
        STDIN.read
        Vajra.stop
      end

      Vajra.start
      Timeout.timeout(5) { stopper_thread.join }
    RUBY

    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      begin
        chunks.each do |chunk|
          socket.write(chunk)
          sleep pause if pause
        end
        socket.close_write
        response = Timeout.timeout(timeout) { socket.read }
      rescue Errno::EPIPE, Errno::ECONNRESET
        begin
          response = Timeout.timeout(timeout) { socket.read }
        rescue Errno::ECONNRESET
          response = ''
        end
      ensure
        socket.close unless socket.closed?
      end

      stdin.close
      status = Timeout.timeout(timeout) { wait_thread.value }

      { exitstatus: status.exitstatus, response:, output: "#{startup_output.join}#{output.read}", port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def raw_request_result(request:, port: disposable_listener_port, env: {}, timeout: 15)
    request_chunks_result(chunks: [request], port:, env:, timeout:)
  end

  def fragmented_request_result(chunks:, port: disposable_listener_port, env: {}, timeout: 15, pause: 0.01)
    request_chunks_result(chunks:, port:, env:, timeout:, pause:)
  end

  def thrash_cycles
    5
  end
end

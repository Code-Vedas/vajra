# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module VajraE2EProcessHelpers
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
    output.include?("Unable to start Vajra: listener bind failed for port #{port}") &&
      output.include?('Address already in use')
  end

  def disposable_listener_port
    0
  end

  def candidate_listener_port
    server = TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, 0)
    server.addr[1]
  ensure
    server&.close
  end

  def bind_port(port: disposable_listener_port)
    TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, port)
  end

  def idle_keep_alive_timeout_result(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      response, = read_http_response(
        socket.tap { |open_socket| open_socket.write("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n") }
      )
      connection_closed = Timeout.timeout(VajraE2EHelpers::IDLE_KEEP_ALIVE_CLOSE_TIMEOUT_SECONDS) { socket.read == '' }
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, connection_closed:, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def idle_shutdown(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, output: output.read, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def request_chunks_result(chunks:, port: disposable_listener_port, env: {}, timeout: 15, pause: nil)
    script = <<~RUBY
      require "timeout"
      require "vajra"
      Thread.report_on_exception = false

      server_thread = Thread.new { Vajra.start }
      begin
        STDIN.read
      ensure
        Vajra.stop
        Timeout.timeout(5) { server_thread.join }
      end
    RUBY

    Open3.popen2e(vajra_env(port:).merge(env), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      begin
        chunks.each do |chunk|
          socket.write(chunk)
          sleep pause if pause
        end
        socket.close_write
        response = Timeout.timeout(timeout) { socket.read }
      ensure
        socket.close unless socket.closed?
      end

      stdin.close
      status = Timeout.timeout(timeout) { wait_thread.value }

      { exitstatus: status.exitstatus, response:, output: output.read, port: selected_port }
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

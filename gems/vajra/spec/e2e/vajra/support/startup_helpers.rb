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
      options[:port] = Integer(ENV["RUBY_PORT"]) if ENV.key?("RUBY_PORT")
      if ENV.key?("RUBY_MAX_REQUEST_HEAD_BYTES")
        options[:max_request_head_bytes] = Integer(ENV.fetch("RUBY_MAX_REQUEST_HEAD_BYTES"))
      end
      Vajra.start(**options)
    RUBY
  end

  def programmatic_shutdown(max_attempts: 3)
    script = <<~RUBY
      require "socket"
      require "timeout"
      require "vajra"

      port = Integer(ENV.fetch("VAJRA_PORT"))
      host = "#{VajraE2EHelpers::LISTENER_HOST}"
      bind_host = "#{VajraE2EHelpers::LISTENER_BIND_HOST}"
      Thread.report_on_exception = false

      def listener_ready?(host, port)
        socket = TCPSocket.new(host, port)
        socket.close
        true
      rescue Errno::ECONNREFUSED, Errno::EHOSTUNREACH
        false
      end

      server_thread = Thread.new { Vajra.start }

      Timeout.timeout(5) do
        loop do
          break if listener_ready?(host, port)
          sleep 0.01
        end
      end

      Vajra.stop
      Timeout.timeout(5) { server_thread.join }

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
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(
        "GET / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "X-Oversized: #{'a' * payload_size}\r\n" \
        "Connection: close\r\n\r\n"
      )
      response = socket.read
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: output.read, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def request_with_body_result(env:, body:)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write(
        "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
      )
      response = +''
      begin
        loop do
          response << socket.readpartial(4096)
        end
      rescue EOFError, Errno::ECONNRESET
        nil
      end
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response:, output: output.read, port: selected_port }
    ensure
      cleanup_process(wait_thread, output)
    end
  end
end

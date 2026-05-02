# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'

RSpec.describe Vajra, :e2e, :integration do
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

  def request_response(port: disposable_listener_port)
    Open3.popen2e(vajra_env(port:), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response: response, port: selected_port }
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
      selected_port = candidate_listener_port
      result = Open3.popen2e(
        vajra_env(port: selected_port), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        status = Timeout.timeout(15) { wait_thread.value }
        { exitstatus: status.exitstatus, output: output.read }
      ensure
        cleanup_process(wait_thread, output)
      end

      next if bind_conflict_output?(result[:output], selected_port) && attempt < max_attempts - 1

      return result
    end
  end

  def bind_port(port: disposable_listener_port)
    TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, port)
  end

  def request_response_from_inline_start(env:, timeout: 15)
    Open3.popen2e(vajra_env.merge(env), *inline_start_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      selected_port = wait_for_banner(output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      status = stop_process(wait_thread, timeout:)

      { exitstatus: status.exitstatus, response: response, port: selected_port, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
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

  def raw_request_result(request:, port: disposable_listener_port, env: {}, timeout: 15)
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
        socket.write(request)
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

  def thrash_cycles
    5
  end

  it 'boots and serves a basic HTTP response' do
    expect(request_response).to include(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end

  it 'rejects a malformed request line with 400 Bad Request' do
    result = raw_request_result(
      request: "GET /only-two-parts\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 400 Bad Request')
    expect(result[:response]).to include('Bad Request')
    expect(result[:output]).to include('request rejected (400 bad request)')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'rejects an invalid HTTP version with 400 Bad Request' do
    result = raw_request_result(
      request: "GET / HTTP/2.0\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 400 Bad Request')
    expect(result[:response]).to include('Bad Request')
  end

  it 'shuts down cleanly while idle and releases the listener for immediate restart' do
    shutdown = idle_shutdown

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:output]).not_to include('accept failed')

    rebound_server = bind_port(port: shutdown[:port])
    rebound_server.close

    expect(request_response(port: shutdown[:port])).to include(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end

  it 'supports programmatic Vajra.stop and releases the listener' do
    shutdown = programmatic_shutdown

    expect(shutdown[:exitstatus]).to eq(0), shutdown[:output]
    expect(shutdown[:output]).not_to include('accept failed')
  end

  it 'fails startup with actionable bind diagnostics and releases startup resources' do
    blocking_server = bind_port
    blocked_port = blocking_server.addr[1]

    begin
      failure = startup_failure(port: blocked_port)

      expect(failure).to match(
        exitstatus: be_positive,
        output: a_string_including(
          "Unable to start Vajra: listener bind failed for port #{blocked_port}"
        )
      )
      expect(failure[:output]).not_to include(listener_banner(blocked_port))
    ensure
      blocking_server.close
    end

    rebound_server = bind_port(port: blocked_port)
    rebound_server.close
  end

  it 'fails startup with actionable VAJRA_PORT validation errors' do
    failure = startup_failure_with_env('not-a-port')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid VAJRA_PORT: not-a-port')
    )
    expect(failure[:output]).to include('Expected an integer between 0 and 65535')
    expect(failure[:output]).to include('Use 0 to request an ephemeral port')
  end

  it 'fails startup with actionable VAJRA_MAX_REQUEST_HEAD_BYTES validation errors' do
    failure = startup_failure_with_config_env('VAJRA_MAX_REQUEST_HEAD_BYTES' => '0')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: invalid VAJRA_MAX_REQUEST_HEAD_BYTES: 0'
      )
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 2147483647')
  end

  it 'fails startup with actionable Ruby option validation errors' do
    failure = startup_failure_with_inline_start('RUBY_PORT' => '-1')

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid port option: -1')
    )
    expect(failure[:output]).to include('Expected an integer between 0 and 65535')
  end

  it 'fails startup with actionable unknown Ruby option errors' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"
      Vajra.start(potr: 3000)
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: unknown start option: potr')
    )
  end

  it 'lets Ruby configure the listener port when VAJRA_PORT is unset' do
    selected_port = candidate_listener_port
    request = request_response_from_inline_start(env: { 'RUBY_PORT' => selected_port.to_s })

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to eq(selected_port)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'prefers VAJRA_PORT over the Ruby port option' do
    ruby_port = candidate_listener_port
    env_port = candidate_listener_port

    request = request_response_from_inline_start(
      env: { 'RUBY_PORT' => ruby_port.to_s, 'VAJRA_PORT' => env_port.to_s }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to eq(env_port)
    expect(request[:port]).not_to eq(ruby_port)
  end

  it 'lets Ruby configure max_request_head_bytes when the env variable is unset' do
    result = oversized_request_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'RUBY_MAX_REQUEST_HEAD_BYTES' => (32 * 1024).to_s
      },
      payload_size: 20 * 1024
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'prefers VAJRA_MAX_REQUEST_HEAD_BYTES over the Ruby max_request_head_bytes option' do
    result = oversized_request_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128',
        'RUBY_MAX_REQUEST_HEAD_BYTES' => (64 * 1024).to_s
      },
      payload_size: 512
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 431 Request Header Fields Too Large')
    expect(result[:output]).to include('request rejected (431 request header fields too large)')
  end

  it 'rejects an oversized header section with 431 Request Header Fields Too Large' do
    result = raw_request_result(
      port: disposable_listener_port,
      env: { 'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128' },
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nX-Oversized: #{'a' * 256}\r\nConnection: close\r\n\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 431 Request Header Fields Too Large')
    expect(result[:response]).to include('Request Header Fields Too Large')
    expect(result[:output]).to include('request rejected (431 request header fields too large)')
  end

  it 'accepts a request when only bytes after the header boundary exceed the configured limit' do
    body = 'b' * 4096
    result = request_with_body_result(
      env: {
        'VAJRA_PORT' => disposable_listener_port.to_s,
        'VAJRA_MAX_REQUEST_HEAD_BYTES' => '128'
      },
      body:
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include('HTTP/1.1 200 OK')
    expect(result[:output]).not_to include('request head exceeds maximum size')
  end

  it 'closes an incomplete request without producing a success response' do
    result = raw_request_result(
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('request rejected (400 bad request)')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'survives repeated process start stop thrashing and releases the listener every time' do
    current_port = disposable_listener_port

    thrash_cycles.times do
      request = request_response(port: current_port)

      expect(request).to include(
        exitstatus: 0,
        response: a_string_including('HTTP/1.1 200 OK')
      )
      current_port = request[:port]

      shutdown = idle_shutdown(port: current_port)
      expect(shutdown[:exitstatus]).to eq(0)
      expect(shutdown[:output]).not_to include('accept failed')

      rebound_server = bind_port(port: current_port)
      rebound_server.close
    end
  end
end

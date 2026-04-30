# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'

RSpec.describe Vajra, :e2e, :integration do
  let(:listener_port) { available_listener_port }

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

  def request_response(port: listener_port)
    Open3.popen2e(vajra_env(port), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      wait_for_banner(output, port: port)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
      socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      response = socket.read
      socket.close

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, response: response }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def idle_shutdown(port: listener_port)
    Open3.popen2e(vajra_env(port), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      wait_for_banner(output, port: port)

      status = stop_process(wait_thread)

      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def startup_failure(port: listener_port)
    Open3.popen2e(vajra_env(port), *vajra_command, chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def programmatic_shutdown(port: listener_port)
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

    Open3.popen2e(vajra_env(port), *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      status = Timeout.timeout(15) { wait_thread.value }
      { exitstatus: status.exitstatus, output: output.read }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def bind_port(port: listener_port)
    TCPServer.new(VajraE2EHelpers::LISTENER_BIND_HOST, port)
  end

  def thrash_cycles
    5
  end

  it 'boots and serves a basic HTTP response' do
    expect(request_response).to match(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end

  it 'shuts down cleanly while idle and releases the listener for immediate restart' do
    shutdown = idle_shutdown

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:output]).not_to include('accept failed')

    rebound_server = bind_port
    rebound_server.close

    expect(request_response).to match(
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
        exitstatus: a_value > 0,
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

  it 'survives repeated process start stop thrashing and releases the listener every time' do
    thrash_cycles.times do
      expect(request_response).to match(
        exitstatus: 0,
        response: a_string_including('HTTP/1.1 200 OK')
      )

      shutdown = idle_shutdown
      expect(shutdown[:exitstatus]).to eq(0)
      expect(shutdown[:output]).not_to include('accept failed')

      rebound_server = bind_port
      rebound_server.close
    end
  end
end

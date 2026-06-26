# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe 'Vajra lifecycle', :e2e, :integration do
  it 'shuts down cleanly while idle and releases the listener for immediate restart' do
    shutdown = idle_shutdown

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:output]).not_to include('accept failed')
    expect(shutdown[:output]).to include(
      '=== vajra boot:',
      'Vajra starting in master-worker mode...',
      '* Environment: development',
      '* Master PID:',
      '* Workers: 1',
      'Use Ctrl-C to stop',
      '- Gracefully shutting down workers...',
      '=== vajra shutdown:',
      '- Goodbye!'
    )
    expect(shutdown[:output]).not_to include(
      'event=worker_ready',
      'event=booting',
      'event=boot_complete',
      'event=serving_entered',
      'event=drain_requested',
      'event=stop_completed'
    )

    rebound_server = bind_port(port: shutdown[:port])
    rebound_server.close

    expect(request_response(port: shutdown[:port])).to include(
      exitstatus: 0,
      response: a_string_including('HTTP/1.1 200 OK')
    )
  end

  it 'shuts down cleanly on SIGTERM and releases the listener immediately' do
    shutdown = idle_shutdown(signal: 'TERM')

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:output]).not_to include('accept failed')

    rebound_server = bind_port(port: shutdown[:port])
    rebound_server.close
  end

  it 'prints the graceful shutdown banner immediately after Ctrl-C even after requests have flowed' do
    shutdown = active_request_shutdown(
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
    )

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:response]).to include('HTTP/1.1 200 OK')
    expect(shutdown[:immediate_output]).to include('- Gracefully shutting down workers...')
    expect(shutdown[:output]).to include(
      '- Gracefully shutting down workers...',
      '=== vajra shutdown:',
      '- Goodbye!'
    )
  end

  it 'drains an active Rack request before worker shutdown exits' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_env|
          STDERR.puts "rack-app-started"
          STDERR.flush
          sleep 0.25
          [200, { "Content-Type" => "text/plain" }, ["drained"]]
        end
      )

      Vajra.start(worker_timeout: 3)
    RUBY

    managed_popen2e(vajra_env(port: disposable_listener_port), *inline_ruby_command(script),
                    chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
      socket.write("GET /slow HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
      runtime_output = +''
      Timeout.timeout(2) do
        loop do
          runtime_output << read_available_output(output)
          break if runtime_output.include?('rack-app-started')

          sleep 0.01
        end
      end

      signal_process_group(wait_thread, 'INT')
      immediate_output = wait_for_graceful_shutdown_banner(output, wait_thread)
      response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'active_drain_shutdown')
      socket.close
      status = wait_for_exit(wait_thread, timeout: 5)

      expect(status.exitstatus).to eq(0)
      expect(response).to include('HTTP/1.1 200 OK')
      expect(response).to end_with('drained')
      expect("#{startup_output.join}#{runtime_output}#{immediate_output}#{output.read}").to include(
        '- Gracefully shutting down workers...',
        '=== vajra shutdown:',
        '- Goodbye!'
      )
    ensure
      socket.close unless socket.nil? || socket.closed?
      cleanup_process(wait_thread, output)
    end
  end

  it 'interrupts an idle keep-alive socket during Ctrl-C drain' do
    shutdown = keep_alive_shutdown_with_open_socket

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:response]).to include('HTTP/1.1 200 OK')
    expect(shutdown[:socket_closed]).to be(true)
    expect(shutdown[:immediate_output]).to include('- Gracefully shutting down workers...')
    expect(shutdown[:output]).to include(
      '- Gracefully shutting down workers...',
      '=== vajra shutdown:',
      '- Goodbye!'
    )
  end

  it 'interrupts a partial next request instead of waiting for request head timeout during Ctrl-C drain' do
    shutdown = keep_alive_shutdown_with_open_socket(
      followup_chunks: [
        "GET /next HTTP/1.1\r\n",
        "Host: localhost\r\n"
      ],
      env: {
        'VAJRA_REQUEST_HEAD_TIMEOUT' => '30',
        'VAJRA_WORKER_TIMEOUT' => '3'
      },
      exit_timeout: 8
    )

    expect(shutdown[:exitstatus]).to eq(0)
    expect(shutdown[:response]).to include('HTTP/1.1 200 OK')
    expect(shutdown[:socket_closed]).to be(true)
    expect(shutdown[:immediate_output]).to include('- Gracefully shutting down workers...')
    expect(shutdown[:output]).to include(
      '- Gracefully shutting down workers...',
      '=== vajra shutdown:',
      '- Goodbye!'
    )
  end

  it 'supports programmatic Vajra.stop and releases the listener' do
    shutdown = programmatic_shutdown

    expect(shutdown[:exitstatus]).to eq(0), shutdown[:output]
    expect(shutdown[:output]).not_to include('accept failed')
    expect(shutdown[:output]).not_to include('event=drain_requested')
  end

  it 'does not let Vajra.stop before startup poison the next start' do
    shutdown = programmatic_shutdown(stop_before_start: true)

    expect(shutdown[:exitstatus]).to eq(0), shutdown[:output]
    expect(shutdown[:output]).not_to include('Vajra already running')
    expect(shutdown[:output]).not_to include('event=drain_requested')
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

  it 'closes after responding to a request with unread body framing' do
    result = request_with_body_result(
      env: { 'VAJRA_PORT' => disposable_listener_port.to_s },
      body: 'body'
    )
    response = parse_http_response(result[:response])

    expect(result[:exitstatus]).to eq(0)
    expect(response).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(response[:headers]).to include(
      'content-type' => 'text/plain',
      'content-length' => '2',
      'connection' => 'close'
    )
  end
end

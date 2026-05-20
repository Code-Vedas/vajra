# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'
require 'tmpdir'
require 'json'

RSpec.describe 'Vajra configuration', :e2e, :integration do # rubocop:disable RSpec/DescribeClass
  def post_request(body)
    "POST / HTTP/1.1\r\n" \
      "Host: localhost\r\n" \
      "Content-Type: text/plain\r\n" \
      "Content-Length: #{body.bytesize}\r\n" \
      "Connection: close\r\n\r\n" \
      "#{body}"
  end

  def fairness_order_script
    <<~RUBY
      require "vajra"

      order_mutex = Mutex.new

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = rack_env.fetch("rack.input").read
          order_mutex.synchronize do
            File.open(ENV.fetch("ORDER_PATH"), "a") { |file| file.puts(body) }
          end

          case body
          when "slow-a"
            sleep 1.0
          when "slow-b"
            sleep 0.2
          end

          [200, { "Content-Type" => "text/plain" }, [body]]
        end
      )

      Vajra.start(
        workers: 2,
        threads: [1, 1],
        queue_capacity: 10,
        scheduler_policy: "least_loaded",
        log_level: "debug"
      )
    RUBY
  end

  def open_request_sockets(port, count)
    Array.new(count) { TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port) }
  end

  def write_staged_request_bodies(sockets, request_bodies)
    sockets[0].write(post_request(request_bodies[0]))
    sockets[1].write(post_request(request_bodies[1]))
    sleep 0.05
    sockets[2].write(post_request(request_bodies[2]))
    sleep 0.05
    sockets[3].write(post_request(request_bodies[3]))
  end

  def read_responses_from_sockets(sockets, wait_thread, output)
    sockets.each_with_index.map do |socket, index|
      read_raw_http_response(
        socket,
        wait_thread:,
        output:,
        request_label: "staged_request_result:#{index}"
      )
    end
  end

  def staged_request_result(script:, env:, request_bodies:)
    Open3.popen2e(
      vajra_env(port: disposable_listener_port).merge(env),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)

      sockets = open_request_sockets(selected_port, request_bodies.length)

      begin
        write_staged_request_bodies(sockets, request_bodies)
        responses = read_responses_from_sockets(sockets, wait_thread, output)

        status = stop_process(wait_thread)
        { exitstatus: status.exitstatus, responses:, output: "#{startup_output.join}#{output.read}" }
      ensure
        sockets.each { |socket| socket.close unless socket.closed? }
      end
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  it 'fails startup with actionable bind diagnostics and releases startup resources' do
    blocking_server = bind_port
    blocked_port = blocking_server.addr[1]

    begin
      failure = startup_failure(port: blocked_port)

      expect(failure).to match(
        exitstatus: be_positive,
        output: a_string_including(
          "Unable to start Vajra: listener bind failed for 0.0.0.0:#{blocked_port}"
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
      output: a_string_including('unknown start option: potr')
    )
  end

  it 'fails startup when Vajra.start is invoked from a non-main Ruby thread' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      worker = Thread.new { Vajra.start }
      worker.join
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: worker-only Vajra.start must be invoked from the Ruby main thread'
      )
    )
  end

  it 'fails startup with actionable Ruby boot diagnostics' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      Vajra::Internal::Boot.install!(lambda do |_boot_request|
        raise "boot exploded"
      end)

      Vajra.start
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: Ruby boot failed (boot_callback_error/boot): RuntimeError: boot exploded'
      )
    )
    expect(failure[:output]).not_to include('listening on port')
  end

  it 'fails startup with actionable worker bootstrap diagnostics' do
    failure = startup_failure_with_inline_script(<<~RUBY)
      require "vajra"

      Vajra::Internal::Boot.install!(lambda do |boot_request|
        next({ status: "ready", role: boot_request.fetch(:runtime_role) }) if boot_request.fetch(:runtime_role) == "ruby_master_preload"

        {
          status: "failed",
          role: boot_request.fetch(:runtime_role),
          diagnostic: {
            code: "worker_boot_failed",
            category: "boot",
            message: "worker activation exploded"
          }
        }
      end)

      Vajra.start
    RUBY

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including(
        'Unable to start Vajra: Ruby worker boot failed (worker_boot_failed/boot): worker activation exploded'
      )
    )
    expect(failure[:output]).not_to include('listening on port')
  end

  it 'preloads once in the master and exposes inherited state to the worker' do
    Dir.mktmpdir('vajra-preload-proof') do |dir|
      trace_path = File.join(dir, 'boot_trace.txt')

      script = <<~RUBY
        require "vajra"

        $vajra_preloaded_marker = nil

        Vajra::Internal::Boot.install!(lambda do |boot_request|
          File.open(ENV.fetch("BOOT_TRACE_PATH"), "a") do |file|
            file.puts(boot_request.fetch(:runtime_role))
          end

          case boot_request.fetch(:runtime_role)
          when "ruby_master_preload"
            $vajra_preloaded_marker = "preloaded-once"
          when "ruby_worker_bootstrap"
            raise "worker did not inherit preloaded marker" unless $vajra_preloaded_marker == "preloaded-once"
          end

          { status: "ready", role: boot_request.fetch(:runtime_role) }
        end)

        options = {}
        options[:port] = Integer(ENV["RUBY_PORT"]) if ENV.key?("RUBY_PORT")
        Vajra.start(**options)
      RUBY

      request = Open3.popen2e(
        vajra_env.merge('RUBY_PORT' => disposable_listener_port.to_s, 'BOOT_TRACE_PATH' => trace_path),
        *inline_ruby_command(script),
        chdir: VajraE2EHelpers::PACKAGE_ROOT
      ) do |_stdin, output, wait_thread|
        startup_output = []
        selected_port = wait_for_banner(output, captured_lines: startup_output)

        socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, selected_port)
        socket.write("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        response = read_raw_http_response(socket, wait_thread:, output:, request_label: 'preload_inheritance')
        socket.close

        status = stop_process(wait_thread)

        { exitstatus: status.exitstatus, response:, port: selected_port, output: "#{startup_output.join}#{output.read}" }
      ensure
        cleanup_process(wait_thread, output)
      end

      expect(request[:exitstatus]).to eq(0), request[:output]
      expect(request[:response]).to include('HTTP/1.1 200 OK')
      expect(File.readlines(trace_path, chomp: true)).to eq(%w[ruby_master_preload ruby_worker_bootstrap])
    end
  end

  it 'lets Ruby configure the listener port when VAJRA_PORT is unset' do
    request = request_response_from_inline_start(env: { 'RUBY_PORT' => disposable_listener_port.to_s })

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'lets Ruby configure the listener host when VAJRA_HOST is unset' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  end

  it 'starts the configured worker count and reports it in startup output' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '4'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include(
      '* Workers: 4',
      '- Worker 0 (PID:',
      '- Worker 1 (PID:',
      '- Worker 2 (PID:',
      '- Worker 3 (PID:'
    )
  end

  it 'prefers WEB_CONCURRENCY over the Ruby worker option when VAJRA_WORKERS is unset' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '2',
        'WEB_CONCURRENCY' => '3'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include('* Workers: 3')
    expect(request[:output]).to include('- Worker 2 (PID:')
  end

  it 'prefers MAX_THREADS over the Ruby max thread option and reports the effective range' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_THREADS' => '2,4',
        'MAX_THREADS' => '6'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
    expect(request[:output]).to include(
      '* Min threads: 2',
      '* Max threads: 6'
    )
  end

  it 'fails startup with actionable thread-range validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_THREADS' => '5,2'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid threads option: expected thread range with 1 <= min <= max')
    )
  end

  it 'fails startup with actionable MAX_THREADS validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_THREADS' => '2,4',
      'MAX_THREADS' => '0'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid MAX_THREADS: 0')
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 1024')
  end

  it 'fails startup with actionable scheduler policy validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_SCHEDULER_POLICY' => 'round_robin'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid scheduler_policy option: round_robin')
    )
    expect(failure[:output]).to include('Expected: least_loaded')
  end

  it 'fails startup with actionable request timeout validation errors' do
    failure = startup_failure_with_inline_start(
      'RUBY_PORT' => disposable_listener_port.to_s,
      'RUBY_REQUEST_TIMEOUT' => '0'
    )

    expect(failure).to match(
      exitstatus: be_positive,
      output: a_string_including('Unable to start Vajra: invalid request_timeout option: 0')
    )
    expect(failure[:output]).to include('Expected an integer between 1 and 2147483647')
  end

  it 'emits internal lifecycle boot events only when debug logging is enabled' do
    request = request_response_from_inline_start(
      env: {
        'RUBY_HOST' => '127.0.0.1',
        'RUBY_PORT' => disposable_listener_port.to_s,
        'RUBY_WORKERS' => '2',
        'RUBY_LOG_LEVEL' => 'debug'
      }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:output]).to include(
      'event=worker_ready',
      'event=booting',
      'event=boot_complete',
      'event=serving_entered',
      'event=drain_requested',
      'event=stop_completed'
    )
  end

  it 'routes concurrent requests across the least-loaded workers and reports scheduler activity in debug output' do
    script = <<~RUBY
      require "json"
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          sleep 0.2
          body = rack_env.fetch("rack.input").read
          [200, { "Content-Type" => "application/json" }, [JSON.generate(pid: Process.pid, body: body)]]
        end
      )

      Vajra.start(
        workers: 2,
        threads: [1, 1],
        queue_capacity: 2,
        scheduler_policy: "least_loaded",
        log_level: "debug"
      )
    RUBY

    request = lambda do |body|
      "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Type: text/plain\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
    end

    result = concurrent_rack_app_request_results(
      script:,
      requests: [request.call('first'), request.call('second')]
    )

    expect(result[:exitstatus]).to eq(0), result[:output]
    payloads = result[:responses].map { |response| JSON.parse(parse_http_response(response)[:body]) }
    expect(payloads.map { |payload| payload.fetch('pid') }.uniq.size).to eq(2)
    expect(result[:output]).to include(
      'event=request_admitted',
      'policy=least_loaded',
      'selected_worker=0',
      'selected_worker=1'
    )
  end

  it 'caps the single global FIFO queue and reports the hard-cap rejection in debug output' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          sleep 0.75
          [200, { "Content-Type" => "text/plain" }, ["OK"]]
        end
      )

      Vajra.start(
        workers: 1,
        threads: [1, 1],
        queue_capacity: 1,
        scheduler_policy: "least_loaded",
        log_level: "debug"
      )
    RUBY

    request = lambda do |body|
      "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Type: text/plain\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
    end

    result = concurrent_rack_app_request_results(
      script:,
      requests: [request.call('one'), request.call('two'), request.call('three')]
    )

    expect(result[:exitstatus]).to eq(0), result[:output]
    responses = result[:responses].map { |response| parse_http_response(response) }
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 200 OK' }).to eq(2)
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 503 Service Unavailable' }).to eq(1)
    expect(result[:output]).to include(
      'event=queue_capacity_reached',
      'queue_capacity=1'
    )
  end

  it 'keeps the queued request in the global FIFO until request_timeout expires' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          sleep 1.2
          [200, { "Content-Type" => "text/plain" }, ["OK"]]
        end
      )

      Vajra.start(
        workers: 1,
        threads: [1, 1],
        queue_capacity: 10,
        request_timeout: 1,
        scheduler_policy: "least_loaded",
        log_level: "debug"
      )
    RUBY

    request = lambda do |body|
      "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Type: text/plain\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
    end

    result = concurrent_rack_app_request_results(
      script:,
      requests: [request.call('slow'), request.call('queued')]
    )

    expect(result[:exitstatus]).to eq(0), result[:output]
    responses = result[:responses].map { |response| parse_http_response(response) }
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 200 OK' }).to eq(1)
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 408 Request Timeout' }).to eq(1)
  end

  it 'preserves FIFO order for queued requests under mixed latency' do
    Dir.mktmpdir('vajra-scheduler-order') do |dir|
      order_path = File.join(dir, 'request_order.txt')

      result = staged_request_result(
        script: fairness_order_script,
        env: { 'ORDER_PATH' => order_path },
        request_bodies: %w[slow-a slow-b queued-c queued-d]
      )

      expect(result[:exitstatus]).to eq(0), result[:output]
      responses = result[:responses].map { |response| parse_http_response(response) }
      expect(responses.map { |response| response[:status_line] }).to all(eq('HTTP/1.1 200 OK'))

      start_order = File.readlines(order_path, chomp: true)
      expect(start_order.index('queued-c')).to be < start_order.index('queued-d')
    end
  end

  it 'keeps queued requests scheduler-owned when a worker times out' do
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |rack_env|
          body = rack_env.fetch("rack.input").read

          case body
          when "hang"
            sleep 2
          else
            sleep 0.2
          end

          [200, { "Content-Type" => "text/plain" }, [body]]
        end
      )

      Vajra.start(
        workers: 2,
        threads: [1, 1],
        queue_capacity: 10,
        request_timeout: 5,
        worker_timeout: 1,
        scheduler_policy: "least_loaded",
        log_level: "debug"
      )
    RUBY

    request = lambda do |body|
      "POST / HTTP/1.1\r\n" \
        "Host: localhost\r\n" \
        "Content-Type: text/plain\r\n" \
        "Content-Length: #{body.bytesize}\r\n" \
        "Connection: close\r\n\r\n" \
        "#{body}"
    end

    result = concurrent_rack_app_request_results(
      script:,
      requests: [request.call('hang'), request.call('fast'), request.call('queued')]
    )

    responses = result[:responses].map { |response| parse_http_response(response) }
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 200 OK' }).to eq(2)
    expect(responses.count { |response| response[:status_line] == 'HTTP/1.1 500 Internal Server Error' }).to eq(1)
    expect(responses.select { |response| response[:status_line] == 'HTTP/1.1 200 OK' }.map { |response| response[:body] })
      .to include('fast', 'queued')
    expect(result[:output]).to include('event=worker_timeout')
    expect(result[:output]).to include('worker process exited unexpectedly due to signal 9')
  end

  it 'applies the configured request head timeout to fragmented request headers' do
    result = fragmented_request_result(
      chunks: [
        "GET /fragmented HTTP/1.1\r\nHost: localhost\r\nConnection: close\r",
        "\n",
        "\r",
        "\n"
      ],
      env: { 'VAJRA_REQUEST_HEAD_TIMEOUT' => '1' },
      pause: 1.2
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'applies the configured first data timeout before the request head starts' do
    result = delayed_request_result(
      request: "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
      initial_pause: 1.2,
      env: { 'VAJRA_FIRST_DATA_TIMEOUT' => '1' }
    )

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to eq('')
    expect(result[:output]).not_to include('HTTP/1.1 200 OK')
  end

  it 'applies the configured persistent timeout between keepalive requests' do
    result = idle_keep_alive_timeout_result(env: { 'VAJRA_PERSISTENT_TIMEOUT' => '1' })

    expect(result[:exitstatus]).to eq(0)
    expect(result[:response]).to include(
      status_line: 'HTTP/1.1 200 OK',
      body: 'OK'
    )
    expect(result[:connection_closed]).to be(true)
    expect(result[:trailing_bytes]).to eq('')
  end

  it 'prefers VAJRA_PORT over the Ruby port option even when the Ruby port would conflict' do
    blocking_server = nil
    blocking_server = bind_port
    ruby_port = blocking_server.addr[1]

    request = request_response_from_inline_start(
      env: { 'RUBY_PORT' => ruby_port.to_s, 'VAJRA_PORT' => disposable_listener_port.to_s }
    )

    expect(request[:exitstatus]).to eq(0)
    expect(request[:port]).to be_positive
    expect(request[:port]).not_to eq(ruby_port)
    expect(request[:response]).to include('HTTP/1.1 200 OK')
  ensure
    blocking_server&.close
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
end

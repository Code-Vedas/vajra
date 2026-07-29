# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'support'

RSpec.describe Vajra, :e2e, :integration do
  def windows_ipc_fault_result(fault)
    script = <<~RUBY
      require "vajra"

      Vajra::Internal::RackExecution.install!(
        lambda do |_rack_env|
          [200, { "Content-Type" => "text/plain" }, ["worker-ok"]]
        end
      )
      Vajra.start(workers: 1, threads: [1, 1], worker_timeout: 1, log_level: "debug")
    RUBY

    env = vajra_env(port: disposable_listener_port).merge('VAJRA_WINDOWS_TEST_FAULT' => fault)
    managed_popen2e(env, *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      successful_before_fault = fault == 'duplicate_dispatch' ? resilience_request(selected_port, wait_thread, output) : nil
      fault_closed_connection = resilience_fault_request(selected_port)
      runtime_output = +''
      wait_for_runtime_output(output, runtime_output, 'event=worker_replacement_ready', timeout: 10) unless fault == 'partial_socket_transfer'
      successful_after_fault = resilience_request(selected_port, wait_thread, output)
      status = stop_process(wait_thread)
      {
        exitstatus: status.exitstatus,
        fault_closed_connection:,
        successful_before_fault:,
        successful_after_fault:,
        output: "#{startup_output.join}#{runtime_output}#{output.read}"
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def resilience_request(port, wait_thread, output)
    single_rack_app_response(
      selected_port: port,
      request: "GET /healthy HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
      wait_thread:,
      output:,
      request_label: 'worker_resilience'
    )
  end

  def resilience_fault_request(port)
    socket = TCPSocket.new(VajraE2EHelpers::LISTENER_HOST, port)
    socket.write("GET /fault HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
    Timeout.timeout(10) { socket.read.empty? }
  rescue Errno::ECONNRESET, Errno::ECONNABORTED
    true
  ensure
    socket&.close
  end

  def windows_drain_timeout_result
    script = <<~RUBY
      require "vajra"
      Vajra::Internal::RackExecution.install!(->(_env) { [200, { "Content-Type" => "text/plain" }, ["ok"]] })
      Vajra.start(workers: 1, threads: [1, 1], worker_timeout: 1, log_level: "debug")
    RUBY
    env = vajra_env(port: disposable_listener_port).merge('VAJRA_WINDOWS_TEST_FAULT' => 'drain_timeout')
    managed_popen2e(env, *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      response = resilience_request(selected_port, wait_thread, output)
      started_at = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      status = stop_process(wait_thread, timeout: 10)
      elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started_at
      { exitstatus: status.exitstatus, elapsed:, response:, output: "#{startup_output.join}#{output.read}" }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def windows_replacement_exhaustion_result
    script = <<~RUBY
      require "vajra"
      Vajra::Internal::RackExecution.install!(->(_env) { [200, { "Content-Type" => "text/plain" }, ["ok"]] })
      Vajra.start(workers: 1, threads: [1, 1], worker_timeout: 1, log_level: "debug")
    RUBY
    env = vajra_env(port: disposable_listener_port).merge('VAJRA_WINDOWS_TEST_FAULT' => 'replacement_exhaustion')
    managed_popen2e(env, *inline_ruby_command(script), chdir: VajraE2EHelpers::PACKAGE_ROOT) do |_stdin, output, wait_thread|
      startup_output = []
      selected_port = wait_for_banner(output, captured_lines: startup_output)
      connection_closed = resilience_fault_request(selected_port)
      status = wait_for_exit(wait_thread, timeout: 15)
      complete_output = "#{startup_output.join}#{output.read}"
      worker_pids = complete_output.scan(/Worker \d+ \(PID: (\d+)\)/).flatten.map { |pid| Integer(pid) }
      Timeout.timeout(10) do
        sleep 0.05 while worker_pids.any? { |pid| windows_process_alive?(pid) }
      end
      rebound_listener = bind_port(port: selected_port)
      rebound_listener.close
      {
        exitstatus: status.exitstatus,
        connection_closed:,
        worker_pids:,
        port_reused: true,
        output: complete_output
      }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  def windows_parent_death_result
    script = <<~RUBY
      require "vajra"
      Vajra::Internal::RackExecution.install!(->(_env) { [200, { "Content-Type" => "text/plain" }, ["ok"]] })
      Vajra.start(workers: 2, threads: [1, 1], log_level: "debug")
    RUBY
    managed_popen2e(
      vajra_env(port: disposable_listener_port),
      *inline_ruby_command(script),
      chdir: VajraE2EHelpers::PACKAGE_ROOT
    ) do |_stdin, output, wait_thread|
      startup_output = []
      wait_for_banner(output, captured_lines: startup_output)
      runtime_output = startup_output.join
      wait_for_runtime_output(output, runtime_output, /Worker \d+ \(PID:/, count: 2, timeout: 10)
      worker_pids = runtime_output.scan(/Worker \d+ \(PID: (\d+)\)/).flatten.map { |pid| Integer(pid) }
      raise "worker PIDs missing from startup output: #{runtime_output}" unless worker_pids.length == 2

      signal_process_group(wait_thread, 'KILL')
      status = wait_for_exit(wait_thread, timeout: 10)
      Timeout.timeout(10) do
        sleep 0.05 while worker_pids.any? { |pid| windows_process_alive?(pid) }
      end
      { termsig: status.termsig, worker_pids: }
    ensure
      cleanup_process(wait_thread, output)
    end
  end

  if Gem.win_platform? && ENV['VAJRA_TEST_FAULT_INJECTION'] == '1'
    %w[
      malformed_frame oversized_frame unknown_frame stale_generation duplicate_dispatch
      partial_socket_transfer dispatch_timeout worker_crash
    ].each do |fault|
      it "recovers safely from #{fault.tr('_', ' ')}" do
        result = windows_ipc_fault_result(fault)

        expect(result[:exitstatus]).to eq(0), result[:output]
        expect(result[:fault_closed_connection]).to be(true)
        expect(result[:successful_before_fault]).to include(status_line: 'HTTP/1.1 200 OK', body: 'worker-ok') if fault == 'duplicate_dispatch'
        expect(result[:successful_after_fault]).to include(status_line: 'HTTP/1.1 200 OK', body: 'worker-ok')
      end
    end

    %w[bootstrap_failure readiness_timeout].each do |fault|
      it "fails startup safely on #{fault.tr('_', ' ')}" do
        result = startup_failure_with_inline_start(
          'RUBY_WORKERS' => '1',
          'RUBY_WORKER_TIMEOUT' => '1',
          'VAJRA_WINDOWS_TEST_FAULT' => fault
        )

        expect(result[:exitstatus]).to be_positive
        expect(result[:output]).to include('Unable to start Vajra:', 'Windows worker failed to become ready')
      end
    end

    it 'clears startup state when console handler installation fails' do
      script = <<~RUBY
        require "vajra"

        first_error = begin
          Vajra.start(workers: 1)
          "missing first startup error"
        rescue => error
          error.message
        end
        second_error = Thread.new do
          begin
            Vajra.start(workers: 1)
            "missing second startup error"
          rescue => error
            error.message
          end
        end.value
        puts first_error
        puts second_error
      RUBY
      result = startup_failure_with_inline_script(
        script,
        env: { 'VAJRA_WINDOWS_TEST_FAULT' => 'console_handler_failure' }
      )

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(result[:output]).to include(
        'injected Windows console control handler failure',
        'worker-only Vajra.start must be invoked from the Ruby main thread'
      )
      expect(result[:output]).not_to include('Vajra already running')
    end

    it 'forces a worker that exceeds its drain deadline to exit' do
      result = windows_drain_timeout_result

      expect(result[:exitstatus]).to eq(0), result[:output]
      expect(result[:elapsed]).to be_between(1, 10)
      expect(result[:response]).to include(status_line: 'HTTP/1.1 200 OK', body: 'ok')
      expect(result[:output]).to include('- Gracefully shutting down workers...', 'Goodbye!')
    end

    it 'bounds replacement attempts and reports terminal failure' do
      result = windows_replacement_exhaustion_result

      expect(result[:connection_closed]).to be(true)
      expect(result[:exitstatus]).to be_positive
      expect(result[:worker_pids]).to contain_exactly(be_positive)
      expect(result[:port_reused]).to be(true)
      expect(result[:output]).to include(
        'event=worker_replacement_terminal_failure',
        'Windows worker replacement attempts exhausted'
      )
    end

    it 'terminates every worker when its parent is killed' do
      result = windows_parent_death_result

      expect(result[:termsig]).to be_nil
      expect(result[:worker_pids]).to contain_exactly(be_positive, be_positive)
    end
  end
end

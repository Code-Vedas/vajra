# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'
require 'fiddle/import' if Gem.win_platform?
require 'open3'
require 'rbconfig'
require 'socket'
require 'timeout'

module VajraE2EHelpers
  TEST_PACKAGE_ROOT = File.expand_path('../..', __dir__)
  PACKAGE_ROOT = if ENV['VAJRA_INSTALLED_GEM'] == '1'
                   ENV.fetch('VAJRA_INSTALLED_GEM_ROOT')
                 else
                   TEST_PACKAGE_ROOT
                 end
  LISTENER_HOST = '127.0.0.1'
  LISTENER_BIND_HOST = '0.0.0.0'
  HTTP_RESPONSE_READ_TIMEOUT_SECONDS = 2
  RUNTIME_ENV_OVERRIDE_KEYS = %w[
    VAJRA_HOST
    VAJRA_PORT
    VAJRA_WORKERS
    VAJRA_THREADS
    VAJRA_SOCKET_QUEUE_CAPACITY
    VAJRA_MAX_REQUEST_HEAD_BYTES
    VAJRA_REQUEST_TIMEOUT
    VAJRA_REQUEST_HEAD_TIMEOUT
    VAJRA_FIRST_DATA_TIMEOUT
    VAJRA_PERSISTENT_TIMEOUT
    VAJRA_WORKER_TIMEOUT
    VAJRA_LOG_LEVEL
    VAJRA_ACCESS_LOG
    VAJRA_ERROR_LOG
    VAJRA_STRUCTURED_LOGS
    VAJRA_STATS_PATH
    VAJRA_METRICS_ENDPOINT
    VAJRA_TRACE_ENABLED
    VAJRA_TRACE_ENDPOINT
    VAJRA_TRACE_SERVICE_NAME
    WEB_CONCURRENCY
    MAX_THREADS
  ].freeze
  # Keep this synchronized with the default request_head_timeout in
  # ext/vajra/vajra.hpp and ext/vajra/request/request_processor.hpp.
  REQUEST_HEAD_READ_TIMEOUT_SECONDS = 5
  IDLE_KEEP_ALIVE_CLOSE_TIMEOUT_SECONDS = REQUEST_HEAD_READ_TIMEOUT_SECONDS + 1

  def vajra_command(*args)
    return packaged_vajra_command + args if ENV['VAJRA_INSTALLED_GEM'] == '1'
    return [RbConfig.ruby, '-rbundler/setup', '-Ilib', 'exe/vajra', *args] if Gem.win_platform?

    ['bundle', 'exec', RbConfig.ruby, '-Ilib', 'exe/vajra', *args]
  end

  def packaged_vajra_command(*args)
    if ENV['VAJRA_INSTALLED_GEM'] == '1'
      return [
        RbConfig.ruby,
        "-I#{File.join(PACKAGE_ROOT, 'lib')}",
        File.join(PACKAGE_ROOT, 'exe', 'vajra'),
        *args
      ]
    end

    if Gem.win_platform?
      return [
        RbConfig.ruby,
        '-rbundler/setup',
        "-I#{File.join(PACKAGE_ROOT, 'lib')}",
        File.join(PACKAGE_ROOT, 'exe', 'vajra'),
        *args
      ]
    end

    ['bundle', 'exec', RbConfig.ruby, "-I#{File.join(PACKAGE_ROOT, 'lib')}", File.join(PACKAGE_ROOT, 'exe', 'vajra'), *args]
  end

  def packaged_bundle_command(*args)
    return [RbConfig.ruby, '-rbundler/setup', *args.drop(1)] if Gem.win_platform? && args.first == RbConfig.ruby

    ['bundle', 'exec', *args]
  end

  def inline_ruby_command(script)
    return [RbConfig.ruby, "-I#{File.join(PACKAGE_ROOT, 'lib')}", '-e', script] if ENV['VAJRA_INSTALLED_GEM'] == '1'

    return [RbConfig.ruby, '-rbundler/setup', '-Ilib', '-e', script] if Gem.win_platform?

    ['bundle', 'exec', RbConfig.ruby, '-Ilib', '-e', script]
  end

  def app_root_bundle_env
    { 'BUNDLE_GEMFILE' => ENV.fetch('VAJRA_PACKAGE_TEST_GEMFILE', File.join(TEST_PACKAGE_ROOT, 'Gemfile')) }
  end

  def vajra_env(host: nil, port: nil, max_request_head_bytes: nil)
    RUNTIME_ENV_OVERRIDE_KEYS.to_h { |key| [key, nil] }.tap do |env|
      env['VAJRA_HOST'] = host unless host.nil?
      env['VAJRA_PORT'] = port.to_s unless port.nil?
      env['VAJRA_MAX_REQUEST_HEAD_BYTES'] = max_request_head_bytes.to_s unless max_request_head_bytes.nil?
    end
  end

  def listener_banner(port)
    "listening on port #{port}"
  end

  def managed_popen2e(*command, **options, &)
    process_options = if Gem.win_platform?
                        options.merge(new_pgroup: true)
                      else
                        options.merge(pgroup: true)
                      end
    Open3.popen2e(*command, **process_options) do |stdin, output, wait_thread|
      windows_process_outputs[wait_thread.pid] = output if Gem.win_platform?
      yield stdin, output, wait_thread
    ensure
      windows_process_outputs.delete(wait_thread.pid) if Gem.win_platform?
    end
  end

  def windows_process_alive?(pid)
    output, status = Open3.capture2e('tasklist', '/FI', "PID eq #{pid}", '/NH', '/FO', 'CSV')
    status.success? && output.match?(/\A"[^"]+","#{pid}"/)
  end

  def windows_process_outputs
    @windows_process_outputs ||= {}
  end

  def wait_for_banner(output, captured_lines: nil)
    Timeout.timeout(30) do
      pending_port = nil
      loop do
        line = startup_output_line(output, captured_lines)

        captured_lines << line if captured_lines

        match = lifecycle_banner_match(line)
        return Integer(match[1]) if match

        raw_match = line.match(%r{\[\d+\] \* (?:Bind:|Listening on) http://[^:]+:(\d+)})
        pending_port = Integer(raw_match[1]) if raw_match
        return pending_port if pending_port && line.include?('Worker ') && line.include?(' booted in ')
      end
    end
  rescue Timeout::Error => e
    captured = captured_lines&.join.to_s
    raise Timeout::Error, "timed out waiting for Vajra startup banner; output=#{captured.inspect}", e.backtrace
  end

  def lifecycle_banner_match(line)
    [
      /\[Vajra\]\[lifecycle\] .* listening on port (\d+)/,
      /\[Vajra\]\[lifecycle\] .* event=boot_complete .* port=(\d+)/,
      /\[Vajra\]\[lifecycle\] .* event=worker_bootstrap_ready .* port=(\d+)/
    ].filter_map { |pattern| line.match(pattern) }.first
  end

  def startup_output_line(output, captured_lines)
    line = output.gets
    return line unless line.nil?

    raise "vajra exited before startup banner: #{captured_lines&.join}"
  end
end

RSpec.configure do |config|
  config.disable_monkey_patching!
  config.expect_with(:rspec) { |c| c.syntax = :expect }
  config.include VajraE2EHelpers
end

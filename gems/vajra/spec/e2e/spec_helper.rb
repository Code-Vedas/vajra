# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'
require 'open3'
require 'rbconfig'
require 'socket'
require 'timeout'

module VajraE2EHelpers
  PACKAGE_ROOT = File.expand_path('../..', __dir__)
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
    ['bundle', 'exec', RbConfig.ruby, '-Ilib', 'exe/vajra', *args]
  end

  def packaged_vajra_command(*args)
    ['bundle', 'exec', RbConfig.ruby, "-I#{File.join(PACKAGE_ROOT, 'lib')}", File.join(PACKAGE_ROOT, 'exe', 'vajra'), *args]
  end

  def packaged_bundle_command(*args)
    ['bundle', 'exec', *args]
  end

  def inline_ruby_command(script)
    ['bundle', 'exec', RbConfig.ruby, '-Ilib', '-e', script]
  end

  def app_root_bundle_env
    { 'BUNDLE_GEMFILE' => File.join(PACKAGE_ROOT, 'Gemfile') }
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
    Open3.popen2e(*command, **options.merge(pgroup: true), &)
  end

  def wait_for_banner(output, captured_lines: nil)
    Timeout.timeout(15) do
      pending_port = nil
      loop do
        line = output.gets
        raise 'vajra exited before startup banner' if line.nil?

        captured_lines << line if captured_lines

        match = line.match(/\[Vajra\]\[lifecycle\] .* listening on port (\d+)/) ||
                line.match(/\[Vajra\]\[lifecycle\] .* event=boot_complete .* port=(\d+)/) ||
                line.match(/\[Vajra\]\[lifecycle\] .* event=worker_bootstrap_ready .* port=(\d+)/)
        return Integer(match[1]) if match

        raw_match = line.match(%r{\[\d+\] \* (?:Bind:|Listening on) http://[^:]+:(\d+)})
        pending_port = Integer(raw_match[1]) if raw_match
        return pending_port if pending_port && line.include?('Worker ') && line.include?(' booted in ')
      end
    end
  end
end

RSpec.configure do |config|
  config.disable_monkey_patching!
  config.expect_with(:rspec) { |c| c.syntax = :expect }
  config.include VajraE2EHelpers
end

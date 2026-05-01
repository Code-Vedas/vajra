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

  def vajra_command(*args)
    ['bundle', 'exec', RbConfig.ruby, '-Ilib', 'exe/vajra', *args]
  end

  def inline_ruby_command(script)
    ['bundle', 'exec', RbConfig.ruby, '-Ilib', '-e', script]
  end

  def vajra_env(port: nil, max_request_head_bytes: nil)
    {}.tap do |env|
      env['VAJRA_PORT'] = port.to_s unless port.nil?
      env['VAJRA_MAX_REQUEST_HEAD_BYTES'] = max_request_head_bytes.to_s unless max_request_head_bytes.nil?
    end
  end

  def listener_banner(port)
    "Vajra listening on port #{port}"
  end

  def wait_for_banner(output)
    Timeout.timeout(15) do
      loop do
        line = output.gets
        raise 'vajra exited before startup banner' if line.nil?

        match = line.match(/Vajra listening on port (\d+)/)
        return Integer(match[1]) if match
      end
    end
  end
end

RSpec.configure do |config|
  config.disable_monkey_patching!
  config.expect_with(:rspec) { |c| c.syntax = :expect }
  config.include VajraE2EHelpers
end

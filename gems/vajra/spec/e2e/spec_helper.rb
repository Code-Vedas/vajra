# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative "../spec_helper"
require "open3"
require "rbconfig"
require "socket"
require "timeout"

module VajraE2EHelpers
  PACKAGE_ROOT = File.expand_path("../..", __dir__)

  def vajra_command(*args)
    ["bundle", "exec", RbConfig.ruby, "-Ilib", "exe/vajra", *args]
  end

  def wait_for_banner(output)
    Timeout.timeout(15) do
      loop do
        line = output.gets
        raise "vajra exited before startup banner" if line.nil?

        return if line.include?("Vajra listening on port 3000")
      end
    end
  end
end

RSpec.configure do |config|
  config.disable_monkey_patching!
  config.expect_with(:rspec) { |c| c.syntax = :expect }
  config.include VajraE2EHelpers
end

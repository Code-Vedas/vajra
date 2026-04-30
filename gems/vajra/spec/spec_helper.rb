# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
unless ENV['NO_COVERAGE'] == '1'
  require 'simplecov'

  SimpleCov.start do
    enable_coverage :branch
    track_files 'lib/**/*.rb'
    add_filter '/spec/'
    minimum_coverage line: 100, branch: 100
  end
end

require_relative '../lib/vajra/version'
require 'vajra'

RSpec.configure do |config|
  # Enable flags like --only-failures and --next-failure
  config.example_status_persistence_file_path = '.rspec_status'

  # Disable RSpec exposing methods globally on `Module` and `main`
  config.disable_monkey_patching!

  config.expect_with :rspec do |c|
    c.syntax = :expect
  end
end

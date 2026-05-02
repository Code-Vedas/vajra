# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../spec_helper'
require_relative 'support/http_helpers'
require_relative 'support/process_helpers'
require_relative 'support/startup_helpers'

module VajraE2ESupport
  include VajraE2EHttpHelpers
  include VajraE2EProcessHelpers
  include VajraE2EStartupHelpers
end

RSpec.configure do |config|
  config.include VajraE2ESupport, :e2e
end

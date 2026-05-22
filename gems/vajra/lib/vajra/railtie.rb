# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'rails/railtie'
require 'rackup/handler/vajra'

module Vajra
  # Rails integration for selecting Vajra as the default `rails server` backend.
  class Railtie < ::Rails::Railtie
    config.before_configuration do
      ENV['RACKUP_HANDLER'] ||= 'vajra'
    end
  end
end

# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative '../vajra'
require_relative 'internal/rack_execution'

module Vajra
  # Rails-specific adapter for installing a Rails application on Vajra's Rack seam.
  module Rails
    module_function

    # Raised when the configured Rails application cannot be installed on Vajra.
    class Error < Vajra::Error; end

    def install!(application = current_application)
      application or raise Error, 'Rails application is not configured'
      validate_application!(application)
      application.initialize! unless application.initialized?
      Vajra::Internal::RackExecution.install!(application)
    rescue Error
      raise
    rescue ::StandardError => e
      raise Error, "Rails application boot failed: #{e.class}: #{e.message}"
    end

    def installed?
      Vajra::Internal::RackExecution.installed?
    end

    def uninstall!
      Vajra::Internal::RackExecution.uninstall!
    end

    def current_application
      return nil unless defined?(::Rails)

      ::Rails.method(:application)
      ::Rails.application
    rescue NameError
      nil
    end
    private_class_method :current_application

    def validate_application!(application)
      application.method(:call)
      application.method(:initialized?)
      application.method(:initialize!)
    rescue NameError => e
      raise Error, "Rails application must respond to ##{e.name}"
    end
    private_class_method :validate_application!
  end
end

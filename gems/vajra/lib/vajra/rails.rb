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
      initialize_application(application) unless application.initialized?
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

    def initialize_application(application)
      executor = rails_executor(application)
      initializer = proc { application.initialize! }
      if (wrap = method_object(executor, :wrap))
        wrap.call(&initializer)
      else
        initializer.call
      end
    end
    private_class_method :initialize_application

    def rails_executor(application)
      if (executor = method_object(application, :executor))
        return executor.call
      end

      current = current_application
      return unless current.equal?(application)

      method_object(current, :executor)&.call
    end
    private_class_method :rails_executor

    def method_object(receiver, name)
      return unless receiver

      receiver.method(name)
    rescue NameError
      nil
    end
    private_class_method :method_object
  end
end

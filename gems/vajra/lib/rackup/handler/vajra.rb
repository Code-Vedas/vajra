# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'rackup/handler'
require_relative '../../vajra'
require_relative '../../vajra/cli'

module Rackup
  # Rackup handler registry with the Vajra adapter registered for framework boot.
  module Handler
    # Rackup handler that runs the Vajra native runtime for Rack applications.
    module Vajra
      module_function

      CONFIG_PATH = File.join('config', 'vajra.rb')

      def run(app, **options)
        ::Vajra::Internal::RackExecution.install!(app)
        ::Vajra.start(**vajra_start_options(options))
      end

      def shutdown
        ::Vajra.stop
      end

      def valid_options
        {
          'Host=HOST' => 'Bind address to listen on (defaults to Rails or Vajra configuration)',
          'Port=PORT' => 'Port to listen on (defaults to Rails or Vajra configuration)',
          'max_request_head_bytes=BYTES' => 'Maximum allowed HTTP request head size'
        }
      end

      def vajra_start_options(options)
        launcher = ::Vajra::CLI::Launcher.new(root: Dir.pwd)
        launcher.load(config_path: nil, default_config_path: CONFIG_PATH)
        start_options = launcher.start_options.dup
        user_supplied_options = options.fetch(:user_supplied_options, [])

        if options.key?(:Port) &&
           (user_supplied_options.include?(:Port) || !start_options.key?(:port))
          start_options[:port] = Integer(options.fetch(:Port))
        end

        if options.key?(:Host) &&
           (user_supplied_options.include?(:Host) || !start_options.key?(:host))
          start_options[:host] = String(options.fetch(:Host))
        end

        start_options[:max_request_head_bytes] = Integer(options.fetch(:max_request_head_bytes)) if options.key?(:max_request_head_bytes)

        start_options
      end
      private_class_method :vajra_start_options
    end

    register :vajra, Vajra
  end
end

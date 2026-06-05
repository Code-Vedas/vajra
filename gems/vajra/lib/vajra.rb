# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require_relative 'vajra/version'
require_relative 'vajra/internal/boot'
require_relative 'vajra/internal/rack_execution'
require_relative 'vajra/internal/tracing'

# Ruby entrypoint for booting the native Vajra HTTP listener.
module Vajra
  # Base error type for Ruby-side Vajra failures.
  class Error < StandardError; end

  autoload :CLI, 'vajra/cli'
  autoload :Rails, 'vajra/rails'

  # Loads the compiled native entrypoint through the canonical package path.
  module NativeExtension
    module_function

    def load!(loader: method(:require), extension_path: File.expand_path('vajra/vajra', __dir__))
      loader.call(extension_path)
    rescue LoadError => e
      raise LoadError, <<~MESSAGE, e.backtrace
        Unable to load the Vajra native extension.
        Run `bundle exec rake compile` from the `gems/vajra/` package directory and retry.
        Original error: #{e.message}
      MESSAGE
    end
  end

  NativeExtension.load!
  Vajra::Internal::Boot.install!
  # Rails integration is optional for non-Rails hosts and is loaded lazily when Rails is present.
  def self.install_optional_railtie
    return unless defined?(::Rails::Railtie)

    require_relative 'vajra/railtie'
  rescue LoadError
    nil
  end
  install_optional_railtie

  class << self
    DOCUMENTED_START_OPTION_KEYS = %i[
      host
      port
      bind
      unix_socket
      backlog
      reuse_port
      workers
      threads
      preload_app
      worker_boot_timeout
      worker_shutdown_timeout
      phased_restart
      max_request_head_bytes
      request_timeout
      first_data_timeout
      persistent_timeout
      worker_timeout
      max_request_body_bytes
      request_head_timeout
      request_body_timeout
      max_keepalive_requests
      linger_timeout
      max_connections
      socket_queue_capacity
      max_requests_per_worker
      tls
      tls_certificate
      tls_private_key
      tls_ca_certificate
      tls_verify_mode
      tls_min_version
      alpn_protocols
      http2
      http2_max_concurrent_streams
      http2_initial_window_size
      http2_max_frame_size
      http2_header_table_size
      log_level
      access_log
      error_log
      structured_logs
      stats_path
      metrics_endpoint
      trace_enabled
      trace_endpoint
      trace_service_name
      pidfile
      state_path
      control_socket
      drain_timeout
      shutdown_timeout
    ].freeze

    NATIVE_START_OPTION_KEYS = %i[
      host
      port
      workers
      threads
      max_connections
      socket_queue_capacity
      max_request_head_bytes
      request_timeout
      request_head_timeout
      first_data_timeout
      persistent_timeout
      worker_timeout
      log_level
      access_log
      error_log
      structured_logs
      stats_path
      metrics_endpoint
      trace_enabled
      trace_endpoint
      trace_service_name
    ].freeze
    UNIMPLEMENTED_START_OPTION_KEYS = (DOCUMENTED_START_OPTION_KEYS - NATIVE_START_OPTION_KEYS).freeze

    alias __native_start__ start
    alias __native_stop__ stop
    private :__native_start__, :__native_stop__

    def configure(&block)
      config_target = CLI.current_config_target
      raise Error, 'Vajra.configure is only available while loading Vajra configuration' unless config_target
      raise Error, 'Vajra.configure requires a block' unless block

      if block.arity.zero?
        config_target.instance_eval(&block)
      else
        yield(config_target)
      end
    end

    def start(**options)
      validate_start_options!(options)
      Vajra::Internal::Tracing.install_from_start_options!(options)
      __native_start__(**options.slice(*NATIVE_START_OPTION_KEYS))
    end

    def stop
      __native_stop__
    end

    def header
      art = <<~'TEXT'
        __      __  _         _   ____       _
        \ \    / / / \       | | |  _ \     / \
         \ \  / / / _ \   _  | | | |_) |   / _ \
          \ \/ / / ___ \ | |_| | |  _ <   / ___ \
           \__/ /_/   \_\ \___/  |_| \_\ /_/   \_\
      TEXT

      "#{art}\nv#{Vajra::VERSION}\n\n"
    end

    private

    def validate_start_options!(options)
      option_keys = options.keys
      invalid_option = option_keys.find do |key|
        !DOCUMENTED_START_OPTION_KEYS.include?(key) || UNIMPLEMENTED_START_OPTION_KEYS.include?(key)
      end
      return unless invalid_option

      raise Error, "start option not implemented yet: #{invalid_option}" if UNIMPLEMENTED_START_OPTION_KEYS.include?(invalid_option)

      raise Error, "unknown start option: #{invalid_option}"
    end
  end
end

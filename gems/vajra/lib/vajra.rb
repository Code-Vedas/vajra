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
  NILABLE_STRING_OPTION_TYPES = { access_log: NilClass }.freeze
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
      workers
      threads
      max_request_head_bytes
      request_timeout
      first_data_timeout
      persistent_timeout
      worker_timeout
      max_request_body_bytes
      request_head_timeout
      request_body_timeout
      max_keepalive_requests
      max_connections
      socket_queue_capacity
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
      access_log_format
      stats_path
      metrics_endpoint
      trace_enabled
      trace_endpoint
      trace_service_name
      trace_otel_owner
    ].freeze

    NATIVE_START_OPTION_KEYS = %i[
      host
      port
      workers
      threads
      max_connections
      socket_queue_capacity
      max_keepalive_requests
      max_request_head_bytes
      max_request_body_bytes
      request_timeout
      request_head_timeout
      first_data_timeout
      request_body_timeout
      persistent_timeout
      worker_timeout
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
      access_log_format
      stats_path
      metrics_endpoint
      trace_enabled
      trace_endpoint
      trace_service_name
      trace_otel_owner
    ].freeze
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
      Vajra::Internal::RackExecution.configure_threads!(effective_max_threads(options))
      Vajra::Internal::Tracing.install_from_start_options!(options)
      __native_start__(**options.slice(*NATIVE_START_OPTION_KEYS))
    end

    def stop
      Vajra::Internal::Tracing.shutdown!
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
        !DOCUMENTED_START_OPTION_KEYS.include?(key)
      end
      return unless invalid_option

      raise Error, "unknown start option: #{invalid_option}"
    ensure
      validate_start_option_values!(options) unless invalid_option
    end

    def validate_start_option_values!(options)
      validate_boolean_options!(options, %i[tls http2 structured_logs trace_enabled trace_otel_owner])
      validate_string_options!(
        options,
        %i[
          host tls_certificate tls_private_key tls_ca_certificate tls_verify_mode tls_min_version
          log_level access_log error_log access_log_format stats_path metrics_endpoint
          trace_endpoint trace_service_name
        ]
      )
      validate_alpn_protocols!(options)
      validate_threads!(options)
      validate_integer_options!(options)
      validate_protocol_options!(options)
    end

    def validate_boolean_options!(options, keys)
      keys.each do |key|
        next unless options.key?(key)

        value = options[key]
        case value
        when true, false
          next
        end

        raise_start_validation_error("#{key} option must be true or false")
      end
    end

    def validate_string_options!(options, keys)
      keys.each do |key|
        next unless options.key?(key)

        value = options[key]
        next if value.is_a?(String)
        next if NILABLE_STRING_OPTION_TYPES[key] == value.class

        raise_start_validation_error("#{key} option must be a String")
      end
    end

    def validate_alpn_protocols!(options)
      return unless options.key?(:alpn_protocols)

      protocols = options[:alpn_protocols]
      type_error = 'alpn_protocols option must be an Array of Strings'
      validate_alpn_protocol_collection!(protocols, type_error)
      raise_start_validation_error('alpn_protocols option must not be empty') if protocols.empty?

      protocols.each do |protocol|
        validate_alpn_protocol_value!(protocol, type_error)
        raise_start_validation_error('alpn_protocols option values must not be empty') if protocol.empty?
      end
    end

    def validate_alpn_protocol_collection!(protocols, type_error)
      raise_start_validation_error(type_error) unless protocols.is_a?(Array)
    end

    def validate_alpn_protocol_value!(protocol, type_error)
      raise_start_validation_error(type_error) unless protocol.is_a?(String)
    end

    def validate_threads!(options)
      return unless options.key?(:threads)

      threads = options[:threads]
      unless threads.is_a?(Array) && threads.length == 2 && threads.all?(Integer)
        raise_start_validation_error('threads option must be an Array of two Integers')
      end

      min_threads, max_threads = threads
      return if min_threads.between?(1, max_threads)

      raise_start_validation_error('invalid threads option: expected thread range with 1 <= min <= max')
    end

    def effective_max_threads(options)
      return 1 unless options.key?(:threads)

      options[:threads].fetch(1)
    end

    def validate_integer_options!(options)
      native_int_max = 2_147_483_647
      native_long_max = 9_223_372_036_854_775_807
      {
        port: [0, 65_535],
        workers: [1, 1_024],
        max_connections: [1, native_int_max],
        socket_queue_capacity: [1, native_long_max],
        max_keepalive_requests: [0, native_int_max],
        max_request_head_bytes: [1, native_int_max],
        max_request_body_bytes: [1, native_int_max],
        request_timeout: [1, native_int_max],
        request_head_timeout: [1, native_int_max],
        first_data_timeout: [1, native_int_max],
        request_body_timeout: [1, native_int_max],
        persistent_timeout: [1, native_int_max],
        worker_timeout: [1, native_int_max],
        http2_max_concurrent_streams: [1, 1_000_000],
        http2_initial_window_size: [0, 2_147_483_647],
        http2_max_frame_size: [16_384, 16_777_215],
        http2_header_table_size: [0, native_int_max]
      }.each do |key, (minimum, maximum)|
        next unless options.key?(key)

        value = options[key]
        next if value.is_a?(Integer) && value >= minimum && value <= maximum

        raise_start_validation_error(
          "invalid #{key} option: #{value}. Expected an integer between #{minimum} and #{maximum}"
        )
      end
    end

    def validate_protocol_options!(options)
      validate_tls_option_values!(options)
      validate_protocol_combination!(options)
    end

    def validate_tls_option_values!(options)
      if options.key?(:tls_verify_mode) && !%w[none peer].include?(options[:tls_verify_mode])
        raise_start_validation_error('tls_verify_mode option must be none or peer')
      end

      return unless options.key?(:tls_min_version) && !%w[TLSv1_2 TLSv1_3].include?(options[:tls_min_version])

      raise_start_validation_error('tls_min_version option must be TLSv1_2 or TLSv1_3')
    end

    def validate_protocol_combination!(options)
      tls_enabled = options.fetch(:tls, false)
      http2_enabled = options.fetch(:http2, false)
      alpn_protocols = options.fetch(:alpn_protocols, tls_enabled && http2_enabled ? %w[h2 http/1.1] : %w[http/1.1])

      validate_tls_credentials!(options) if tls_enabled
      return unless alpn_protocols.include?('h2') && !http2_enabled

      raise_start_validation_error('alpn_protocols cannot include h2 unless http2 is enabled')
    end

    def validate_tls_credentials!(options)
      return unless options.fetch(:tls_certificate, '').empty? || options.fetch(:tls_private_key, '').empty?

      raise_start_validation_error('tls requires tls_certificate and tls_private_key')
    end

    def raise_start_validation_error(message)
      raise Error, "Unable to start Vajra: #{message}"
    end
  end
end

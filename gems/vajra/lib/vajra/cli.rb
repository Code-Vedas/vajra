# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'optparse'
require 'rack'
require_relative '../vajra'
require_relative 'internal/rack_execution'
require_relative 'rails'

module Vajra
  # CLI and config-file launcher for the canonical `vajra` executable.
  module CLI
    module_function

    # Raised when Vajra cannot parse or load CLI configuration.
    class Error < Vajra::Error; end

    DEFAULT_CONFIG_PATH = File.join('config', 'vajra.rb')
    DEFAULT_RACKUP_PATH = 'config.ru'
    SETTING_TYPE_GROUPS = {
      array: %i[threads alpn_protocols].freeze,
      integer: %i[
        port
        backlog
        workers
        worker_boot_timeout
        worker_shutdown_timeout
        worker_timeout
        max_request_head_bytes
        request_timeout
        first_data_timeout
        persistent_timeout
        max_request_body_bytes
        request_head_timeout
        request_body_timeout
        max_keepalive_requests
        linger_timeout
        max_connections
        socket_queue_capacity
        max_requests_per_worker
        http2_max_concurrent_streams
        http2_initial_window_size
        http2_max_frame_size
        http2_header_table_size
        drain_timeout
        shutdown_timeout
      ].freeze,
      string: %i[
        host
        bind
        unix_socket
        tls_certificate
        tls_private_key
        tls_ca_certificate
        tls_verify_mode
        tls_min_version
        log_level
        access_log
        access_log_format
        error_log
        stats_path
        metrics_endpoint
        trace_endpoint
        trace_service_name
        pidfile
        state_path
        control_socket
      ].freeze,
      boolean: %i[
        reuse_port
        preload_app
        phased_restart
        tls
        http2
        structured_logs
        trace_enabled
        trace_otel_owner
      ].freeze
    }.freeze
    DOCUMENTED_SERVER_SETTINGS = SETTING_TYPE_GROUPS.values.flatten.freeze
    NULLABLE_STRING_SETTINGS = %i[access_log].freeze
    ARRAY_SETTING_NORMALIZERS = {
      threads: lambda do |values|
        raise Error, 'threads expects one or two integer values' unless [1, 2].include?(values.length)

        normalized_values = values.map { |value| Integer(value) }
        normalized_values.length == 1 ? [normalized_values.first, normalized_values.first] : normalized_values
      end,
      alpn_protocols: lambda do |values|
        raise Error, 'alpn_protocols expects at least one value' if values.empty?

        values.map { |value| String(value) }
      end
    }.freeze

    def with_config_target(target)
      current_thread = nil
      previous_target = nil
      current_thread = config_target_thread
      previous_target = current_thread[:vajra_config_target] if current_thread
      current_thread[:vajra_config_target] = target if current_thread
      yield
    ensure
      current_thread[:vajra_config_target] = previous_target if current_thread
    end

    def current_config_target
      Thread.current[:vajra_config_target]
    end

    def start(argv: ARGV, root: Dir.pwd, stdout: $stdout)
      launcher = Launcher.new(root:)
      launcher.load(config_path: parse_args(argv.dup), default_config_path: DEFAULT_CONFIG_PATH)

      stdout.puts Vajra.header
      stdout.flush
      launcher.start
    end

    def parse_args(argv)
      config_path = nil

      parser = OptionParser.new
      parser.on('-C', '--config PATH', 'Load Vajra settings from PATH') { |path| config_path = path }
      parser.parse!(argv)

      raise Error, "unknown arguments: #{argv.join(' ')}" unless argv.empty?

      config_path
    rescue OptionParser::ParseError => e
      raise Error, e.message
    end
    private_class_method :parse_args

    def config_target_thread
      Thread.current
    end
    private_class_method :config_target_thread

    # Evaluates `config/vajra.rb` and the fallback rackup path for the CLI runtime.
    class Launcher
      attr_reader :start_options

      def initialize(root:)
        @root = root
        @start_options = {}
        @application_installer = nil
      end

      def load(config_path:, default_config_path:)
        Array(resolve_config_path(config_path, default_config_path)).each do |resolved_config_path|
          load_config_file(resolved_config_path)
        end
        use_default_rackup unless @application_installer || !File.file?(resolve_path(DEFAULT_RACKUP_PATH))
        self
      end

      def start
        @application_installer&.call
        Vajra.start(**@start_options)
      end

      def rackup(path = DEFAULT_RACKUP_PATH)
        @application_installer = lambda {
          rackup_path = resolve_path(path)
          rack_app, = Rack::Builder.parse_file(rackup_path)
          Vajra::Internal::RackExecution.install!(rack_app)
        }
      end

      def app(*args, &block)
        raise Error, 'app expects at most one Rack app argument' if args.length > 1
        raise Error, 'app requires either a Rack app argument or a block' if args.empty? && !block

        rack_app = args.first

        application_loader = block || -> { rack_app }
        @application_installer = lambda {
          Vajra::Internal::RackExecution.install!(application_loader.call)
        }
      end

      def rails(environment_path = File.join('config', 'environment'))
        @application_installer = lambda {
          require resolve_path(environment_path)
          Vajra::Rails.install!
        }
      end

      DOCUMENTED_SERVER_SETTINGS.each do |directive|
        define_method(directive) do |*values|
          @start_options[directive] = normalize_setting_values(directive, values)
        end
      end

      def method_missing(name, ...)
        raise Error, "unsupported configuration directive: #{name}"
      end

      def respond_to_missing?(name, *args)
        include_private = args.fetch(0, false)
        DOCUMENTED_SERVER_SETTINGS.include?(name) || super(name, include_private)
      end

      private

      def resolve_config_path(config_path, default_config_path)
        return resolve_path(config_path) if config_path

        default_path = resolve_path(default_config_path)
        return default_path if File.file?(default_path)

        nil
      end

      def load_config_file(path)
        ::Vajra::CLI.with_config_target(self) do
          instance_eval(File.read(path), path, 1)
        end
      rescue Errno::ENOENT => e
        raise Error, "config file not found: #{path} (#{e})"
      rescue ScriptError, StandardError => e
        raise Error, "unable to load #{path}: #{e.class}: #{e}"
      end

      def resolve_path(path)
        File.expand_path(path, @root)
      end

      def use_default_rackup
        rackup(DEFAULT_RACKUP_PATH)
      end

      def normalize_setting_values(directive, values)
        boolean_setting = setting_type_include?(:boolean, directive)
        return true if boolean_setting && values.empty?
        return normalize_array_setting(directive, values) if setting_type_include?(:array, directive)

        raise Error, "#{directive} expects a single value" unless values.length == 1

        first_value = values.first
        return nil if NULLABLE_STRING_SETTINGS.include?(directive) && first_value.nil?
        return Integer(first_value) if setting_type_include?(:integer, directive)
        return String(first_value) if setting_type_include?(:string, directive)

        first_value
      end

      def normalize_array_setting(directive, values)
        first_value = values.first
        normalized_values = values.length == 1 && first_value.is_a?(Array) ? first_value : values
        normalizer = ARRAY_SETTING_NORMALIZERS.fetch(directive, nil)
        return normalized_values unless normalizer

        normalizer.call(normalized_values)
      end

      def setting_type_include?(type, directive)
        SETTING_TYPE_GROUPS.fetch(type).include?(directive)
      end
    end
  end
end

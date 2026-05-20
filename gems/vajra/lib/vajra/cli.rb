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
        keepalive_timeout
        max_keepalive_requests
        linger_timeout
        max_connections
        queue_capacity
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
        scheduler_policy
        tls_certificate
        tls_private_key
        tls_ca_certificate
        tls_verify_mode
        tls_min_version
        log_level
        access_log
        error_log
        stats_path
        metrics_endpoint
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
      ].freeze
    }.freeze
    DOCUMENTED_SERVER_SETTINGS = (
      %i[
        bind
        unix_socket
      ] + SETTING_TYPE_GROUPS.values.flatten
    ).freeze

    def with_config_target(target)
      current_thread = Thread.current
      previous_target = nil
      previous_target = current_thread[:vajra_config_target]
      current_thread[:vajra_config_target] = target
      yield
    ensure
      current_thread[:vajra_config_target] = previous_target
    end

    def current_config_target
      Thread.current[:vajra_config_target]
    end

    def start(argv: ARGV, root: Dir.pwd, stdout: $stdout)
      launcher = Launcher.new(root:)
      launcher.load(config_path: parse_args(argv.dup), default_config_path: DEFAULT_CONFIG_PATH)

      stdout.puts Vajra.header
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

      # :reek:ControlParameter
      def app(rack_app = nil, &block)
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
        return normalize_array_setting(values) if setting_type_include?(:array, directive)

        raise Error, "#{directive} expects a single value" unless values.length == 1

        first_value = values.first
        return Integer(first_value) if setting_type_include?(:integer, directive)
        return String(first_value) if setting_type_include?(:string, directive)
        return first_value if boolean_setting

        raise Error, "unsupported configuration directive: #{directive}"
      end

      def normalize_array_setting(values)
        first_value = values.first
        return first_value if values.length == 1 && first_value.is_a?(Array)

        values
      end

      def setting_type_include?(type, directive)
        SETTING_TYPE_GROUPS.fetch(type).include?(directive)
      end
    end
  end
end

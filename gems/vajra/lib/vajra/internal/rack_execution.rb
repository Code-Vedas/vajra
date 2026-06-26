# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'English'

module Vajra
  module Internal
    # Executes an installed Rack-compatible app against translated native env entries.
    module RackExecution
      module_function

      RACK_VERSION = [1, 6].freeze
      UNSET_BODY = Object.new.freeze

      # Stores the currently installed Rack-compatible app.
      AppState = Struct.new(:app, :max_threads)
      APP_MUTEX = Mutex.new
      APP_STATE = AppState.new(nil, 1)

      def install!(app)
        app.method(:call)

        APP_MUTEX.synchronize { APP_STATE.app = app }
        # The native executor uses the installed app directly; this callback
        # remains for bootstrap/direct Ruby execution paths.
        __native_set_app__(app)
        __native_configure_threads__(APP_STATE.max_threads)
        __native_set_callback__(bootstrap_callback)
        app
      rescue NameError
        raise TypeError, 'Rack app must respond to #call'
      end

      def uninstall!
        APP_MUTEX.synchronize do
          APP_STATE.app = nil
          APP_STATE.max_threads = 1
        end
        __native_set_app__(nil)
        __native_set_callback__(nil)
      end

      def configure_threads!(max_threads)
        APP_MUTEX.synchronize { APP_STATE.max_threads = max_threads }
        __native_configure_threads__(max_threads)
      end

      def installed?
        !current_app.equal?(nil)
      end

      def call(env_entries, request_body)
        body = UNSET_BODY
        body_close_managed = false
        request_input = nil
        app = current_app
        return nil if app.equal?(nil)

        env = build_env(env_entries, request_body)
        request_input = env.fetch('rack.input')
        status, headers, body = Vajra::Internal::Tracing.with_request_span(env) { app.call(env) }
        normalize_response(status, headers, body) do
          body_close_managed = true
        end
      ensure
        close_body(body) if !body_close_managed && !body.equal?(UNSET_BODY)
        close_body(request_input) if request_input
      end

      def call_native(app, env)
        Vajra::Internal::Tracing.with_request_span(env) { app.call(env) }
      end

      def build_env(env_entries, request_body)
        env = {}
        env_entries.each do |key, value|
          env[key] = value
        end

        env['SCRIPT_NAME'] ||= ''
        env['QUERY_STRING'] ||= ''
        env['rack.version'] = RACK_VERSION
        env['rack.url_scheme'] ||= 'http'
        env['rack.input'] = rack_input_for(request_body)
        env['rack.errors'] = $stderr
        env['rack.multithread'] = current_max_threads > 1
        env['rack.multiprocess'] = false
        env['rack.run_once'] = false
        env
      end
      private_class_method :build_env

      def rack_input_for(request_body)
        return Vajra::NativeInput.from_string(binary_request_body(request_body)) if request_body.is_a?(String)
        raise TypeError, 'request_body must be a String' if request_body.nil?

        validate_rack_input!(request_body)
        request_body
      end
      private_class_method :rack_input_for

      def validate_rack_input!(input)
        %i[read gets each rewind close].each do |method_name|
          input.method(method_name)
        end
      rescue NameError => e
        raise TypeError, "rack.input must respond to ##{e.name}"
      end
      private_class_method :validate_rack_input!

      def binary_request_body(request_body)
        raise TypeError, 'request_body must be a String' unless request_body.is_a?(String)

        body = request_body
        body = body.dup if body.frozen?
        body.force_encoding(Encoding::BINARY) unless body.encoding == Encoding::BINARY
        body
      end
      private_class_method :binary_request_body

      def normalize_headers(headers)
        normalized_headers = []
        headers.each do |name, value|
          append_normalized_header(normalized_headers, name, value)
        end
        normalized_headers
      end
      private_class_method :normalize_headers

      def append_normalized_header(normalized_headers, name, value)
        normalized_headers << [normalize_header_name(name), String(value)]
      end
      private_class_method :append_normalized_header

      def normalize_header_name(name)
        String(name).tr('_', '-')
      end
      private_class_method :normalize_header_name

      def current_app
        APP_MUTEX.synchronize { APP_STATE.app }
      end
      private_class_method :current_app

      def current_max_threads
        APP_MUTEX.synchronize { APP_STATE.max_threads }
      end
      private_class_method :current_max_threads

      def bootstrap_callback
        proc { |env_entries, request_body| Vajra::Internal::RackExecution.call(env_entries, request_body) }
      end
      private_class_method :bootstrap_callback

      def normalize_response(status, headers, body)
        [
          Integer(status),
          normalize_headers(headers),
          begin
            yield
            collect_body(body)
          end
        ]
      end
      private_class_method :normalize_response

      def collect_body(body)
        collected_body = []
        # Rack requires #each with a block; it does not require #map or an enumerator-returning #each.
        # rubocop:disable Style/MapIntoArray
        body.each do |chunk|
          collected_body << chunk.to_s.b
        end
        # rubocop:enable Style/MapIntoArray
        collected_body
      ensure
        close_body(body)
      end
      private_class_method :collect_body

      def close_body(body)
        body.close
      rescue NoMethodError => e
        raise unless e.name == :close && e.receiver.equal?(body)
      end
      private_class_method :close_body

      at_exit do
        Vajra::Internal::RackExecution.uninstall!
      end
    end
  end
end

# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

require 'stringio'

module Vajra
  module Internal
    # Executes an installed Rack-compatible app against translated native env entries.
    module RackExecution
      module_function

      RACK_VERSION = [1, 6].freeze
      UNSET_BODY = Object.new.freeze

      # Stores the currently installed Rack-compatible app.
      AppState = Struct.new(:app)
      APP_MUTEX = Mutex.new
      APP_STATE = AppState.new(nil)

      def install!(app)
        app.method(:call)

        APP_MUTEX.synchronize { APP_STATE.app = app }
        __native_set_callback__(proc { |env_entries, request_body| Vajra::Internal::RackExecution.call(env_entries, request_body) })
        app
      rescue NameError
        raise TypeError, 'Rack app must respond to #call'
      end

      def uninstall!
        APP_MUTEX.synchronize { APP_STATE.app = nil }
        __native_set_callback__(nil)
      end

      def installed?
        !current_app.equal?(nil)
      end

      def call(env_entries, request_body)
        body = UNSET_BODY
        body_close_managed = false
        app = current_app
        return nil if app.equal?(nil)

        env = build_env(env_entries, request_body)
        status, headers, body = app.call(env)
        normalized_response = [
          Integer(status),
          normalize_headers(headers),
          begin
            body_close_managed = true
            collect_body(body)
          end
        ]
        normalized_response
      ensure
        close_body(body) if !body_close_managed && !body.equal?(UNSET_BODY)
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
        env['rack.input'] = StringIO.new(binary_request_body(request_body))
        env['rack.errors'] = $stderr
        env['rack.multithread'] = false
        env['rack.multiprocess'] = false
        env['rack.run_once'] = false
        env
      end
      private_class_method :build_env

      def binary_request_body(request_body)
        raise TypeError, 'request_body must be a String' unless request_body.is_a?(String)

        body = request_body
        body_encoding = body.encoding
        body = body.dup if body.frozen? || body_encoding != Encoding::BINARY
        body.force_encoding(Encoding::BINARY) unless body_encoding == Encoding::BINARY
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

      def collect_body(body)
        collected_body = String.new(encoding: Encoding::BINARY)
        body.each do |chunk|
          collected_body << String(chunk).b
        end
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
    end
  end
end

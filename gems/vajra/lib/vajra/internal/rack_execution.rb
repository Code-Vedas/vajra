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

      # Stores the currently installed Rack-compatible app.
      AppState = Struct.new(:app)
      APP_MUTEX = Mutex.new
      APP_STATE = AppState.new(nil)

      def install!(app)
        APP_MUTEX.synchronize { APP_STATE.app = app }
      end

      def uninstall!
        APP_MUTEX.synchronize { APP_STATE.app = nil }
      end

      def installed?
        !current_app.equal?(nil)
      end

      def call(env_entries)
        app = current_app
        return nil if app.equal?(nil)

        env = build_env(env_entries)
        status, headers, body = app.call(env)
        [
          Integer(status),
          normalize_headers(headers),
          collect_body(body)
        ]
      end

      def build_env(env_entries)
        env = {}
        env_entries.each do |key, value|
          env[key] = value
        end

        env['SCRIPT_NAME'] ||= ''
        env['QUERY_STRING'] ||= ''
        env['rack.url_scheme'] ||= 'http'
        env['rack.input'] = StringIO.new('')
        env['rack.errors'] = $stderr
        env['rack.multithread'] = false
        env['rack.multiprocess'] = false
        env['rack.run_once'] = false
        env
      end
      private_class_method :build_env

      def normalize_headers(headers)
        normalized_headers = []
        headers.each do |name, value|
          append_normalized_header(normalized_headers, name, value)
        end
        normalized_headers
      end
      private_class_method :normalize_headers

      def append_normalized_header(normalized_headers, name, value)
        normalized_headers << [String(name), String(value)]
      end
      private_class_method :append_normalized_header

      def current_app
        APP_MUTEX.synchronize { APP_STATE.app }
      end
      private_class_method :current_app

      def collect_body(body)
        collected_body = +''
        body.each do |chunk|
          collected_body << String(chunk)
        end
        collected_body
      ensure
        begin
          body.close
        rescue NoMethodError
          nil
        end
      end
      private_class_method :collect_body
    end
  end
end

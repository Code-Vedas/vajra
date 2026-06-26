# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module DocumentedServerOptions
  module_function

  def config_file_contents
    native_config_file_contents
  end

  def native_config_file_contents
    <<~RUBY
      Vajra.configure do |config|
        config.host "127.0.0.1"
        config.port 4321
        config.workers 4
        config.threads 5, 5
        config.worker_timeout 60
        config.request_timeout 25
        config.first_data_timeout 30
        config.persistent_timeout 30
        config.request_head_timeout 15
        config.request_body_timeout 30
        config.max_keepalive_requests 1000
        config.max_connections 256
        config.socket_queue_capacity 5000
        config.log_level "info"
        config.access_log "log/vajra-access.log"
        config.access_log_format "json"
        config.error_log "log/vajra-error.log"
        config.structured_logs
        config.stats_path "/__vajra/stats"
        config.metrics_endpoint "/metrics"
        config.trace_enabled
        config.trace_endpoint "http://127.0.0.1:4318/v1/traces"
        config.trace_service_name "vajra-test"
        config.trace_otel_owner
        config.max_request_head_bytes 2048
        config.max_request_body_bytes 10_485_760
        config.app ->(_env) { [200, { "Content-Type" => "text/plain" }, ["OK"]] }
      end
    RUBY
  end

  def start_options
    native_start_options
  end

  def native_start_options
    {
      host: '127.0.0.1',
      port: 4321,
      workers: 4,
      threads: [5, 5],
      worker_timeout: 60,
      request_timeout: 25,
      first_data_timeout: 30,
      persistent_timeout: 30,
      request_head_timeout: 15,
      request_body_timeout: 30,
      max_keepalive_requests: 1000,
      max_connections: 256,
      socket_queue_capacity: 5000,
      log_level: 'info',
      access_log: 'log/vajra-access.log',
      access_log_format: 'json',
      error_log: 'log/vajra-error.log',
      structured_logs: true,
      stats_path: '/__vajra/stats',
      metrics_endpoint: '/metrics',
      trace_enabled: true,
      trace_endpoint: 'http://127.0.0.1:4318/v1/traces',
      trace_otel_owner: true,
      trace_service_name: 'vajra-test',
      max_request_head_bytes: 2048,
      max_request_body_bytes: 10_485_760
    }
  end
end

# frozen_string_literal: true

# Copyright Codevedas Inc. 2025-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

module DocumentedServerOptions
  module_function

  def config_file_contents
    <<~RUBY
      Vajra.configure do |config|
        config.host "127.0.0.1"
        config.port 4321
        config.bind "tcp://127.0.0.1:4321"
        config.unix_socket "tmp/vajra.sock"
        config.backlog 1024
        config.reuse_port
        config.workers 4
        config.threads 5, 5
        config.preload_app
        config.worker_boot_timeout 30
        config.worker_shutdown_timeout 45
        config.worker_timeout 60
        config.phased_restart
        config.tls
        config.request_timeout 25
        config.first_data_timeout 30
        config.persistent_timeout 30
        config.max_request_body_bytes 10_485_760
        config.request_head_timeout 15
        config.request_body_timeout 30
        config.max_keepalive_requests 1000
        config.linger_timeout 5
        config.max_connections 256
        config.queue_capacity 5000
        config.max_requests_per_worker 10_000
        config.scheduler_policy "least_loaded"
        config.tls_certificate "config/certs/server.crt"
        config.tls_private_key "config/certs/server.key"
        config.tls_ca_certificate "config/certs/ca.crt"
        config.tls_verify_mode "peer"
        config.tls_min_version "TLSv1_2"
        config.alpn_protocols %w[h2 http/1.1]
        config.http2
        config.http2_max_concurrent_streams 128
        config.http2_initial_window_size 65_535
        config.http2_max_frame_size 16_384
        config.http2_header_table_size 4096
        config.log_level "info"
        config.access_log "log/vajra-access.log"
        config.error_log "log/vajra-error.log"
        config.structured_logs
        config.stats_path "/__vajra/stats"
        config.metrics_endpoint "/metrics"
        config.pidfile "tmp/pids/vajra.pid"
        config.state_path "tmp/vajra.state"
        config.control_socket "tmp/vajra-control.sock"
        config.drain_timeout 30
        config.shutdown_timeout 60
        config.max_request_head_bytes 2048
        config.app ->(_env) { [200, { "Content-Type" => "text/plain" }, ["OK"]] }
      end
    RUBY
  end

  def start_options
    {
      access_log: 'log/vajra-access.log',
      alpn_protocols: %w[h2 http/1.1],
      backlog: 1024,
      bind: 'tcp://127.0.0.1:4321',
      control_socket: 'tmp/vajra-control.sock',
      drain_timeout: 30,
      error_log: 'log/vajra-error.log',
      host: '127.0.0.1',
      http2: true,
      http2_header_table_size: 4096,
      http2_initial_window_size: 65_535,
      http2_max_concurrent_streams: 128,
      http2_max_frame_size: 16_384,
      linger_timeout: 5,
      log_level: 'info',
      max_connections: 256,
      max_keepalive_requests: 1000,
      max_request_body_bytes: 10_485_760,
      max_request_head_bytes: 2048,
      max_requests_per_worker: 10_000,
      metrics_endpoint: '/metrics',
      phased_restart: true,
      pidfile: 'tmp/pids/vajra.pid',
      port: 4321,
      preload_app: true,
      request_timeout: 25,
      first_data_timeout: 30,
      persistent_timeout: 30,
      queue_capacity: 5000,
      request_body_timeout: 30,
      request_head_timeout: 15,
      reuse_port: true,
      scheduler_policy: 'least_loaded',
      shutdown_timeout: 60,
      state_path: 'tmp/vajra.state',
      stats_path: '/__vajra/stats',
      structured_logs: true,
      threads: [5, 5],
      tls: true,
      tls_ca_certificate: 'config/certs/ca.crt',
      tls_certificate: 'config/certs/server.crt',
      tls_min_version: 'TLSv1_2',
      tls_private_key: 'config/certs/server.key',
      tls_verify_mode: 'peer',
      unix_socket: 'tmp/vajra.sock',
      worker_boot_timeout: 30,
      worker_timeout: 60,
      worker_shutdown_timeout: 45,
      workers: 4
    }
  end
end

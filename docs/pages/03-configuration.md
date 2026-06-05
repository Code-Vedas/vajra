---
title: Configuration
nav_order: 3
permalink: /configuration/
---

# Configuration

Vajra reads server configuration from `config/vajra.rb`.

Use one explicit configuration block:

```ruby
Vajra.configure do |config|
  # Load Rails from the default config/environment path.
  config.rails

  # Load Rails from a custom environment file.
  config.rails "config/environment"

  # Load a Rack app from config.ru.
  config.rackup

  # Load a Rack app from a specific rackup file.
  config.rackup "config.ru"

  # Use an explicit Rack application object.
  config.app MyRackApp

  # Build the Rack application from a block.
  config.app { MyRackApp.new }

  # Bind the TCP listener to an address.
  config.host "0.0.0.0"

  # Bind the TCP listener to a port.
  config.port 3000

  # Use an explicit bind target.
  config.bind "tcp://127.0.0.1:3000"

  # Bind to a Unix-domain socket.
  config.unix_socket "tmp/vajra.sock"

  # Set the accepted-connection backlog.
  config.backlog 1024

  # Enable multi-process port reuse when supported by the platform.
  config.reuse_port false

  # Set the number of Ruby worker processes.
  config.workers 4

  # Set minimum and maximum request execution threads per worker.
  config.threads 5, 5

  # Preload the application before workers fork.
  config.preload_app true

  # Limit how long a worker may take to boot.
  config.worker_boot_timeout 30

  # Limit graceful worker shutdown time.
  config.worker_shutdown_timeout 30

  # Enable rolling worker replacement behavior when supported by the runtime.
  config.phased_restart false

  # Limit request-head bytes before parsing fails.
  config.max_request_head_bytes 16_384

  # Limit accepted request-body bytes.
  config.max_request_body_bytes 104_857_600

  # Limit total queue wait before execution starts.
  config.request_timeout 25

  # Limit how long the server waits for a complete request head.
  config.request_head_timeout 5

  # Limit how long the server waits for first request data.
  config.first_data_timeout 30

  # Limit idle keepalive time between requests.
  config.persistent_timeout 30

  # Limit stuck worker lifecycle time.
  config.worker_timeout 60

  # Limit sequential keepalive requests on one connection.
  config.max_keepalive_requests 1000

  # Control close-and-linger behavior after response completion.
  config.linger_timeout 5

  # Limit accepted connections and native handler capacity.
  config.max_connections 256

  # Limit the global pending request queue.
  config.socket_queue_capacity 100_000

  # Enable TLS for the listener.
  config.tls false

  # Set the TLS certificate chain path.
  config.tls_certificate "config/certs/server.crt"

  # Set the TLS private key path.
  config.tls_private_key "config/certs/server.key"

  # Set the TLS client CA bundle path.
  config.tls_ca_certificate "config/certs/ca.crt"

  # Set TLS peer verification behavior.
  config.tls_verify_mode "none"

  # Set the minimum TLS version.
  config.tls_min_version "TLSv1_2"

  # Set advertised ALPN protocols.
  config.alpn_protocols %w[http/1.1]

  # Enable HTTP/2 when supported by the runtime.
  config.http2 false

  # Set HTTP/2 concurrent stream capacity.
  config.http2_max_concurrent_streams 128

  # Set HTTP/2 stream flow-control window size.
  config.http2_initial_window_size 65_535

  # Set HTTP/2 maximum frame size.
  config.http2_max_frame_size 16_384

  # Set HTTP/2 HPACK dynamic table size.
  config.http2_header_table_size 4096

  # Set lifecycle and runtime log level.
  config.log_level "info"

  # Disable request access logging.
  config.access_log nil

  # Write one access-log line per request to a file.
  config.access_log "log/vajra-access.log"

  # Write error logs to a file.
  config.error_log "log/vajra-error.log"

  # Emit structured runtime logs.
  config.structured_logs false

  # Expose an in-process stats endpoint.
  config.stats_path "/__vajra/stats"

  # Expose a metrics endpoint.
  config.metrics_endpoint "/metrics"

  # Write the runtime PID to a file.
  config.pidfile "tmp/pids/vajra.pid"

  # Write runtime state to a file.
  config.state_path "tmp/vajra.state"

  # Configure a control-plane socket.
  config.control_socket "tmp/vajra-control.sock"

  # Limit graceful drain time.
  config.drain_timeout 30

  # Limit full shutdown time.
  config.shutdown_timeout 60
end
```

## Environment Overrides

These environment variables override the matching runtime settings:

| Environment Variable | Setting |
| --- | --- |
| `VAJRA_HOST` | `host` |
| `VAJRA_PORT` | `port` |
| `VAJRA_WORKERS`, `WEB_CONCURRENCY` | `workers` |
| `VAJRA_THREADS`, `MAX_THREADS` | `threads` |
| `VAJRA_MAX_REQUEST_HEAD_BYTES` | `max_request_head_bytes` |
| `VAJRA_REQUEST_TIMEOUT` | `request_timeout` |
| `VAJRA_REQUEST_HEAD_TIMEOUT` | `request_head_timeout` |
| `VAJRA_FIRST_DATA_TIMEOUT` | `first_data_timeout` |
| `VAJRA_PERSISTENT_TIMEOUT` | `persistent_timeout` |
| `VAJRA_WORKER_TIMEOUT` | `worker_timeout` |
| `VAJRA_SOCKET_QUEUE_CAPACITY` | `socket_queue_capacity` |
| `VAJRA_LOG_LEVEL` | `log_level` |
| `VAJRA_ACCESS_LOG` | `access_log` |

`access_log` is disabled by default. Prefer `config.access_log nil` when access
logs are intentionally disabled. Use a file path when Vajra should write one
access-log line per request.

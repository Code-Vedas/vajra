---
title: Configuration
nav_order: 7
permalink: /configuration/
---

# Configuration

Vajra uses one canonical server configuration file:

- `config/vajra.rb`

The normal file shape is one explicit block:

```ruby
Vajra.configure do |config|
  # Application selection
  config.rails
  config.rackup "config.ru"
  config.app MyRackApp
  config.app { MyRackApp.new }

  # Listener
  config.host "127.0.0.1"
  config.port 3000
  config.bind "tcp://127.0.0.1:3000"
  config.unix_socket "tmp/vajra.sock"
  config.backlog 1024
  config.reuse_port true

  # Runtime topology
  config.workers 4
  config.threads 5, 5
  config.preload_app true
  config.worker_boot_timeout 30
  config.worker_shutdown_timeout 30
  config.phased_restart true

  # Request and connection limits
  config.max_request_head_bytes 32_768
  config.max_request_body_bytes 104_857_600
  config.request_timeout 25
  config.request_head_timeout 5
  config.first_data_timeout 30
  config.request_body_timeout 30
  config.persistent_timeout 30
  config.worker_timeout 60
  config.max_keepalive_requests 1000
  config.linger_timeout 5

  # Scheduling and admission
  config.max_connections 256
  config.queue_capacity 100_000
  config.max_requests_per_worker 10_000
  config.scheduler_policy "least_loaded"

  # TLS
  config.tls true
  config.tls_certificate "config/certs/server.crt"
  config.tls_private_key "config/certs/server.key"
  config.tls_ca_certificate "config/certs/ca.crt"
  config.tls_verify_mode "peer"
  config.tls_min_version "TLSv1_2"
  config.alpn_protocols %w[h2 http/1.1]

  # HTTP/2
  config.http2 true
  config.http2_max_concurrent_streams 128
  config.http2_initial_window_size 65_535
  config.http2_max_frame_size 16_384
  config.http2_header_table_size 4096

  # Observability
  config.log_level "info"
  config.access_log "log/vajra-access.log"
  config.error_log "log/vajra-error.log"
  config.structured_logs true
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"

  # Operations
  config.pidfile "tmp/pids/vajra.pid"
  config.state_path "tmp/vajra.state"
  config.control_socket "tmp/vajra-control.sock"
  config.drain_timeout 30
  config.shutdown_timeout 60
end
```

The table below lists the full `Vajra.configure` DSL. The `Native environment
override` column identifies the entries that flow directly into the native
runtime. Directives without a native environment override still belong to the
configuration DSL and keep their declared server meaning in Vajra's product
surface. Framework launchers such as `bin/rails server -p 4000` remain valid
where the framework owns that surface.

Vajra uses one pending request queue. That queue is global and FIFO.
`queue_capacity` applies to that one global queue. The scheduler assigns the
oldest live queued request to the least-busy worker.

## Configuration Reference

| Setting | Default | Native environment override | Example | Purpose |
| --- | --- | --- | --- | --- |
| `rails` | `config/environment` | none | `config.rails` | load Rails and install `Rails.application` |
| `rails PATH` | `config/environment` | none | `config.rails "config/environment"` | load a non-default Rails environment file |
| `rackup` | `config.ru` | none | `config.rackup` | load `config.ru` |
| `rackup PATH` | `config.ru` | none | `config.rackup "config.ru"` | load a specific rackup file |
| `app OBJECT` | none | none | `config.app MyRackApp` | install an explicit Rack application object |
| `app { ... }` | none | none | `config.app { MyRackApp.new }` | install an app through a block loader |
| `host` | `"0.0.0.0"` | `VAJRA_HOST` | `config.host "127.0.0.1"` | bind address for TCP listeners |
| `port` | `3000` | `VAJRA_PORT` | `config.port 3000` | TCP port |
| `bind` | none | none | `config.bind "tcp://127.0.0.1:3000"` | explicit bind target |
| `unix_socket` | none | none | `config.unix_socket "tmp/vajra.sock"` | Unix-domain listener path |
| `backlog` | framework and runtime controlled | none | `config.backlog 1024` | accepted pending-connection backlog |
| `reuse_port` | `false` | none | `config.reuse_port true` | enable multi-process port reuse |
| `workers` | `1` | `VAJRA_WORKERS`, `WEB_CONCURRENCY` | `config.workers 4` | number of Ruby worker processes |
| `threads` | `1, 1` | `VAJRA_THREADS`, `MAX_THREADS` | `config.threads 5, 5` | min and max worker threads |
| `preload_app` | `true` | none | `config.preload_app true` | preload before worker fork |
| `worker_boot_timeout` | `30` | none | `config.worker_boot_timeout 30` | worker readiness deadline |
| `worker_shutdown_timeout` | `30` | none | `config.worker_shutdown_timeout 30` | graceful worker stop deadline |
| `phased_restart` | `false` | none | `config.phased_restart true` | rolling worker replacement behavior |
| `max_request_head_bytes` | `16_384` | `VAJRA_MAX_REQUEST_HEAD_BYTES` | `config.max_request_head_bytes 32_768` | maximum HTTP request head size |
| `max_request_body_bytes` | runtime controlled | none | `config.max_request_body_bytes 104_857_600` | maximum accepted request-body size |
| `request_timeout` | `25` | `VAJRA_REQUEST_TIMEOUT` | `config.request_timeout 25` | total queue wait budget before execution starts |
| `request_head_timeout` | `5` | `VAJRA_REQUEST_HEAD_TIMEOUT` | `config.request_head_timeout 5` | header read timeout |
| `first_data_timeout` | `30` | `VAJRA_FIRST_DATA_TIMEOUT` | `config.first_data_timeout 30` | time allowed before the first request data arrives |
| `request_body_timeout` | `30` | none | `config.request_body_timeout 30` | request-body read timeout |
| `persistent_timeout` | `30` | `VAJRA_PERSISTENT_TIMEOUT` | `config.persistent_timeout 30` | idle keepalive timeout between requests |
| `worker_timeout` | `60` | `VAJRA_WORKER_TIMEOUT` | `config.worker_timeout 60` | stuck worker lifecycle timeout |
| `max_keepalive_requests` | `1000` | none | `config.max_keepalive_requests 1000` | maximum sequential requests per connection |
| `linger_timeout` | `5` | none | `config.linger_timeout 5` | close and linger behavior after response completion |
| `max_connections` | `256` | none | `config.max_connections 10_000` | global accepted-connection ceiling |
| `queue_capacity` | signed `long` max | `VAJRA_QUEUE_CAPACITY` | `config.queue_capacity 100_000` | global pending request-queue limit |
| `max_requests_per_worker` | `10_000` | none | `config.max_requests_per_worker 10_000` | worker recycle threshold |
| `scheduler_policy` | `"least_loaded"` | `VAJRA_SCHEDULER_POLICY` | `config.scheduler_policy "least_loaded"` | worker selection policy |
| `tls` | `false` | none | `config.tls true` | enable TLS on the listener |
| `tls_certificate` | none | none | `config.tls_certificate "config/certs/server.crt"` | certificate chain path |
| `tls_private_key` | none | none | `config.tls_private_key "config/certs/server.key"` | private-key path |
| `tls_ca_certificate` | none | none | `config.tls_ca_certificate "config/certs/ca.crt"` | client CA bundle |
| `tls_verify_mode` | `"none"` | none | `config.tls_verify_mode "peer"` | peer verification behavior |
| `tls_min_version` | `"TLSv1_2"` | none | `config.tls_min_version "TLSv1_2"` | minimum TLS protocol version |
| `alpn_protocols` | `["http/1.1"]` | none | `config.alpn_protocols %w[h2 http/1.1]` | advertised ALPN protocols |
| `http2` | `false` | none | `config.http2 true` | enable HTTP/2 support |
| `http2_max_concurrent_streams` | `128` | none | `config.http2_max_concurrent_streams 128` | per-connection stream concurrency |
| `http2_initial_window_size` | `65_535` | none | `config.http2_initial_window_size 65_535` | stream flow-control window |
| `http2_max_frame_size` | `16_384` | none | `config.http2_max_frame_size 16_384` | HTTP/2 frame size limit |
| `http2_header_table_size` | `4096` | none | `config.http2_header_table_size 4096` | HPACK dynamic table size |
| `log_level` | `"info"` | `VAJRA_LOG_LEVEL` | `config.log_level "info"` | lifecycle and runtime log level |
| `access_log` | none | none | `config.access_log "log/vajra-access.log"` | access-log target |
| `error_log` | none | none | `config.error_log "log/vajra-error.log"` | error-log target |
| `structured_logs` | `false` | none | `config.structured_logs true` | structured log output |
| `stats_path` | none | none | `config.stats_path "/__vajra/stats"` | in-process stats endpoint |
| `metrics_endpoint` | none | none | `config.metrics_endpoint "/metrics"` | metrics scrape path |
| `pidfile` | none | none | `config.pidfile "tmp/pids/vajra.pid"` | runtime PID file |
| `state_path` | none | none | `config.state_path "tmp/vajra.state"` | persistent runtime state path |
| `control_socket` | none | none | `config.control_socket "tmp/vajra-control.sock"` | control-plane attachment point |
| `drain_timeout` | `30` | none | `config.drain_timeout 30` | graceful drain deadline |
| `shutdown_timeout` | `60` | none | `config.shutdown_timeout 60` | full shutdown deadline |

## Related Reading

- [Frameworks](/frameworks/)
- [Running Vajra](/running-vajra/)
- [Runtime Model](/runtime-model/)
- [Operations](/operations/)

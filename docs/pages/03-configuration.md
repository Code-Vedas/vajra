---
title: Configuration
nav_order: 3
permalink: /configuration/
---

# Configuration

Vajra reads server configuration at startup. Change a setting in
`config/vajra.rb` or the process environment, then restart the server.

The configuration file has two responsibilities:

- choose the Rack application to run
- set Vajra-owned server behavior such as listener address, process count, request limits, TLS, logging, metrics, and tracing

## Minimal Configuration

Rack, Sinatra, Roda, and Hanami applications can start from a standard
`config.ru`:

```bash
bundle exec vajra
```

Vajra loads `config.ru` automatically when `config/vajra.rb` is absent.

Rails applications continue to use the normal Rails launcher:

```bash
bin/rails server
```

Add `config/vajra.rb` when the application needs explicit server settings.

```ruby
Vajra.configure do |config|
  config.host "0.0.0.0"
  config.port 3000
  config.workers 2
  config.threads 5, 5
end
```

Production deployments should set listener address, worker count, thread count,
request limits, and observability outputs explicitly:

```ruby
Vajra.configure do |config|
  config.rackup "config.ru"

  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads 5, 5

  config.max_request_head_bytes 16_384
  config.max_request_body_bytes 104_857_600
  config.request_timeout 25
  config.request_head_timeout 5
  config.first_data_timeout 30
  config.persistent_timeout 30

  config.access_log "log/vajra-access.log"
  config.access_log_format "json"
  config.error_log "log/vajra-error.log"
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
end
```

## Application Loading

Use one application-loading directive.

| Directive                            | Use                                          |
| ------------------------------------ | -------------------------------------------- |
| `config.rackup`                      | Load `config.ru`.                            |
| `config.rackup "path/to/config.ru"`  | Load a specific rackup file.                 |
| `config.rails`                       | Load Rails from `config/environment`.        |
| `config.rails "path/to/environment"` | Load Rails from a specific environment file. |
| `config.app MyRackApp`               | Use an explicit Rack application object.     |
| `config.app { MyRackApp.new }`       | Build the Rack application from a block.     |

If no directive is present and `config.ru` exists, Vajra uses that file.

## Configuration Recipes

### Local Rack App

```ruby
Vajra.configure do |config|
  config.rackup "config.ru"
  config.port 3000
  config.threads 1, 1
end
```

### Production Rack App

```ruby
Vajra.configure do |config|
  config.rackup "config.ru"
  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads 5, 5
  config.max_connections 256
  config.socket_queue_capacity 256
  config.max_request_head_bytes 16_384
  config.max_request_body_bytes 104_857_600
  config.request_timeout 25
  config.request_head_timeout 5
  config.request_body_timeout 30
  config.first_data_timeout 30
  config.persistent_timeout 30
end
```

### Rails

```ruby
Vajra.configure do |config|
  config.rails
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads Integer(ENV.fetch("RAILS_MAX_THREADS", "5")),
                 Integer(ENV.fetch("RAILS_MAX_THREADS", "5"))
end
```

Size each database pool to at least the per-worker maximum thread count.

### TLS

```ruby
Vajra.configure do |config|
  config.tls true
  config.tls_certificate "/etc/vajra/server.crt"
  config.tls_private_key "/etc/vajra/server.key"
  config.tls_min_version "TLSv1_2"
end
```

The certificate file should contain the served certificate chain. The private
key must be readable by the runtime user.

### HTTP/2 And h2c

```ruby
Vajra.configure do |config|
  config.http2 true
  config.tls true
  config.alpn_protocols %w[h2 http/1.1]
  config.http2_max_concurrent_streams 128
  config.http2_initial_window_size 1_048_576
end
```

With `http2 true`, TLS listeners negotiate HTTP/2 through ALPN and plain
listeners accept h2c prior knowledge and HTTP/1.1 upgrade.

### Observability

```ruby
Vajra.configure do |config|
  config.access_log "log/vajra-access.log"
  config.access_log_format "json"
  config.error_log "log/vajra-error.log"
  config.structured_logs true
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
  config.trace_enabled true
  config.trace_endpoint "http://127.0.0.1:4318/v1/traces"
  config.trace_service_name "my-rack-app"
end
```

Protect `stats_path` and `metrics_endpoint` at the network or reverse-proxy
layer.

### Strict Public Listener

```ruby
Vajra.configure do |config|
  config.max_request_head_bytes 16_384
  config.max_request_body_bytes 16_777_216
  config.request_head_timeout 5
  config.request_body_timeout 30
  config.first_data_timeout 15
  config.max_keepalive_requests 1000
end
```

Use stricter limits for internet-facing deployments and raise them only for
routes that need larger uploads or longer body delivery windows.

### Environment Overrides

Environment variables override Ruby config:

```bash
VAJRA_PORT=4000 VAJRA_WORKERS=4 bundle exec vajra
```

With that command, `port` and `workers` from `config/vajra.rb` are ignored for
the process.

### Invalid Config

Unknown start keywords and unsupported config-file directives fail before the
server begins serving:

```ruby
Vajra.start(bind: "tcp://0.0.0.0:3000")
```

```text
unknown start option: bind
```

```ruby
Vajra.configure do |config|
  config.bind "tcp://0.0.0.0:3000"
end
```

```text
unsupported configuration directive: bind
```

## Full Runtime Reference

These settings are the supported runtime configuration surface. The table tracks
the Ruby `Vajra.start` keyword arguments, the `config/vajra.rb` DSL, and the
native runtime loader.

### Listener And Concurrency

| Setting                 | Default     | Env Override                       | Valid Values      | Effect                                               |
| ----------------------- | ----------- | ---------------------------------- | ----------------- | ---------------------------------------------------- |
| `host`                  | `"0.0.0.0"` | `VAJRA_HOST`                       | String            | TCP bind host.                                       |
| `port`                  | `3000`      | `VAJRA_PORT`                       | `0..65_535`       | TCP bind port. Use `0` for an ephemeral port.        |
| `workers`               | `1`         | `VAJRA_WORKERS`, `WEB_CONCURRENCY` | `1..1024`         | Number of worker processes.                          |
| `threads`               | `1, 1`      | `VAJRA_THREADS`, `MAX_THREADS`     | `1 <= min <= max` | Ruby Rack execution threads per worker.              |
| `max_connections`       | `256`       | none                               | Integer `>= 1`    | Worker-side active connection capacity.              |
| `socket_queue_capacity` | `256`       | `VAJRA_SOCKET_QUEUE_CAPACITY`      | Integer `>= 1`    | Pending dispatch capacity before admission pressure. |

Vajra keeps listener ownership in the master process. Accepted connections are dispatched to workers through controlled file-descriptor handoff.

### Request Limits And Timeouts

| Setting                  | Default      | Env Override                   | Valid Values   | Effect                                                                            |
| ------------------------ | ------------ | ------------------------------ | -------------- | --------------------------------------------------------------------------------- |
| `max_request_head_bytes` | `16_384`     | `VAJRA_MAX_REQUEST_HEAD_BYTES` | Integer `>= 1` | Maximum request-head bytes before parser rejection.                               |
| `max_request_body_bytes` | `16_777_216` | `VAJRA_MAX_REQUEST_BODY_BYTES` | Integer `>= 1` | Maximum accepted request-body bytes.                                              |
| `request_timeout`        | `25`         | `VAJRA_REQUEST_TIMEOUT`        | Integer `>= 1` | Maximum queue wait before execution starts.                                       |
| `request_head_timeout`   | `5`          | `VAJRA_REQUEST_HEAD_TIMEOUT`   | Integer `>= 1` | Time allowed to receive a complete request head.                                  |
| `request_body_timeout`   | `30`         | `VAJRA_REQUEST_BODY_TIMEOUT`   | Integer `>= 1` | Time allowed to receive a complete request body after the request head.           |
| `first_data_timeout`     | `30`         | `VAJRA_FIRST_DATA_TIMEOUT`     | Integer `>= 1` | Time allowed for first bytes on a new connection.                                 |
| `persistent_timeout`     | `30`         | `VAJRA_PERSISTENT_TIMEOUT`     | Integer `>= 1` | Idle keep-alive timeout between requests.                                         |
| `worker_timeout`         | `60`         | `VAJRA_WORKER_TIMEOUT`         | Integer `>= 1` | Worker health timeout.                                                            |
| `max_keepalive_requests` | `0`          | `VAJRA_MAX_KEEPALIVE_REQUESTS` | Integer `>= 0` | Maximum sequential requests on one keep-alive connection. `0` disables the limit. |

Request bodies are exposed to Rack through `Vajra::NativeInput`. The native input layer buffers in bounded chunks, spills large bodies to temporary storage, and wakes Rack readers when bytes, EOF, close, or errors are available.

### TLS And HTTP/2

| Setting                        | Default                                                | Env Override                         | Valid Values                                      | Effect                                                                                        |
| ------------------------------ | ------------------------------------------------------ | ------------------------------------ | ------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| `tls`                          | `false`                                                | `VAJRA_TLS`                          | `true`, `false`                                   | Enable TLS on the listener.                                                                   |
| `tls_certificate`              | `""`                                                   | `VAJRA_TLS_CERTIFICATE`              | String path                                       | Certificate chain file. Required when `tls` is true.                                          |
| `tls_private_key`              | `""`                                                   | `VAJRA_TLS_PRIVATE_KEY`              | String path                                       | Private key file. Required when `tls` is true.                                                |
| `tls_ca_certificate`           | `""`                                                   | `VAJRA_TLS_CA_CERTIFICATE`           | String path                                       | CA bundle for peer verification.                                                              |
| `tls_verify_mode`              | `"none"`                                               | `VAJRA_TLS_VERIFY_MODE`              | `"none"`, `"peer"`                                | TLS peer verification mode.                                                                   |
| `tls_min_version`              | `"TLSv1_2"`                                            | `VAJRA_TLS_MIN_VERSION`              | `"TLSv1_2"`, `"TLSv1_3"`                          | Minimum TLS protocol version.                                                                 |
| `alpn_protocols`               | `["http/1.1"]` or `["h2", "http/1.1"]` with TLS HTTP/2 | `VAJRA_ALPN_PROTOCOLS`               | Array or comma-separated list of protocol strings | Advertised TLS ALPN protocols.                                                                |
| `http2`                        | `false`                                                | `VAJRA_HTTP2`                        | `true`, `false`                                   | Enable HTTP/2 over TLS ALPN and cleartext h2c on plain listeners.                             |
| `http2_max_concurrent_streams` | `128`                                                  | `VAJRA_HTTP2_MAX_CONCURRENT_STREAMS` | `1..1_000_000`                                    | Maximum concurrent HTTP/2 streams per connection.                                             |
| `http2_initial_window_size`    | `1_048_576`                                            | `VAJRA_HTTP2_INITIAL_WINDOW_SIZE`    | `0..2_147_483_647`                                | HTTP/2 stream flow-control window.                                                            |
| `http2_max_frame_size`         | `1_048_576`                                            | `VAJRA_HTTP2_MAX_FRAME_SIZE`         | `16_384..16_777_215`                              | Maximum HTTP/2 DATA frame size; HEADERS frames are capped separately for request-head safety. |
| `http2_header_table_size`      | `4096`                                                 | `VAJRA_HTTP2_HEADER_TABLE_SIZE`      | `0..2_147_483_647`                                | HPACK dynamic table size.                                                                     |

When `http2` is enabled, TLS listeners negotiate HTTP/2 through ALPN and plain
listeners accept h2c prior knowledge and HTTP/1.1 `Upgrade: h2c`. Ordinary
textual `HTTP/2.0` request lines remain invalid HTTP/1.x requests. Extended
CONNECT provides bidirectional HTTP/2 stream IO, including
WebSocket-over-HTTP/2. Priority scheduling applies to response DATA and tunnel
DATA; server push is not implemented.

### Logging, Metrics, And Tracing

| Setting              | Default   | Env Override               | Valid Values                                                  | Effect                                                          |
| -------------------- | --------- | -------------------------- | ------------------------------------------------------------- | --------------------------------------------------------------- |
| `log_level`          | `"info"`  | `VAJRA_LOG_LEVEL`          | `"debug"`, `"info"`, `"warn"`, `"error"`, `"fatal"`           | Runtime log verbosity.                                          |
| `access_log`         | `""`      | `VAJRA_ACCESS_LOG`         | String path or `nil`                                          | Access-log destination. Empty or `nil` disables access logging. |
| `access_log_format`  | `""`      | `VAJRA_ACCESS_LOG_FORMAT`  | `"text"`, `"json"`, `"common"`, `"combined"`, or token string | Access-log format.                                              |
| `error_log`          | `""`      | `VAJRA_ERROR_LOG`          | String path                                                   | Error-log destination.                                          |
| `structured_logs`    | `false`   | `VAJRA_STRUCTURED_LOGS`    | `true`, `false`                                               | Emit structured runtime logs.                                   |
| `stats_path`         | `""`      | `VAJRA_STATS_PATH`         | String path                                                   | JSON stats endpoint. Empty disables the endpoint.               |
| `metrics_endpoint`   | `""`      | `VAJRA_METRICS_ENDPOINT`   | String path                                                   | Prometheus metrics endpoint. Empty disables the endpoint.       |
| `trace_enabled`      | `false`   | `VAJRA_TRACE_ENABLED`      | `true`, `false`                                               | Enable request tracing.                                         |
| `trace_endpoint`     | `""`      | `VAJRA_TRACE_ENDPOINT`     | String URL                                                    | OTLP/HTTP trace endpoint when Vajra exports spans.              |
| `trace_service_name` | `"vajra"` | `VAJRA_TRACE_SERVICE_NAME` | String                                                        | OpenTelemetry service name.                                     |
| `trace_otel_owner`   | `false`   | `VAJRA_TRACE_OTEL_OWNER`   | `true`, `false`                                               | Let Vajra own native OTLP export.                               |

Vajra also reads standard OpenTelemetry environment variables:
`OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`,
`OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_TRACES_EXPORTER`,
`OTEL_METRICS_EXPORTER`, `OTEL_PROPAGATORS`, `OTEL_TRACES_SAMPLER`, and
`OTEL_TRACES_SAMPLER_ARG`.

## Environment Precedence

Configuration precedence is:

1. `VAJRA_*` environment variables
2. explicit `config/vajra.rb`
3. standard `OTEL_*` environment variables for tracing defaults when no Vajra-specific tracing override is present
4. Vajra defaults

Environment variables are process-level runtime settings and override explicit
Ruby configuration. Use them for deployment-owned values such as ports, worker
counts, TLS file paths, or OpenTelemetry endpoints.

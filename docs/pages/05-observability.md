---
title: Observability
nav_order: 5
permalink: /observability/
---

# Observability

Vajra exposes request logging, runtime logs, a JSON stats endpoint, a
Prometheus-compatible metrics endpoint, and soft-optional OpenTelemetry request
tracing.

## Logging

Access logging is disabled by default. Enable it with a file path:

```ruby
Vajra.configure do |config|
  config.access_log "log/vajra-access.log"
  config.access_log_format "json"
  config.error_log "log/vajra-error.log"
  config.structured_logs true
end
```

Supported access log formats:

| Format | Behavior |
| --- | --- |
| `text` | Human-readable Vajra access log lines. |
| `json` | Stable structured access events for ingestion. |
| `common` | Common-log-compatible request lines. |
| `combined` | Combined-log-compatible request lines with referer and user agent. |
| token string | Small custom format using tokens such as `%m`, `%U`, `%s`, `%b`, `%a`, `%H`, `%i`, `%D`, `%T`, and `%S`. |

Structured access logs include method, target, status, duration, response body
bytes, remote address, HTTP protocol, host, user agent, referer, request id,
worker pid/index, connection outcome, and incoming `traceparent` trace/span ids
when present.

Send `SIGUSR1` to reopen configured access and error log files after rotation:

```bash
kill -USR1 <vajra-worker-pid>
```

## Stats And Metrics

Configure the control-plane endpoints:

```ruby
Vajra.configure do |config|
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
end
```

The stats endpoint returns JSON with master state, tracing availability,
scheduler pressure, worker health, worker lifecycle, request timing, execution
counts, and restart/replacement counters.

The metrics endpoint remains Prometheus text. It includes runtime liveness,
active connections, active/idle executions, accepts, dispatches, completed
requests, request timing totals, local queue depth, worker lifecycle/health
states, replacement counters, timeout escalations, and unexpected exits.

Prometheus scrape example:

```yaml
scrape_configs:
  - job_name: vajra
    metrics_path: /metrics
    static_configs:
      - targets:
          - 127.0.0.1:3000
```

## OpenTelemetry

Tracing is soft-optional. Vajra boots safely when OpenTelemetry gems are absent.

```ruby
Vajra.configure do |config|
  config.trace_enabled true
  config.trace_endpoint "http://127.0.0.1:4318/v1/traces"
  config.trace_service_name "my-rack-app"
  config.trace_otel_owner false
end
```

When `trace_otel_owner` is false, Vajra uses the application's existing global
OpenTelemetry provider. When it is true, Vajra creates the provider, installs an
OTLP exporter with a batch span processor, and flushes/shuts it down during
`Vajra.stop`.

Collector example:

```yaml
receivers:
  otlp:
    protocols:
      http:
        endpoint: 127.0.0.1:4318

exporters:
  logging:
    verbosity: basic

service:
  pipelines:
    traces:
      receivers: [otlp]
      exporters: [logging]
```

Precedence is explicit Vajra config, then `VAJRA_*`, then standard `OTEL_*`,
then Vajra defaults. Vajra reads:

- `OTEL_SERVICE_NAME`
- `OTEL_RESOURCE_ATTRIBUTES`
- `OTEL_EXPORTER_OTLP_ENDPOINT`
- `OTEL_TRACES_EXPORTER`
- `OTEL_METRICS_EXPORTER`
- `OTEL_PROPAGATORS`
- `OTEL_TRACES_SAMPLER`
- `OTEL_TRACES_SAMPLER_ARG`

Request spans use stable HTTP server attributes for the current request path:
`http.request.method`, `url.path`, `url.scheme`,
`http.response.status_code` where available, `server.address`, `server.port`,
`network.protocol.name`, and `network.protocol.version`.

Native request failures, such as malformed request heads, request-body
disconnects, queue-capacity rejections, queue wait timeouts, and execution
errors, emit server spans with `vajra.request.outcome`, `vajra.failure.kind`,
and `vajra.response.sent`.

## Log Correlation

Structured JSON access logs are the stable ingestion format:

```json
{
  "component": "access",
  "method": "GET",
  "target": "/orders",
  "status": 200,
  "duration_nanoseconds": 1250000,
  "bytes_written": 512,
  "remote_addr": "127.0.0.1",
  "protocol": "HTTP/1.1",
  "host": "example.test",
  "request_id": "request-123",
  "worker_index": 0,
  "connection_outcome": "close",
  "trace_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "span_id": "bbbbbbbbbbbbbbbb"
}
```

When an active OpenTelemetry request span exists, Vajra uses that span's
`trace_id` and `span_id` for access-log correlation. If no active span id is
available, Vajra falls back to valid incoming `traceparent` ids.

Access log format needs map to Vajra formats:

| Need | Vajra Format |
| --- | --- |
| Common log lines | `common` |
| Combined log lines with referer and user agent | `combined` |
| Structured ingestion | `json` |
| Small custom line | token string, such as `%a %m %U %s %b %D %T %S` |

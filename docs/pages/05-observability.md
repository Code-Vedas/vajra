---
title: Observability
nav_order: 7
permalink: /observability/
---

# Observability

Vajra exposes request logging, runtime logs, a JSON stats endpoint, a
Prometheus-compatible metrics endpoint, and optional OpenTelemetry request
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

| Format       | Behavior                                                                                                 |
| ------------ | -------------------------------------------------------------------------------------------------------- |
| `text`       | Plain text Vajra access log lines.                                                                        |
| `json`       | Stable structured access events for ingestion.                                                           |
| `common`     | Common-log-compatible request lines.                                                                     |
| `combined`   | Combined-log-compatible request lines with referer and user agent.                                       |
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

Stats response shape:

```json
{
  "master_pid": 1200,
  "master_rss_bytes": 31576064,
  "socket_queue_capacity": 256,
  "workers": [
    {
      "worker_index": 0,
      "pid": 1201,
      "rss_bytes": 112107520,
      "active_connections": 1,
      "active_execution_count": 0,
      "idle_execution_count": 5,
      "local_queue_depth": 0,
      "lifecycle_state_name": "ready",
      "health_state_name": "healthy",
      "completed_request_count": 188880
    }
  ],
  "profiling": {
    "request_head_nanoseconds": 241483561,
    "request_parse_nanoseconds": 809158976,
    "request_body_nanoseconds": 4320679678,
    "request_total_nanoseconds": 75423539010,
    "rack_finish_nanoseconds": 65393403363,
    "response_write_nanoseconds": 2666749033
  }
}
```

Operational fields:

| Field                        | Meaning                                          |
| ---------------------------- | ------------------------------------------------ |
| `active_connections`         | Worker-owned connections in active processing.   |
| `active_execution_count`     | Rack execution threads running application code. |
| `idle_execution_count`       | Rack execution threads available for work.       |
| `local_queue_depth`          | Worker-local queued work.                        |
| `completed_request_count`    | Completed requests per worker.                   |
| `health_state_name`          | Worker health classification.                    |
| `lifecycle_state_name`       | Worker lifecycle state.                          |
| `request_*_nanoseconds`      | Cumulative request timing buckets.               |
| `response_write_nanoseconds` | Cumulative response write time.                  |

Stats top-level fields:

| Field                  | Meaning                                  |
| ---------------------- | ---------------------------------------- |
| `master_pid`           | Native runtime master process id.        |
| `master_rss_bytes`     | Master process RSS when available.       |
| `socket_queue_capacity` | Configured pending dispatch capacity.   |
| `workers`              | Per-worker runtime state array.          |
| `profiling`            | Cumulative timing and dispatch counters. |
| `native_observability` | Native request/span event counters.      |
| `health_counts`        | Worker count by health state.            |

Worker fields include `worker_index`, `pid`, `rss_bytes`, connection and
execution counts, queue depth, availability, lifecycle/health/recovery names,
accept/dispatch/receive counters, completed requests, replacement counters,
timeout escalation counters, unexpected exit counters, recovery timing, and
terminal replacement failure state.

Prometheus metric examples:

```text
vajra_runtime_up 1
vajra_worker_active_connections{worker="0"} 1
vajra_worker_idle_executions{worker="0"} 5
vajra_worker_completed_requests_total{worker="0"} 188880
vajra_worker_request_nanoseconds_total{worker="0"} 75423539010
vajra_worker_response_write_nanoseconds_total{worker="0"} 2666749033
vajra_worker_health_state{worker="0",state="healthy"} 1
```

Metric catalog:

| Metric                                                      | Meaning                                     |
| ----------------------------------------------------------- | ------------------------------------------- |
| `vajra_runtime_up`                                          | Runtime metrics endpoint is serving.        |
| `vajra_worker_active_connections`                           | Active worker-owned connections.            |
| `vajra_worker_active_executions`                            | Rack execution threads running app code.    |
| `vajra_worker_idle_executions`                              | Idle Rack execution threads.                |
| `vajra_worker_accept_total`                                 | Accepted connections.                       |
| `vajra_worker_dispatch_total`                               | Connections dispatched to a worker.         |
| `vajra_worker_receive_total`                                | Worker receive events.                      |
| `vajra_worker_completed_requests_total`                     | Completed requests.                         |
| `vajra_worker_request_head_nanoseconds_total`               | Cumulative request-head read time.          |
| `vajra_worker_request_parse_nanoseconds_total`              | Cumulative request parsing time.            |
| `vajra_worker_request_body_nanoseconds_total`               | Cumulative request-body time.               |
| `vajra_worker_request_nanoseconds_total`                    | Cumulative total request time.              |
| `vajra_worker_rack_execution_nanoseconds_total`             | Cumulative Ruby Rack execution time.        |
| `vajra_worker_response_write_nanoseconds_total`             | Cumulative response write time.             |
| `vajra_worker_http2_receive_nanoseconds_total`              | Cumulative HTTP/2 receive time.             |
| `vajra_worker_http2_frame_precheck_nanoseconds_total`       | Cumulative HTTP/2 frame precheck time.      |
| `vajra_worker_http2_nghttp2_recv_nanoseconds_total`         | Cumulative nghttp2 receive time.            |
| `vajra_worker_http2_execution_enqueue_nanoseconds_total`    | Cumulative HTTP/2 enqueue time.             |
| `vajra_worker_http2_execution_queue_wait_nanoseconds_total` | Cumulative HTTP/2 queue wait time.          |
| `vajra_worker_http2_execution_drain_nanoseconds_total`      | Cumulative HTTP/2 execution drain time.     |
| `vajra_worker_http2_response_submit_nanoseconds_total`      | Cumulative HTTP/2 response submission time. |
| `vajra_worker_http2_session_send_nanoseconds_total`         | Cumulative HTTP/2 session send time.        |
| `vajra_worker_local_queue_depth`                            | Current worker-local queue depth.           |
| `vajra_worker_lifecycle_state`                              | Current worker lifecycle state label.       |
| `vajra_worker_health_state`                                 | Current worker health state label.          |
| `vajra_worker_replacement_attempts_total`                   | Worker replacement attempts.                |
| `vajra_worker_replacement_success_total`                    | Successful worker replacements.             |
| `vajra_worker_replacement_failure_total`                    | Failed worker replacements.                 |
| `vajra_worker_timeout_escalations_total`                    | Worker timeout escalations.                 |
| `vajra_worker_unexpected_exits_total`                       | Unexpected worker exits.                    |

Metric labels are intentionally low cardinality. `worker` is the worker index;
`state` is the current lifecycle or health state. Do not add request path, user,
host, or tenant labels at this layer.

Use stats for direct runtime inspection and Prometheus metrics for scraping,
alerting, dashboards, and long-term storage.

Prometheus scrape example:

```yaml
scrape_configs:
  - job_name: vajra
    metrics_path: /metrics
    static_configs:
      - targets:
          - 127.0.0.1:3000
```

PromQL examples:

```promql
sum(rate(vajra_worker_completed_requests_total[5m]))
sum(vajra_worker_local_queue_depth)
sum(vajra_worker_active_executions) / sum(vajra_worker_active_executions + vajra_worker_idle_executions)
increase(vajra_worker_unexpected_exits_total[10m])
increase(vajra_worker_timeout_escalations_total[10m])
```

Alert starting points:

| Alert                       | Example Condition                                       |
| --------------------------- | ------------------------------------------------------- |
| Queue pressure              | `sum(vajra_worker_local_queue_depth) > 0` for 5 minutes |
| Worker exits                | `increase(vajra_worker_unexpected_exits_total[10m]) > 0` |
| Timeout escalation          | `increase(vajra_worker_timeout_escalations_total[10m]) > 0` |
| No idle execution capacity  | `sum(vajra_worker_idle_executions) == 0` for 5 minutes  |
| Runtime metrics unavailable | scrape failure or missing `vajra_runtime_up`            |

## OpenTelemetry

Tracing is optional. Vajra boots when OpenTelemetry gems are absent; tracing is
reported as unavailable until the required SDK/exporter components are present.

```ruby
Vajra.configure do |config|
  config.trace_enabled true
  config.trace_endpoint "http://127.0.0.1:4318/v1/traces"
  config.trace_service_name "my-rack-app"
  config.trace_otel_owner false
end
```

When `trace_otel_owner` is false, Vajra uses the application's existing global
OpenTelemetry provider so application and library spans share the same active
Rack context. When it is true, Vajra owns request-span export through its native
OTLP/HTTP pipeline and shuts that native exporter down during `Vajra.stop`.

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

Tracing follows the same runtime precedence as the rest of Vajra config:
`VAJRA_*` environment variables override explicit Ruby settings. Standard
`OTEL_*` variables provide tracing defaults when no Vajra-specific setting is
present. Vajra reads:

- `OTEL_SERVICE_NAME`
- `OTEL_RESOURCE_ATTRIBUTES`
- `OTEL_EXPORTER_OTLP_ENDPOINT`
- `OTEL_TRACES_EXPORTER`
- `OTEL_METRICS_EXPORTER`
- `OTEL_PROPAGATORS`
- `OTEL_TRACES_SAMPLER`
- `OTEL_TRACES_SAMPLER_ARG`

Request spans use stable HTTP server attributes for the active request path:
`http.request.method`, `url.path`, `url.scheme`,
`http.response.status_code` where available, `server.address`, `server.port`,
`network.protocol.name`, and `network.protocol.version`.

Native request failures, such as malformed request heads, request-body
disconnects, queue-capacity rejections, queue wait timeouts, and execution
errors, emit server spans with `vajra.request.outcome`, `vajra.failure.kind`,
and `vajra.response.sent`.

Lifecycle spans use `vajra.<event>` names and include worker attributes such as
`vajra.worker.lifecycle_event`, `vajra.worker.lifecycle_state`,
`health_state`, `recovery_state`, worker index, worker pid, availability,
replacement state, exit classification, and exit detail.

Exporter ownership:

| `trace_otel_owner` | Behavior |
| ------------------ | -------- |
| `false`            | Vajra uses the application's OpenTelemetry provider and active Rack context when available. |
| `true`             | Vajra owns native OTLP/HTTP export to `trace_endpoint`. |

Protect tracing endpoints as internal infrastructure. Spans can include request
paths, hosts, status codes, failure kinds, worker ids, and lifecycle state.

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

Common access-log requirements map to Vajra formats:

| Need                                           | Vajra Format                                    |
| ---------------------------------------------- | ----------------------------------------------- |
| Common log lines                               | `common`                                        |
| Combined log lines with referer and user agent | `combined`                                      |
| Structured ingestion                           | `json`                                          |
| Small custom line                              | token string, such as `%a %m %U %s %b %D %T %S` |

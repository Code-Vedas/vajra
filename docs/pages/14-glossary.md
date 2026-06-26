---
title: Glossary
nav_order: 18
permalink: /glossary/
---

# Glossary

## Runtime Terms

| Term                 | Meaning                                                             |
| -------------------- | ------------------------------------------------------------------- |
| Master process       | Native runtime process that owns listener admission and supervision. |
| Worker process       | Process that owns accepted sockets, protocol IO, and Rack execution. |
| Rack execution pool  | Fixed Ruby thread pool that runs application code inside a worker.   |
| Control plane        | Internal stats and metrics routes configured by `stats_path` and `metrics_endpoint`. |
| Descriptor handoff   | Native transfer of accepted client file descriptors to workers.      |
| Drain                | Shutdown phase where Vajra stops admitting work and waits for active Rack execution. |

## Protocol Terms

| Term                  | Meaning                                                             |
| --------------------- | ------------------------------------------------------------------- |
| ALPN                  | TLS protocol negotiation used to select `h2` or `http/1.1`.          |
| h2c                   | Cleartext HTTP/2, either prior knowledge or HTTP/1.1 upgrade.        |
| Extended CONNECT      | HTTP/2 CONNECT request with a `:protocol` pseudo-header.             |
| Stream tunnel         | Bidirectional byte stream exposed as `Vajra::HTTP2::Stream`.         |
| Flow-control credit   | HTTP/2 capacity released as Rack consumes request or tunnel bytes.   |
| Full hijack           | Rack API that gives Ruby ownership of an HTTP/1.x client connection. |
| Partial hijack        | Rack response-header hijack form; Vajra does not implement it.       |

## Rack Terms

| Term               | Meaning                                                        |
| ------------------ | -------------------------------------------------------------- |
| `rack.input`       | Request body object, exposed by Vajra as `Vajra::NativeInput`. |
| Rack response      | Three-element array: status, headers, body.                    |
| Body close         | `close` call on Rack body after response conversion when present. |
| Rewind             | Resetting request-body read offset after the body is complete. |

## Observability Terms

| Term              | Meaning                                                        |
| ----------------- | -------------------------------------------------------------- |
| Stats endpoint    | JSON runtime state snapshot.                                   |
| Metrics endpoint  | Prometheus text endpoint.                                      |
| Access log        | Per-request log line or JSON event.                            |
| Lifecycle event   | Worker boot, readiness, stop, exit, replacement, or health event. |
| Native span       | Vajra-emitted OpenTelemetry span when native tracing is enabled. |

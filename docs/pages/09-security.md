---
title: Security
nav_order: 11
permalink: /security/
---

# Security

Vajra exposes the controls needed to run a Rack application at the network
edge, but the application and deployment still own authentication,
authorization, secret management, and upstream trust boundaries.

## TLS

Enable TLS only with readable certificate and private-key files:

```ruby
Vajra.configure do |config|
  config.tls true
  config.tls_certificate "/etc/vajra/server.crt"
  config.tls_private_key "/etc/vajra/server.key"
  config.tls_min_version "TLSv1_2"
end
```

Use a certificate chain file for `tls_certificate`. Keep the private key owned
by the runtime user or group and unreadable by other users.

Use `TLSv1_3` when all clients support it. Use the default `TLSv1_2` minimum
when older clients still need access.

## Mutual TLS

Client certificate verification requires a CA bundle:

```ruby
Vajra.configure do |config|
  config.tls true
  config.tls_certificate "/etc/vajra/server.crt"
  config.tls_private_key "/etc/vajra/server.key"
  config.tls_ca_certificate "/etc/vajra/client-ca.pem"
  config.tls_verify_mode "peer"
end
```

Use mTLS only when clients are provisioned with certificates and the application
expects certificate-based trust. Otherwise leave `tls_verify_mode "none"` and
perform authentication at the application layer or reverse proxy.

## Reverse Proxies

When TLS terminates before Vajra, protect the plain listener behind private
networking. Do not expose an unencrypted Vajra listener directly to the internet
unless that is the intended deployment.

Forward only the headers the application trusts. If the app uses
`X-Forwarded-For`, `Forwarded`, `X-Forwarded-Proto`, or request ids, configure
the proxy and application together so untrusted clients cannot spoof identity or
scheme.

## Request Limits

Use request limits as abuse controls:

```ruby
Vajra.configure do |config|
  config.max_request_head_bytes 16_384
  config.max_request_body_bytes 104_857_600
  config.first_data_timeout 30
  config.request_head_timeout 5
  config.request_body_timeout 30
  config.request_timeout 25
end
```

Recommended starting points:

| Setting                  | Protects Against                         |
| ------------------------ | ---------------------------------------- |
| `max_request_head_bytes` | Oversized request heads and header abuse. |
| `max_request_body_bytes` | Unbounded uploads.                       |
| `first_data_timeout`     | Idle accepted connections.               |
| `request_head_timeout`   | Slow request head delivery.              |
| `request_body_timeout`   | Slow request body delivery.              |
| `request_timeout`        | Excessive queue wait before Rack runs.   |

Set larger body limits only on routes that need them, and prefer application
authorization before accepting expensive uploads.

## Control Plane

`stats_path` and `metrics_endpoint` expose runtime state. Keep them private:

```ruby
Vajra.configure do |config|
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
end
```

Protect these paths with network policy, a reverse-proxy allowlist, or internal
service discovery. They are not authentication endpoints.

## Logs

Access logs can include request targets, host names, user agents, referers,
request ids, worker details, and trace ids. Treat logs as operational data with
the same retention and access controls used for the application.

Use structured logs when logs are ingested by a collector:

```ruby
Vajra.configure do |config|
  config.access_log "log/vajra-access.log"
  config.access_log_format "json"
  config.structured_logs true
end
```

Avoid logging secrets in query strings. If the application accepts tokens in
URLs, sanitize those routes before they reach shared log sinks.

## Telemetry

OpenTelemetry spans can include request path, scheme, host, protocol, status,
worker identity, failure kind, and lifecycle state. Export spans only to trusted
collectors.

When the application already owns OpenTelemetry setup, keep
`trace_otel_owner false` so Vajra uses the app's provider. Use
`trace_otel_owner true` when Vajra should export native spans to the configured
OTLP/HTTP endpoint.

## Known Limits

Vajra does not implement application authentication, request authorization,
cookie/session policy, CSRF defense, rate limiting, or WAF behavior. Keep those
controls in the application, reverse proxy, or platform layer.

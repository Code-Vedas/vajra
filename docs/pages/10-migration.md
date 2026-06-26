---
title: Migration
nav_order: 13
permalink: /migration/
---

# Migration

This page maps common Rack server settings and behaviors to Vajra. Use it as a
checklist when moving an existing app; verify production behavior under the
application's own workload before replacing the current server.

## From Puma

Common setting mappings:

| Puma Concept          | Vajra Equivalent                         |
| --------------------- | ---------------------------------------- |
| `bind "tcp://..."`   | `host` and `port` TCP settings.          |
| `port`                | `port`.                                  |
| `workers`             | `workers`.                               |
| `threads min, max`    | `threads min, max`.                      |
| `worker_timeout`      | `worker_timeout`.                        |
| Control/status server | `stats_path` and `metrics_endpoint`.     |
| Phased restart        | Not supported as a public Vajra feature. |
| Puma plugins          | No Vajra plugin API.                     |

Vajra uses a native master/worker runtime. The master owns listener admission
and workers own request IO, Rack execution, and response transport after
descriptor handoff.

## From Passenger

Passenger can integrate with Nginx or Apache and can manage applications behind
those integrations. Vajra is a Rack application server packaged as a Ruby gem.
Run it under a process supervisor, container runtime, or platform process
manager.

Migration checks:

- Replace Passenger process options with Vajra `workers` and `threads`.
- Move reverse proxy config to Nginx, Caddy, HAProxy, or the platform.
- Replace Passenger status/admin workflows with `stats_path`, Prometheus
  metrics, structured logs, and OpenTelemetry.
- Keep app boot in `config.ru`, Rails boot, or `config/vajra.rb`.

## From Falcon

Falcon is built around async Ruby and fiber-oriented IO. Vajra uses native IO
and a fixed Ruby Rack execution pool.

Migration checks:

- Replace async server config with Vajra worker/thread sizing.
- Verify apps that depend on fiber-specific server behavior.
- Use HTTP/2 stream tunnels for Extended CONNECT and WebSocket-over-HTTP/2.
- Use Rack full hijack for HTTP/1.x connection ownership.

## From Unicorn

Unicorn-style deployments often rely on process count and external buffering.
When moving to Vajra:

- Start with fewer workers and more Rack threads if the app is thread-safe.
- Review database pool size per worker.
- Configure request body limits and timeouts explicitly.
- Replace signal/restart workflows with the supervisor's normal restart path
  and Vajra shutdown drain behavior.

## From Thin Or WEBrick

Thin and WEBrick development setups usually have minimal server config. Move the
application boot into `config.ru`, add Vajra to the bundle, and start with:

```bash
bundle exec vajra
```

Add production settings only after the app boots:

```ruby
Vajra.configure do |config|
  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads 5, 5
end
```

## Behavior Differences To Check

| Area                  | Vajra Behavior                                               |
| --------------------- | ------------------------------------------------------------ |
| Config precedence     | `VAJRA_*` environment variables override Ruby config.        |
| Request body object   | Rack receives `Vajra::NativeInput`.                          |
| HTTP/1 hijack         | Full hijack is available through `env["rack.hijack"]`.       |
| HTTP/2 bidirectional  | Extended CONNECT uses `env["vajra.http2.stream"]`.          |
| Partial hijack        | `rack.hijack` response-header partial hijack is unsupported. |
| No-body responses     | `HEAD`, `1xx`, `204`, `205`, and `304` do not send bodies.   |
| Server push           | HTTP/2 server push is unsupported.                           |

## Migration Checklist

1. Add `gem "vajra"` and run `bundle install`.
2. Boot locally through `bundle exec vajra` or `bin/rails server`.
3. Add `config/vajra.rb` with listener, worker, thread, and timeout settings.
4. Size database pools for per-worker thread count.
5. Configure logs, stats, metrics, and tracing.
6. Run the app's request, upload, streaming, WebSocket, and shutdown tests.
7. Deploy behind the same reverse proxy or platform routing used in production.
8. Watch queue depth, worker health, request latency, errors, and unexpected
   exits during the first rollout.

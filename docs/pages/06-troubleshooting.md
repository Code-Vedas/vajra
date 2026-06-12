---
title: Troubleshooting
nav_order: 16
permalink: /troubleshooting/
---

# Troubleshooting

Use this page to triage package setup, native extension builds, executable boot
failures, docs-site problems, and repository checks.

The fastest way to use this page is to identify the symptom first, then jump to
the boundary that owns it: package setup, native build, executable boot, docs
build, or repository checks.

## Common Starting Points

- if `require "vajra"` fails, start with the native extension build path
- if the executable exits before binding the port, start with package setup and
  local runtime boot
- if docs changes do not render, start with the Jekyll workflow under `docs/`
- if repository checks fail, start with `scripts/run-all`

## Support Bundle

When opening an issue or debugging an incident, capture:

- Vajra version and Ruby version
- `config/vajra.rb` with secrets removed
- active `VAJRA_*`, `RACK_ENV`, `RAILS_ENV`, `PORT`, `WEB_CONCURRENCY`, and
  tracing environment variables
- startup logs and lifecycle logs
- access/error log excerpts for the failing window
- stats endpoint output if enabled
- metrics scrape if enabled
- protocol client output for TLS, h2c, or tunnel failures

## Native Extension Does Not Load

Run:

```bash
cd gems/vajra
bundle exec rake compile
```

The package raises an explicit load error when the compiled extension cannot be
found or loaded through the canonical path.

## Port `3000` Is Already In Use

If local startup fails because the listener cannot bind:

- stop the conflicting local process
- retry `bundle exec exe/vajra`
- confirm the process reaches the startup banner before sending a request

## Executable Exits Before Serving Requests

Use the package-local check flow:

```bash
cd gems/vajra
bin/setup
bin/rspec-unit
```

That path checks both the extension load boundary and startup smoke behavior.

If production startup fails, check the failure boundary in this order:

1. CLI arguments
2. `config/vajra.rb` load
3. application boot
4. native config validation
5. listener bind
6. worker readiness

## Application Does Not Boot

If `bundle exec vajra` fails in an app root:

- check `config/vajra.rb` first
- if no Vajra config file is present, check `config.ru`
- for Rails, confirm `config/environment.rb` loads and defines
  `Rails.application`
- for Rack-first frameworks, confirm `config.ru` returns a valid Rack app

If `bin/rails server` says:

```text
Could not find a server gem. Maybe you need to add one to the Gemfile?
```

confirm that:

- `gem "vajra"` is in the application bundle
- Vajra's Railtie has been loaded through Bundler
- the app is not forcing another Rack server through conflicting environment
  variables or server flags

Rails applications using Vajra do not need another Rack server gem for the
server command.

## Lifecycle Log Looks Like The Master Is Serving Requests

If the logs show a serving transition from the runtime process, read the
ownership fields carefully:

- `process_role` identifies which process emitted the event
- `request_execution_role` identifies which role executes application requests

This line is normal in a master-worker runtime:

```text
event=serving_entered ... process_role=native_runtime_control request_execution_role=ruby_worker_bootstrap
```

It means:

- the native runtime owns the listener and entered serving state
- the Ruby worker owns application request execution

## OpenTelemetry Output Is Empty

Tracing is optional. If `trace_enabled` is true but no spans are exported:

- install the OpenTelemetry SDK and exporter gems when the application owns OTel
- set `trace_otel_owner true` only when Vajra should use native OTLP/HTTP export
- check `OTEL_EXPORTER_OTLP_ENDPOINT` or `VAJRA_TRACE_ENDPOINT`
- set `OTEL_TRACES_EXPORTER=otlp`; `none` disables trace exporting
- confirm the stats endpoint reports tracing as enabled and available
- confirm the collector accepts OTLP/HTTP on the configured endpoint

When an application already owns OpenTelemetry setup, leave
`trace_otel_owner` disabled and configure the provider in the application.

If the collector is reachable but has no spans, check that its pipeline has an
OTLP receiver and a traces pipeline. For local collector testing, use
`OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318`.

If lifecycle spans are missing but request spans exist, confirm lifecycle
telemetry callback installation ran before native startup and that
`trace_otel_owner` matches the intended exporter ownership model.

## Latency Is High

Check these signals:

- `vajra_worker_local_queue_depth`
- active versus idle execution counts
- request timing counters
- response write timing counters
- worker health state
- application database pool saturation
- reverse-proxy queueing headers if present

If queue depth grows while idle executions are zero, increase worker or thread
capacity only after checking database and downstream service capacity.

## Worker Keeps Restarting

Inspect lifecycle logs, `vajra_worker_unexpected_exits_total`,
`vajra_worker_timeout_escalations_total`, and stats fields for replacement
attempts and terminal replacement failure. A worker that exits before ready is
usually an application boot problem. A worker that times out while active needs
application-level latency and blocking IO inspection.

## HTTP/1 WebSockets or `rack.hijack` Fail to Connect

Vajra supports connection hijacking via `env['rack.hijack']` for HTTP/1.x
requests. If this is failing:

- use an HTTP/1.x client
- for TLS clients, offer ALPN `http/1.1`
- consume `rack.input` before calling `env['rack.hijack']` on requests with a body
- call the hijack proc only once
- use full hijack, not a `rack.hijack` response header

See [Rack Hijack](/architecture/rack-hijack/) and
[Rack Compatibility](/rack-compatibility/) for the contract.

## HTTP/2 WebSockets or Extended CONNECT Fail

HTTP/2 Extended CONNECT exposes a stream object at
`env["vajra.http2.stream"]`. For WebSocket-over-HTTP/2,
`env["vajra.http2.websocket"]` is `true` and the stream carries raw WebSocket
frame bytes.

If an HTTP/2 tunnel fails:

- enable HTTP/2 with `http2 true`
- use TLS ALPN `h2` or cleartext h2c
- send an Extended CONNECT request with `:method = CONNECT` and a `:protocol`
  pseudo-header
- call `env["vajra.http2.stream"].accept` before writing tunnel bytes
- handle WebSocket framing in the application or WebSocket library

See [HTTP/2 Stream Tunnels](/architecture/http2-stream-tunnels/) for the
stream contract.

## TLS Startup Fails

TLS startup validates credentials before the listener enters serving state. Check:

- `tls_certificate` points to a readable certificate chain file
- `tls_private_key` points to the matching private key
- `tls_ca_certificate` is set when `tls_verify_mode "peer"` is used
- `tls_min_version` is `TLSv1_2` or `TLSv1_3`
- file permissions allow the Vajra worker process to read the credential files

For local self-signed testing, set `tls_verify_mode "none"` on the server and
disable verification in the test client.

## HTTP/2 Negotiation Fails

Enable HTTP/2 with `http2 true`. TLS listeners use ALPN and plain listeners
accept h2c prior knowledge and HTTP/1.1 upgrade:

```ruby
Vajra.configure do |config|
  config.http2 true
  config.tls true
  config.alpn_protocols %w[h2 http/1.1]
end
```

For TLS, confirm the client offered `h2` in ALPN and that `http2` is enabled. If
startup fails, remove `h2` from `alpn_protocols` or enable `http2`.

Use `curl --http2 --insecure https://localhost:<port>/` to confirm ALPN and
request handling against a self-signed local endpoint. For cleartext h2c, use
`curl --http2-prior-knowledge http://localhost:<port>/` or an HTTP/1.1 upgrade
client that sends `Upgrade: h2c`, `Connection: Upgrade, HTTP2-Settings`, and one
valid `HTTP2-Settings` header.

Extended CONNECT is advertised through HTTP/2 settings when `http2 true` is
enabled. Clients that require RFC 8441 WebSocket-over-HTTP/2 support must wait
for that setting before opening the tunnel.

Use `h2spec` for external protocol conformance checks:

```bash
go install github.com/summerwind/h2spec/cmd/h2spec@latest
export PATH="$HOME/go/bin:$PATH"
scripts/run-h2spec-all
```

If HTTP/2 clients reset streams after negotiation succeeds, check for invalid
request headers, mismatched `content-length`, unsupported request shape, or an
application exception during Rack execution. Use the access log outcome, runtime
error logs, and h2 client debug output together.

## Large Uploads Fail

Check:

- `max_request_body_bytes`
- `request_body_timeout`
- `first_data_timeout`
- client disconnects
- proxy body limits in front of Vajra
- whether the application consumes `rack.input`

For full hijack, consume the request body before calling `env["rack.hijack"]`.

## Access Logs Do Not Rotate

Vajra reopens configured access and error log files on `SIGUSR1`:

```bash
kill -USR1 <vajra-worker-pid>
```

`/dev/null` and `nil` access logs remain disabled. If a reopened file cannot be
opened, Vajra keeps the previous sink and writes a diagnostic to stderr.

If access or error logs do not open on startup, check that the parent directory
exists and that the runtime user can write to it. Use an absolute path when the
working directory differs between local execution, Rails, and process managers.

## Clean Checkout Fails

Run:

```bash
scripts/ci-install-bundles
scripts/run-all
```

That flow validates both the gem and the docs site from the repository root.

## Docs Site Does Not Build

Run:

```bash
cd docs
bundle exec jekyll build
```

If the docs build fails, fix the problem in `docs/`. Repository and package docs
use the same canonical paths, commands, and product naming.

## Repository Shape

If a file contains stale names, wrong paths, or missing support files:

- compare the touched folder against the repository conventions
- keep `gems/vajra` as the only gem package
- keep native sources under `gems/vajra/ext/vajra`
- remove stale copied names; do not document around them

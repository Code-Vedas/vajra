# Vajra Performance

Small local benchmark project for comparing Vajra against peer Rack servers.

It generates Rack, Rails, Roda, Sinatra, and Hanami fixtures under `tmp/<timestamp>/`,
starts each fixture through each selected server, runs k6 against the already-running
server, and reports the standard performance profile:

- requests per second
- requests per CPU core
- request count
- error rate
- p95 and p99 latency
- RSS min, max, and final bytes for the server process group

## Run

```bash
cd gems/vajra/performance
bundle exec rake performance:run
```

`performance:run` runs all profiles: the full framework/server comparison,
the Vajra-only observability comparison, and the Vajra-only protocol comparison.
To run one profile directly:

```bash
bundle exec rake performance:main
bundle exec rake performance:observability
bundle exec rake performance:protocol
```

From the repository root, the same profile commands are exposed as scripts:

```bash
scripts/run-performance-main
scripts/run-performance-observability
scripts/run-performance-protocol
```

Local knobs are ENV variables:

```bash
SERVERS=vajra,puma WORKER_COUNT=1 THREAD_COUNT=4 bundle exec rake performance:run
```

Access logging is off by default. Set `ACCESS_LOG=1` to enable fair access
logging for every selected server.

Vajra observability comparisons run after the standard profile and only benchmark
Vajra on the Rack fixture. The observability section uses a shorter fixed run so
it reports every mode without multiplying the full framework/server matrix.
The observability modes are:

- `off`
- `access_text`
- `access_json`
- `access_common`
- `access_combined`
- `access_token`
- `structured_json`
- `otel_otlp`: Vajra-owned native OTLP span export to the local drain
- `otel_all_otlp`: `otel_otlp` plus structured JSON access logging

The protocol profile also runs after the standard profile and only benchmarks
Vajra on the Rack fixture. It uses the standard run duration, including
`SOAK_SECONDS` when set. The default protocol modes are:

- `vajra_http1`: plaintext HTTP/1.1
- `vajra_tls_http1`: HTTP/1.1 over TLS
- `vajra_tls_http2`: HTTP/2 over TLS through ALPN
- `vajra_h2c_prior`: prior-knowledge cleartext HTTP/2
- `vajra_h2c_upgrade`: HTTP/1.1 Upgrade to cleartext HTTP/2
- `vajra_tls_http2_upload`: HTTP/2 upload over TLS
- `vajra_h2c_upload`: cleartext HTTP/2 upload
- `vajra_tls_http2_concurrent_streams`: concurrent HTTP/2 streams over TLS
- `vajra_h2c_concurrent_streams`: concurrent cleartext HTTP/2 streams
- `vajra_tls_http1_keepalive`: HTTP/1.1 keep-alive over TLS
- `vajra_http1_hijack_echo`: plaintext HTTP/1.1 full-hijack echo
- `vajra_tls_http1_hijack_echo`: TLS HTTP/1.1 full-hijack echo
- `vajra_http1_hijack_backpressure`: plaintext HTTP/1.1 full-hijack backpressure
- `vajra_tls_http1_hijack_backpressure`: TLS HTTP/1.1 full-hijack backpressure
- `vajra_tls_http2_small_mixed`: mixed small HTTP/2 requests over TLS
- `vajra_h2c_extended_connect_echo`: cleartext HTTP/2 Extended CONNECT echo
- `vajra_tls_http2_extended_connect_echo`: TLS HTTP/2 Extended CONNECT echo
- `vajra_h2c_websocket_echo`: cleartext HTTP/2 WebSocket echo tunnel
- `vajra_tls_http2_websocket_echo`: TLS HTTP/2 WebSocket echo tunnel
- `vajra_h2c_tunnel_backpressure`: cleartext HTTP/2 tunnel backpressure
- `vajra_tls_http2_tunnel_backpressure`: TLS HTTP/2 tunnel backpressure

Set `SOAK_SECONDS=300` to run the same matrix as a practical local soak. The
default run duration is 20 seconds per server/fixture pair.

Each generated fixture uses the same request mix:

- `GET /text`
- `GET /json`
- `POST /json`
- `POST /form` with text fields and a file
- `POST /upload` with a file

Generated apps, k6 scripts, summaries, and logs are written under
`tmp/<timestamp>/<fixture>/`. Each server/fixture pair writes
`<server>.run-summary.json`, and the whole run writes `summary.json` at the
timestamp root. Observability mode artifacts are written under
`tmp/<timestamp>/<fixture>/observability/<mode>/`. Protocol mode artifacts are
written under `tmp/<timestamp>/<fixture>/protocol/<mode>/`. The comparison
tables print to stdout.

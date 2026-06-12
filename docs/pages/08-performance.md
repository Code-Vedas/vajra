---
title: Performance
nav_order: 12
permalink: /performance/
---

# Performance

Vajra includes a package-local performance runner that exercises Rack, Rails,
Roda, Sinatra, and Hanami fixtures against Vajra and peer Rack servers.

Run from the package performance directory:

```bash
cd gems/vajra/performance
bundle exec rake performance:run
```

The individual profiles are also exposed from the repository root:

```bash
scripts/run-performance-main
scripts/run-performance-observability
scripts/run-performance-protocol
```

Set `DOCKER=1` on any of those scripts to run the profile inside the Linux test image.

## Reproducing A Run

Use the same operating system, Ruby version, worker count, thread count, and
load profile for every comparison. Native extension behavior and socket
behavior can differ between macOS and Linux, so use Docker or a Linux host for
production-facing claims.

Checklist:

- run from a clean checkout
- install bundle dependencies for the root, docs, gem, and performance package
- compile Vajra inside the target environment
- record CPU model, core count, memory, OS, Ruby version, and container image
- keep duration, warmup, workers, threads, and route mix unchanged between runs

## Profiles

| Task                                                                 | Use                                                                                                                 |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `performance:main`, `scripts/run-performance-main`                   | Compare Vajra and peer servers across framework fixtures.                                                           |
| `performance:observability`, `scripts/run-performance-observability` | Measure Vajra with access logs, structured logs, metrics, and tracing modes.                                        |
| `performance:protocol`, `scripts/run-performance-protocol`           | Measure Vajra protocol modes, including HTTP/1, TLS, HTTP/2, h2c, uploads, keep-alive, multiplexing, and tunnels.    |
| `performance:run`                                                    | Run the main, observability, and protocol profiles.                                                                 |

## Workload Shape

The main workload uses a mixed route set:

- `HEAD /text`
- small text and JSON GETs
- header-heavy GETs
- small JSON POSTs
- larger JSON POSTs
- form POSTs
- multipart upload POSTs
- raw body POSTs
- streamed `rack.input.read`
- line-oriented `rack.input.gets`

The workload is intended to expose request parsing, Rack env construction,
request-body handling, response writing, logging, and memory behavior.

## Protocol Workload

The protocol profile measures Vajra transport behavior across connection setup,
request bodies, keep-alive reuse, multiplexing, Rack hijack, and stream tunnels.
It includes:

- plain HTTP/1
- TLS HTTP/1.1
- TLS HTTP/2
- h2c prior knowledge
- HTTP/1.1 `Upgrade: h2c`
- TLS HTTP/2 upload through `rack.input.read`
- h2c upload through `rack.input.read`
- concurrent streams on one TLS HTTP/2 connection
- concurrent streams on one h2c connection
- Rack full hijack over plain HTTP/1.x and TLS HTTP/1.1
- Rack full hijack backpressure over plain HTTP/1.x and TLS HTTP/1.1
- Extended CONNECT echo tunnels over TLS HTTP/2 and h2c
- WebSocket-over-HTTP/2 raw-frame echo over TLS HTTP/2 and h2c
- tunnel backpressure over TLS HTTP/2 and h2c
- TLS HTTP/1.1 keep-alive reuse
- TLS HTTP/2 small mixed GET/POST traffic

The profile uses k6 for ordinary HTTP workloads. h2c, same-connection HTTP/2
multiplexing, Rack hijack, and tunnel lanes use a dedicated protocol driver that
validates frames, response bodies, stream resets, and unexpected connection
closes directly.

The concurrent-stream and tunnel lanes exercise HTTP/2 priority scheduling,
dependency changes, and tunnel backpressure under constrained HTTP/2 windows.
Use route-level metrics and per-lane latency when comparing protocol behavior;
aggregate throughput alone can hide scheduler or flow-control regressions.

## Output

Each run writes artifacts under:

```text
gems/vajra/performance/tmp/<timestamp>/
```

The top-level `summary.json` records:

- requests per second
- requests per CPU core
- total requests
- error rate
- p95 and p99 latency
- process-group RSS min, max, and final memory
- route-level throughput and latency
- Vajra runtime stats snapshots when available

Use route-level metrics when diagnosing regressions. A single aggregate number
can hide a problem isolated to uploads, line reads, TLS, HTTP/2, response
writing, or observability.

## Comparing Runs

Compare by workload lane, not only by aggregate throughput. Check:

- requests per second
- p95 and p99 latency
- error rate
- requests per CPU core
- process-group RSS min, max, and final memory
- route-level throughput and latency
- skipped lanes and skip reasons

Skipped lanes mean the runner could not produce a valid measurement for that
server or mode. Treat them as missing data, not as a pass or a failure, until
the skip reason is understood.

Protocol and tunnel lanes can be noisier than simple HTTP lanes because they
exercise connection setup, flow control, stream scheduling, and custom protocol
drivers. Rerun a noisy lane before calling a regression.

## Linux Validation

Validate Linux behavior in a Linux environment. Native extension behavior,
socket behavior, and scheduler behavior can differ from macOS.

The extension must be compiled inside the Linux environment being measured.
Copying a host-built extension into a Linux container is not a valid
performance run.

## CI Expectations

Performance scripts are useful for release confidence and regression analysis.
They are not a substitute for correctness checks such as C++ tests, RSpec,
h2spec, RBS validation, and the docs build.

When a performance result is used in docs or release notes, include the artifact
path and the workload axis being claimed. Avoid broad claims from one favorable
route.

## Interpreting Results

Report benchmark claims by axis:

- throughput
- p95 and p99 latency
- memory
- error rate
- route-level behavior
- observability overhead
- protocol mode

Avoid turning one favorable route or one platform run into a broad product claim.
A production claim should name the workload, platform, Ruby version, worker
count, thread count, duration, and artifact path.

---
title: Upgrading
nav_order: 14
permalink: /upgrading/
---

# Upgrading

Use the changelog and the application test suite together. Vajra is a native
server runtime, so upgrades should validate Ruby behavior, native extension
loading, protocol behavior, and deployment shutdown.

## Before Upgrading

1. Read the repository [CHANGELOG](https://github.com/Code-Vedas/vajra/blob/main/CHANGELOG.md).
2. Confirm the target version supports the application's Ruby version.
3. Record current `config/vajra.rb` and `VAJRA_*` environment variables.
4. Capture a baseline from `stats_path` and `/metrics` if enabled.
5. Run the application's request, upload, streaming, hijack, and HTTP/2 tests.

## Upgrade Steps

```bash
bundle update vajra
bundle exec ruby -rvajra -e 'puts Vajra::VERSION'
```

Then validate boot:

```bash
bundle exec vajra --config config/vajra.rb
```

Rails applications should also validate:

```bash
bin/rails server
```

## Config Review

Review these areas on every upgrade:

- listener settings: `host`, `port`
- concurrency: `workers`, `threads`, `max_connections`,
  `socket_queue_capacity`
- request limits and timeouts
- TLS and HTTP/2 settings
- access/error log settings
- stats, metrics, and tracing settings

`VAJRA_*` environment variables override Ruby config. Check deployment
environment before assuming a value in `config/vajra.rb` is active.

## Validation

Run the app's normal test suite, then add server-focused checks:

```bash
curl -f http://127.0.0.1:3000/
curl -f http://127.0.0.1:3000/__vajra/stats
curl -f http://127.0.0.1:3000/metrics
```

Use the configured paths for stats and metrics. If HTTP/2 is enabled, validate
TLS ALPN and h2c clients as applicable.

## Rollout

Deploy one environment at a time. Watch:

- startup failures
- worker lifecycle state
- worker health state
- queue depth
- request latency
- completed requests
- timeout escalations
- unexpected exits
- application errors

Stop rollout and rollback if worker replacement failures, unexpected exits, or
request errors increase beyond the application's normal baseline.

## Rollback

Rollback by restoring the previous bundle and runtime config, then restarting
the process through the platform supervisor. Do not reuse a native extension
built for a different gem version or target platform.

## Compatibility Policy

The docs describe current supported behavior. If a release changes public
configuration, Rack environment objects, native APIs, or protocol behavior, the
change should be reflected in the changelog and the relevant docs page.

---
title: Observability
nav_order: 9
permalink: /observability/
---

# Observability

Observability starts as a build-and-boot contract, not just a metrics feature.

You should be able to tell whether Vajra built correctly, loaded correctly,
started correctly, and handled a basic request without reverse-engineering the
repository.

## Signals

- startup banner on successful boot
- native-load error messages when the extension is missing or stale
- process output from the native runtime path

## Observability Outputs

The observability surface includes:

- structured logs for boot, worker readiness, shutdown, and failures
- debug scheduler logs for request admission, global queue depth, least-busy worker selection, queue-capacity hits, and queued-request timeouts
- startup and bind diagnostics
- request-path visibility through e2e-verifiable HTTP behavior
- metrics, stats, and control surfaces configured through Vajra's runtime
  settings

## Why These Signals Matter

These signals carry direct operator value:

- the startup banner confirms the executable reached the native listener
- load errors identify packaging or extension drift immediately
- process output makes the request path visible during local development and
  smoke validation

## What Good Observability Looks Like

Good local observability for Vajra means:

- a failed extension load points directly to the package-local rebuild command
- a boot failure is distinguishable from a bind failure
- a successful request path is visible without attaching a debugger
- repository validation catches build and startup regressions before release

## Integrations

Vajra's observability contract is log-first:

- stdout and stderr carry lifecycle and failure signals
- startup and shutdown emit explicit lifecycle events
- load, boot, bind, and request-execution failures stay diagnosable without
  attaching a debugger

## Lifecycle Field Meanings

The lifecycle log vocabulary is intentionally split:

- `process_role` identifies the process emitting the lifecycle event
- `request_execution_role` identifies the role executing application requests
- `mode` identifies the topology such as single-process or master-worker
- `listener_owned` identifies whether the emitting process owns the active listener
- scheduler debug events expose `selected_worker`, `queue_depth`, `inflight`, and `queue_capacity`
- queue depth refers to the one global FIFO queue, not a worker-local backlog
- worker-local execution backlog is internal today and not yet surfaced as a
  first-class observable signal

This prevents operator confusion between:

- the process that owns the socket
- the worker that executes the Rack or Rails request

## Related Reading

- [Running Vajra](/running-vajra/)
- [Runtime Model](/runtime-model/)
- [Troubleshooting](/troubleshooting/)
- [Development](/development/)

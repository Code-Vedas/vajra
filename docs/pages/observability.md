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
- startup and bind diagnostics
- request-path visibility through e2e-verifiable HTTP behavior

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

The supported observability contract is log-first:

- stdout and stderr carry lifecycle and failure signals
- startup and shutdown emit explicit lifecycle events
- load, boot, bind, and request-execution failures stay diagnosable without
  attaching a debugger

## Related Reading

- [Running Vajra](/running-vajra/)
- [Runtime Model](/runtime-model/)
- [Troubleshooting](/troubleshooting/)
- [Development](/development/)

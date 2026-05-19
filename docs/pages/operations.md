---
title: Operations
nav_order: 8
permalink: /operations/
---

# Operations

Use this page when Vajra is already installed and the question is how to run,
observe, control, and tune it in normal service.

## Operating Posture

Vajra is designed around explicit runtime behavior:

- boot succeeds cleanly or fails with actionable diagnostics
- runtime ownership stays obvious from logs, metrics, and control points
- degraded behavior is visible before it becomes a mystery outage
- platform, protocol, and transport behavior remains explicit and inspectable

## Normal Operating Flow

The normal operator path is:

1. start the server through the supported entrypoint
2. confirm startup diagnostics and listener readiness
3. verify request handling and response behavior
4. watch lifecycle, overload, and failure signals
5. perform controlled shutdown when maintenance or deploys require it

## What Operators Need To Know

### Boot

Operators answer:

- did the native extension load correctly?
- did the listener bind correctly?
- did the application boot path complete?
- did the configured workers become ready?
- are lifecycle diagnostics consistent with the configured runtime shape?

### Steady State

Operators observe:

- request success and failure behavior
- structured runtime events for boot, readiness, serving, drain, shutdown, and failures
- whether the listener is reachable and the application responds correctly
- whether process ownership and execution ownership match expectations

### Shutdown

Controlled shutdown is explicit:

- stop accepting new work
- close the request channel to the worker
- wait for worker exit
- release the listener cleanly

## Operating In Degraded States

When the runtime is degraded, the first questions are:

- is the problem in package/load state, application boot, transport, or runtime
  lifecycle?
- is the service overloaded, wedged, or recovering?
- are failures isolated or poisoning unrelated work?

The linked engineering pages define the ownership model behind those answers.

## Lifecycle Log Reading

Vajra's lifecycle logs are designed to answer two different questions:

- which process owns the current runtime transition
- which role executes application requests

The key fields are:

- `process_role`
- `request_execution_role`
- `mode`
- `worker_processes`
- `listener_owned`
- `listener_fd`

In a master-worker runtime, it is normal for:

- `process_role=native_runtime_control`
- `request_execution_role=ruby_worker_bootstrap`

on serving-related events. That means the runtime process owns the listener
while the worker owns request execution.

## Related Reading

- [Running Vajra](/running-vajra/)
- [Observability](/observability/)
- [Benchmarking](/benchmarking/)
- [Troubleshooting](/troubleshooting/)
- [Engineering Runtime](/engineering-runtime/)

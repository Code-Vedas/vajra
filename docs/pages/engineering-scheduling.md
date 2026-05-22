---
title: Engineering Scheduling
nav_order: 18
permalink: /engineering-scheduling/
---

# Engineering Scheduling

This page records the scheduler boundary and request-admission model.

## Scheduler Boundary

Scheduling work does not blur into unrelated concerns such as:

- native extension packaging
- listener boot and shutdown
- framework integration wiring
- protocol or transport feature flags

## Admission And Capacity Model

Vajra's scheduler defines:

- the admission queue and ownership boundary for incoming work
- worker selection and dispatch rules
- process and thread capacity controls
- queue saturation and backpressure behavior

The scheduler model is:

- one pending queue
- that queue is global
- that queue is FIFO
- scheduler push to the least-busy worker
- no worker-owned pending queues
- queue-first behavior until execution starts, `request_timeout` expires, or the client disconnects
- `queue_capacity` as the hard finite guardrail

That keeps request admission, worker placement, timeout handling, and queue
ownership in one explicit subsystem.

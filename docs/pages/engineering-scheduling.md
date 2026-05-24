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

- one pending queue before worker assignment
- that queue is global
- that queue is FIFO
- scheduler push to the least-busy worker
- assigned work enters a bounded worker-local execution queue before a worker execution thread starts it
- current worker-local ingress still matches configured `max_threads`, so the global queue remains the primary backlog surface
- queue-first behavior until execution starts, `request_timeout` expires, or the client disconnects
- `queue_capacity` as the hard finite guardrail

That keeps request admission, worker placement, timeout handling, and queue
ownership in one explicit subsystem.

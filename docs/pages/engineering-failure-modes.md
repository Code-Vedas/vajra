---
title: Engineering Failure Modes
nav_order: 19
permalink: /engineering-failure-modes/
---

# Engineering Failure Modes

This page records Vajra failure boundaries, detection signals, and recovery
behavior.

## Early Failure Boundaries

Today the most important failure boundaries are:

- native extension missing or stale at load time
- listener bind failure during startup
- request-read or socket-level failure during the runtime path
- shutdown while the listener is active

## Current Recovery Position

The runtime favors explicit failure over hidden fallback:

- package load fails with actionable diagnostics
- startup fails early when the listener cannot be established
- transport, protocol, and platform behavior remains explicit rather than
  silently degraded

## Detection Signals

Failure and degradation are visible through:

- boot diagnostics
- lifecycle transition events
- structured runtime logs
- metrics and telemetry hooks as those observability layers land
- operator-facing visibility into worker, scheduling, and recovery state

## Future Recovery Direction

This page documents:

- restart semantics
- worker lifecycle recovery
- failure isolation boundaries
- control-path timeouts and escalation behavior

## Known Risks And Unresolved Areas

The engineering docs record runtime risk explicitly:

- protocol and transport expansion change failure surfaces materially
- supervision and recovery work can easily erode explicit lifecycle boundaries
  if they are not documented and tested carefully
- Windows, TLS, and HTTP/2 introduce distinct failure classes that are not
  hand-waved as extensions of the plain HTTP/1.1 path

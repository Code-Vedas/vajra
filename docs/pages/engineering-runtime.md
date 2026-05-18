---
title: Engineering Runtime
nav_order: 16
permalink: /engineering-runtime/
---

# Engineering Runtime

This page is the engineering reference for Vajra’s runtime ownership model and
lifecycle shape.

## Runtime Ownership

- Ruby owns package loading, build diagnostics, and executable entrypoints
- C++ owns listener setup, request reads, response writes, and shutdown hooks

## Lifecycle

The runtime lifecycle is:

1. Ruby boots the package entrypoint
2. the native extension is loaded through the canonical require path
3. Ruby preload boot runs in the main process
4. the main process forks one Ruby worker
5. the worker reports boot readiness through the control path
6. the native server instance in the main process binds the listener
7. requests are accepted in the native path and executed in the worker
8. signals or programmatic stop initiate shutdown

## Runtime State Model

The runtime state model is explicit and readable:

- booting
- listening
- serving
- draining
- stopped
- failed

The runtime preserves those explicit transitions rather than burying lifecycle
state in incidental flags or file-descriptor ownership.

## Supervision Direction

The runtime is foreground-oriented with one controlling process and one Ruby
worker. The listener, request parsing, and response transport stay in the
native runtime process; application request execution stays in the Ruby worker.

## Supervision Expectations

Contributors and operators can reason about:

- preloaded master-versus-worker ownership
- worker fork lifecycle
- request-channel versus control-channel responsibilities
- how shutdown flows across the runtime
- which signals are diagnostics and which are lifecycle transitions

## Engineering Boundaries

The repo shape answers these questions:

- where runtime boot begins
- where listener state lives
- where diagnostics are emitted
- where shutdown ownership transitions from request serving to worker teardown
- where lifecycle state stays explicit without being inferred from listener flags

The runtime also preserves a hard split between:

- request-path frames that carry executable application data
- control-path frames that carry lifecycle, readiness, compatibility, and diagnostics

That split is part of the runtime ownership model, not an optional transport
detail.

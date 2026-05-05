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
3. the native server instance binds the listener
4. requests are accepted and handled in the native path
5. signals initiate shutdown

## Runtime State Model

The intended runtime state model is explicit and readable:

- booting
- listening
- serving
- draining
- stopped
- failed

Supervision preserves those explicit transitions rather than burying lifecycle
state in incidental flags.

## Supervision Direction

The runtime is single-process and foreground-oriented. Supervision and worker
lifecycle work extend this documented lifecycle rather than replacing the
package and load contract underneath it.

## Supervision Expectations

Contributors and operators can reason about:

- preloaded master-versus-worker ownership
- worker fork/spawn lifecycle
- task queue and thread lifecycle within a worker
- how shutdown and replacement flow across the runtime
- which signals are diagnostics, which are lifecycle transitions, and which are
  recovery triggers

## Engineering Boundaries

The repo shape answers these questions:

- where runtime boot begins
- where listener state lives
- where diagnostics are emitted
- where shutdown ownership transitions from Ruby to native code

The runtime also preserves a hard split between:

- request-path frames that carry executable application data
- control-path frames that carry lifecycle, readiness, compatibility, and diagnostics

That split is part of the runtime ownership model, not an optional transport
detail.

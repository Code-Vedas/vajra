---
title: Engineering IPC
nav_order: 17
permalink: /engineering-ipc/
---

# Engineering IPC

This page defines the engineering boundary for IPC and supervision work.

## Current Position

The runtime exposes a supervision and control boundary with explicit ownership.

## Why Document It Now

IPC keeps these boundaries from blurring:

- server runtime control
- request execution data flow
- build and package diagnostics
- operator-facing control paths

## Responsibility Split

The IPC contract keeps these concerns separate:

- request-channel responsibilities
- control-channel responsibilities
- lifecycle and shutdown signals
- diagnostics and observability for cross-process state

This page keeps IPC from becoming undocumented ad hoc glue.

## Message Responsibility Model

The IPC contract defines at least:

- request-channel messages for handing executable work into the runtime
- control-channel messages for lifecycle, drain, stop, and operator actions
- status and telemetry messages for health, overload, and failure visibility
- explicit protocol versioning and compatibility rules

## Failure And Recovery Expectations

IPC is not a hidden failure domain. The design makes it obvious:

- when a malformed message is rejected
- when a worker is considered unavailable
- how request-channel and control-channel failures are isolated
- how recovery and restart behavior interact with control messaging

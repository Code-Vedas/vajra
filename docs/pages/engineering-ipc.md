---
title: Engineering IPC
nav_order: 17
permalink: /engineering-ipc/
---

# Engineering IPC

This page defines the engineering boundary for IPC and supervision work.

## IPC Boundary

IPC keeps these boundaries explicit:

- server runtime control
- request execution data flow
- build and package diagnostics
- operator-facing control paths

## Channel Split

The IPC contract keeps these concerns separate:

- request-channel traffic carries execution data flow only
- control-channel traffic carries lifecycle, compatibility, and operator-facing coordination
- request execution never piggybacks on control messages
- lifecycle, diagnostics, and observability never piggyback on request frames

This page keeps IPC from becoming undocumented ad hoc glue.

## Framing Direction

Vajra uses two framing directions with distinct roles:

- the request path uses custom binary framing suited to hot-path execution data
- the control path uses MessagePack-backed payloads suited to lifecycle and operator coordination
- frame-family responsibilities stay explicit and separate from byte-layout details

## Debuggability

The IPC contract keeps debugging work explicit instead of hiding protocol state:

- every frame header identifies its channel, frame family, and protocol version directly
- unsupported channel or frame-family combinations fail with explicit decode outcomes
- protocol mismatch is diagnosed on the control path before request execution starts
- reserved frame families stay distinguishable in code and diagnostics instead of being treated as generic unknown traffic

## Request-Channel Frame Families

The request channel is reserved for application-execution flow between the
native runtime and later Ruby execution workers.

| Frame family | Producers | Consumers | Allowed responsibility | Explicitly excluded |
| --- | --- | --- | --- | --- |
| Request execution input | native runtime | request executor / worker | request line, normalized headers, routing target, and execution metadata needed to begin app handling | lifecycle commands, telemetry, readiness state, operator actions |
| Request body continuation | native runtime | request executor / worker | additional request body bytes when body transport cannot fit in the initial execution input frame | control acknowledgements, worker registration, response metadata |
| Response metadata or result | request executor / worker | native runtime | status, response headers, and response-shape decisions needed for native-side transport ownership | drain or stop commands, health signals, compatibility negotiation |
| Response body continuation | request executor / worker | native runtime | response body bytes or bounded body segments when application output must cross the execution boundary incrementally | lifecycle state, telemetry, registration, request admission decisions |

## Control-Channel Frame Families

The control channel is reserved for runtime coordination that must remain
separate from request execution throughput and request-data ownership.

| Frame family | Producers | Consumers | Allowed responsibility | Explicitly excluded |
| --- | --- | --- | --- | --- |
| Protocol version negotiation | native runtime, Ruby control peer | control peer, native runtime | protocol version markers, compatibility checks, and unsupported-version outcomes | request payloads, response bodies, application execution data |
| Process registration and identity | Ruby master or worker, native runtime | control peer, native runtime | process identity, role declaration, registration, and channel attachment state | request execution, telemetry streams, response metadata |
| Readiness and boot result | Ruby master or worker | native runtime | boot success, boot failure, readiness, and bounded startup diagnostics | request bodies, drain commands, overload reporting |
| Lifecycle command | native runtime, operator-facing control peer | Ruby control peer, native runtime | drain, stop, shutdown orchestration, and later replacement or restart commands | request dispatch payloads, health snapshots, app response data |
| Lifecycle state notification | Ruby control peer, native runtime | control peer, native runtime | state transitions such as booting, ready, draining, stopped, or failed | request or response transport data |
| Diagnostics and error reporting | Ruby control peer, native runtime | control peer, native runtime | actionable control-path errors, malformed-control-message outcomes, and bounded failure context | request execution payloads, telemetry time series, response bodies |
| Reserved telemetry and status | Ruby workers, native runtime | control peer, native runtime | future health, overload, queue pressure, and observability signals | request execution commands, lifecycle mutations, app response data |

## Protocol Versioning And Compatibility

The IPC contract is versioned explicitly. Versioning is part of the protocol
surface, not an implementation detail.

### Version Markers

- every peer advertises a protocol version before relying on frame-family
  behavior beyond transport attachment
- the version marker belongs to the control channel, not the request channel
- the version marker identifies the framing and compatibility contract, not the
  application, Ruby version, or operating-system version

### Compatibility Rules

- identical protocol versions are compatible by default
- a peer may accept a limited set of older or newer protocol versions only when
  compatibility is explicitly defined
- compatibility is defined at the protocol level, not inferred from whether a
  single frame happens to decode
- unsupported versions fail closed rather than silently downgrading into
  partially understood behavior

### Mismatch Behavior

- if no compatible protocol version exists, the control peer rejects the
  connection or session explicitly
- version mismatch is reported as a control-path compatibility failure, not as
  a request execution failure
- once compatibility negotiation fails, neither side may send request-channel
  execution frames on the assumption that the other side will cope
- partial compatibility must be explicit about which frame families remain safe
  to use and which are unavailable

### Extension Rules

- new frame families require a protocol-version change or an explicitly
  negotiated capability that is defined by the versioned contract
- existing frame families may only gain new required fields or semantics when a
  compatibility rule defines how older peers behave
- optional fields and reserved space are preferred for additive evolution that
  does not change the meaning of existing compatible frames
- reserved telemetry or future-status families remain unavailable until a
  versioned contract activates them explicitly

### Compatibility Ownership

- control-channel negotiation owns protocol version decisions for the whole IPC
  relationship
- request-channel execution assumes compatibility has already been established
- runtime lifecycle state must not advance to request-serving readiness until
  protocol compatibility is confirmed
- diagnostics for version mismatch belong to the control path and remain
  actionable enough to distinguish unsupported version, malformed negotiation,
  and incompatible capability expectations

## Responsibility Rules

These rules are part of the contract:

- request-channel frames exist to move executable request or response data only
- control-channel frames exist to coordinate runtime behavior only
- a frame family may have one primary purpose; mixed-purpose frames are rejected
- request data needed for observability is derived from the request path or reported separately, not stuffed into control commands
- lifecycle or telemetry state needed for scheduling is reported on the control path, not piggybacked onto application response frames

## Failure-Boundary Expectations

IPC is not a hidden failure domain. The design makes it obvious:

- malformed request-channel frames are rejected within request-path handling and do not redefine control-plane state implicitly
- malformed control-channel frames are rejected within lifecycle or coordination handling and do not masquerade as request failures
- protocol incompatibility and version-mismatch outcomes belong to control-channel negotiation, not hidden request execution behavior
- worker availability or unavailability is surfaced through control-path state changes rather than inferred from ad hoc request-frame behavior
- recovery and restart logic build on control-channel state and commands instead of overloading request transport frames

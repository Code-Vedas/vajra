---
title: Runtime Model
nav_order: 6
permalink: /runtime-model/
---

# Runtime Model

Vajra is a native Ruby application server with a split runtime:

- a native runtime process owns listener lifecycle and transport
- Ruby worker processes execute application requests
- a control path coordinates boot, readiness, drain, and shutdown
- a request path carries request and response execution data

## Boot Flow

1. Ruby loads the `vajra` package.
2. The package loads the compiled native extension.
3. The executable calls `Vajra.start`.
4. Ruby preload boot runs in the control process.
5. the control process forks Ruby workers from the preloaded application state.
6. worker readiness is reported back to the runtime.
7. the native runtime binds listeners, accepts connections, and transports
   requests and responses.

## Process Model

Vajra uses an explicit master-and-worker split:

- the control process owns boot orchestration and worker supervision
- the native runtime owns listener lifecycle, connection acceptance, request
  parsing, and response transport
- Ruby workers own application request execution
- the request path between them uses Vajra's request-channel IPC
- `Vajra.start` must run on the Ruby main thread so that the runtime can fork
  and supervise the worker safely

## Listener Model

The listener model is native-owned:

- one foreground native runtime process owns the listener lifecycle
- the native runtime binds the socket, listens, accepts connections, and writes
  responses
- Ruby workers do not own client sockets or write directly to the network

## Connection Lifecycle

For each accepted connection, the runtime:

1. accepts the client socket
2. reads request bytes until the header boundary is reached
3. parses the request line and headers into explicit native request state
4. streams request-body bytes to the Ruby worker when the request carries a
   body
5. rejects malformed request heads with a bounded `400 Bad Request` or
   `431 Request Header Fields Too Large` response
6. receives response metadata and response-body segments back from the worker
7. serializes an explicit HTTP/1.1 response for valid request heads
8. reuses the connection for the next sequential request when the current
   exchange is safely message-bounded
9. closes the client socket when the client asks to close, the exchange is not
   reusable, or an error path forces termination

That response path keeps the boot contract, smoke tests, and shutdown behavior
clear and enforceable.

## Lifecycle Signals

Vajra emits lifecycle events that separate process ownership from execution
ownership.

- `process_role` identifies which process emitted the event
- `request_execution_role` identifies which role executes application requests
- `mode` identifies the runtime topology
- `worker_processes` identifies the configured worker count

That distinction matters in a master-worker deployment: the runtime process can
enter `serving` because it owns the listener while request execution remains in
Ruby workers.

## Shutdown Model

The native runtime installs signal handlers for `SIGINT` and `SIGTERM`. A stop
request transitions the listener through an explicit draining state, closes the
request channel, waits for worker exit, and then releases the listener.

## Runtime Scope

The runtime model centers the listener lifecycle, request handling path,
shutdown behavior, and the ownership split between Ruby packaging, native
transport ownership, and Ruby worker request execution.

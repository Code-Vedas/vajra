---
title: Runtime Model
nav_order: 6
permalink: /runtime-model/
---

# Runtime Model

Vajra starts from the Ruby executable and transfers control to the native
extension.

## Boot Flow

1. Ruby loads the `vajra` package.
2. The package loads the compiled native extension.
3. The executable calls `Vajra.start`.
4. The native runtime opens a listener and handles requests.

## Listener Model

The listener model is direct:

- one foreground process owns the listener lifecycle
- the native runtime binds the socket, listens, accepts connections, and writes
  the response
- Ruby does not sit in the request path after the handoff into the extension

## Connection Lifecycle

For each accepted connection, the runtime:

1. accepts the client socket
2. reads request bytes until the header boundary is reached
3. emits the request bytes to process output for local visibility
4. writes a fixed `200 OK` response
5. closes the client socket

That response path keeps the boot contract, smoke tests, and shutdown behavior
clear and enforceable.

## Shutdown Model

The native runtime installs signal handlers for `SIGINT` and `SIGTERM`. A stop
request transitions the listener toward shutdown and releases the server
instance from the process.

## Runtime Scope

The runtime model centers the listener lifecycle, request handling path,
shutdown behavior, and the ownership split between Ruby packaging and native
execution.

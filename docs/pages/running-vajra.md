---
title: Running Vajra
nav_order: 4
permalink: /running-vajra/
---

# Running Vajra

Use the package-local executable from `gems/vajra`.

```bash
cd gems/vajra
bundle exec exe/vajra
```

Vajra binds to port `3000` and serves a basic serialized HTTP/1.1 response path.

## Runtime Expectations

- runs in the foreground
- logs to stdout and stderr
- exits on interrupt
- uses the compiled native extension as the runtime entrypoint

## Boot Chain

The runtime boot path is direct:

1. Ruby starts the `exe/vajra` entrypoint.
2. `require "vajra"` loads the gem and validates the native extension contract.
3. The package transfers control to `Vajra.start`.
4. The native runtime opens the listener, accepts connections, and writes the
   response.

That narrow chain matters because build verification, executable smoke tests,
and runtime troubleshooting all point back to the same load boundary.

## Local Request Check

With the server running:

```bash
curl -i http://127.0.0.1:3000/
```

The local response is `HTTP/1.1 200 OK` with a plain-text `OK` body, and the
runtime keeps the connection reusable for the next sequential HTTP/1.1 request
unless the client or the response path forces close.

## Shutdown Behavior

The runtime is designed for direct operation:

- `Ctrl+C` stops the process cleanly
- the listener stays attached to the foreground process
- request handling remains in the native runtime, not in a Ruby request loop

If the executable exits before the startup banner or fails to bind the port,
use [Troubleshooting](/troubleshooting/) next.

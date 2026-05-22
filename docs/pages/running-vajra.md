---
title: Running Vajra
nav_order: 4
permalink: /running-vajra/
---

# Running Vajra

Use the launcher that matches the host framework.

```bash
bundle exec vajra
```

For Rails, the normal entrypoint is:

```bash
bin/rails server
```

The launcher contract is:

- Rails: `bin/rails server`
- Sinatra, Roda, Hanami, and generic Rack apps: `bundle exec vajra`

Vajra binds to port `3000` by default and serves an HTTP/1.1 response path
through the native runtime.

## Runtime Expectations

- runs in the foreground
- starts from the Ruby main thread
- logs to stdout and stderr
- exits on interrupt
- uses the compiled native extension as the runtime entrypoint
- uses the configured Ruby workers for application execution, defaulting to one

## Boot Chain

The runtime boot path is direct:

1. Ruby starts the selected launcher entrypoint.
2. `require "vajra"` loads the gem and validates the native extension contract.
3. the launcher loads `config/vajra.rb` when present.
4. if no explicit Vajra config file is present, the standalone executable falls back to
   `config.ru`.
5. the package installs the selected Rack or Rails application.
6. the package transfers control to `Vajra.start`.
7. Ruby preload boot runs in the main process.
8. the main process forks the configured Ruby workers, defaulting to one, and waits
   for worker readiness.
9. the native runtime opens the listener, accepts connections, and writes the
   response while the worker executes Rack requests.

That narrow chain matters because build verification, executable smoke tests,
and runtime troubleshooting all point back to the same load boundary.

## Local Request Check

With the server running:

```bash
curl -i http://127.0.0.1:3000/
```

The local response is `HTTP/1.1 200 OK` with a plain-text `OK` body, and the
runtime keeps the connection reusable for the next sequential HTTP/1.1 request
unless the client or the response path forces close. If the client leaves that
reusable connection idle, Vajra applies the request-head read timeout to the
next request and closes the connection when that timeout expires.

## Shutdown Behavior

The runtime is designed for direct operation:

- `Ctrl+C` stops the process cleanly
- the listener stays attached to the foreground runtime process
- request transport remains in the native runtime
- Rack application execution remains in the Ruby worker

If the executable exits before the startup banner or fails to bind the port,
use [Troubleshooting](/troubleshooting/) next.

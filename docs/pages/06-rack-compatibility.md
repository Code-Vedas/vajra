---
title: Rack Compatibility
nav_order: 8
permalink: /rack-compatibility/
---

# Rack Compatibility

Vajra presents applications with a standard Rack environment and executes the
Rack app through the normal `call(env)` contract.

## Rack Input

`env["rack.input"]` is a `Vajra::NativeInput` object. It supports the Rack input
methods applications and middleware expect:

- `read`
- `gets`
- `each`
- `rewind`
- `close`
- `external_encoding`

Body strings are binary-safe. `external_encoding` returns `ASCII-8BIT`.

`rewind` is available after the request body is complete. Large rewindable
bodies use native spill storage, which keeps Ruby memory from holding the whole
body.

## Rack Hijack

HTTP/1.x requests expose `env["rack.hijack"]`. Calling it performs full
hijack. Plain HTTP returns the client socket as a Ruby `IO`; TLS HTTP/1.1
returns an IO-like object for decrypted connection bytes.

Full hijack requirements:

- the request must use HTTP/1.x
- the request body must be fully consumed
- the callable may be used once
- the application owns and closes the returned object

Partial hijack through the `rack.hijack` response header is not implemented.

## HTTP/2 Stream Tunnels

HTTP/2 Extended CONNECT requests expose `env["vajra.http2.stream"]`, a
full-duplex object for one HTTP/2 stream.

The stream object supports:

- `accept`
- `read`
- `write`
- `flush`
- `close`
- `reset`
- `closed?`
- `protocol`
- `stream_id`

For `:protocol = websocket`, Vajra sets
`env["vajra.http2.websocket"]` to `true` and transports raw WebSocket frame
bytes over HTTP/2 DATA frames. WebSocket frame parsing remains the
application's responsibility.

## Response Shape

Rack applications return:

```ruby
[status, headers, body]
```

Vajra validates response status and headers before writing bytes to the client.
Applications should return the Rack response shape and leave HTTP framing,
including `Content-Length` and connection headers, to Vajra.

## Frameworks

Rails, Sinatra, Roda, and Hanami run through their Rack entrypoints. The
framework owns routing, middleware, and application behavior. Vajra owns the
listener, request transport, response writing, worker lifecycle, and
observability endpoints.

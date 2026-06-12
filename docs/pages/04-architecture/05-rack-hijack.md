---
title: Rack Hijack
parent: Architecture
nav_order: 6
permalink: /architecture/rack-hijack/
---

# Rack Hijack

Rack full hijack gives an application direct control of a client connection.
Vajra exposes it through `env["rack.hijack"]` for HTTP/1.x requests, including
TLS HTTP/1.1.

## Full Hijack

The hijack callable is present in plain HTTP/1.0, plain HTTP/1.1, and TLS
HTTP/1.1 Rack environments. Plain HTTP returns the client socket as a Ruby `IO`.
TLS HTTP/1.1 returns an IO-like object backed by Vajra's TLS layer, so Ruby reads
and writes decrypted bytes while Vajra handles encryption on the wire.

After a successful hijack:

- Vajra stops parsing that connection
- normal Rack response writing is skipped
- keep-alive reuse is disabled
- Ruby owns the returned connection object
- closing the returned object closes the underlying client connection

The callable is single-use. Calling it twice raises an `IOError`.

## Request Body Requirement

The request body must be drained before hijack. Vajra rejects hijack while
unread body bytes remain, because native parsing has already consumed the
request head and may have buffered body bytes. Returning a raw socket at that
point would risk data loss or protocol corruption.

## Partial Hijack

Partial hijack through a response header is not implemented. Use full hijack for
raw connection ownership.

## HTTP/2 Stream IO

HTTP/2 multiplexes many streams on one connection, so Vajra exposes per-stream
IO through a stream object. Applications that need bidirectional HTTP/2 IO use
[HTTP/2 Stream Tunnels](/architecture/http2-stream-tunnels/).

## Code Signposts

- Full hijack callable and TLS hijack IO: `gems/vajra/ext/vajra/rack/ruby_execution_bridge.cpp`.
- HTTP/1 request-body consumption checks: `gems/vajra/ext/vajra/rack/native_input.cpp`.
- TLS connection release for hijack: `gems/vajra/ext/vajra/transport/tls_connection.cpp`.

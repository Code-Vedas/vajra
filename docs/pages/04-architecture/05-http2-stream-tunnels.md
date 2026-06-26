---
title: HTTP/2 Stream Tunnels
parent: Architecture
nav_order: 5
permalink: /architecture/http2-stream-tunnels/
---

# HTTP/2 Stream Tunnels

HTTP/2 Extended CONNECT gives Rack applications a full-duplex byte stream
without taking over the whole client connection. Vajra keeps the HTTP/2 session
and its other streams alive; the application works with one
`Vajra::HTTP2::Stream` object.

## Rack Environment

Valid Extended CONNECT requests expose:

- `env["vajra.http2.extended_connect"]`
- `env["vajra.http2.stream"]`
- `env["vajra.http2.websocket"]`

`env["vajra.http2.stream"]` is the `Vajra::HTTP2::Stream` object for that
request.

For WebSocket-over-HTTP/2, Vajra sets:

- `REQUEST_METHOD` to `CONNECT`
- `SERVER_PROTOCOL` to `HTTP/2`
- `env["vajra.http2.extended_connect"]` to `true`
- `env["vajra.http2.websocket"]` to `true`

Vajra then carries raw WebSocket frame bytes inside HTTP/2 DATA frames. The Rack
application, or its WebSocket library, owns WebSocket framing.

## Stream API

`Vajra::HTTP2::Stream` supports:

- `accept(status = 200, headers = {})`
- `read(length = nil, outbuf = nil)`
- `write(string)`
- `flush`
- `close`
- `reset(error_code = :cancel)`
- `closed?`
- `protocol`
- `stream_id`

Calling `accept` sends the HTTP/2 response headers and puts the request in
tunnel mode. After that point the stream object's `read` and `write` methods
carry DATA frames. If the application never accepts the stream, Vajra serializes
the Rack response as a standard HTTP/2 response.

## Flow Control

Inbound DATA frames feed a bounded native stream buffer. Rack reads drain that buffer and release HTTP/2 flow-control credit. Outbound writes use the remote stream and connection windows. Blocking reads and writes release the Ruby GVL while waiting for bytes, capacity, EOF, close, or reset.

Accepted tunnels and ordinary response bodies share the HTTP/2 priority
scheduler. Stream weights, dependencies, and exclusive reprioritization apply
to both.

Reset and close wake blocked readers and writers deterministically:

- peer `END_STREAM` becomes EOF for Ruby reads
- app `close` sends `END_STREAM` when possible
- peer `RST_STREAM` raises on pending and subsequent stream IO
- app `reset` sends `RST_STREAM`

## Example

```ruby
class EchoTunnel
  def call(env)
    stream = env["vajra.http2.stream"]
    return [404, {}, []] unless stream

    stream.accept(200, "content-type" => "application/octet-stream")
    while (chunk = stream.read(16 * 1024))
      stream.write(chunk)
    end
    stream.close

    [200, {}, []]
  end
end
```

## Relationship To Rack Hijack

Rack hijack is connection-level and fits HTTP/1.x. HTTP/2 stream tunnels are
stream-level: Ruby gets bidirectional IO for one Extended CONNECT stream while
Vajra keeps managing the shared HTTP/2 connection.

## Code Signposts

- Rack environment object installation: `gems/vajra/ext/vajra/rack/ruby_execution_bridge.cpp`.
- Ruby stream API: `gems/vajra/ext/vajra/rack/http2_stream.cpp`.
- HTTP/2 accept, DATA scheduling, reset, and flow-control integration:
  `gems/vajra/ext/vajra/request/http2_session.cpp`.

---
title: Protocols
parent: Architecture
nav_order: 4
permalink: /architecture/protocols/
---

# Protocols

Vajra supports HTTP/1.x over plain TCP, HTTP/1.1 over TLS, HTTP/2 over TLS ALPN, and cleartext h2c when HTTP/2 is enabled.

| Capability                         | Support                                                                                |
| ---------------------------------- | -------------------------------------------------------------------------------------- |
| HTTP/1.0                           | Supported. Connections close by default unless keep-alive is requested.                |
| HTTP/1.1                           | Supported. Keep-alive is the default unless the request asks to close.                 |
| TLS HTTP/1.1                       | Supported with configured certificate and private key.                                 |
| HTTP/2 over TLS ALPN               | Supported when `tls true` and `http2 true` are set.                                    |
| h2c cleartext HTTP/2               | Supported when `http2 true` is set. Prior knowledge and HTTP/1.1 upgrade are accepted. |
| HTTP/2 server push                 | Not supported.                                                                         |
| HTTP/2 priority scheduling         | Weighted dependencies and exclusive reprioritization for response and tunnel DATA.       |
| HTTP/2 extended CONNECT            | Supported through `env["vajra.http2.stream"]`.                                         |
| HTTP/2 WebSocket                   | Supported as raw WebSocket frame bytes over extended CONNECT.                          |
| Rack full hijack over HTTP/1.x     | `env["rack.hijack"]` on plain HTTP/1.x and TLS HTTP/1.1 requests.                     |
| Rack partial hijack                | Not implemented.                                                                        |

## HTTP/1.x

Workers parse request heads natively, read request bodies into `Vajra::NativeInput`, and serialize Rack responses back to the client socket. HTTP/1.1 keep-alive sockets return to the worker reactor after a response when the request and configured limits allow reuse.

## TLS

TLS is configured on the Vajra listener. TLS startup validates the certificate chain and private key before the listener enters serving state. ALPN controls whether the connection uses HTTP/1.1 or HTTP/2.

## HTTP/2

HTTP/2 runs through Vajra's native nghttp2 integration. TLS connections enter HTTP/2 through ALPN. Plain connections enter through h2c prior-knowledge preface detection or HTTP/1.1 `Upgrade: h2c` with a valid `HTTP2-Settings` header.

Request headers create a Rack request context. DATA frames feed the same `Vajra::NativeInput` abstraction used by HTTP/1.x. Flow-control credit is released as Rack consumes body bytes.

Extended CONNECT requests receive `env["vajra.http2.stream"]`, a native stream
object for one HTTP/2 stream. Rack applications call `accept`, then read and
write DATA bytes through that object. For `:protocol = websocket`, Vajra marks
the Rack environment as WebSocket-capable and carries raw WebSocket frame bytes.
It does not parse WebSocket frames.

Outbound DATA scheduling honors HTTP/2 dependencies, weights, exclusive
reprioritization, and in-flight reprioritization. Normal response DATA and
accepted tunnel DATA use the same scheduler.

For bidirectional application-owned IO, use
[Rack Hijack](/architecture/rack-hijack/) with HTTP/1.x connections and
[HTTP/2 Stream Tunnels](/architecture/http2-stream-tunnels/) with Extended
CONNECT streams.

## HTTP/2 Capabilities Not Implemented

Server push is not implemented.

## Code Signposts

- HTTP/1 parsing and response writing: `request_processor.cpp`, `response_serializer.cpp`, and `response_writer.cpp`.
- TLS context and connection handling: `transport/tls_connection.cpp`.
- HTTP/2 session, validation, flow control, and response submission: `request/http2_session.cpp`.
- HTTP/2 stream tunnel Ruby object: `rack/http2_stream.cpp`.

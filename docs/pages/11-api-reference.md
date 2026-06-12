---
title: API Reference
nav_order: 9
permalink: /api-reference/
---

# API Reference

Vajra keeps application APIs Rack-compatible. The classes below are native
objects exposed in the Rack environment for request-body transport, full hijack,
and HTTP/2 stream tunnels.

## `Vajra::NativeInput`

`Vajra::NativeInput` is exposed as `env["rack.input"]`.

| Method                    | Returns                    | Behavior                                  |
| ------------------------- | -------------------------- | ----------------------------------------- |
| `read`                    | String                     | Reads the remaining body.                 |
| `read(length)`            | String or `nil`            | Reads up to `length`; returns `nil` at EOF. |
| `read(length, outbuf)`    | String or `nil`            | Replaces `outbuf` with the returned data. |
| `gets(separator = "\n")` | String or `nil`            | Reads one separator-delimited segment.    |
| `each`                    | self or Enumerator         | Yields lines from `gets`.                 |
| `rewind`                  | `0`                        | Rewinds a complete rewindable body.       |
| `close`                   | `nil`                      | Closes the input object.                  |
| `external_encoding`       | `Encoding::ASCII_8BIT`     | Reports binary body encoding.             |

Negative read lengths raise `ArgumentError`. Reads on failed or closed native
input raise `IOError`. Blocking reads release the Ruby GVL while waiting for
bytes, EOF, close, or body failure.

Bodies can be buffered in memory or native spill storage. `rewind` resets the
read offset and is intended for middleware that needs to replay a completed
body.

## `Vajra::NativeHijack`

`Vajra::NativeHijack` is the callable exposed at `env["rack.hijack"]` for
HTTP/1.x requests.

| Method | Returns                                      | Behavior                       |
| ------ | -------------------------------------------- | ------------------------------ |
| `call` | Ruby `IO` or `Vajra::NativeTlsHijackIO`       | Takes ownership of connection. |

The callable is single-use. It raises `IOError` when hijack is unavailable,
already called, already committed, or when `rack.input` has not been fully
consumed.

Plain HTTP returns a Ruby `IO` created from the client file descriptor. TLS
HTTP/1.1 returns `Vajra::NativeTlsHijackIO`.

## `Vajra::NativeTlsHijackIO`

TLS HTTP/1.1 full hijack returns an IO-like object backed by Vajra's TLS layer.
Ruby reads and writes decrypted bytes while Vajra handles TLS on the wire.

| Method              | Returns             | Behavior                             |
| ------------------- | ------------------- | ------------------------------------ |
| `write(string)`     | bytes written       | Writes all bytes or raises `IOError`. |
| `<<(string)`        | self                | Appends bytes and returns self.      |
| `read(length = nil)` | String or `nil`    | Reads bytes; `nil` at EOF for sized reads. |
| `readpartial(length)` | String            | Reads at least one byte or raises `EOFError`. |
| `flush`             | self                | Validates the object and returns self. |
| `close`             | `nil`               | Shuts down TLS and closes the fd.    |
| `closed?`           | `true` or `false`   | Reports close state.                 |

Negative read lengths raise `ArgumentError`. Failed TLS reads or writes raise
`IOError`.

## `Vajra::HTTP2::Stream`

HTTP/2 Extended CONNECT requests expose a stream object at
`env["vajra.http2.stream"]`.

| Method                         | Returns          | Behavior                                |
| ------------------------------ | ---------------- | --------------------------------------- |
| `accept(status = 200, headers = {})` | self       | Sends tunnel response headers.          |
| `read(length = nil, outbuf = nil)`   | String or `nil` | Reads inbound DATA bytes.              |
| `write(string)`                | bytes written    | Queues outbound DATA after accept.      |
| `flush`                        | self             | Wakes the HTTP/2 session sender.        |
| `close`                        | `nil`            | Sends END_STREAM when possible.         |
| `reset(error_code = :cancel)`  | `nil`            | Resets the stream.                      |
| `closed?`                      | `true` or `false` | Reports close, reset, or peer EOF.     |
| `protocol`                     | String           | Returns the Extended CONNECT protocol.  |
| `stream_id`                    | Integer          | Returns the HTTP/2 stream id.           |

`write` before `accept` raises `IOError`. A second `accept` raises `IOError`.
Reads and writes block when the native buffers or HTTP/2 flow-control windows
require it, and blocking waits release the Ruby GVL. Peer reset or local reset
wakes blocked readers and writers.

## Rack Environment Keys

| Key                              | Protocol                  | Value                         |
| -------------------------------- | ------------------------- | ----------------------------- |
| `rack.input`                     | HTTP/1.x and HTTP/2       | `Vajra::NativeInput`.         |
| `rack.hijack`                    | HTTP/1.x                  | `Vajra::NativeHijack`.        |
| `vajra.http2.extended_connect`   | HTTP/2 Extended CONNECT   | `true`.                       |
| `vajra.http2.websocket`          | HTTP/2 WebSocket CONNECT  | `true` when protocol is websocket. |
| `vajra.http2.stream`             | HTTP/2 Extended CONNECT   | `Vajra::HTTP2::Stream`.       |

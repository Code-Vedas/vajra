---
title: Native Input
parent: Architecture
nav_order: 3
permalink: /architecture/native-input/
---

# Native Input

`Vajra::NativeInput` is Vajra's Rack input object. It implements the Rack input methods used by applications and middleware while keeping body transport in the native runtime.

Rack applications receive it through `env["rack.input"]`.

## Contract

`Vajra::NativeInput` supports:

- `read`
- `read(length)`
- `read(length, outbuf)`
- `gets`
- `each`
- `rewind`
- `close`
- `external_encoding`

Returned strings are binary-safe and use `ASCII-8BIT` external encoding.

## Buffering

Incoming body bytes are appended by the native request path. Rack code pulls bytes by calling `read`, `gets`, or `each`.

The native input state tracks:

- unread bytes
- consumed bytes
- EOF
- close state
- body failure state
- in-memory buffering
- spill storage for larger rewindable bodies
- high- and low-watermark pressure

Reads block until bytes, EOF, close, or failure are available. Blocking waits release the Ruby GVL.

## Backpressure

Native input watermarks prevent unbounded request-body memory growth. When buffered data reaches the high watermark, producers wait for capacity. When Rack reads consume bytes and the buffer drops below the low watermark, producers resume.

HTTP/2 uses the same body abstraction. Consumed-byte accounting is used to release flow-control credit as Rack drains the input.

## Rewind

`rewind` requires the body to be complete. Small bodies can rewind from memory. Bodies that exceed the memory threshold use native spill storage so Rack middleware can rewind without keeping the whole body in RAM.

## Code Signposts

- Public Ruby methods and native state: `gems/vajra/ext/vajra/rack/native_input.cpp`.
- HTTP/1 body producer: `gems/vajra/ext/vajra/request/request_body_reader.cpp`.
- HTTP/2 body producer and consumed-byte flow control: `gems/vajra/ext/vajra/request/http2_session.cpp`.

The input object must not allocate Ruby strings while holding native locks; read
paths copy native bytes first, then build Ruby strings after releasing mutexes.

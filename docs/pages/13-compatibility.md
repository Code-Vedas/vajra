---
title: Compatibility
nav_order: 15
permalink: /compatibility/
---

# Compatibility

This page lists Vajra's documented support surface and known limitations. It is
grounded in the current gem, CLI, RBS, and native runtime configuration surface.

## Ruby And Rack

| Area          | Status                                      |
| ------------- | ------------------------------------------- |
| Ruby          | Ruby 3.2 or newer.                          |
| Rack          | Standard Rack three-element response shape. |
| Rails         | Supported through Vajra's Rails handler.    |
| Sinatra       | Supported through Rack.                     |
| Roda          | Supported through Rack.                     |
| Hanami        | Supported through Rack.                     |

## Platforms

| Platform       | Status                                                |
| -------------- | ----------------------------------------------------- |
| Linux          | Primary production and performance validation target. |
| macOS          | Useful for development and local testing.             |
| Windows        | Not documented as a supported production target.      |

Build native extensions inside the deployment target. Do not copy a macOS-built
extension into a Linux image.

## Protocols

| Protocol Or Mode             | Status                                                   |
| ---------------------------- | -------------------------------------------------------- |
| HTTP/1.0                     | Supported.                                               |
| HTTP/1.1                     | Supported.                                               |
| TLS HTTP/1.1                 | Supported with certificate and private key.              |
| HTTP/2 over TLS ALPN         | Supported when `tls true` and `http2 true` are set.      |
| h2c prior knowledge          | Supported when `http2 true` is set.                      |
| HTTP/1.1 `Upgrade: h2c`      | Supported when `http2 true` is set.                      |
| HTTP/2 Extended CONNECT      | Supported through `Vajra::HTTP2::Stream`.                |
| WebSocket-over-HTTP/2        | Supported as raw WebSocket frames over HTTP/2 DATA.      |
| HTTP/2 server push           | Not supported.                                           |

## Rack Features

| Feature                  | Status                                                     |
| ------------------------ | ---------------------------------------------------------- |
| `rack.input`             | `Vajra::NativeInput`.                                      |
| Full Rack hijack         | Supported for HTTP/1.x, including TLS HTTP/1.1.            |
| Partial hijack           | Not supported.                                             |
| HTTP/2 bidirectional IO  | Supported through Extended CONNECT stream tunnels.         |
| Rewindable request body  | Supported after body completion through native buffering.  |
| `HEAD` responses         | Headers are preserved; response body DATA is suppressed.   |
| No-body statuses         | `1xx`, `204`, `205`, and `304` do not send message bodies. |

## Server Features

| Feature                  | Status                                      |
| ------------------------ | ------------------------------------------- |
| TCP host and port bind   | Supported through `host` and `port`.        |
| Unix socket bind         | Not in the public supported config surface. |
| Worker processes         | Supported through `workers`.                |
| Rack execution threads   | Supported through `threads`.                |
| Access/error logs        | Supported.                                  |
| Stats endpoint           | Supported.                                  |
| Prometheus metrics       | Supported.                                  |
| OpenTelemetry tracing    | Supported when configured.                  |
| Daemonization            | Use an external supervisor.                 |
| Phased restart           | Not a public Vajra feature.                 |
| Plugin API               | Not provided.                               |

## Config Surface

Supported runtime settings are listed in [Configuration](/configuration/).
Unknown `Vajra.start` keywords fail as unknown start options. Unknown
`config/vajra.rb` directives fail while loading configuration.

## Known Limitations

- Control-plane endpoints are plain Rack routes and must be protected by the
  deployment.
- HTTP/2 stream tunnels expose byte streams; WebSocket framing remains the
  application's responsibility.
- Full hijack requires the request body to be fully consumed first.
- Native extension behavior should be validated on the same OS and architecture
  used in production.

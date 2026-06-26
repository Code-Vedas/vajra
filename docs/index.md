---
title: Vajra
nav_order: 1
permalink: /
description: Vajra product overview and documentation entrypoint.
---

# Vajra

Vajra is a native Ruby application server for Rack and Rails applications. It is
distributed as a Ruby gem with a C++ runtime that owns the listener, request
parsing, response writing, worker lifecycle, and shutdown behavior.

Use Vajra when an application needs Rack compatibility with native listener,
protocol, and worker lifecycle management.

## Protocol Support

Vajra supports HTTP/1.0, HTTP/1.1, TLS HTTP/1.1, TLS HTTP/2, and cleartext h2c.
HTTP/2 includes prior-knowledge h2c, HTTP/1.1 `Upgrade: h2c`, Extended CONNECT
stream tunnels, and WebSocket-over-HTTP/2 as raw WebSocket frame transport.

## Framework Support

Vajra uses the standard Rack contract, so the same runtime can serve common Ruby
web frameworks.

| Framework | How Vajra Fits                                                                                                 |
| --------- | -------------------------------------------------------------------------------------------------------------- |
| Rack      | Loads `config.ru` and executes the Rack app through Vajra's native request path.                               |
| Rails     | Integrates with `bin/rails server`; Rails owns application boot and middleware, Vajra owns the server runtime. |
| Sinatra   | Runs through the app's Rack entrypoint; Sinatra keeps routing and application behavior.                        |
| Roda      | Runs through `config.ru`; Roda keeps routing while Vajra owns listener and response transport.                 |
| Hanami    | Runs as a Rack application with Hanami routing and application code behind Vajra's server boundary.            |

## Documentation

1. [Installation](/installation/)
2. [Configuration](/configuration/)
3. [Command Reference](/command-reference/)
4. [Frameworks](/frameworks/)
5. [Architecture](/architecture/)
6. [Observability](/observability/)
7. [Rack Compatibility](/rack-compatibility/)
8. [API Reference](/api-reference/)
9. [Production Deployment](/production/)
10. [Security](/security/)
11. [Performance](/performance/)
12. [Migration](/migration/)
13. [Upgrading](/upgrading/)
14. [Compatibility](/compatibility/)
15. [Troubleshooting](/troubleshooting/)
16. [Development](/development/)
17. [Glossary](/glossary/)

## Quick Start

Add Vajra to an application bundle:

```ruby
gem "vajra"
```

For Rack, Sinatra, Roda, or Hanami apps, keep the normal `config.ru` and run:

```bash
bundle exec vajra
```

For Rails apps, keep the normal Rails command:

```bash
bin/rails server
```

Add `config/vajra.rb` when the app needs server-specific settings such as host,
port, worker count, thread count, access logs, or request limits.

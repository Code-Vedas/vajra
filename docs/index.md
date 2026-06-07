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

Use Vajra when you want a Rack-compatible Ruby server with a native runtime
boundary and explicit operational behavior.

## Framework Support

Vajra plays through the standard Rack contract, so the same server runtime can
host common Ruby web framework shapes.

| Framework | How Vajra Fits |
| --- | --- |
| Rack | Loads a standard `config.ru` and executes the Rack app through Vajra's native request path. |
| Rails | Integrates with `bin/rails server`; Rails owns application boot and middleware, Vajra owns the server runtime. |
| Sinatra | Runs through the app's Rack entrypoint with Sinatra keeping routing and application behavior. |
| Roda | Runs through `config.ru`; Roda keeps routing while Vajra owns listener and response transport. |
| Hanami | Runs as a Rack application with Hanami routing/application code behind Vajra's server boundary. |

## Documentation

1. [Installation](/installation/)
2. [Configuration](/configuration/)
3. [Architecture](/architecture/)
4. [Observability](/observability/)
5. [Development](/development/)
6. [Troubleshooting](/troubleshooting/)

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

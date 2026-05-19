---
title: Frameworks
nav_order: 11
permalink: /frameworks/
---

# Frameworks

Vajra supports Ruby web frameworks through two integration families:

- Rails through the native `bin/rails server` path
- Rack-first frameworks through the canonical `config.ru` path

## Framework Map

### Rails

- launcher: `bin/rails server`
- app entrypoint: `config/environment.rb`
- Vajra settings: `config/vajra.rb`
- adapter: built-in Railtie plus `Vajra::Rails`

### Sinatra

- launcher: `bundle exec vajra`
- app entrypoint: `config.ru`
- optional Vajra settings: `config/vajra.rb`
- adapter: shared Rack seam

### Roda

- launcher: `bundle exec vajra`
- app entrypoint: `config.ru`
- optional Vajra settings: `config/vajra.rb`
- adapter: shared Rack seam

### Hanami

- launcher: `bundle exec vajra`
- app entrypoint: `config.ru` with `require "hanami/boot"`
- optional Vajra settings: `config/vajra.rb`
- adapter: shared Rack seam

## Shared Runtime Model

All supported framework hosts keep the same server-owned/runtime-owned split:

- the framework owns application boot, routes, middleware, and response
  semantics
- Vajra owns listener lifecycle, request parsing, worker execution, and
  response transport

That means framework selection changes the application contract, not the server
architecture.

## Choosing an Integration Shape

Use:

- [Rails](/frameworks/rails/) when the app should keep the normal
  `bin/rails server` operator workflow
- [Sinatra](/frameworks/sinatra/), [Roda](/frameworks/roda/), or
  [Hanami](/frameworks/hanami/) when the app already has a standard rackup
  entrypoint and Vajra should be the explicit server command

## Related Reading

- [Integration](/integration/)
- [Configuration](/configuration/)
- [Rails](/frameworks/rails/)
- [Sinatra](/frameworks/sinatra/)
- [Roda](/frameworks/roda/)
- [Hanami](/frameworks/hanami/)

---
title: Rails
parent: Frameworks
nav_order: 2
permalink: /frameworks/rails/
---

# Rails

Rails is Vajra's native framework path. The normal launcher is still
`bin/rails server`, but the server implementation is Vajra instead of Puma.

## Position

- first-class supported framework
- Rails-native launcher path through `bin/rails server`
- no separate server gem required in the application bundle
- Rails keeps application boot, routing, middleware, and controllers
- Vajra keeps listener lifecycle, request parsing, worker execution, and
  response transport

## Install Shape

Add Vajra to the application bundle:

```ruby
# Gemfile
gem "vajra"
```

Create a Vajra config file for server-owned settings:

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.host "127.0.0.1"
  config.port 3000
  config.max_request_head_bytes 32768
end
```

Then start the app with the normal Rails command:

```bash
bin/rails server
```

That is the canonical Rails launcher path. A Rails app using Vajra does not
need `gem "puma"` just to satisfy `rails server`.

## Canonical File Layout

The Rails integration assumes this shape:

```text
Gemfile
bin/rails
config/application.rb
config/environment.rb
config/routes.rb
config/vajra.rb
```

Rails still owns `config/application.rb`, `config/environment.rb`, and
`config/routes.rb`. Vajra owns `config/vajra.rb`.

## Boot Flow

When `bin/rails server` runs:

1. Bundler loads the Rails app bundle, including `vajra`.
2. Vajra's Railtie selects the `vajra` Rackup handler for the server command.
3. Rails loads `config/application.rb` and `config/environment.rb`.
4. `Rails.application` is installed onto Vajra's Rack seam.
5. Vajra reads `config/vajra.rb` for server-owned settings.
6. Vajra starts the native runtime, preloads the Ruby master, forks one worker,
   and serves requests through worker execution.

That keeps the Rails operator workflow familiar while moving transport and
runtime ownership into Vajra.

## Common Scenarios

### Minimal Rails Setup

```ruby
# Gemfile
gem "vajra"
```

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 3000
end
```

```bash
bin/rails server
```

### Explicit Request-Head Limit

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 3000
  config.max_request_head_bytes 65536
end
```

Use this when the app needs a larger request-head allowance than the baseline.

### Standalone Vajra Launcher

```bash
bundle exec vajra -C config/vajra.rb
```

Use the standalone launcher when you want Vajra to be the explicit entrypoint
instead of the Rails command path. The application still boots through
`config/environment.rb`.

## Configuration Ownership

Keep these responsibilities separate:

- Rails-owned:
  - routes
  - middleware
  - controllers
  - environments
  - application boot
- Vajra-owned:
  - port
  - request-head limits
  - launcher selection
  - runtime boot and worker lifecycle
  - socket and transport behavior

`config/vajra.rb` should describe server behavior, not application behavior.

## Failure Boundaries

Rails startup errors stay attributable:

- application boot failures come from `config/application.rb` or
  `config/environment.rb`
- Rails adapter failures come from `Rails.application` installation
- runtime startup failures come from the Vajra listener or worker boot path

That separation matters when the app boots correctly under Rails but the server
cannot bind, or when the server is healthy but the app contract is broken.

## Related Concepts

- [Configuration](/configuration/): config-file directives and precedence
- [Integration](/integration/): runtime ownership boundaries
- [Running Vajra](/running-vajra/): executable startup and shutdown behavior

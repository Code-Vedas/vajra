---
title: Hanami
parent: Frameworks
nav_order: 5
permalink: /frameworks/hanami/
---

# Hanami

Hanami integrates with Vajra through Hanami's normal Rack boot shape: keep
`config.ru`, boot the app with `hanami/boot`, and let Vajra own the runtime.

## Position

- first-class supported framework
- standard Hanami rackup entrypoint
- no Hanami-specific launcher shim required
- Hanami keeps app boot, routes, and actions
- Vajra keeps listener lifecycle, worker execution, and transport

## Install Shape

Add the framework and server gems:

```ruby
# Gemfile
gem "hanami"
gem "vajra"
```

Use the standard Hanami rackup entrypoint:

```ruby
# config.ru
require "hanami/boot"
run Hanami.app
```

Optional Vajra-owned settings live in:

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 3000
  config.max_request_head_bytes 32768
end
```

That keeps Hanami's app boot shape intact while making Vajra the server.

## Canonical Launcher

```bash
bundle exec vajra
```

The launcher loads `config/vajra.rb` when present and otherwise falls back to
Hanami's `config.ru`.

## Common Scenarios

### Minimal Hanami Host

```text
Gemfile
config.ru
config/app.rb
config/routes.rb
app/
```

```bash
bundle exec vajra
```

### Hanami Host With Explicit Server Settings

```text
Gemfile
config.ru
config/vajra.rb
config/app.rb
config/routes.rb
app/
```

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 4000
  config.max_request_head_bytes 65536
end
```

### Direct Rack Installer

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.rackup "config.ru"
  config.port 3000
end
```

Use this when you want the Vajra config file to be the single explicit server
entrypoint while still booting the Hanami app through the standard rackup
script.

## Ownership Split

- Hanami owns:
  - app boot
  - routes
  - actions
  - application middleware
- Vajra owns:
  - process boot
  - native listener
  - worker lifecycle
  - request parsing
  - response transport

## Related Concepts

- [Configuration](/configuration/)
- [Integration](/integration/)
- [Frameworks](/frameworks/)

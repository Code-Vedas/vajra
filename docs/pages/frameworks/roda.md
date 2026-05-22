---
title: Roda
parent: Frameworks
nav_order: 4
permalink: /frameworks/roda/
---

# Roda

Roda is a natural Vajra host because both are explicit about boundaries:
Roda owns application routing, while Vajra owns the server runtime.

## Position

- first-class supported framework
- standard Rack rackup entrypoint
- no Roda-specific adapter required
- Roda keeps routes and app behavior
- Vajra keeps listener lifecycle, worker execution, and transport

## Install Shape

Add the framework and server gems:

```ruby
# Gemfile
gem "roda"
gem "vajra"
```

Keep the application and rackup files explicit:

```ruby
# app.rb
require "roda"

class MyApp < Roda
  route do |r|
    r.root do
      "OK"
    end
  end
end
```

```ruby
# config.ru
require_relative "./app"
run MyApp.app
```

Optional Vajra-owned settings live in:

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 3000
  config.max_request_head_bytes 32768
end
```

## Canonical Launcher

```bash
bundle exec vajra
```

That keeps the application boot on the standard Rack path while making Vajra
the explicit runtime entrypoint.

## Common Scenarios

### Minimal Roda Host

```text
Gemfile
app.rb
config.ru
```

```bash
bundle exec vajra
```

### Roda Host With Explicit Server Settings

```text
Gemfile
app.rb
config.ru
config/vajra.rb
```

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.port 4000
  config.max_request_head_bytes 65536
end
```

### Direct App Installation

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.app MyApp.app
  config.port 3000
end
```

Use this when you want Vajra's config file to select the Roda app directly
instead of relying on `config.ru`.

## Ownership Split

- Roda owns:
  - route tree
  - plugins and middleware
  - request and response behavior
- Vajra owns:
  - native listener
  - process boot
  - worker lifecycle
  - request parsing
  - response transport

## Related Concepts

- [Configuration](/configuration/)
- [Integration](/integration/)
- [Frameworks](/frameworks/)

---
title: Sinatra
parent: Frameworks
nav_order: 3
permalink: /frameworks/sinatra/
---

# Sinatra

Sinatra uses Vajra through the standard Rack path: keep the Sinatra app and
its `config.ru`, then let Vajra own the runtime.

## Position

- first-class supported framework
- no Sinatra-specific adapter required
- standard Rack rackup entrypoint
- Sinatra keeps routes and app behavior
- Vajra keeps listener lifecycle, worker execution, and transport

## Install Shape

Add the framework and server gems:

```ruby
# Gemfile
gem "sinatra"
gem "vajra"
```

Keep the application and rackup files explicit:

```ruby
# app.rb
require "sinatra/base"

class MyApp < Sinatra::Base
  get "/" do
    "OK"
  end
end
```

```ruby
# config.ru
require_relative "./app"
run MyApp.new
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

The standalone launcher looks for:

1. `config/vajra.rb`
2. `config.ru`

If the Vajra config file installs no app directly, the launcher falls back to
the rackup file.

## Common Scenarios

### Minimal Sinatra Host

```text
Gemfile
app.rb
config.ru
```

```bash
bundle exec vajra
```

### Sinatra Host With Explicit Server Settings

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

Use this when the app needs explicit listener or parsing limits beyond the
default runtime settings.

### Explicit Rack Installer

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.app MyApp.new
  config.port 3000
end
```

Use the direct app installer only when you want `config/vajra.rb` to own the
application selection instead of `config.ru`.

## Ownership Split

- Sinatra owns:
  - routes
  - middleware
  - request handlers
  - response semantics
- Vajra owns:
  - process boot
  - native listener
  - worker lifecycle
  - request parsing
  - response transport

That keeps Sinatra thin and server behavior explicit.

## Related Concepts

- [Configuration](/configuration/)
- [Integration](/integration/)
- [Frameworks](/frameworks/)

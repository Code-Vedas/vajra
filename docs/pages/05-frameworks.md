---
title: Frameworks
nav_order: 5
permalink: /frameworks/
---

# Frameworks

Vajra serves Rack applications. Frameworks keep their normal boot and routing
behavior; Vajra owns the server runtime, listener, request parsing, request body
transport, response writing, and worker lifecycle.

## Rails

Add Vajra to the application bundle:

```ruby
# Gemfile
gem "vajra"
```

Start with the normal Rails command:

```bash
bin/rails server
```

Use `config/vajra.rb` for server settings:

```ruby
Vajra.configure do |config|
  config.rails
  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads Integer(ENV.fetch("RAILS_MAX_THREADS", "5")),
                 Integer(ENV.fetch("RAILS_MAX_THREADS", "5"))
  config.stats_path "/__vajra/stats"
  config.metrics_endpoint "/metrics"
end
```

Keep the database pool at least as large as the per-worker maximum thread count.
For the example above, each worker can run five Rack requests concurrently.

Rails owns application health routes such as `/up`. Vajra's stats and metrics
endpoints expose runtime internals and should stay private.

## Rack

Rack applications can run from `config.ru`:

```ruby
class App
  def call(env)
    [200, { "content-type" => "text/plain" }, ["hello\n"]]
  end
end

run App.new
```

Start from the app root:

```bash
bundle exec vajra
```

Add server settings only when needed:

```ruby
Vajra.configure do |config|
  config.rackup "config.ru"
  config.port 3000
  config.threads 4, 4
end
```

## Sinatra

Classic and modular Sinatra applications run through Rack:

```ruby
# config.ru
require_relative "app"
run Sinatra::Application
```

For modular apps:

```ruby
# config.ru
require_relative "app"
run MySinatraApp
```

Vajra does not change Sinatra routing, middleware, params, or response behavior.
Use the same middleware ordering that the application uses with other Rack
servers.

## Roda

Roda applications run through `config.ru`:

```ruby
require "roda"

class App < Roda
  route do |r|
    r.root { "hello" }
  end
end

run App.freeze.app
```

Use `config/vajra.rb` only for server settings.

## Hanami

Hanami applications run through their Rack entrypoint. Keep the framework's
normal boot file and let Vajra load it through `config.ru` or `config.rackup`.

```ruby
# config.ru
require_relative "config/app"
run Hanami.app
```

If the app has framework-specific boot requirements, keep them in Hanami's boot
files rather than in Vajra server config.

## Response Bodies

Rack responses must return the standard three-element response shape:

```ruby
[status, headers, body]
```

The body must respond to `each`. If it responds to `close`, Vajra calls it after
response conversion. For `HEAD`, `1xx`, `204`, `205`, and `304` responses, Vajra
preserves headers but does not send a response body.

## Request Bodies

Applications read request bodies through `env["rack.input"]`, a
`Vajra::NativeInput`. It implements `read`, `gets`, `each`, `rewind`, `close`,
and `external_encoding`.

Large request bodies use native buffering and spill storage. Tune
`max_request_body_bytes`, `request_body_timeout`, and `first_data_timeout` for
public endpoints that accept uploads.

## Troubleshooting

Use these checks first:

- Rails cannot find a server: confirm `gem "vajra"` is in the bundle.
- Rack app does not boot: confirm `config.ru` returns a Rack app.
- Database pool exhaustion: make the pool at least the per-worker thread count.
- Hijack fails: drain `rack.input` before calling `env["rack.hijack"]`.
- HTTP/2 tunnel fails: use Extended CONNECT and `env["vajra.http2.stream"]`.

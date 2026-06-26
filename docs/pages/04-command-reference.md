---
title: Command Reference
nav_order: 4
permalink: /command-reference/
---

# Command Reference

The `vajra` executable starts a Rack application from the application root.
Rails applications can also start Vajra through the normal Rails server
launcher.

## Synopsis

```bash
bundle exec vajra [--config PATH]
bundle exec vajra [-C PATH]
```

The executable accepts one option:

| Option              | Meaning                    |
| ------------------- | -------------------------- |
| `-C PATH`           | Load Vajra config at PATH. |
| `--config PATH`     | Load Vajra config at PATH. |

Unknown options or positional arguments are rejected before startup.

## Startup Files

Vajra resolves application startup in this order:

1. `-C` or `--config`
2. `config/vajra.rb`
3. `config.ru`

Use `config/vajra.rb` for Vajra server settings and application loading
directives. Use `config.ru` for ordinary Rack boot.

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.rackup "config.ru"
  config.host "0.0.0.0"
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads 5, 5
end
```

If the config file contains an unsupported directive, startup fails while the
file is loading. This is intentional: unsupported server features should fail
before the native runtime starts.

## Rack Applications

Rack, Sinatra, Roda, and Hanami applications can run from `config.ru`:

```ruby
# config.ru
run MyRackApplication
```

Start the server from the application root:

```bash
bundle exec vajra
```

Add `config/vajra.rb` only when the application needs explicit Vajra settings
such as listener address, worker count, limits, TLS, logging, metrics, or
tracing.

## Rails Applications

Rails applications use the standard Rails launcher:

```bash
bin/rails server
```

Vajra installs a Rails server handler when the gem is in the application bundle.
Add `config/vajra.rb` for server-specific settings.

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.rails
  config.port Integer(ENV.fetch("PORT", "3000"))
  config.workers Integer(ENV.fetch("WEB_CONCURRENCY", "2"))
  config.threads Integer(ENV.fetch("RAILS_MAX_THREADS", "5")),
                 Integer(ENV.fetch("RAILS_MAX_THREADS", "5"))
end
```

## Startup Banner

On successful startup, Vajra prints the runtime banner and the listener address.
When `port 0` is configured, the operating system chooses an ephemeral port and
the banner reports the actual bound port.

Use an explicit `PORT` in production. Use `port 0` for tests and local scripts
that discover the port from the startup output.

## Failure Behavior

Common startup failures:

| Failure                         | Boundary                                             |
| ------------------------------- | ---------------------------------------------------- |
| Unknown CLI option              | Command-line parsing rejects the process.            |
| Unknown config directive        | `config/vajra.rb` load fails.                        |
| Invalid `Vajra.start` keyword   | Ruby validation raises before native startup.        |
| TLS cert or key missing         | Native runtime validation rejects TLS startup.       |
| Port already in use             | Listener bind fails before serving requests.         |
| `config.ru` does not load a app | Rack application loading fails before native startup. |

See [Troubleshooting](/troubleshooting/) for symptom-based recovery steps.

## Supported Config Directives

The CLI config DSL supports the same native-backed server settings documented in
[Configuration](/configuration/). Application loading directives are:

| Directive       | Effect                                  |
| --------------- | --------------------------------------- |
| `rackup`        | Load `config.ru`.                       |
| `rackup "path"` | Load a specific rackup file.            |
| `rails`         | Load `config/environment`.              |
| `rails "path"` | Load a specific Rails environment file. |
| `app object`    | Install an explicit Rack app.           |
| `app { ... }`   | Build and install a Rack app.           |

Unsupported directives fail as `unsupported configuration directive`.

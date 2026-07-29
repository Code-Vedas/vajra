---
title: Installation
nav_order: 2
permalink: /installation/
---

# Installation

Use this page when adding Vajra to an application. Repository setup, native extension development, conformance checks, performance profiles, and release validation live in [Development](/development/).

## Add The Gem

Add Vajra to the application bundle.

```ruby
# Gemfile
gem "vajra"
```

Install the bundle from the application root.

```bash
bundle install
```

Vajra is currently published as a source gem. Bundler compiles its native extension for the active Ruby toolchain during installation. Windows requires a 64-bit RubyInstaller UCRT Ruby (`x64-mingw-ucrt`); MSVC-built Ruby is not supported.

Validate the package loads:

```bash
bundle exec ruby -rvajra -e 'puts Vajra::VERSION'
```

If the native extension does not load, rebuild the source gem through Bundler using the active Ruby toolchain, then see [Troubleshooting](/troubleshooting/#native-extension-does-not-load). Do not copy or rename an extension built for another Ruby installation. Runtime development prerequisites and repository validation commands live in [Development](/development/), not on this installation path.

## Rails

Rails applications continue to use the normal Rails launcher.

```bash
bin/rails server
```

Vajra supplies the server handler for `bin/rails server`; Rails applications do not need another Rack server gem for that command.

Add `config/vajra.rb` when the application needs explicit Vajra server settings.

```ruby
# config/vajra.rb
Vajra.configure do |config|
  config.host "0.0.0.0"
  config.port 3000
  config.workers 2
  config.threads 5, 5
end
```

## Rack, Sinatra, Roda, And Hanami

Keep the framework's standard `config.ru`.

```ruby
# config.ru
run MyRackApplication
```

Start Vajra from the application root.

```bash
bundle exec vajra
```

Vajra loads `config.ru` automatically when no explicit `config/vajra.rb` is present. Create a Vajra config file for server settings such as `host`, `port`, request limits, TLS, logging, metrics, or tracing.

## Next Steps

- See [Configuration](/configuration/) for the supported runtime settings.
- See [Command Reference](/command-reference/) for executable behavior.
- See [Frameworks](/frameworks/) for framework-specific setup.
- See [Production](/production/) for deployment guidance.
- See [Troubleshooting](/troubleshooting/) for common boot and native build issues.

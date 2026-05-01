# Vajra Gem

`vajra` is the canonical Ruby package for the Vajra server runtime.

It provides the Ruby entrypoints, packaging contract, executable, signatures,
and native-extension bridge for the Vajra server implementation.

## Use This Package When

- you are building or packaging Vajra itself
- you need the `vajra` executable entrypoint
- you are working on the Ruby-to-native extension boundary
- you need the source of truth for package-local development commands

## Product Role

This package owns:

- the Ruby package and executable surface
- the native extension build contract
- package-local validation lanes
- runtime boot wiring into the native server

## Repository Pairing

- `gems/vajra` is the one supported gem package in this repository
- `docs/` is the product documentation source of truth
- root `scripts/` provide repository-level install and verification entrypoints
- `.github/` workflows validate this package and the docs site together

## Development

```bash
bundle install
bin/rspec
bin/rspec-unit
bin/rspec-e2e
bin/rubocop
bin/reek
bin/clint
bin/ctest
rbs -I sig validate
bundle exec exe/vajra
```

`bin/rspec-unit` runs the committed package spec suite, including the clean
Ruby/package behavior checks. `bin/rspec-e2e` runs the integration-style boot
check without coverage. `bin/clint` runs the native C++ lint lane, and
`bin/ctest` builds and runs the native C++ lifecycle tests.

## Runtime Configuration

Vajra accepts runtime config from both `Vajra.start(...)` and environment variables.
Environment variables take precedence when both are present.

- `port`
  - Ruby: `Vajra.start(port: 9292)`
  - env: `VAJRA_PORT`
  - default: `3000`
  - accepted range: `0..65535`
  - `0` asks the OS for an ephemeral port, which Vajra prints in the startup banner
- `max_request_head_bytes`
  - Ruby: `Vajra.start(max_request_head_bytes: 32768)`
  - env: `VAJRA_MAX_REQUEST_HEAD_BYTES`
  - default: `16384` (`16 KiB`)
  - accepted range: integers greater than `0`

## Native Extension

The canonical native source tree lives under `ext/vajra/`.

Use the package-local build flow to compile and refresh the extension:

```bash
bundle exec rake clobber compile
```

If the extension is missing or stale, `require "vajra"` raises an actionable
load error that points back to the package-local compile command.

## Package Discipline

Keep changes here aligned across:

- `lib/` for Ruby entrypoints
- `ext/vajra/` for native sources
- `sig/` for RBS truth
- `spec/` for direct package validation
- `exe/` and `bin/` for runtime and developer entrypoints

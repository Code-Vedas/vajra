---
title: Configuration
nav_order: 7
permalink: /configuration/
---

# Configuration

Vajra uses an explicit configuration surface with documented precedence and
clear ownership boundaries.

## Configuration Principles

- prefer explicit startup inputs over hidden global behavior
- keep package-local commands authoritative for build and validation
- keep native-runtime configuration understandable from the executable path
- avoid duplicate configuration surfaces that describe the same behavior in
  different ways
- keep precedence rules explicit enough that operators can explain why a value
  won

## Current Inputs

- Ruby toolchain and Bundler environment
- native compilation toolchain
- executable startup from `gems/vajra`
- listener port
- maximum request-head size

## Build-Time Configuration

The build contract is driven through:

- `gems/vajra/Gemfile`
- `gems/vajra/ext/vajra/extconf.rb`
- `bundle exec rake compile`

Those files and commands define how the extension is compiled and where the
compiled artifact lives.

## Runtime Configuration Families

The supported configuration story is organized around these families:

- listener and socket behavior
- request parsing limits
- documented environment overrides for runtime-owned values

## Runtime Configuration Posture

Runtime inputs stay discoverable from the executable and from this docs site
rather than being spread across undocumented environment conventions.

## Integration Configuration Direction

Framework integration follows an explicit-config-first model:

- application entrypoint and adapter selection should be explicit
- `VAJRA_`-prefixed environment overrides are part of the supported config path
- application-owned behavior stays in the application, not in hidden server
  toggles

## Precedence Model

The precedence model is:

1. explicit server/application configuration
2. documented `VAJRA_` environment overrides
3. implementation defaults for supported runtime-owned settings

Operators can reason about effective configuration without reverse-engineering
source.

## Supported Runtime Settings

The supported runtime-owned settings are:

- `port` via `Vajra.start(port: ...)`
- `max_request_head_bytes` via `Vajra.start(max_request_head_bytes: ...)`
- `VAJRA_PORT`
- `VAJRA_MAX_REQUEST_HEAD_BYTES`

The environment variables override the corresponding Ruby startup options.

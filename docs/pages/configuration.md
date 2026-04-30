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
- the default listener port owned by the runtime
- platform-specific settings within the supported platform surface
- protocol and transport settings within the supported runtime surface

## Build-Time Configuration

The build contract is driven through:

- `gems/vajra/Gemfile`
- `gems/vajra/ext/vajra/extconf.rb`
- `bundle exec rake compile`

Those files and commands define how the extension is compiled and where the
compiled artifact lives.

## Runtime Configuration Families

The supported configuration story is organized around these families:

- boot and application entrypoint selection
- listener and socket behavior
- worker and scheduling controls as those runtime layers land
- observability and telemetry outputs
- protocol- and transport-specific settings only after those milestones ship

## Runtime Configuration Posture

Runtime inputs stay discoverable from the executable and from this docs site
rather than being spread across undocumented environment conventions.

## Integration Configuration Direction

Framework integration follows an explicit-config-first model:

- application entrypoint and adapter selection should be explicit
- `VAJRA_`-prefixed environment overrides are part of the supported config path
- transport, protocol, and platform-specific toggles belong to documented
  supported surfaces

## Precedence Model

The intended precedence model is:

1. explicit server/application configuration
2. documented `VAJRA_` environment overrides
3. implementation defaults for supported runtime-owned settings

Operators can reason about effective configuration without reverse-engineering
source.

## Core Tuning Surfaces

The core supported tuning surfaces include:

- concurrency/process settings such as `WEB_CONCURRENCY` and `MAX_THREADS`
- scheduling and admission-control settings
- observability exporter and verbosity settings
- protocol and transport settings

These surfaces are documented as part of the supported runtime contract.

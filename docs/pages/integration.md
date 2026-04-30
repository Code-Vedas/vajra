---
title: Integration
nav_order: 10
permalink: /integration/
---

# Integration

Use this page when moving from a local server entrypoint to a real application
integration.

Vajra’s integration surface is organized around two immediate reader needs:

- generic Rack application hosting
- Rails application hosting

## Support Boundary

The integration story is explicit:

- Rack is the generic application contract
- Rails is the first framework-specific path documented here
- additional framework integrations belong to their own documented contracts

## Shared Integration Principles

Every supported integration preserves the same high-level contract:

- the application remains responsible for application semantics
- Vajra remains responsible for listener lifecycle, request execution entry, and
  runtime ownership
- boot failures identify whether the problem is application boot,
  adapter wiring, or server startup
- configuration precedence is explicit and documented

## Rack Integration

The Rack path keeps application ownership obvious:

1. the application builds or exposes a Rack app object
2. Vajra owns listener startup and request execution
3. the integration boundary translates native request state into Rack execution
   inputs

The configuration story is explicit and centered on the server entrypoint, not
on hidden middleware side effects.

Rack integrations define:

- boot assumptions for the Rack app object
- request environment translation guarantees
- response translation guarantees
- lifecycle expectations when the runtime preloads or supervises workers

## Rails Integration

The Rails path builds on the same ownership split:

- Rails keeps application boot, routing, middleware, and controller execution
- Vajra owns the server runtime, connection lifecycle, and listener behavior
- adapter wiring is explicit enough that startup failures are diagnosable

Rails integration defines:

- application boot expectations
- preload and worker lifecycle assumptions
- common Rails boot failure diagnostics
- where framework-specific behavior ends and server-owned behavior begins

## Configuration Expectations

Readers move from quickstart to real-app configuration without guessing:

- where the application entrypoint is defined
- which settings are server-owned versus app-owned
- which environment overrides are supported
- which protocol or platform features are part of the documented support surface

## Configuration Precedence

The precedence model is:

1. explicit application/server configuration
2. documented `VAJRA_`-prefixed environment overrides
3. runtime defaults for server-owned values

Undocumented environment variables or adapter-specific side channels are outside
the supported integration behavior.

## Related Reading

- [Getting Started](/getting-started/)
- [Installation](/installation/)
- [Configuration](/configuration/)
- [Architecture](/architecture/)
- [Operations](/operations/)

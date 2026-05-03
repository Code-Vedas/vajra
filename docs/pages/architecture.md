---
title: Architecture
nav_order: 5
permalink: /architecture/
---

# Architecture

Vajra is organized as one product, one repository, and one canonical gem, with
an explicit split between Ruby package ownership and native runtime ownership.

## Ownership Model

Vajra uses a deliberate split architecture:

- Ruby owns package management, executable entrypoints, and extension loading
- C++ owns the native runtime implementation

## Subsystem Map

Vajra’s architecture is organized through a small set of durable subsystem
families:

- packaging and native build
- runtime lifecycle and listener ownership
- Rack and Rails execution bridge
- supervision, IPC, and worker lifecycle
- scheduling and admission control
- observability and recovery surfaces
- protocol and transport extensions such as TLS and HTTP/2

Within the supervision and IPC family, Vajra keeps request-execution framing
separate from control and lifecycle framing so future Rack execution, worker
coordination, and observability work do not collapse into one mixed protocol.

## Repository Boundaries

| Path                    | Role                                               |
| ----------------------- | -------------------------------------------------- |
| `gems/vajra/lib/`       | Ruby package entrypoints and load boundary         |
| `gems/vajra/exe/`       | executable startup path                            |
| `gems/vajra/ext/vajra/` | native extension sources and build entrypoint      |
| `gems/vajra/sig/`       | RBS truth for the Ruby package surface             |
| `gems/vajra/spec/`      | package-local validation and smoke coverage        |
| `docs/`                 | product documentation and docs-site build          |
| `.github/`              | workflows, issue templates, and release automation |
| `scripts/`              | repository-level verification entrypoints          |

## Native Boundary

The Ruby package requires the compiled extension through a single canonical load
path. Build, smoke tests, and executable boot all validate the same contract.

## Boot Responsibility Chain

The runtime boot path is short:

1. `exe/vajra` starts the package entrypoint.
2. `lib/vajra.rb` owns extension loading and user-facing load errors.
3. `ext/vajra/` owns compilation and Ruby-native binding.
4. the native runtime owns socket setup, signal handling, connection acceptance,
   and response writing.

This split keeps the Ruby layer focused on packaging and diagnostics while the
native layer stays responsible for network-facing runtime behavior.

## Documentation Boundary

`docs/` is the product-doc surface for installation, runtime behavior,
operations, and development workflow. Package READMEs stay concise and defer to
the docs site for the full product story.

Engineering pages under `docs/pages/` are the prose reference for subsystem
boundaries and intended behavior. They complement code and tests, not issue
history archaeology.

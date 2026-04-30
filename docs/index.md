---
title: Vajra
nav_order: 1
permalink: /
description: Vajra product overview and documentation entrypoint.
---

# Vajra

Vajra is a native Ruby application server implemented in C++ and delivered as a
Ruby extension. The project combines a package-local Ruby entrypoint with a
native runtime that owns the server loop, connection lifecycle, and runtime
orchestration.

## What The Product Includes

- a canonical gem under `gems/vajra`
- a native extension source tree under `gems/vajra/ext/vajra`
- a package-local executable for local runtime startup
- explicit build, smoke-test, and release workflows
- product documentation for setup, runtime behavior, and operations

## Product Posture

Vajra is organized as one gem with one clear runtime boundary.

- Ruby owns package installation, executable entrypoints, developer tooling, and
  the contract for loading the extension.
- C++ owns the native listener, request loop, signal-aware shutdown behavior,
  and response path.
- `docs/` is the product documentation surface. Package READMEs support it;
  they do not replace it.

## Recommended Reading Path

1. [Getting Started](/getting-started/)
2. [Installation](/installation/)
3. [Running Vajra](/running-vajra/)
4. [Architecture](/architecture/)
5. [Runtime Model](/runtime-model/)
6. [Configuration](/configuration/)
7. [Operations](/operations/)
8. [Observability](/observability/)
9. [Benchmarking](/benchmarking/)
10. [Troubleshooting](/troubleshooting/)
11. [Development](/development/)

## Support Snapshot

| Area                  | Position                                                                       |
| --------------------- | ------------------------------------------------------------------------------ |
| Canonical package     | `gems/vajra`                                                                   |
| Native source tree    | `gems/vajra/ext/vajra`                                                         |
| Runtime executable    | `gems/vajra/exe/vajra`                                                         |
| Default local startup | `bundle exec exe/vajra`                                                        |
| Native build flow     | `bundle exec rake compile`                                                     |
| Package validation    | `bin/rspec-unit`, `bin/rubocop`, `bin/reek`, `bundle exec rbs -I sig validate` |
| Repository validation | `scripts/ci-install-bundles`, `scripts/run-all`                                |
| Docs surface          | Jekyll + Just the Docs under `docs/`                                           |
| Public docs host      | `https://vajra.codevedas.com`                                                  |

## Explore The Docs

- [Getting Started](/getting-started/): repository bootstrap, first validation,
  and the fastest reading path through the project
- [Installation](/installation/): toolchain requirements, native build path,
  artifact expectations, and clean rebuild workflow
- [Running Vajra](/running-vajra/): executable startup, request handling
  expectations, and shutdown behavior
- [Architecture](/architecture/): repository map, ownership boundaries, and the
  Ruby-to-native boot chain
- [Runtime Model](/runtime-model/): listener lifecycle, connection handling, and
  where runtime behavior lives
- [Configuration](/configuration/): configuration surfaces, precedence, and
  tuning families
- [Operations](/operations/): boot, shutdown, runtime checks, and the operator
  view of normal and degraded service
- [Observability](/observability/): startup diagnostics, request visibility, and
  operating signals
- [Integration](/integration/): Rack and Rails integration posture plus the
  framework-support boundary
- [Benchmarking](/benchmarking/): benchmark interpretation, fixture families, and
  environment caveats
- [Troubleshooting](/troubleshooting/): build, boot, docs, and local runtime
  failure paths
- [Development](/development/): package-local workflow, repository checks, and
  release-supporting maintenance
- [Docs Publishing](/docs-publishing/): local preview, CI validation, Pages
  deployment, and `vajra.codevedas.com`
- [Engineering Runtime](/engineering-runtime/): runtime lifecycle and ownership
  boundaries
- [Engineering IPC](/engineering-ipc/): supervision and control-channel
  boundaries
- [Engineering Scheduling](/engineering-scheduling/): scheduling, admission, and
  capacity boundaries
- [Engineering Failure Modes](/engineering-failure-modes/): boot, shutdown, and
  recovery-oriented engineering guidance

---
title: Installation
nav_order: 3
permalink: /installation/
---

# Installation

Use this page for the canonical package install path, native-extension build
requirements, and artifact expectations.

## Prerequisites

- Linux and macOS are the supported development platforms
- Windows belongs to the Windows parity track and has its own platform-specific
  contract
- Ruby 3.2 or newer
- Bundler
- a C++ toolchain suitable for building Ruby native extensions
- standard system headers and build tools required by your Ruby installation

## Canonical Package Path

The canonical package lives at `gems/vajra`.

Vajra is a single-gem repository. Build and packaging commands resolve through
that path.

## Package Setup

```bash
cd gems/vajra
bin/setup
```

`bin/setup` installs gem dependencies and compiles the native extension through
the package-local build path.

## Expected Build Outputs

The native build flow produces artifacts that line up with the gem’s canonical
load path:

- sources live under `gems/vajra/ext/vajra/`
- the extension is compiled through `ext/vajra/extconf.rb`
- the load path resolves through `require "vajra"` and the internal
  `require_relative "vajra/vajra"` bridge

## Manual Rebuild

```bash
cd gems/vajra
bundle exec rake clobber compile
```

Use the clobber-and-compile path when changing extension code, validating clean
checkout behavior, or debugging a stale local artifact.

## Repository-Level Setup

For a full repository install and validation pass:

```bash
scripts/ci-install-bundles
scripts/run-all
```

That path checks the gem, docs site, and repository automation assumptions
together.

## Validation Path

The supported validation lanes are:

- `bin/rspec-unit` for unit tests with coverage
- `bin/rspec-e2e` for integration tests with no coverage
- `bundle exec rake clobber compile build` for clean rebuild verification
- `scripts/run-all` for the repository-wide baseline

## Packaging Expectations

The packaged gem includes the runtime package surface only:

- `lib/`, `exe/`, `ext/`, `sig/`, and package-local support files
- no repository-only workflow files
- no docs build output
- no transient build directories in the published artifact

The smoke suite and gem build flow validate that the package contents stay lean
and loadable.

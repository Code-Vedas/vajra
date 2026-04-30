# Vajra

Vajra is a native Ruby application server implemented in C++ and packaged as a
Ruby extension.

The repository contains the canonical gem under `gems/vajra`, the published
documentation site under `docs/`, and the GitHub automation and contributor
guidance needed to build, validate, and release the project.

## Product Shape

Vajra keeps one deliberate ownership split:

- Ruby owns packaging, executable boot, signatures, and build diagnostics.
- C++ owns the native listener, request loop, connection handling, and shutdown
  behavior.
- `docs/` owns the full product story for installation, runtime behavior,
  observability, troubleshooting, and development workflow.

## Repository Map

- `gems/vajra`: canonical gem, executable, signatures, and native extension
  sources
- `docs/`: product documentation site built with Jekyll and Just the Docs
- `.github/`: issue templates, workflows, release drafting, and repository
  instructions
- `scripts/`: root-level convenience commands for CI and local verification

## Local Development

Install dependencies and run the shared repository validation flow from the
repository root:

```bash
scripts/ci-install-bundles
scripts/run-all
```

For focused gem work:

```bash
cd gems/vajra
bin/setup
bin/rspec-unit
bin/rspec-e2e
bin/rubocop
bin/reek
bundle exec exe/vajra
```

`bin/rspec-unit` is the covered unit lane. `bin/rspec-e2e` is the integration
lane and runs without coverage.

## Documentation

The docs site under `docs/` is the authoritative product documentation surface
for installation, runtime behavior, operations, and troubleshooting.

Start with:

- [`docs/index.md`](docs/index.md)
- [`docs/pages/getting-started.md`](docs/pages/getting-started.md)
- [`docs/pages/installation.md`](docs/pages/installation.md)
- [`docs/pages/runtime-model.md`](docs/pages/runtime-model.md)
- [`docs/pages/architecture.md`](docs/pages/architecture.md)
- [`docs/pages/troubleshooting.md`](docs/pages/troubleshooting.md)
- [`docs/pages/development.md`](docs/pages/development.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, workflow, documentation, and
release guidance.

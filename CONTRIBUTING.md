# Contributing to Vajra

Thank you for contributing to Vajra. This repository contains one canonical
gem under `gems/vajra`, a product documentation site under `docs/`, and the
automation and governance files used to validate and release the project.

## Repository Layout

| Path          | Purpose                                                |
| ------------- | ------------------------------------------------------ |
| `gems/vajra/` | Canonical gem, executable, signatures, and native code |
| `docs/`       | Product documentation site                             |
| `.github/`    | Workflows, issue templates, release drafting, guidance |
| `danger/`     | Pull request automation                                |
| `scripts/`    | Root-level verification helpers                        |

## Development Baseline

Use Ruby `3.2+` and run package-local commands from `gems/vajra` unless a root
script explicitly says otherwise.

```bash
cd gems/vajra
bundle install
bin/rspec
bin/rspec-unit
bin/rspec-e2e
bin/rubocop
bin/reek
bundle exec rbs -I sig validate
bundle exec exe/vajra
```

The native extension source of truth lives under `gems/vajra/ext/vajra/`.
When Ruby files are split by responsibility, mirror that split in direct specs
under `gems/vajra/spec/` where the file owns behavior.
Unit tests are unit tests with coverage. `bin/rspec-e2e` is the integration
lane with `NO_COVERAGE=1`.

## Docs Development

The docs site is built with Jekyll and Just the Docs.

```bash
cd docs
bundle install
bundle exec jekyll serve
```

Update docs whenever commands, paths, runtime behavior, or support boundaries
change.
The intended public docs host is `vajra.codevedas.com`.

## Pull Requests

Before opening a PR:

1. Create a branch using a meaningful prefix such as `feat/`, `bugfix/`,
   `docs/`, `chore/`, or `ci/`.
2. Run the relevant local checks.
3. Update docs when behavior or usage changes.
4. Complete the PR template with enough detail for reviewers.

The root verification flow is:

```bash
scripts/ci-install-bundles
scripts/run-all
```

That flow covers unit tests with coverage, e2e integration tests without
coverage, clean native rebuild verification, package build validation, and docs
build validation.

## Security

To report a security issue, follow [SECURITY.md](SECURITY.md).

## Release Process

1. Create `release/<version>` from `main`.
2. Update `gems/vajra/lib/vajra/version.rb` and `gems/vajra/vajra.gemspec`.
3. Update `CHANGELOG.md`, root docs, and package docs for the release.
4. Run `scripts/run-all`.
5. Open a PR to `main` and label it appropriately.
6. After merge, create a GitHub Release with tag `v<version>`.
7. Docs are published through `.github/workflows/jekyll-gh-pages.yml` to
   `vajra.codevedas.com`.
8. The release workflow publishes the gem from `gems/vajra`.

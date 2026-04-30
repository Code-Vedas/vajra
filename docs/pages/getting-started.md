---
title: Getting Started
nav_order: 2
permalink: /getting-started/
---

# Getting Started

Use this path when evaluating Vajra, setting up the repository for development,
or validating that the canonical gem and docs site are both healthy.

Vajra development starts from one canonical gem under `gems/vajra`. The
repository-level scripts exist to keep the gem, docs, and automation in sync.

## Repository Setup

```bash
git clone git@github.com:Code-Vedas/vajra.git
cd vajra
scripts/ci-install-bundles
```

The install script checks the Bundler surfaces needed by the gem and the docs
site.

## First Validation Pass

```bash
scripts/run-all
```

That flow installs bundle dependencies for the gem and docs site, then runs the
shared repository checks.

## Recommended Reading Order

1. Read [Architecture](/architecture/) for the repository map and ownership
   split between Ruby and C++.
2. Read [Installation](/installation/) for the native build contract and
   artifact layout.
3. Read [Running Vajra](/running-vajra/) for the executable startup path and
   listener expectations.
4. Read [Runtime Model](/runtime-model/) for the boot and request lifecycle.
5. Keep [Troubleshooting](/troubleshooting/) open while doing the first native
   build or runtime boot.

## Focused Gem Workflow

For package-local work:

```bash
cd gems/vajra
bin/setup
bin/rspec-unit
bin/rspec-e2e
bin/rubocop
bin/reek
bundle exec rbs -I sig validate
```

Use package-local commands when changing gem code, extension code, signatures,
or smoke tests.

## Docs Workflow

For documentation work:

```bash
cd docs
bundle exec jekyll serve
```

Use the docs site as the authoritative product-documentation surface. Update it
when package paths, commands, behavior, or ownership boundaries change.

## First Runtime Check

After the package setup succeeds:

```bash
cd gems/vajra
bundle exec exe/vajra
```

Vajra binds to port `3000`, prints a startup banner, and responds to a basic
HTTP request. That runtime path is covered by the committed e2e suite.

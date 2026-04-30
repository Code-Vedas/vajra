---
title: Troubleshooting
nav_order: 12
permalink: /troubleshooting/
---

# Troubleshooting

Use this page to triage package setup, extension build drift, executable boot
failures, docs-site issues, and small repository-shape regressions.

The fastest way to use this page is to identify the symptom first, then jump to
the boundary that owns it: package setup, native build, executable boot, docs
build, or repository validation.

## Common Starting Points

- if `require "vajra"` fails, start with the native extension build path
- if the executable exits before binding the port, start with package setup and
  local runtime boot
- if docs changes do not render, start with the Jekyll workflow under `docs/`
- if repository checks drift, start with `scripts/run-all`

## Native Extension Does Not Load

Run:

```bash
cd gems/vajra
bundle exec rake compile
```

The package raises an explicit load error when the compiled extension cannot be
found or loaded through the canonical path.

## Port `3000` Is Already In Use

If local startup fails because the listener cannot bind:

- stop the conflicting local process
- retry `bundle exec exe/vajra`
- confirm the process reaches the startup banner before sending a request

## Executable Exits Before Serving Requests

Use the package-local validation flow:

```bash
cd gems/vajra
bin/setup
bin/rspec-unit
```

That path validates both the extension load boundary and the committed startup
smoke behavior.

## Clean Checkout Fails

Run:

```bash
scripts/ci-install-bundles
scripts/run-all
```

That flow validates both the gem and the docs site from the repository root.

## Docs Site Does Not Build

Run:

```bash
cd docs
bundle exec jekyll build
```

If the docs build fails, fix the issue in `docs/` before treating the change as
complete. Repository and package docs agree on canonical paths, commands, and
product naming.

## Small Shape Drift

If a change leaves stale names, wrong paths, or missing support files:

- compare the touched folder against the repository conventions
- keep `gems/vajra` as the only gem package
- keep native sources under `gems/vajra/ext/vajra`
- remove stale copied names rather than documenting around them

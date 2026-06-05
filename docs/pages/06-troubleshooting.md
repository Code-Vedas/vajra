---
title: Troubleshooting
nav_order: 6
permalink: /troubleshooting/
---

# Troubleshooting

Use this page to triage package setup, extension build drift, executable boot
failures, docs-site problems, and small repository-shape regressions.

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

## Application Does Not Boot

If `bundle exec vajra` fails in an app root:

- check `config/vajra.rb` first
- if no Vajra config file is present, check `config.ru`
- for Rails, confirm `config/environment.rb` loads and defines
  `Rails.application`
- for Rack-first frameworks, confirm `config.ru` returns a valid Rack app

If `bin/rails server` says:

```text
Could not find a server gem. Maybe you need to add one to the Gemfile?
```

confirm that:

- `gem "vajra"` is in the application bundle
- Vajra's Railtie has been loaded through Bundler
- the app is not forcing another Rack server through conflicting environment
  variables or server flags

Rails applications using Vajra do not need `puma` just to satisfy the server
command.

## Lifecycle Log Looks Like The Master Is Serving Requests

If the logs show a serving transition from the runtime process, read the
ownership fields carefully:

- `process_role` identifies which process emitted the event
- `request_execution_role` identifies which role executes application requests

This line is normal in a master-worker runtime:

```text
event=serving_entered ... process_role=native_runtime_control request_execution_role=ruby_worker_bootstrap
```

It means:

- the native runtime owns the listener and entered serving state
- the Ruby worker still owns application request execution

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

If the docs build fails, fix the problem in `docs/` before treating the change as
complete. Repository and package docs agree on canonical paths, commands, and
product naming.

## Small Shape Drift

If a change leaves stale names, wrong paths, or missing support files:

- compare the touched folder against the repository conventions
- keep `gems/vajra` as the only gem package
- keep native sources under `gems/vajra/ext/vajra`
- remove stale copied names rather than documenting around them

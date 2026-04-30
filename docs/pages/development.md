---
title: Development
nav_order: 14
permalink: /development/
---

# Development

This repository follows one canonical package, package-local validation,
central docs, and repository automation that verifies the whole shape together.

## Repository Workflow

From the repository root:

```bash
scripts/ci-install-bundles
scripts/run-all
```

Those scripts coordinate gem validation and docs validation from one shared
entrypoint.

## Package-Local Commands

```bash
cd gems/vajra
bin/rspec
bin/rspec-unit
bin/rspec-e2e
bin/rubocop
bin/reek
bundle exec rbs -I sig validate
bundle exec rake build
```

`bin/rspec` is the general package runner. `bin/rspec-unit` is the fast default
lane for committed non-integration specs. `bin/rspec-e2e` is the integration
lane and runs without coverage.

## Native Extension Work

When changing the extension or load path, use:

```bash
cd gems/vajra
bundle exec rake clobber compile
bin/rspec-unit
bin/rspec-e2e
```

Keep the extension sources, load path, executable boot path, and smoke coverage
aligned in the same change.

## Root-Level Commands

```bash
scripts/ci-install-bundles
scripts/run-all
```

The docs site is a first-class product surface:

- `docs/` is the source of truth for product documentation
- package READMEs stay concise and point back to `docs/`
- update docs whenever commands, paths, ownership boundaries, or runtime
  behavior change

## Docs

```bash
cd docs
bundle exec jekyll serve
```

For a clean preview of the published site shape:

```bash
cd docs
bundle exec jekyll build
```

The supported public publication target is `https://vajra.codevedas.com`.

## Docs Publishing

Docs publishing uses the GitHub Pages workflow in
`.github/workflows/jekyll-gh-pages.yml`.

- shared CI validates the docs build before merge
- GitHub Pages deploys the built site artifact
- `docs/CNAME` records the supported hostname
- contributors update navigation and cross-links when adding pages

## Release-Supporting Maintenance

Repository hygiene work belongs in development too:

- keep file and folder names free of stale copied project names
- keep license headers aligned with repository conventions
- keep workflows, scripts, and docs pointed at `gems/vajra`
- keep package contents explicit so release artifacts stay lean

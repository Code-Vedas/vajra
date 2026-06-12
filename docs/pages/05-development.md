---
title: Development
nav_order: 6
permalink: /development/
---

# Development

Vajra development uses one canonical package, package-local checks, central
docs, and repository automation.

## Repository Workflow

From the repository root:

```bash
scripts/ci-install-bundles
scripts/run-all
```

Those scripts run the gem and docs checks from one shared entrypoint.

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

`bin/rspec` is the general package runner. `bin/rspec-unit` runs unit specs with
coverage. `bin/rspec-e2e` runs integration specs without coverage.

## Native Extension Work

When changing the extension or load path, use:

```bash
cd gems/vajra
bundle exec rake clobber compile
bin/rspec-unit
bin/rspec-e2e
```

Keep the extension sources, load path, executable boot path, and smoke coverage
aligned.

## Performance Test

Run the local performance comparison from the Vajra package:

```bash
cd gems/vajra/performance
bundle exec rake performance:run
```

The runner starts Vajra and peer Rack servers against generated Rack, Rails,
Roda, Sinatra, and Hanami fixtures. It reports throughput, p95/p99 latency,
error rate, requests per CPU core, and process-group RSS min, max, and final
memory.

## Root-Level Commands

```bash
scripts/ci-install-bundles
scripts/run-all
```

Docs are part of the product surface:

- `docs/` contains the product documentation
- package READMEs stay concise and point back to `docs/`
- update docs whenever commands, paths, ownership boundaries, or runtime
  behavior change

## Docs

```bash
cd docs
bundle exec jekyll serve
```

For a clean preview of the published site:

```bash
cd docs
bundle exec jekyll build
```

The supported public publication target is `https://vajra.codevedas.com`.

## Docs Publishing

Docs publishing uses the GitHub Pages workflow in
`.github/workflows/jekyll-gh-pages.yml`.

- shared CI runs the docs build
- GitHub Pages deploys the built site artifact
- `docs/CNAME` records the supported hostname
- contributors update navigation and cross-links when adding pages

## Release-Supporting Maintenance

Repository hygiene work belongs in development too:

- keep file and folder names free of stale copied project names
- keep license headers aligned with repository conventions
- keep workflows, scripts, and docs pointed at `gems/vajra`
- keep package contents explicit so release artifacts stay lean

---
title: Development
nav_order: 17
permalink: /development/
---

# Development

Vajra development uses one canonical package, package-local checks, central docs, and repository automation.

## Prerequisites

- Linux or macOS
- Ruby 3.2 or newer
- Bundler
- a C++ compiler and Ruby development headers for native extension work
- Go with `h2spec` on `PATH` for external HTTP/2 conformance checks
- `k6` for performance profiles outside the Linux test image
- Docker for running repository checks inside the Linux test image

Install h2spec with Go:

```bash
go install github.com/summerwind/h2spec/cmd/h2spec@latest
export PATH="$HOME/go/bin:$PATH"
```

## Repository Workflow

From the repository root:

```bash
scripts/ci-install-bundles
scripts/run-all
```

Those scripts run the gem and docs checks from one shared entrypoint.
`scripts/run-all` includes external HTTP/2 conformance checks through h2spec.

For Linux validation from another host platform, set `DOCKER=1` on any root script:

```bash
DOCKER=1 scripts/run-all
DOCKER=1 scripts/run-rspec-e2e-all
DOCKER=1 scripts/run-h2spec-all
```

The Docker path uses `Dockerfile.test`, mounts the repository at `/workspace`, and keeps container bundle installs under `tmp/docker`.

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

`bin/rspec` is the general package runner. `bin/rspec-unit` runs unit specs with coverage. `bin/rspec-e2e` runs integration specs without coverage.

## Native Extension Work

When changing the extension or load path, use:

```bash
cd gems/vajra
bundle exec rake clobber compile
bin/rspec-unit
bin/rspec-e2e
```

Keep the extension sources, load path, executable boot path, and smoke coverage aligned.

## Performance Test

Run the local performance comparison from the Vajra package:

```bash
cd gems/vajra/performance
bundle exec rake performance:run
```

The profile-specific root commands are:

```bash
scripts/run-performance-main
scripts/run-performance-observability
scripts/run-performance-protocol
```

Use `DOCKER=1` with those scripts for Linux validation from another host platform.

The runner starts Vajra and peer Rack servers against generated Rack, Rails, Roda, Sinatra, and Hanami fixtures. It reports throughput, p95/p99 latency, error rate, requests per CPU core, and process-group RSS min, max, and final memory.

See [Performance](/performance/) for profile names, workload shape, artifacts, and result interpretation.

## Root-Level Commands

```bash
scripts/ci-install-bundles
scripts/run-all
scripts/run-h2spec-all
```

`scripts/run-h2spec-all` starts a temporary h2c-enabled Vajra server and runs the full h2spec suite against it.

`scripts/run-performance-protocol` runs the protocol benchmark lanes. It uses k6
for ordinary HTTP/1 and TLS HTTP/2 lanes, and a custom protocol driver for h2c,
h2c upgrade, upload, Rack hijack, tunnels, and same-connection HTTP/2 concurrent
stream coverage.

Docs are part of the product surface:

- `docs/` contains the product documentation
- package READMEs stay concise and point back to `docs/`
- revise docs when commands, paths, ownership boundaries, or runtime behavior change

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

Docs publishing uses the GitHub Pages workflow in `.github/workflows/jekyll-gh-pages.yml`.

- shared CI runs the docs build
- GitHub Pages deploys the built site artifact
- `docs/CNAME` records the supported hostname
- contributors maintain navigation and cross-links when adding pages

## Release-Supporting Maintenance

Repository hygiene work belongs in development too:

- keep file and folder names free of stale copied project names
- keep license headers aligned with repository conventions
- keep workflows, scripts, and docs pointed at `gems/vajra`
- keep package contents explicit so release artifacts stay lean

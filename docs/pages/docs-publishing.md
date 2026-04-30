---
title: Docs Publishing
nav_order: 15
permalink: /docs-publishing/
---

# Docs Publishing

This page defines the supported path for previewing, validating, and publishing
the docs site.

## Public Hostname

The intended and supported public docs hostname is:

```text
vajra.codevedas.com
```

That hostname is recorded in `docs/CNAME` and paired with the GitHub Pages
deployment workflow.

## Site Structure

Treat these paths as the docs platform shape:

- `docs/index.md` for the docs entrypoint
- `docs/pages/` for product and engineering pages
- `docs/_includes/` for small layout-level customization hooks
- `docs/_config.yml` for readable site configuration
- `docs/CNAME` for the public hostname contract

## Local Preview

```bash
cd docs
bundle install
bundle exec jekyll serve
```

Use local preview when changing navigation, layout, cross-links, or page
structure.

## Local Validation

```bash
cd docs
bundle exec jekyll build
```

This is the local build check and passes before merge.

## CI Validation

The shared CI workflow validates the docs build through
`scripts/run-docs-build-all`.
If that job fails, fix the site before treating the change as ready.

## Publishing Flow

The supported GitHub-hosted publication path is:

1. merge docs changes to the main line
2. let shared CI validate the docs build
3. let `.github/workflows/jekyll-gh-pages.yml` publish the site from pushes to `main`
4. publish to GitHub Pages with `vajra.codevedas.com` as the served hostname

Manual local publication is outside the supported path.

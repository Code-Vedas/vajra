---
applyTo: "gems/vajra/lib/**/*.rb,gems/vajra/spec/**/*.rb,gems/vajra/exe/**/*,gems/vajra/bin/**/*"
---

# Vajra Package Instructions

- `gems/vajra` is the canonical runtime package for this repository.
- Keep native-extension ownership explicit: Ruby entrypoints in `lib/`, native
  sources in `ext/vajra/`, signatures in `sig/`, and mirrored direct specs in
  `spec/`.
- Prefer responsibility-named files over generic helpers or utils.
- Keep package-local commands authoritative. Run validation from `gems/vajra`,
  not from the repo root with ad hoc paths.
- Use actionable error messages when boot, build, or native-load behavior
  fails.
- Update package docs when commands, layout, or supported development flows
  change.

---
applyTo: "gems/vajra/sig/**/*.rbs"
---

# Vajra RBS Instructions

- RBS must stay true to the Ruby implementation it mirrors.
- Keep signatures aligned with ownership, visibility, argument names,
  optionality, and return types.
- Remove stale signatures when methods or modules move or disappear.
- When Ruby changes under `gems/vajra/lib`, check the mirrored path under
  `gems/vajra/sig` in the same change.

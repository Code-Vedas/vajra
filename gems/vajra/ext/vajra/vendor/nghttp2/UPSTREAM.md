# Vendored nghttp2

Vajra vendors nghttp2 release `v1.69.0` from
<https://github.com/nghttp2/nghttp2/releases/tag/v1.69.0> under the upstream
MIT license recorded in this directory.

The scheduled `nghttp2 version` workflow runs `scripts/check-nghttp2-version` and fails when this source tree no longer matches the latest upstream release. When updating the vendored source, update the version and release link in this file in the same change, then run:

```bash
new_version=v1.70.0
scripts/check-nghttp2-version "${new_version}"
scripts/run-ctest-all
scripts/run-h2spec-all
```

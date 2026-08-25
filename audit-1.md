## Vajra repository assessment

I reviewed the current `Code-Vedas/vajra` repository referenced here , including the C/C++ runtime, Ruby bridge, HTTP/1 and HTTP/2 request paths, buffering/backpressure, TLS integration, runtime queues, four Bundler dependency roots, CI/release workflows, vendored nghttp2, and the existing performance harness. I also cross-checked current Ruby, Rails, nghttp2, GitHub Advisory/RubySec/NVD material as of **August 24, 2026**.

The architecture is generally strong: native I/O is kept outside the Ruby GVL, request limits exist, HTTP framing validation is defensive, worker admission is bounded, HTTP/2 uses nghttp2 rather than a homegrown framing implementation, and the repository already has a substantial k6/h2spec performance harness.

But I found **one significant source-level security bug, several high-value RPS/memory opportunities, and multiple dependency/security-baseline issues that should be addressed now.**

### Priority findings

| Priority  | Finding                                                  | Assessment                                                                | Recommended action                                         |
| --------- | -------------------------------------------------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------- |
| **P0**    | **Unbounded attacker-controlled Ruby header-key cache**  | **Confirmed memory-exhaustion DoS**                                       | Remove caching of arbitrary `HTTP_*` keys                  |
| **P0**    | Ruby **3.4.3** baseline                                  | Security baseline contains subsequently fixed vulnerabilities             | Move repository, Docker and release baseline to **3.4.10** |
| **P0**    | Active Storage **8.1.3**                                 | **CVE-2026-66066 Critical**; test/compat dependency, not shipped by Vajra | Rails **8.1.3.1+**                                         |
| **P0/P1** | `json 2.20.0`                                            | **CVE-2026-71847** affected; no Vajra reachability found                  | `json >= 2.21.2`                                           |
| **P1**    | Rack responses fully materialized before send            | Major large-response RPS/RSS/disk penalty                                 | Implement bounded streaming response bridge                |
| **P1**    | `NativeInput` double copies request body                 | Allocation + memcpy + Ruby GC overhead                                    | Direct-fill Ruby buffers / reusable spill scratch          |
| **P1**    | HTTP/2 execution cap effectively not wired               | Aggregate queue/resource amplification                                    | Add explicit bounded H2 pending-execution admission        |
| **P1**    | HTTP/2 per-stream buffers lack a connection-wide budget  | Memory amplification under multiplexing                                   | Add connection memory budget and pressure propagation      |
| **P1**    | No ASan/UBSan/fuzz lane                                  | Important network-parser coverage gap                                     | Add sanitizer CI + libFuzzer corpus                        |
| **P2**    | Vendored nghttp2 **1.69.0** stale                        | Latest is **1.70.0**                                                      | Upgrade vendored source                                    |
| **P2**    | `bundler-audit` installed but not run by CI              | Dependency-CVE gate missing                                               | Audit all four lockfiles                                   |
| **P2**    | GitHub Actions/tool images use mutable major/latest tags | Supply-chain reproducibility gap                                          | Pin commit SHAs/digests                                    |

---

# 1. Confirmed memory DoS in Vajra itself

This is the most important code finding.

The Ruby execution bridge has a process-global:

```cpp
std::shared_mutex header_cache_mutex;
std::unordered_map<std::string, VALUE> header_key_cache;
```

For a cache miss, Vajra constructs the Rack `HTTP_*` key, freezes the Ruby string and calls:

```cpp
rb_gc_register_mark_object(key);
```

then stores it permanently in `header_key_cache`.

The lookup/insert path accepts header-derived names rather than only a fixed vocabulary.

That means an unauthenticated client can continually send unique valid header names:

```text
X-A-000001
X-A-000002
X-A-000003
...
```

Every previously unseen name produces:

1. a native `std::string`,
2. an `unordered_map` entry,
3. a Ruby `String`,
4. a permanently registered GC root.

I found no corresponding eviction, clear or GC unregister path. The only use of `rb_gc_register_mark_object` in the repository is this mechanism.

So RSS grows with attacker-generated header diversity for the **lifetime of the worker**.

### Correct fix

Do **not** replace this with an LRU of permanently registered Ruby objects.

Cache only the small fixed vocabulary where caching makes sense:

* `CONTENT_LENGTH`
* `CONTENT_TYPE`
* common HTTP headers if profiling proves worthwhile
* Vajra's fixed Rack keys

For arbitrary headers, construct the Ruby string for that request and let the Rack env own its lifetime. Critically, **do not intern/symbolize attacker-controlled header names**.

This also improves RPS because unique headers currently force shared-lock lookup followed by exclusive-lock insertion on a global cache.

I would treat this as a **High severity availability vulnerability** and fix it before the next release.

---

# 2. Biggest RPS + RSS improvement: stop materializing Rack responses

The current response bridge is not truly streaming.

Vajra first iterates the Ruby Rack body and copies each Ruby string into native `std::string` storage. It keeps up to **256 KiB** in memory; once that threshold is exceeded it opens a temporary file, copies the already-collected chunks into it, and writes subsequent body data there.

Only **after the Rack body has been consumed** does `ResponseWriter` stream the stored chunks/file to the network.

That creates several penalties:

| Workload                        | Current penalty                                                |
| ------------------------------- | -------------------------------------------------------------- |
| Small responses                 | Ruby → native copy                                             |
| Medium responses                | multiple allocations and retained native strings               |
| Large responses                 | Ruby → native → temp file → native read → socket               |
| Slow client                     | response generated/spooled before natural network backpressure |
| Streaming/SSE                   | fundamentally poor fit because body is materialized            |
| Many concurrent large responses | `/tmp` I/O and disk-space amplification                        |

### Better architecture

Use a **bounded producer/consumer response channel**:

```text
Ruby Rack body.each
       │
       ▼
small bounded native queue
       │
       │ backpressure
       ▼
native socket/TLS writer
```

Ruby produces perhaps 32–128 KiB ahead of the writer. When capacity is reached, the Ruby execution waits while relinquishing the GVL appropriately.

That gives you:

* real backpressure,
* no arbitrary response spooling,
* much lower RSS,
* faster time-to-first-byte,
* substantially better streaming/SSE behavior,
* fewer memory copies,
* lower `/tmp` activity,
* improved high-concurrency RPS.

For plaintext file responses, a later optimization can add `sendfile`; for normal plaintext chunks, `writev` can combine response head + initial body chunks.

This is probably the **largest structural performance improvement available** for large-body workloads.

---

# 3. Request-body path does unnecessary copying

`Vajra::NativeInput` already has sensible bounded buffering: approximately 1 MiB memory/high watermark with spill-to-file behavior and backpressure.

But the consumer path is expensive.

`pull_to_string_locked()`:

1. allocates a new `std::string`,
2. copies/freads bytes into it,
3. returns it,
4. `binary_string_from()` allocates a Ruby `String`,
5. copies those bytes again.

For `read(nil)`, Ruby strings can subsequently be concatenated again.

So an application doing:

```ruby
request.body.read
```

or JSON/form processing pays avoidable traffic-sized memory copies.

### Optimization

Split the implementation:

**In-memory chunks:** copy directly into the final Ruby string/outbuf.

```text
deque chunk → Ruby String
```

instead of:

```text
deque chunk → std::string → Ruby String
```

**Spilled input:** use a reusable native scratch buffer or carefully managed final Ruby buffer rather than allocating a new `std::string` for every read.

Also aggressively reuse the Rack `read(length, outbuf)` buffer capacity.

This should improve:

* upload throughput,
* JSON/form POST RPS,
* allocation rate,
* Ruby GC frequency,
* peak temporary memory.

---

# 4. HTTP/2 has several worthwhile optimizations

`Http2StreamState` maintains separate inbound/outbound `deque<std::string>` structures, with 1 MiB high-watermarks and 512 KiB low-watermarks.

The Ruby-facing implementation repeats some of the `NativeInput` problems:

* partial stream reads copy into a fresh `std::string`;
* then copy to a Ruby string;
* partial consumption uses `front.erase(0, n)`, which moves the remainder of the string;
* writes first copy the Ruby argument to a native string and then copy slices into outbound chunks.

Replace mutable front-erasing strings with something equivalent to:

```cpp
struct BufferSlice {
    std::shared_ptr<Buffer> buffer;
    size_t offset;
    size_t length;
};
```

or at least `{string, offset}`.

Then consuming N bytes becomes:

```cpp
offset += N;
```

rather than a memmove.

### More important: aggregate memory control

Per-stream high watermarks are not enough for HTTP/2. Multiplexing means resource admission needs to operate at:

```text
stream
connection
worker
```

levels.

Add a connection-wide budget covering:

* request body buffers,
* paused DATA,
* tunnel inbound buffers,
* tunnel outbound buffers,
* unsent response data,
* pending execution tasks,
* HPACK/nghttp2 state.

When the budget is reached, stop granting flow-control credit and/or refuse streams rather than letting every stream independently approach its limit.

---

# 5. HTTP/2 execution admission needs a direct bound

`Http2Config` supports `max_pending_executions`, and the HTTP/2 session contains saturation logic. But in the normal `RequestProcessor` construction path I found the request head/body/keepalive values being copied into `http2_config_` while no normal runtime configuration value wires a nonzero pending-execution cap.

The saturation test therefore has no direct queue bound in the ordinary configuration path.

This is **not infinite memory from one request**: stream and connection limits provide indirect bounds. But it allows total pending HTTP/2 work across multiplexed connections to become much larger than the Ruby execution pool can service.

Expose something like:

```text
http2_max_pending_executions
```

with a default related to:

```text
execution_threads × small multiplier
```

rather than connection count.

Then expose:

* queue depth,
* queue wait,
* admission rejections,
* execution saturation

in runtime metrics.

---

# 6. Additional hot-path work

These are secondary, but worth doing once the previous four are addressed.

| Area                      | Opportunity                                                                                                      |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| Request head              | Keep offsets/string_views into the original head longer rather than producing individual C++ strings immediately |
| Rack env                  | Reduce repeated header string normalization/copying                                                              |
| Chunked input             | Replace prefix `erase()`/compaction with cursor/ring-buffer semantics                                            |
| HTTP/2 receive pending    | Avoid vector prefix erase/memmove                                                                                |
| HTTP/2 send               | Reuse the session's ~32 KiB serialization scratch buffer rather than allocating/reserving repeatedly             |
| HTTP/2 paused bodies      | Maintain a ready/blocked stream queue instead of repeatedly scanning all streams                                 |
| New connections           | Batch POSIX `SCM_RIGHTS` handoffs if profiling shows master dispatch dominating                                  |
| Plain file bodies         | `sendfile()` fast path                                                                                           |
| Plain multi-buffer output | `writev()`/`sendmsg()`                                                                                           |
| Allocator                 | Benchmark jemalloc/mimalloc **after** removing lifetime/copy problems                                            |

Vajra already does some useful glibc allocator tuning, including limiting arenas, so I would **not** start by swapping allocators.

---

# 7. Current CVE/dependency audit

## Action required

| Component      |     Repository version | Finding                                                                                                          | Reachability/status                                                            |
| -------------- | ---------------------: | ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| Ruby baseline  |              **3.4.3** | Contains default-gem versions subsequently affected by security advisories; most importantly zlib CVE-2026-27820 | Repository/test/release baseline affected                                      |
| zlib           | Ruby 3.4 era **3.2.1** | **CVE-2026-27820**, memory corruption/buffer overflow; affected ≤3.2.2                                           | No direct Vajra `Zlib::GzipReader` path found, but vulnerable runtime baseline |
| `json`         |             **2.20.0** | **CVE-2026-71847**, heap UAF/process crash                                                                       | Vulnerable dependency present; no `ResumableParser#partial_value` use found    |
| Active Storage |              **8.1.3** | **CVE-2026-66066**, Critical arbitrary file read / possible RCE                                                  | Compatibility/test/performance dependency, **not Vajra runtime**               |
| nghttp2        |             **1.69.0** | Not current; upstream **1.70.0**                                                                                 | Upgrade for current fixes/hardening                                            |

The repository pins Ruby 3.4.3 in `.ruby-version`, its Docker test image and release workflow.

Ruby's official advisory says CVE-2026-27820 affects zlib ≤3.2.2 and recommends 3.2.3+. Ruby 3.4.9 incorporated the fix, and the current maintained 3.4 release is **3.4.10**. ([Ruby][1])

I would therefore change all exact 3.4.3 pins to **3.4.10** rather than individually layering security-gem overrides.

Ruby 3.4-era default gems also included versions later affected by `resolv`, URI and REXML advisories. Ruby documents the `resolv` DoS as affecting Ruby 3.4's `resolv <=0.6.1`; URI 1.0.0–1.0.3 is affected by CVE-2025-61594; and REXML 3.3.3–3.4.1 is affected by CVE-2025-58767. ([Ruby][2]) Main Bundler resolution already overrides several of these with patched versions, but moving the base runtime remains the cleaner answer.

### JSON

The main/performance dependency set pins `json 2.20.0`.

CVE-2026-71847 affects json **2.20.0 through 2.21.1**. It is a native heap use-after-free that can terminate the process when `JSON::ResumableParser#partial_value` is used under a particular streaming-input condition. The patched version is 2.21.2+. ([GitHub][3])

I searched Vajra and found no `ResumableParser`/`partial_value` invocation, so I would call this:

**vulnerable dependency present, currently no confirmed Vajra reachability**.

Still upgrade it.

### Active Storage

Rails/Active Storage 8.1.3 is below the fixed **8.1.3.1** release. The July 29 advisory is Critical and describes unauthenticated arbitrary file reading with potential escalation to RCE in affected image-variant configurations. ([Ruby on Rails][4])

Important qualification: Rails is in Vajra's compatibility/test matrix; `vajra.gemspec` itself declares no Rails runtime dependency.

So this is a **repository development/test dependency vulnerability**, not a vulnerability automatically inherited by users installing the Vajra gem.

---

# 8. nghttp2 status

Vajra vendors **nghttp2 1.69.0** and statically compiles the library sources into the extension.

Two relevant current CVEs need distinction:

**CVE-2026-27135** affected nghttp2 through 1.68.0 and was fixed in 1.68.1. Vajra's 1.69.0 is **not affected**. ([NVD][5])

**CVE-2026-58055** technically lists nghttp2 through 1.69.0, but the vulnerability is specifically in the **nghttpx proxy's** HTTP/1 Upgrade/backend forwarding behavior. Vajra vendors the reusable `libnghttp2` sources, not the nghttpx proxy, so I classify this one as **not applicable to Vajra**. ([NVD][6])

However, upstream released **nghttp2 1.70.0 on July 29, 2026**, so the vendored 1.69.0 copy is currently stale. ([GitHub][7])

The repository already has a good weekly workflow designed to catch exactly this condition.

Upgrade to 1.70.0.

---

# 9. Dependencies that are already on patched versions

The main lockfile is actually quite current in several important places.

Recent vulnerability families I checked are already covered by the pinned versions, including:

* Rack **3.2.6**
* Puma **8.0.2**
* websocket-driver **0.8.2**
* Nokogiri **1.19.4**
* rails-html-sanitizer **1.7.1**
* Loofah **2.25.2**
* concurrent-ruby **1.3.7**
* REXML **3.4.4**
* URI **1.1.1**
* ERB **6.0.4**

So I would **not** mass-upgrade blindly merely because the graph is large.

The main outstanding exact-version issues I identified are Ruby 3.4.3, Active Storage/Rails 8.1.3, json 2.20.0, and the stale vendored nghttp2.

---

# 10. OpenSSL is an unresolved part of a “full CVE audit”

Vajra's extension discovers and links the OpenSSL available on the build system:

```text
pkg-config openssl
or
-lssl -lcrypto
```

rather than pinning an OpenSSL source version.

Therefore the repository itself cannot tell us whether a deployed Vajra binary is vulnerable to an OpenSSL CVE.

For releases, record an SBOM/provenance record containing at minimum:

```text
Ruby version
OpenSSL gem version
OpenSSL library version
libssl OS package version
nghttp2 version/hash
compiler version
target OS/libc
```

The same applies to Debian security backports: an apparently older upstream OpenSSL version can still contain the distro's security patches, so actual package provenance matters.

---

# 11. Security CI gaps

You already have unusually good basic coverage:

* CodeQL for Ruby and C/C++ with a real native build.
* Linux/macOS/Windows test matrices.
* Ruby 3.2, 3.4 and 4.0.
* h2spec.
* C++ tests.
* RuboCop/Reek/RBS.
* daily Dependabot on all four Bundler roots and Actions.

But three layers are missing.

First, `bundler-audit` is in the Gemfile but I found no CI invocation. Run it separately against:

```text
gems/vajra/Gemfile.lock
gems/vajra/performance/Gemfile.lock
docs/Gemfile.lock
danger/Gemfile.lock
```

Second, I found **no ASan/UBSan CI configuration and no fuzzing harness**. For this project, that is important because attacker-controlled network bytes cross native parsers, state machines, HPACK/nghttp2 callbacks and Ruby native-extension ownership.

The initial fuzz targets should be:

```text
HTTP/1 request line + header parser
Content-Length / Transfer-Encoding parser
chunked-body parser
h2c upgrade parser
HTTP/2 frame prechecker
Rack response-header validator
traceparent parser
```

Third, pin release/CI Actions and downloaded tools to immutable SHAs/digests. The RubyGems credential action is already SHA-pinned, which is good, but most other actions use mutable major tags.

---

# 12. Security posture that is already good

I specifically looked for common request-smuggling primitives.

The HTTP/1 path has strong defensive behavior around:

* duplicate `Content-Length`,
* invalid numeric Content-Length,
* simultaneous `Transfer-Encoding` and Content-Length,
* unsupported transfer codings,
* malformed request heads,
* Host handling,
* header/head size bounds,
* request body size bounds,
* slow request timeouts.

That is a good base. I did **not** find an obvious CL.TE / TE.CL request-smuggling primitive in the inspected parser path.

The security documentation also correctly calls out the fact that stats/metrics endpoints are not authentication endpoints and must be protected externally, and explicitly states that Vajra does not provide WAF/rate-limit/application-auth policy.

---

# Recommended implementation order

| Order  | Change                                       | Primary benefit                        |
| ------ | -------------------------------------------- | -------------------------------------- |
| **1**  | Remove arbitrary dynamic header-key cache    | Fix confirmed memory DoS               |
| **2**  | Ruby 3.4.3 → **3.4.10** everywhere           | Runtime CVE baseline                   |
| **3**  | Rails 8.1.3 → **8.1.3.1+**                   | Critical repository dependency CVE     |
| **4**  | json 2.20.0 → **2.21.2+**                    | Native UAF dependency                  |
| **5**  | nghttp2 1.69.0 → **1.70.0**                  | Vendored dependency currency           |
| **6**  | Add `bundler-audit` + ASan/UBSan + fuzzing   | Prevent recurrence                     |
| **7**  | Implement true Rack response streaming       | Largest large-body RPS/RSS improvement |
| **8**  | Eliminate NativeInput double copy            | POST/upload RPS + GC                   |
| **9**  | Add H2 execution + connection-memory budgets | H2 resilience/RSS                      |
| **10** | Replace buffer-prefix erases with cursors    | H2/chunked RPS                         |
| **11** | Optimize env/header allocations              | Small-request RPS                      |
| **12** | `sendfile`/`writev` + handoff batching       | Final syscall-level gains              |

### Performance measurement

Vajra already has the right benchmark foundation: RPS, RPS/core, p95/p99, route-level behavior and process-group RSS across HTTP/1, TLS, HTTP/2, uploads, multiplexing and tunnels.

For every optimization above, I would compare the existing routes on the **same machine/build** and specifically add:

```text
allocations/request
bytes copied/request
Ruby GC time/request
temp-file bytes/request
H2 queued bytes/connection
H2 pending execution count
header-key-cache cardinality (until removed)
```

I would not attach speculative “+15% RPS” numbers to these findings. The source shows where work is being wasted, but the existing benchmark harness should determine the actual gain.

### Audit limitation

This was a **full static source/dependency/advisory review of the visible repository**, not a claim of penetration-test certification. I was able to inspect the repository through the connected GitHub source and query current advisories, but the local execution environment could not fetch a checkout/dependency set, so I did **not** actually execute k6, `bundler-audit`, ASan, fuzzers or exploit tests. That distinction matters: the header-cache issue and copy/buffering paths are confirmed directly from source; quantitative RPS gains still need the benchmark harness, and native-memory security should be followed by sanitizer/fuzzer execution.

If I were choosing where to put engineering effort first, I would fix the **header-key memory DoS**, update the four vulnerable/stale baselines, and then redesign the **response body bridge**. Those changes address the highest security risk and the highest-probability performance ceiling before spending time on micro-optimizations.

I can also keep the Ruby/Rails/json/nghttp2 advisories under watch and flag new Vajra-relevant CVEs as they appear.

[1]: https://www.ruby-lang.org/en/news/2026/03/05/buffer-overflow-zlib-cve-2026-27820/?utm_source=chatgpt.com "CVE-2026-27820: Buffer overflow vulnerability in Zlib::GzipReader | Ruby"
[2]: https://www.ruby-lang.org/en/news/2025/07/08/dos-resolv-cve-2025-24294/?utm_source=chatgpt.com "CVE-2025-24294: Possible Denial of Service in resolv gem | Ruby"
[3]: https://github.com/ruby/json/security/advisories/GHSA-9hj4-r449-hfvc?utm_source=chatgpt.com "JSON::ResumableParser#partial_value dereferences a freed input buffer and crashes on truncated duplicate-key streams · Advisory · ruby/json · GitHub"
[4]: https://rubyonrails.org/2026/7/29/Rails-Versions-7-2-3-2-8-0-5-1-and-8-1-3-1-have-been-released?utm_source=chatgpt.com "Rails Versions 7.2.3.2, 8.0.5.1, and 8.1.3.1 have been released!"
[5]: https://nvd.nist.gov/vuln/detail/CVE-2026-27135?utm_source=chatgpt.com "NVD - CVE-2026-27135"
[6]: https://nvd.nist.gov/vuln/detail/CVE-2026-58055?utm_source=chatgpt.com "NVD - CVE-2026-58055"
[7]: https://github.com/nghttp2/nghttp2/releases?utm_source=chatgpt.com "Releases · nghttp2/nghttp2 · GitHub"

---

# Final disposition — approved August 25, 2026

**Decision: keep the complete audit slice.** The user approved retention after the final validation and matched benchmark. The implementation is retained in commit `d9e9ea5` (`feat: performance and security audit`) on `feat/performance-security-audit`; no audit implementation was reverted.

## Applied recommendations

| Audit recommendation | Disposition and delivered scope |
| --- | --- |
| Remove the unbounded attacker-controlled header-key cache | **Applied.** Only fixed/common keys remain process-lifetime GC roots. Arbitrary `HTTP_*` Rack keys are now created for, and owned by, the request environment rather than inserted into the global cache. |
| Move the Ruby security baseline from 3.4.3 to 3.4.10 | **Applied in repository configuration.** `gems/vajra/.ruby-version`, `docs/.ruby-version`, Docker, and the relevant workflows now use 3.4.10; the Docker image is digest-pinned. See the runtime-validation caveat below. |
| Update Rails/Active Storage | **Applied.** The Vajra and performance lockfiles resolve Rails and Active Storage to 8.1.3.1. |
| Update `json` | **Applied.** All four Bundler dependency roots resolve `json` to 2.21.2. |
| Upgrade vendored nghttp2 | **Applied.** The vendored source and `UPSTREAM.md` identify nghttp2 v1.70.0. |
| Run dependency-CVE checks in CI | **Applied.** Shared CI installs `bundler-audit`, refreshes its advisory database, and checks the Vajra, performance, docs, and Danger lockfiles. |
| Add native sanitizer and parser-fuzz coverage | **Applied.** Shared CI runs ASan/UBSan CTest and bounded libFuzzer executions for seven targets: HTTP/1 request head, body framing, chunked body, h2c upgrade, HTTP/2 frame precheck, response headers, and `traceparent`. Corpus files and local runner scripts are included. |
| Pin mutable CI/release inputs and record build provenance | **Applied.** The audited GitHub Action references use immutable commit SHAs, the Ruby container is digest-pinned, and `scripts/write-build-provenance` records Ruby, OpenSSL, nghttp2, compiler, target, and Git revision data. This is build provenance, not a complete package SBOM. |
| Replace Rack response materialization with bounded streaming | **Applied for the Rack response path.** HTTP/1 and HTTP/2 use bounded response-body streams with producer backpressure and cancellation; regression coverage includes producer wake-up and HTTP/2 stream-local failure behavior. |
| Remove the avoidable `NativeInput` intermediary copy | **Applied.** Reads fill the destination Ruby buffer directly rather than first allocating and copying through a return `std::string`; the spill/read path was updated with the same lifetime and interruption handling. |
| Add explicit HTTP/2 execution admission | **Applied.** `http2_max_pending_executions` defaults to `2 * max_threads`, has an environment override, and is surfaced in runtime metrics. |
| Add HTTP/2 connection-wide buffer accounting and remove prefix erases | **Applied.** `http2_max_connection_buffer_bytes` defaults to 16 MiB; tracked H2 queued data uses a connection ledger with metrics. Stream queues now use front offsets instead of repeated prefix `erase()` operations. |

## No-op and intentionally deferred recommendations

No implementation recommendation was rejected outright. The following outcomes are deliberately distinct from rejection.

| Audit item | Status | Reason and boundary |
| --- | --- | --- |
| Broad dependency-graph upgrade beyond Ruby, Rails/Active Storage, `json`, and nghttp2 | **No-op by design.** The audit found several relevant dependencies already on patched versions; changing unrelated packages without a specific advisory or compatibility need would add risk without addressing a finding. |
| Pin an OpenSSL source version in the repository | **No-op at repository level.** Vajra links the platform OpenSSL; the new provenance script records the actual library and package context. Release/deployment environments still need their own package/SBOM verification. |
| Make response materialization disappear for every response origin | **Partially applied.** Rack responses now have a bounded streaming path. Legacy/custom materialized responses can still exist before later HTTP/2 ledger admission, so this is not a claim that every response path is zero-copy or never materialized. |
| Treat the H2 ledger as a total connection-memory or RSS cap | **Explicitly not claimed.** The 16 MiB setting covers Vajra-owned tracked queues; nghttp2/HPACK, TLS, kernel buffers, allocator overhead, and receive scratch remain outside it. |
| Reuse H2 write scratch, retain request-head views longer, reduce Rack-env normalization, replace all receive/ready scans with ring/ready queues | **Profile-gated future work.** These candidates were not forced without profiling evidence after the structural changes. |
| `writev`/`sendmsg`, plaintext `sendfile`, SCM_RIGHTS batching, or allocator replacement | **Profile-gated future work.** They need workload-specific measurements; no syscall, dispatch, or allocator change was included solely on source inspection. |

## Validation evidence

The final validation completed before the decision:

* `scripts/run-all`: exit 0.
* Unit suite: 256 examples, 100% line and branch coverage.
* E2E suite: 155 examples.
* h2spec: 146/146; CTest: 1/1.
* RuboCop, Reek, Clint, RBS, gem builds, and docs build: pass.
* Seven parser targets: 100 bounded LLVM/ASan/UBSan runs each, with no reported sanitizer failure.

### Matched performance result

The decision used three baseline and three final Rack runs, after warm-up exclusion: 20 virtual users for 20 seconds per run, Vajra Rack fixture, access logging off, 10 detected CPU cores, one worker, and four threads. The request mix contains 13 routes: `HEAD /text`; `GET /text`, `/json`, and `/headers`; and POST JSON, form, upload, raw-body, stream-read, and line-read variants. RSS is each run's recorded maximum, converted from bytes to MiB (`2^20` bytes).

| Metric (mean of three runs unless noted) | Baseline | Final | Change |
| --- | ---: | ---: | ---: |
| Mean RPS | 13,629.72 | 13,107.15 | -3.83% |
| Median RPS | 13,576.11 | 13,040.29 | -3.95% |
| p50 latency | 1.173 ms | 1.271 ms | +8.36% |
| p95 latency | 3.210 ms | 2.844 ms | -11.42% |
| p99 latency | 4.372 ms | 4.128 ms | -5.58% |
| Mean maximum RSS | 221.51 MiB | 255.53 MiB | +15.36% |
| Error rate | 0.00% | 0.00% | 0.00 pp |

Baseline artifacts: `gems/vajra/performance/tmp/20260824T232534Z/summary.json`, `gems/vajra/performance/tmp/20260824T232555Z/summary.json`, and `gems/vajra/performance/tmp/20260824T232615Z/summary.json`.

Final artifacts: `gems/vajra/performance/tmp/20260825T033538Z/summary.json`, `gems/vajra/performance/tmp/20260825T033601Z/summary.json`, and `gems/vajra/performance/tmp/20260825T033623Z/summary.json`.

This measured a security/correctness and tail-latency tradeoff, **not** a throughput or memory improvement: mean throughput fell 3.83% and mean maximum RSS increased 15.36%, while p95 and p99 improved and no run reported errors.

### Ruby runtime caveat

The local final validation and benchmark ran on Ruby 3.4.3. Repository configuration now requests Ruby 3.4.10, but 3.4.10 was not locally executed in this audit session. Treat the runtime-baseline update as configured and pending CI or equivalent Ruby-3.4.10 execution proof; do not represent it as locally validated by the results above.

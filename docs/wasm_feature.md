# WASM as a composable feature — design

**Status:** design (not implemented). The last large vendored engine still
compiled into the base by default. Finishes the base-subtraction axis begun by
#113 (runtimes) and #114 (HTTP core). Sequenced as its **own epic** after a
release cut, because it is materially harder than the runtime/HTTP splits (see
"Why this is harder" below).

Related: [docs/http_feature_phase1.md](http_feature_phase1.md) (the seam +
whole-archive pattern this reuses), [docs/features_and_flavors.md](features_and_flavors.md)
(the taxonomy), [docs/composed_feature_signing.md](composed_feature_signing.md)
(which covers this feature for free — see "Signing").

## The gap

Today WAMR (~256 KB) is a **compile-time flag** (`HL_ENABLE_WASM`, default 1):
one binary either has `compute.*` or it doesn't. Like the runtime and HTTP work,
we want it to be a **composition decision of the produced app** instead — a
single distributed `hull` builds both compute apps and compute-free apps, and a
compute-free app links **zero** WAMR (~256 KB smaller, smaller attack surface,
no `wasm_*` symbols).

This mirrors the mandatory auto-composed axis: the native base becomes
**compute-less**, and `hull build` composes `libhull_feature-wasm.a` back only
when the app needs it. Like the runtimes and HTTP core (and unlike `--with=gpu`),
the archive is **embedded in `hull`** and **auto-composed** — never
`hull feature install`. Cosmo (fat APE) keeps WASM compiled in.

## Why this is harder than #113 / #114

The runtime split inferred from the entry extension (`.lua`/`.js`); the HTTP
split inferred from a resolved cap (`HL_MOD_CAP_HTTP`) tripped by module-
conditional `app.get`/etc. decorations. WASM has **three** entanglements that
neither of those had:

1. **Not cleanly module-inferable.** `hull/compute` declares `HL_MOD_CAP_WASM`,
   so the *direct* `compute.*` path is inferable exactly like HTTP. But WAMR is
   **also** reachable through a path that carries no module declaration:
2. **`db.udf` is WASM-backed.** `cap/db_udf.c` (part of the always-present base
   DB layer) calls `hl_cap_wasm_instance_create/call/destroy` directly:
   `db.udf.register(name, "wasm_module")` runs a compute module inside SQLite.
   That app may declare only `hull/db` and never `hull/compute`. So the base DB
   layer references WASM cap symbols, and "needs WASM" cannot be read from
   `hull/compute` alone.
3. **The unified buffer protocol touches `WasmBuffer`.** `compute.buffer(...)`
   and the zero-copy `WasmBuffer` userdata (`cap/wasm_buffer.c`) are woven into
   `mod_buffer` / `mod_compute` and the GPU path. Dropping WASM must leave the
   buffer protocol coherent (a `WasmBuffer` simply cannot be created).

These are exactly why roadmap.md still lists the WASM *interpreter* under
"deliberately kept core" for the near term, while roadmap_next.md carries the
"WASM follows the same model (separate epic)" long-term plan. Both are true: it
is the last featurify target, and it is a distinct, later epic.

## Resolving "needs WASM" (the two-signal gate)

`hull build` composes the WASM feature iff **either** build-time signal fires
(the roadmap's "two signals", realized at build time):

| Signal | Source | Catches |
|--------|--------|---------|
| **S1: compute cap declared** | `hl_module_set_required_caps & HL_MOD_CAP_WASM` (an app declaring `hull/compute`), exposed from `tool.modules_resolve` as `needs_wasm` — identical plumbing to `needs_http`. | the direct `compute.*` path |
| **S2: app ships `compute/*.wasm`** | `build.lua` already discovers + embeds `compute/*.wasm` (and AOT-compiles them). A non-empty compute set is the signal. | the WASM-backed `db.udf` path (a wasm UDF needs a `.wasm` to register), even when only `hull/db` is declared |

`needs_wasm = S1 or S2`. If **neither** fires, the app links no WAMR. S2 is the
critical difference from HTTP: a UDF-only app trips S2 (it ships the `.wasm`)
without S1, so it keeps the runtime it needs. Conservative by construction: a
declared-but-unused `hull/compute`, or a stray `compute/*.wasm`, composes WASM
(correct, just not minimal). A compute-free app is the common case and gets the
slim binary.

**Edge to document:** a Lua/JS-function `db.udf` (not WASM-backed) needs no WAMR;
it must keep working on a WASM-free base (see the seam below). Only
`db.udf.register(name, "module")` (WASM-backed) requires the feature, and that
app ships the `.wasm` → S2.

## The seam

Mirror the HTTP seam (`http_feature.h` weak defaults in `cap/http_feature.c`,
strong overrides whole-archived from the feature lib).

**Moves into `libhull_feature-wasm.a`:**
- `cap/wasm.c`, `cap/wasm_buffer.c`, `cap/wasm_data.c`, `cap/wasm_stream.c`
- `worker_wasm.o` (the async compute worker)
- `WAMR_OBJS` (the vendored WAMR archive, the ~256 KB)

**Stays in the base, gains a weak seam** (`include/hull/wasm_feature.h`,
weak no-ops in `cap/wasm_feature.c`, real-signature weak stubs in a base TU
`src/hull/wasm_weakstub.c` for the symbols base objects reference):

- **`cap/db_udf.c`** (base DB layer) references `hl_cap_wasm_instance_*`. On a
  WASM-free base those resolve to weak stubs that fail closed:
  `db.udf.register(name, <lua/js fn>)` (scalar/aggregate function UDFs) still
  works — that path never calls WASM; `db.udf.register(name, "module")`
  (WASM-backed) returns a clear "WASM feature not composed" error. This is the
  central seam: the DB layer stays whole, only the wasm-backed branch degrades.
- **`compute.*` runtime bindings.** Two options, decide in Phase 0:
  - **(a) weak-seam in place (simpler).** `mod_compute.o` stays in the runtime
    archive and calls `hl_cap_wasm_*` through the seam; on a WASM-free base
    `compute.available()` → false and every `compute.call/async/instance/segment/
    stream/buffer` returns "not_available". No new per-runtime archive.
  - **(b) per-runtime bridge (parallels HTTP-C).** `mod_compute.o` moves into
    `libhull_feature-wasm-<rt>.a`, composed with the core when `needs_wasm`. The
    pure runtime keeps weak stubs for the few `mod_compute` symbols `modules.o`
    references. Cleaner symbol separation, one more archive per runtime.
  Recommendation: **(a)** — the `compute.*` binding surface is small and the
  buffer-protocol coupling (`WasmBuffer`) makes a clean per-runtime extraction
  fiddlier than it was for the web bindings. Prove with an `nm` assertion that a
  compute-free app has zero `wasm_*`/WAMR symbols regardless.
- **`compute.buffer` / `WasmBuffer`.** On a WASM-free base, `compute.buffer(...)`
  fails ("WASM feature not composed"); a `WasmBuffer` cannot be created, so the
  unified buffer protocol's other producers (`fs.mmap` → `MappedBuffer`,
  `ArrayBuffer`) are unaffected. Verify GPU + image paths still accept the
  remaining buffer types.

Whole-archive at compose (`-force_load` ld64 / `--whole-archive` +
`--start-group` GNU-ld), inside the platform-lib group — WAMR references libc/pthread
and the caps reference base symbols, same as the HTTP core.

## Composition + Makefile

- `libhull_feature-wasm.a`: `make feature-wasm` (ar over the wasm caps +
  worker_wasm + WAMR_OBJS). Add to `EMBEDDED_*` (a new `embedded_wasm.h` via the
  `XXD_CONST_PIPE` pattern) and `RUNTIME_FEATURE_LIBS`, gated so the base drops
  the WAMR objects from `PLATFORM_OBJS`.
- `build.lua` composes it (embedded-first resolve ladder, like the runtime/http
  libs) when `needs_wasm`, via `feature_compose.lua` (`resolve_wasm_lib`) +
  `build_assets.c` (`hl_build_extract_feature_wasm`).
- Cosmo exempt: the fat APE keeps WASM in-base (a fat APE can't force-load a
  native feature archive), same carve-out as the runtimes and HTTP core.

## Signing (free)

Because the WASM feature is an **embedded, auto-composed** archive (not
`hull feature install`), it lands in the `platform_domain` of
`package.sig.gethull.composed`. The composed-feature signing already shipped
([docs/composed_feature_signing.md](composed_feature_signing.md)) covers it with
**no new mechanism**: add `libhull_feature-wasm` as the 8th embedded archive in
`release.yml`'s `platform-features-<arch>` set, one line in the signed platform
manifest, and one entry in the `TRUST_FEATURE_LIBS` list. `build.lua`
`record_composed(...)` already tags any composed archive; the runtime §5c check
attests it automatically.

## Phase plan

- **Phase 0 — seam (additive, no base-flip).** Add `wasm_feature.h` +
  `cap/wasm_feature.c` weak defaults + `wasm_weakstub.c`; route `cap/db_udf.c`
  and `mod_compute` through the seam. Base still compiles WAMR in. Prove the seam
  with the existing `test_wasm` / `test_wasm_buffer` unchanged. Decide (a) vs (b).
- **Phase 1 — base-flip + embed.** Drop `WAMR_OBJS` + wasm caps + `worker_wasm`
  from `PLATFORM_OBJS` into `libhull_feature-wasm.a`; embed it; base is
  compute-less. `make` composes it always (behavior-identical), then gate on
  `needs_wasm`. `nm` assertion: a compute-free app has zero WAMR symbols.
- **Phase 2 — the two-signal gate.** Wire `needs_wasm = S1 or S2` (`S1` from
  `tool.modules_resolve`, `S2` from the discovered `compute/*.wasm` set). E2E:
  (i) `compute.*` app composes; (ii) WASM-backed-`db.udf`-only app (only
  `hull/db`, ships a `.wasm`) composes via S2; (iii) function-`db.udf` app with
  no `.wasm` stays WASM-free and its UDF still runs; (iv) plain web app is
  WASM-free and ~256 KB smaller.
- **Phase 3 — signing + release wire-up.** Extend `release.yml`
  `platform-features` + the signed manifest + `TRUST_FEATURE_LIBS`. Extend
  `e2e_composed_sig.sh`'s manifest to the 8th archive.
- **Phase 4 — docs + flavor interplay.** `--flavor=pure-compute` × WASM (the
  reduced-flavor × feature orthogonality now that #114 landed); update
  CLAUDE.md, AGENTS.md, the taxonomy tables.

## Testing

- Seam unit tests: a WASM-free base build links; `compute.available()` → false;
  a function-`db.udf` runs; a WASM-backed `db.udf.register` fails with the clear
  "feature not composed" message.
- `nm` symbol assertion: a compute-free app exports no `wasm_*` / WAMR / iwasm
  symbols (the base-flip invariant), and the buffer protocol still resolves.
- `tests/e2e_feature_wasm.sh`: compose WASM (both runtimes), the four Phase-2
  scenarios, and a slim-binary size assertion (~256 KB delta).
- `e2e_composed_sig.sh`: the 8th embedded archive is attested; tamper fatal.
- Regression: existing `test_wasm` (55), `test_wasm_buffer` (12), `e2e_compute*`
  unchanged when WASM is composed.

## Non-goals

- `hull feature install wasm` (WASM is embedded + auto-composed, not
  install-on-demand — same as the runtimes/HTTP).
- Cosmo WASM slim (the universal APE keeps WAMR in-base).
- Featurizing `wamrc` (already correctly a **tool**), or the AOT path itself.
- Making `crypto`/SHA/HMAC or the default SQLite DB a feature (out of scope;
  crypto is the trust substrate, SQLite is the default backend).

## Risks

- **db.udf seam correctness (highest).** The failure mode is a wasm-backed UDF
  silently mis-degrading or a function-UDF regressing. Cover both branches
  explicitly; the wasm-backed branch must fail *loud and closed*.
- **Buffer-protocol coherence.** `WasmBuffer` disappearing must not break the
  `MappedBuffer`/`ArrayBuffer` paths into GPU/image. Test the cross-buffer
  matrix on a WASM-free base.
- **S2 false-negative.** An app that loads a compute module by a path build.lua
  doesn't discover would miss S2 and link no WAMR. Today all compute modules come
  from `compute/*.wasm` (embedded), so the discovery is complete — assert there
  is no other module source, and document that dynamic/out-of-tree `.wasm` is
  unsupported (as it already is).
- **Effort/risk vs payoff.** ~256 KB on a base that ships far larger vendored
  pieces (mbedTLS, SQLite). Worth it for the compute-free web/CLI app and to
  finish the axis, but lower leverage than a new additive feature (e.g. the
  Redis/Valkey client). Sequence accordingly.

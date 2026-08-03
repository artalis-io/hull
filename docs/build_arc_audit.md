# Build / toolchain-free arc — audit findings & roadmap

Audit date: 2026-08-03. Four-dimension review (Makefile/mk, build.lua/compose,
linker/toolchain-free seam, extension-system consistency). Items are tracked
with status so we can check back. Severity is post-review judgment; ✓ = the
claim was verified against the code during the audit.

Execution order (agreed): **#6+#1 → #7 → #4 → #5 → #2 → #3 → #8**, then Tier 4.

---

## Tier 1 — correctness bugs

- [x] **#1 `hull eject` composes only runtime+http — broken for db/compute/image/TLS apps.** ✓
  `eject.lua` calls only `resolve_runtime_lib` / `resolve_http_lib` / `resolve_http_rt_lib`;
  `build.lua`'s `compose_features()` also composes wasm/wasm-rt/sqlite-rt/image/image-rt/tls/keel.
  Ejecting a db/compute/image/HTTPS app yields a project that won't link/run. Drift
  the modularization arc introduced. **Fix converges with #6.** DONE (PR #203): shared fcompose.plan_mandatory; eject now composes the full ordered set (validated: ejected compute app links + runs with WAMR).

- [x] **#2 Cross-compile silently mis-links.** DONE (PR #206): early cross-target guard + target_spec arch/os validation + --target= parsing. ✓ `--target=<foreign>` emits a correct cross
  `app_registry.o`, but only the `zig` backend consumes the triple (system/lld `(void)tgt`),
  and the bundled `app_main.o` + platform `.a` are host-only, so end-to-end cross can't
  succeed regardless. No guard. Minimum fix: reject a host-mismatched `--target` that can't
  be satisfied (needs zig + a target platform lib). `target_spec` (build.lua:339) is also
  lenient — validate arch/os.

- [x] **#3a `FEATURE_ARCHIVES` / `RUNTIME_FEATURE_LIBS` drifted from the registry** — `tls`,
  `keel`, `sqlite` archives miss the `: Makefile` rebuild guard (latent stale-`ar`
  duplicate-symbol class bug, uncaught by CI). Fix: derive from `$(FEATURE_EMBEDDED_LIBS)`. DONE (PR #207).
- [x] **#3b `HL_ENABLE_MYSQL` missing from the config-sentinel fingerprint** (asymmetric with
  POSTGRES, Makefile:~1823). Add `MYSQL=$(HL_ENABLE_MYSQL)|`. DONE (PR #207).
- [x] **#3c Tier B (`lld-static`) omits `--gc-sections`** (linker_lld.c direct branch) — dead
  sections / latent dangling-symbol risk that the zig backend explicitly guards against. DONE (PR #207).

## Tier 2 — make the toolchain-free story real for install-only users

- [ ] **#4 No published musl `libhull_platform.a`.** ✓ Release builds Linux libs on glibc only,
  so `--linker=lld-static` and `--linker=zig --target=…-musl` work ONLY from a source build.
  Publish a signed `libhull_platform-musl-<arch>.a` (+ runtime/feature archives) and teach
  `prepare_platform` to select it for a musl target. Highest-value "make it real" gap.
- [x] **#5 `--linker=zig` has zero e2e** ✓. DONE (PR #205): `tests/e2e_linker_zig.sh` builds +
  runs a real app through the zig backend on Linux x86_64 (skips elsewhere - cross needs a
  target platform lib, #2/#4); wired into Linux CI. A `test_linker.c` unit test was deferred
  (backends spawn via `hl_tool_spawn`; low regression risk now that the backend is e2e-covered).

## Tier 3 — simplifications (two prevent Tier-1 bugs)

- [x] **#6 Table-drive `compose_features()`** — DONE (PR #203, converged with #1): build.lua
  2365->1995 lines; compose blocks -> one data-driven plan + loop; extract_manifest centralized.
- [x] **#7 `HULL_CORE_OBJS`** — DONE (PR #204): shipped `HULL_LINK_OBJS` (the hull-target
  prereq+recipe double-listing, byte-identical 74-token diff) + the sentinel purge drift fix
  (fs_util.o + ~20 others). Full cross-target `HULL_CORE_OBJS`-with-`PLATFORM_OBJS` DEFERRED:
  the shared vars are interleaved with each list's distinct ones, so a shared-core extraction
  would REORDER the link line and risk weak/strong seam resolution.
- [x] **#8 Consolidate trust-critical duplicate `hex_decode`** — DONE. Deleted the 4 byte-identical
  local copies (`hex_nibble` + `hex_decode`/`hex_decode_pk` in `signature.c`, `release.c`,
  `commands/verify_release.c`, `commands/verify_self.c`) and routed all ~10 call sites through the
  canonical `hl_cap_crypto_hex_decode`. Resolution of the contract mismatch (canonical returns the
  BYTE COUNT with out_size as a capacity; the locals returned 0/-1 with an exact `hex_len==out_len*2`
  check): the exact-length behavior is preserved by checking `rc == N` (not `rc != 0`), which is
  provably equivalent — success requires `hex_len/2 == N` exactly, since a short input returns `<N`
  and a long one returns `-1` (insufficient capacity). No security regression: fail-closed on
  short/malformed input is retained, and the canonical adds NULL-guards + explicit bounds the
  locals lacked. The `hex_decode_pk` sites pass `strlen(pubkey_hex)` so an over/under-length
  `--pubkey` still rejects. No new object / link-list wiring (canonical's `cap_crypto.o` is a base
  object already linked wherever `signature.o`/`release.o` are, all flavors). Validated:
  `test_signature` (20), `test_release` (20), `test_crypto` (58), `test_dispatch` (4),
  `test_verify_self` (5), full `make test` green, cppcheck clean. (`hex_encode` ×N, `secure_zero` ×N
  have the same "identical-to-each-other but a different-contract canonical exists" shape — left as a
  Tier-4 follow-on; `hex_encode` stays local in `signature.c`/`release.c` since it has no exact-length
  hazard and a differing signature.)

## Tier 4 — architectural consistency & docs (lower urgency)

- [ ] **T4a "base cap module" taxonomy category.** image/mime/blob/**tar** are all-C in-base cap
  modules, but the CLAUDE.md taxonomy says stdlib = "no new C" and has no home for them; mime
  `.pure=1` vs tar `.pure=0` exposes the inconsistency. Document a 5th row + the small+in-base
  vs large+off-by-default rule vs a feature. Also add an explicit "tar rides CAP_OBJS,
  always-in-base" note (today it's implicit via `wildcard` + denylist omission).
- [ ] **T4b image half-migrated** — `#ifdef HL_ENABLE_IMAGE` in `modules.c` coexists with the
  composed-feature weak-stub seam; document the dual role or unify on the seam.
- [ ] **T4c Weak-hook seam divergence** — two shapes (header hook + `_present()` vs
  weak-stub-as-sentinel, forcing WASM's bespoke `HL_CAP_WASM_ABSENT`); no `HL_WEAK` macro,
  no single "how to add a seam" doc. Add a canonical section to features_and_flavors.md;
  optionally an `HL_WEAK` macro over the ~21 raw `__attribute__((weak))` sites.
- [ ] **T4d tools "bundle" facts duplicated** across `release.yml` (3×) + `tools.c`; registry
  isn't the single source the comment claims. Consider `hull tools list --json --assets`.
  Extend `release_smoke.sh` to install one bundle shape (zig/floor), not just wamrc.
- [ ] **T4e FEATURE_SPECS not cross-checked** against `feature.c` FEATURES[] / Makefile —
  extend `scripts/check_feature_registry.sh` to assert `feature_specs.lua` keys ⊇ installable
  stems.
- [ ] **T4f doc-rot** — build.lua tcc hints (retired), "planned" COFF/Mach-O comments (done),
  mold "near-term" (deferred/not-started), CLAUDE.md tool row missing `.tar` bundle shape,
  `obj_emit.h`/`linker.h` "planned" comments.

## Already resolved (do not re-chase)

- Config-sentinel **EMBED_PLATFORM** fingerprint gap — FIXED in commit 532991dd (2026-08-02).
  The `project_build_modularization_plan.md` memory note is stale.
- **cap/tar.c** `hl_tar_extract` mkdir-p dest + `release_io` 512 MB cap + lld-bundle-dropped —
  fixed in PR #202 (the #201 dry-run findings).

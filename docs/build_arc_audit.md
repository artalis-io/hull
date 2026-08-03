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
- [~] **#8 Consolidate trust-critical duplicate helpers** — ASSESSED + DEFERRED (needs focused,
  security-reviewed work). The `hex_decode` ×4 copies (`signature.c`, `release.c`,
  `verify_release.c`, `verify_self.c`) are byte-identical to EACH OTHER, but the canonical
  `hl_cap_crypto_hex_decode` has a DIFFERENT contract: it returns the BYTE COUNT (not 0) on
  success and does NOT enforce exact length (out_size is a capacity, not the expected size),
  whereas the local copies return 0/-1 and require `hex_len == out_len*2` exactly. So a naive
  swap breaks every caller's `!= 0` check AND drops the exact-length enforcement — a SECURITY
  regression risk on Ed25519 signature-verify paths (a short/malformed sig could decode
  partially instead of failing closed). Two safe paths, either a focused PR: (a) a shared
  `hl_hex_decode_exact` helper preserving the local 0/-1 exact-length contract (no canonical
  change; needs its own object + link-list wiring); or (b) adapt each of the ~10 call sites to
  the canonical's byte-count/`< 0` contract AND add an explicit `hex_len == N*2` check. NOT a
  mechanical mkdir_p-style consolidation. (`hex_encode` ×9, `secure_zero` ×5 have the same
  "identical-to-each-other but a different-contract canonical exists" shape.)

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

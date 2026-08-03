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

- [ ] **#2 Cross-compile silently mis-links.** ✓ `--target=<foreign>` emits a correct cross
  `app_registry.o`, but only the `zig` backend consumes the triple (system/lld `(void)tgt`),
  and the bundled `app_main.o` + platform `.a` are host-only, so end-to-end cross can't
  succeed regardless. No guard. Minimum fix: reject a host-mismatched `--target` that can't
  be satisfied (needs zig + a target platform lib). `target_spec` (build.lua:339) is also
  lenient — validate arch/os.

- [ ] **#3a `FEATURE_ARCHIVES` / `RUNTIME_FEATURE_LIBS` drifted from the registry** — `tls`,
  `keel`, `sqlite` archives miss the `: Makefile` rebuild guard (latent stale-`ar`
  duplicate-symbol class bug, uncaught by CI). Fix: derive from `$(FEATURE_EMBEDDED_LIBS)`.
- [ ] **#3b `HL_ENABLE_MYSQL` missing from the config-sentinel fingerprint** (asymmetric with
  POSTGRES, Makefile:~1823). Add `MYSQL=$(HL_ENABLE_MYSQL)|`.
- [ ] **#3c Tier B (`lld-static`) omits `--gc-sections`** (linker_lld.c direct branch) — dead
  sections / latent dangling-symbol risk that the zig backend explicitly guards against.

## Tier 2 — make the toolchain-free story real for install-only users

- [ ] **#4 No published musl `libhull_platform.a`.** ✓ Release builds Linux libs on glibc only,
  so `--linker=lld-static` and `--linker=zig --target=…-musl` work ONLY from a source build.
  Publish a signed `libhull_platform-musl-<arch>.a` (+ runtime/feature archives) and teach
  `prepare_platform` to select it for a musl target. Highest-value "make it real" gap.
- [x] **#5 `--linker=zig` has zero e2e** ✓. DONE (PR #205): `tests/e2e_linker_zig.sh`
  stages a system zig, `hull build --linker=zig` a real app, runs it; wired into the
  Linux CI `build` job (installs zig 0.13.0, `runner.arch == X64`). Runs on Linux x86_64
  only and SKIPS elsewhere - a foreign-target link needs a target-matching platform lib
  (this is exactly the #2/#4 cross-compile gap, which the e2e surfaced directly: a
  darwin-arm64 platform lib can't link an x86_64-linux binary). A `test_linker.c` unit test
  was DEFERRED: the backends' `is_available`/`link` spawn via `hl_tool_spawn`, so a
  meaningful unit test needs the tool-spawn stub + only covers the constructor-name
  contract (low regression risk now that the backend is e2e-covered).

## Tier 3 — simplifications (two prevent Tier-1 bugs)

- [x] **#6 Table-drive `compose_features()`** — 690-line fn, same 12-line block ×8. Collapse to a
  data table + one loop; **fixes #1 for free** (eject calls the same helper). DONE (PR #203): build.lua 2365->1995 lines; compose blocks -> one data-driven plan + loop; extract_manifest also centralized. e2e composed_sig/build/feature_runtime/feature_wasm green.
- [x] **#7 One `HULL_CORE_OBJS` variable** (HULL_LINK_OBJS + purge fix; full cross-target collapse deferred as interleaved-reorder-risk — PR #204) — the ~60-token object list is duplicated across 8
  sites (hull link prereqs+recipe, `PLATFORM_OBJS`, 5 test lists); drift already exists
  (`fs_util.o` missing from the sentinel purge). Collapse to one var + `$^` recipe; derive
  `PLATFORM_OBJS` via `filter-out`. Also: derive the sentinel purge list from the same set.
- [ ] **#8 Consolidate trust-critical duplicate helpers** — `hex_decode` ×4 byte-identical on
  Ed25519 signature-verify paths (`signature.c`, `release.c`, `verify_release.c`,
  `verify_self.c`) while canonical `hl_cap_crypto_hex_decode` exists. Same as `mkdir_p`
  consolidation, higher stakes. (`hex_encode` ×9, `secure_zero` ×5 follow.)

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

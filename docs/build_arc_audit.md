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

- **#4 No published musl `libhull_platform.a`.** ✓ Release builds Linux libs on glibc only,
  so `--linker=lld-static` and `--linker=zig --target=…-musl` work ONLY from a source build.
  A coupled producer+pipeline+consumer epic; split into sub-steps:
  - [x] **#4a Producer.** `scripts/build_musl_platform.sh` builds the FULL musl archive set
    (`make platform` + `platform-slim` + `feature-embedded` = base + slim + all 18
    `libhull_feature-*.a`), read-only-mount-safe (builds in a container-internal copy so a host
    checkout's `build/` is never clobbered). Validated end-to-end in `alpine:3.20`: all 20
    archives build under musl + pack to a ~15.8 MB flat ustar. Mirrors `build_musl_floor.sh`.
    Docs: musl_build.md "Producing the musl platform archive set".
  - [x] **#4b Publish (release pipeline).** DONE (PR #211). A `build-platform-musl` Alpine job
    (mirror `build-floor-musl`) runs #4a per arch and packs `hull-platform-musl-<arch>.tar`; the
    final `release` job hashes it into the signed `hull.sha256` (release-key trust chain, same as
    the floor / wamrc / zig tars), adds it to the SLSA attestation subjects, and publishes it.
    Scoped to the FETCHABLE tar in `hull.sha256` - deliberately NOT the embedded platform-sig
    (`platform-manifest.txt`/§5c); the artifact is named `hull-platform-musl-` (not `platform-`)
    so it does NOT leak into the untouched `sign-platform-manifest` job. **Validated by a real
    pre-release dry-run** (`v0.9.1-musl4b-dryrun.1`, run 30847822944): both musl jobs green, the
    full signed release published, `hull-platform-musl-x86_64.tar` SHA `4d731724…` matched its
    `hull.sha256` entry byte-for-byte, tar held all 20 archives, prerelease flag set (v0.9.0 stayed
    Latest), then torn down (`gh release delete --cleanup-tag`).
  - [x] **#4c Fetch + select.** DONE. `hull tools install platform-musl-<arch>` (a data-only bundle
    in `tools_install.c`'s REGISTRY, mirror of `libc-musl-<arch>`, release-key-verified to
    `~/.hull/tools/platform-musl-<arch>/`; all three native hosts publish it since it is a
    cross-target asset). `feature_compose` gained a `ctx.musl_dir` that takes precedence over the
    embedded glibc copies for EVERY resolver (base + all feature archives, atomically);
    `prepare_platform` sources the musl base from it and sets `platform_sig_blob = nil`; the `#206`
    guard now resolves the bundle for a `-musl` target (fails closed with a `hull tools install`
    hint if absent). The two link objects `obj_emit` can't produce (`app_main.o` +
    `app_feature_registry.o`, compiled C) are cross-compiled for the target with
    `zig cc --target`; the `is_darwin`/`--start-group` link-flag gates were fixed to key on the
    TARGET format, not the host. **§5c question RESOLVED (the simple branch): no 2nd dry-run
    needed.** A cross target sets `platform_sig_blob = nil`, so build.lua writes NO gethull/composed
    block, so §5c is present-gated off in the produced app (and the musl base is
    `HL_EMBED_PLATFORM_SIG=0` anyway); trust is the release-signed install + the developer app sig.
    Also allowlisted the zig backend's benign `-Wl,--gc-sections` / `-Wl,-dead_strip` in
    `hl_tool_validate_args`. **Validated end-to-end**: `hull build --target=x86_64-linux-musl
    --linker=zig` on macOS/arm64 → a static x86_64 musl ELF that RUNS on Alpine (`app.main` →
    `exit 0`); native macOS build unregressed (`make test` green).
  - [x] **#4d Smoke + e2e.** DONE. `release_smoke.sh` gained a `platform-musl-x86_64` install
    section (LIVE fetch → SHA-256 verify → bundle extract → assert the base + slim + a feature
    archive → uninstall), mirroring the floor. New `tests/e2e_musl_cross.sh` (+ `make
    e2e-musl-cross`, wired into the Linux-x86_64 CI job) does the FULL loop per-push: builds the
    musl archive set (`build_musl_platform.sh`, Alpine), stages it + zig, cross-builds a compute
    app with `--target=x86_64-linux-musl --linker=zig`, and RUNS the static musl result in Alpine
    (asserts ELF/x86-64 + statically-linked + the app's stdout). Skips cleanly off Linux-x86_64 /
    without Docker or zig.
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
- [x] **T4d tools "bundle" facts duplicated** — DONE. Fixed the misleading `tools_install.c`
  registry comment (it claimed the registry was the single source; it is the CONSUMER side -
  release.yml is a second, coordinated edit site) and added `scripts/check_tools_registry.sh`
  (+ `make check-tools-registry`, wired into the Linux CI `build` job) that asserts every REGISTRY
  `.name` has a matching `hull-<name>` asset in release.yml - catching drift per push instead of
  only at `release_smoke.sh` time. The `release_smoke.sh` bundle-install half was already done in
  #4d (floor + platform-musl + zig). `hull tools list --json --assets` deliberately NOT added (the
  drift guard covers the need; a machine-readable asset dump is a speculative feature).
- [x] **T4e FEATURE_SPECS not cross-checked** — DONE. Extended `scripts/check_feature_registry.sh`
  (already run in CI) to assert every INSTALLABLE `--with` stem has a top-level `FEATURE_SPECS`
  entry in `feature_specs.lua` (the codegen source `hull build --with=<name>` reads). Directional
  (installable ⊆ FEATURE_SPECS keys, since FEATURE_SPECS also carries the mandatory `sqlite`
  default). Negative-tested: removing a spec key fails the check with a fix-it hint.
- [ ] **T4f doc-rot** — build.lua tcc hints (retired), "planned" COFF/Mach-O comments (done),
  mold "near-term" (deferred/not-started), CLAUDE.md tool row missing `.tar` bundle shape,
  `obj_emit.h`/`linker.h` "planned" comments.

## Already resolved (do not re-chase)

- Config-sentinel **EMBED_PLATFORM** fingerprint gap — FIXED in commit 532991dd (2026-08-02).
  The `project_build_modularization_plan.md` memory note is stale.
- **cap/tar.c** `hl_tar_extract` mkdir-p dest + `release_io` 512 MB cap + lld-bundle-dropped —
  fixed in PR #202 (the #201 dry-run findings).

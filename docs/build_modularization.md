# Build modularization: one record per feature

**Status: COMPLETE (phases 0-4, shipped post-v0.9.0).** The Makefile split into
`mk/` fragments (5,344 -> ~3,090 lines): 13 `mk/features/*.mk` + `mk/feature.mk`
macros, 15 `mk/vendor/*.mk`, `mk/platform/{darwin,linux,cosmo,windows}.mk`,
`mk/{flags,hardening,libhull,tests,fetch}.mk`. `build.lua`'s `main()` staged from
~1,585 to ~328 lines (`discover()` -> a ctx table, `prepare_platform()`,
`compose_features()`) with `FEATURE_SPECS` hoisted to the `hull.feature_specs`
module. Phase 4 (cross-file registry unify): the canonical `FEATURE_EMBEDDED_STEMS`
/ `FEATURE_INSTALLABLE_STEMS` in `mk/feature.mk` drive `release.yml`'s
build/upload/sign lists, and `make check-feature-registry` (a CI fail-fast) guards
`feature.c` `FEATURES[]` against drift. Every network-fetch target (CA bundle,
pwned, HTMX, unicode, wgpu, DuckDB, cosmocc) lives in `mk/fetch.mk`.

The remaining root Makefile (~3,090 lines) is the irreducible core: the CFLAGS
accumulation pipeline, the object registry + compile pattern rules, and the single
platform-lib + `hull` link assembly point. The rest of this document is the
original design + phasing that the shipped work followed.

## Problem

The feature/flavor axis is one logical record repeated across five files.
Adding or removing a feature today means editing ~10 scattered spots, and a
build that omits a feature cannot cleanly drop that feature's Makefile pieces
(they are inline, not includable units).

| File | Per-feature scatter today |
|------|---------------------------|
| `Makefile` (5344 lines, monolithic, no `include`) | flag (`HL_ENABLE_*` / `HL_*_FEATURE`) → `FEATURE_*_OBJS` → `libhull_feature-*.a` rule → `feature-*` phony → per-runtime bridge archives → `EMBEDDED_*_H` embed → `BUILD_ASSET_OBJ` dep. Spread over lines ~380-3650. |
| `stdlib/cli/lua/hull/build.lua` (2205) | `FEATURE_SPECS` (only `--with`) + `needs_*` gates + compose logic inside a ~600-line `main()`. |
| `stdlib/cli/lua/hull/feature_compose.lua` (348) | ~11 near-identical `resolve_*_lib` functions. |
| `.github/workflows/release.yml` | the `feature-*` target list and `libhull_feature-*.a` artifact paths, hardcoded **4×**. |
| `src/hull/commands/feature.c` | `FEATURES[]` install registry (the `--with` catalog). |

The shape is extremely regular, which is what makes it a good modularization
target.

## Goal

A single declarative record per feature that the Makefile, build.lua,
release.yml, and feature.c all derive from. Adding a feature is one edit; a
build that omits a feature omits its rules, objects, and embed. More
maintainable, scalable for future features (Redis/kv is the next one), and
easier to reason about.

## The feature record (schema)

Each feature is one record. The canonical source is a small data file the
Makefile and the Lua tool both read (`mk/features.mk` for Make + a generated
mirror for Lua/C, see "Cross-file single source of truth").

| Field | Meaning |
|-------|---------|
| `name` | archive stem: `http`, `keel`, `tls`, `wasm`, `image`, `sqlite`, `tui`, `lua`, `js`, `duckdb`, `postgres`, `mysql`, `gpu` |
| `kind` | `runtime` (lua/js) \| `auto` (auto-composed, embedded in hull) \| `with` (installable `hull build --with=`) |
| `gate` | the flag / `needs_*` signal that includes it (`HL_ENABLE_WASM`, `needs_http`, ...) |
| `core_objs` | objects of the runtime-agnostic core archive |
| `rt_bridges` | per-runtime bridge objects (`lua`/`js`), or none |
| `embed` | whether embedded in hull (auto/runtime kinds) → generates `embedded_<name>.h` |
| `cxx` | needs C++ link (duckdb) |
| `whole_archive` | force-load, no anchor symbol (tui) |
| `base_group` | wrap with platform lib in `--start-group` on GNU ld (sqlite/postgres/mysql/duckdb) |
| `hook` | weak hook symbol the archive strong-overrides (backend features) |
| `extra_libs` | link libs that cannot live in a `.a` (duckdb `-lstdc++`, gpu frameworks) |
| `cpe` / `license` / `url` / `role` | SBOM + `feature.c` metadata |

These fields already exist informally: `FEATURE_SPECS` in build.lua carries
`backend`/`type`/`hook`/`base_group`/`whole_archive`/`cxx`; the SBOM (as of the
v0.9.0 prep) carries `tier`/`feature`/`cpe`/`license`/`url`/`role`. The record
unifies them.

## Makefile layout

```
Makefile          # variables + `include mk/*.mk` + top-level targets only
mk/
  toolchain.mk    # CC detection, CFLAGS, hardening probes, sanitizer stamp
  flags.mk        # HL_ENABLE_* / HL_*_FEATURE flags + derivation (HTTP_ANY, LINK_TLS, ...)
  sources.mk      # source/object lists (CAP_OBJS, runtime objs, vendor objs)
  feature.mk      # the `define-feature` template macro
  features.mk     # the feature records (data) + `$(foreach ... $(eval $(call define-feature,...)))`
  features/
    http.mk keel.mk tls.mk wasm.mk image.mk sqlite.mk tui.mk runtime.mk
    duckdb.mk postgres.mk mysql.mk gpu.mk      # feature-specific quirks only
  base.mk         # libhull_platform.a, SLIM base composition, cosmo
  embed.mk        # embedded_*.h aggregation + BUILD_ASSET_OBJ
  release.mk      # print-feature-* helpers, platform-cosmo, sign targets
  tests.mk        # test discovery + e2e phonies
```

**Coarse split first (readability), template second (DRY).** Each
`mk/features/<name>.mk` sets the feature-specific vars (`FEAT_<name>_CORE_OBJS`,
quirks) and calls the shared macro; the mechanical archive/embed/phony rules
come from `define-feature`. A fragment self-registers into master lists
**guarded by its gate**, so an off feature contributes nothing:

```make
# mk/features/image.mk
FEAT_image_CORE_OBJS := $(BUILDDIR)/cap_image.o $(BUILDDIR)/cap_image_stb.o $(BUILDDIR)/stb_impl.o
FEAT_image_RT        := lua js          # per-runtime bridges (mod_image)
FEAT_image_EMBED     := 1               # embedded_image.h
$(eval $(call define-feature,image))
```

### Build on the existing `AR_FEATURE_LIB` (do not re-invent the ar body)

The Makefile already has a `define AR_FEATURE_LIB` macro used by **17** archive
rules; it encapsulates the `ar rcs` body AND the `TRUST_FEATURE_LIBS=1`
release-stage-3 branch (trust a pre-built signed artifact instead of rebuilding).
So the recipe body is *already* factored - the real duplication is the rule
**scaffolding** around it. `define-feature` generates the scaffolding and calls
`AR_FEATURE_LIB`; it never re-implements the ar step.

### Three archive-rule shapes (the macro is not one-size-fits-all)

1. **Simple** (`$(call AR_FEATURE_LIB,$(FEAT_<NAME>_CORE_OBJS))`) - 17 rules
   (http, keel, tls, wasm, image, lua, js, postgres, mysql, tui, and the
   per-runtime bridges). `define-feature` covers these.
2. **Inconsistent** - `sqlite`'s rule currently uses a raw `$(AR) rcs` instead of
   `AR_FEATURE_LIB` (Makefile ~line 3054), so it **bypasses the
   `TRUST_FEATURE_LIBS` trust branch** that `feature-sqlite` relies on at release
   stage 3. Routing it through the macro during the refactor **fixes** this latent
   inconsistency - a correctness payoff, not just tidiness.
3. **Bundle** - `duckdb` and `gpu` (only these two) merge vendored static
   archives (`$(DUCKDB_ARCHIVES)` / `$(WGPU_LIB)`) via a `mktemp -d; for a in ...;
   ar x; ar rcs` recipe. This is a distinct shape; add a small
   `define-feature-bundle` variant (the two differ only by the archive-list var),
   or keep the two recipes hand-written in `duckdb.mk` / `gpu.mk`.

### `define-feature` macro contract

`$(call define-feature,NAME)` reads `FEAT_<NAME>_*` vars and emits:

- `$(BUILDDIR)/libhull_feature-NAME.a` via `$(call AR_FEATURE_LIB,...)`
  (`base_group`/`cxx` are link-time quirks consumed by build.lua at compose, not
  by the ar step)
- `feature-NAME:` phony target (what release.yml invokes)
- one bridge archive rule per `FEAT_<NAME>_RT` runtime
  (`libhull_feature-NAME-<rt>.a`)
- when `FEAT_<NAME>_EMBED`, an `EMBEDDED_NAME_H` rule + append to
  `BUILD_ASSET_DEPS`
- appends the archive(s) to `FEATURE_LIBS` and (for embedded kinds) the header
  to `EMBED_HEADERS`

Feature-specific quirks stay explicit parameters, never hidden in the macro:
`FEAT_<NAME>_CXX`, `_WHOLE_ARCHIVE`, `_BASE_GROUP`, `_EXTRA_LIBS`, `_HOOK`.

GNU Make `define`/`$(eval)`/`$(call)` is the mechanism; it is kept to the
mechanical rule-emission only. Anything conditional or quirky (the bundle shape,
cosmo) stays as plain Make in the per-feature fragment, where a misfire is
legible.

## build.lua + feature_compose.lua

- Collapse the ~11 `resolve_*_lib` clones in `feature_compose.lua` into one
  data-driven `M.resolve(spec, rt, tmpdir, ctx)` over a `COMPOSE_SPECS` table
  (the Lua mirror of the feature record). Per-feature quirks
  (`whole_archive`, `base_group`, per-rt bridge) become table fields.
- Extend build.lua's `FEATURE_SPECS` to the full registry (auto + with), or
  generate it from the shared record.
- Break `main()` into named stages: `discover` → `resolve_manifest` →
  `compose_features` → `sign` → `link`. Each stage is independently readable
  and testable.

## Cross-file single source of truth

The unification the scope decision asked for:

- **release.yml** stops hardcoding the feature lists (4×). A
  `make print-feature-targets` / `print-feature-libs` prints the canonical
  set from `mk/features.mk`; release.yml consumes that (a generated step
  input, or a `make`-driven job). Adding a feature never touches release.yml.
- **feature.c** `FEATURES[]` (the `--with` install catalog) and build.lua
  `FEATURE_SPECS`: either generate both from the shared record, or keep the C
  table and add a CI parity check (`make print-with-features` vs the compiled
  `FEATURES[]`) that fails when they drift. Codegen is cleaner; the parity
  check is lower-risk. Start with the parity check, move to codegen if it earns
  its keep.

Net: **add a feature = one record + one `mk/features/<name>.mk`** (for any
feature-specific link quirk). Makefile rules, build.lua compose, release
enumeration, and the install catalog all follow.

## Phasing (each phase independently green)

**Structure precedes template: Phase 1 (split) before Phase 2 (macro).** The
split is the order-sensitive, higher-risk step (see the `make -pn` gate below);
keeping it a *verbatim relocation* isolates that risk and makes it exactly
verifiable. Applying the macro is trivial once each feature lives in its own
fragment, and doing bulk macro conversions in the 5300-line monolith would
re-create the scattered-hunt problem the split exists to remove.

0. **Design + macro PoC.** This doc + `define-feature-archive` proven on one
   feature (image: core + 2 bridges) with `make` green + `e2e-feature-image`
   passing. This step is an *out-of-order mechanism proof* - it applies the
   macro in the monolith to de-risk the machinery. It is NOT Phase 2 starting;
   the monolith `eval` call relocates into `mk/features/image.mk` in Phase 1.
1. **Physical split (verbatim).** Move the coarse sections into `mk/*.mk` via
   `include` at their exact original positions, AND move each feature's rules
   into `mk/features/<name>.mk`, unchanged. No new templating beyond the Phase 0
   PoC. `make check` green + `make -pn` diff clean after each move.
2. **Featurize.** Within each fragment, convert the hand-written archive rules to
   `$(call define-feature-archive,...)` (+ normalize sqlite's raw-ar → the macro,
   fixing the F5 `TRUST_FEATURE_LIBS` bypass; add `define-feature-bundle` for
   duckdb/gpu). One feature per commit, `make && make test` after each.
3. **build.lua.** `COMPOSE_SPECS` + generic resolver; `main()` stages.
4. **Cross-file.** `print-feature-*` helpers; release.yml consumes them;
   feature.c parity check (or codegen).

## Validation (behavior-preserving)

- **Resolved-value diff (the ordering gate).** The hazard is *evaluation order*,
  not just output: `CFLAGS` is `+=`-accumulated across the whole file (from
  ~line 245, through the flags block, and beyond), so a fragment `include`d at
  the wrong position silently reorders flags in a way an archive diff would not
  catch. Every `mk/*.mk` that touches `CFLAGS` (or any `+=`/`override` variable)
  must be included at its **exact original textual position**. Gate: dump the
  *resolved values* of the moved/affected variables with
  `make -pn 2>/dev/null | grep -E '^(CFLAGS|HL_...) '` before and after, and diff
  (normalize the `-DHL_VERSION` git-describe string, which carries a `-dirty`
  suffix once the tree has uncommitted edits). Do NOT diff the *whole* `make -pn`
  dump: touching the Makefile marks the tree stale, so the dry-run leaks rebuild
  recipe echoes into the dump and drowns the real signal. Follow with a full
  `make` (which must reach "Nothing to be done" on a second run) + the e2e
  suite.
- `make` (default), `make platform`, `make check` (clean + ASan + test + e2e),
  `make self-build` (reproducible hull→hull2→hull3).
- `make e2e-feature-runtime / -tui / -wasm / -image / -duckdb / -gpu`,
  `make e2e-build-flavor` (pure-compute × runtime, symbol-level Keel/http drop).
- `nm` symbol-set diff on `libhull_platform.a` and each `libhull_feature-*.a`
  before/after: the object membership of every archive must be identical.
- A pre-release **tag dry-run** (hyphenated pre-release tag) before merge, so
  the full multi-arch release matrix is exercised on the refactored build.

## Invariants

- No behavior change: same objects in same archives, same link order, same
  reproducibility. The refactor is a move + a template, not a redesign.
- The cosmo path is preserved: a fat APE keeps everything in-base; features are
  native-only. `mk/base.mk` owns the cosmo branch; feature fragments are
  native-only and simply not composed on cosmo.
- An off feature contributes zero rules, objects, and embed bytes.
- The SLIM app-build base (`HL_APP_BASE_SQLITELESS=1 HL_APP_BASE_TLSLESS=1` +
  Keel-less) stays the shipped default; the split does not change what the
  distributed hull embeds.
- **Ordered includes.** `mk/*.mk` fragments are `include`d at their original
  textual positions, never reordered; `CFLAGS`/`+=`/`override` accumulation
  order is load-bearing (see Validation). The existing tail `-include
  $(DEPS_ALL)` (the `.d` auto-deps) and `-include $(SANITIZER_STAMP)` stay in
  the root Makefile at their positions and coexist with the new includes.

## What remains in the root Makefile (~4.7k lines) and why

After the 13 feature fragments + `mk/{flags,feature,hardening}.mk`, the root
Makefile is ~4.7k lines. It is NOT more feature-extractable - what is left is the
BASE build machinery, which falls into three modularizability classes:

| Lines | Bucket | Further modularizable? |
|------:|--------|------------------------|
| ~220  | toolchain / CFLAGS base / sanitizer / version / dirs | **No** - `CFLAGS :=`/`+=` accumulation root; everything downstream depends on order |
| ~1140 | vendored-lib configs (QuickJS, Lua, Keel, mbedTLS, SQLite, log.c, sh_arena, sh_json, TweetNaCl, stb, unicode, **WAMR**, LTO, CFI, wgpu, DuckDB, pledge, CA-bundle, pwned, HTMX) | **Partly** - a `mk/vendor/<lib>.mk` per lib is possible, but each carries a `CFLAGS +=` so it's order-sensitive like the coarse sections |
| ~550  | Hull source-file object lists (`CAP_OBJS`, `RT_OBJS`, all `*_OBJ`) + compile pattern rules (`%.o: %.c`) | **No** - the central object registry + the ~10 pattern rules every archive/link consumes |
| ~440  | stdlib/context/asset/app embedding (xxd) + registries | Partly (a `mk/embed-stdlib.mk`), low value |
| ~85   | include paths + build-flag fingerprint | No - tiny, central |
| ~180  | **platform-lib composition + the `hull` link rule** | **No - this is the assembly point.** `PLATFORM_OBJS` and the single `cc -o build/hull ...` line reference *everything*; splitting them loses the one-place-to-see-the-link property |
| ~295  | base variants (SQLite/TLS/Keel-less + SLIM) + wamrc | No - they re-invoke the base build with flag overrides; inherently central |
| ~83   | embedded build assets (`embedded_platform.h`, `embedded_templates.h`) + `BUILD_ASSET_OBJ` | No - the aggregation point the feature embeds feed into |
| ~500  | libhull no-runtime embedding library | Yes → a `mk/libhull.mk` (self-contained), a reasonable future extraction |
| ~760  | debug / tests / msan / fuzz / e2e | Yes → `mk/tests.mk`, but weak gate (rules, no value-diff) + `TEST_COMMON_*` shared with bench |
| ~445  | self-build / repro / check / analysis / bench / coverage / lint / fetch / docs / clean | Partly → `mk/dev.mk` (dev/CI targets), low value |

**Why the core cannot shrink much further.** Three structural facts:
1. **The `CFLAGS`/`LDFLAGS` accumulation is a single ordered pipeline.** Anything
   touching it can only move as a whole block at its exact position (proven with
   `flags.mk` / `hardening.mk`); you cannot scatter it into per-topic files
   without a reorder hazard.
2. **The link + platform-lib composition is an assembly point by nature.** Its
   value is that one `PLATFORM_OBJS` list and one `hull` link line name every
   object; fragmenting them trades legibility for file count.
3. **The compile pattern rules + object registry are the shared substrate** every
   fragment already references; they belong in one place.

The genuinely-extractable remainder (`libhull.mk`, `tests.mk`, per-vendor
configs) is *tidying*, not the feature/flavor axis - lower value, weaker gates.

## Platform axis (Darwin / Linux / Cosmopolitan / future Windows)

Orthogonal to the feature axis. Platform conditionals split two ways, and the
split dictates where each belongs:

- **Platform-GLOBAL policy → `mk/platform/<os>.mk`:** OS build policy that is not
  tied to any one vendor or feature - today just the **sandbox backend**. A
  computed `PLATFORM := darwin|linux|cosmo` at the top of the Makefile (COSMO
  checked first, since a cosmo build reports `UNAME_S=Linux`) selects
  `include mk/platform/$(PLATFORM).mk`.
- **Feature-LOCAL platform choices → stay in the feature/vendor fragment:** gpu's
  `WGPU_FRAMEWORKS` (Metal vs Vulkan) in `mk/vendor/wgpu.mk`,
  WAMR's per-OS `platform_init`, duckdb/tui cosmo-exclusion. Moving
  these into a platform file would fragment cohesion - the Metal-vs-Vulkan choice
  belongs *with* gpu, not in `darwin.mk`. **This is the key design call: the
  platform file holds OS policy, not a feature's per-OS wiring.**

**What the vendor/feature extraction already achieved.** By the time this axis
landed, the earlier `mk/vendor/*` + `mk/features/*` work had *already* moved
almost every platform conditional out of the root **with its concern** (WAMR
per-OS wiring, wgpu frameworks, TUI cosmo force-load). What
remained platform-GLOBAL in the root was a single block: the **jart/pledge
polyfill** (the Linux-only `pledge()`/`unveil()` implementation). There are no
per-OS link libs left in the root (the hull link is the universal
`-lm -lpthread`), and no darwin/cosmo-global blocks. So the axis is small by
design - the modularization did the heavy lifting.

Concretely:
- **`mk/platform/linux.mk`** holds the `PLEDGE_*` vars + the `pledge_%.o` compile
  rule (no `ifeq(Linux)/ifndef(COSMO)` guard - the file is only included when
  `PLATFORM=linux`). `PLEDGE_OBJS ?=` in the root defaults it empty elsewhere, so
  the hull link references it unconditionally.
- **`mk/platform/{darwin,cosmo}.mk`** are thin seams: macOS uses seatbelt (applied
  in `sandbox.c`, no compiled polyfill) and cosmo has pledge/unveil built in, so
  neither needs Makefile content today. They document where any future OS-global
  policy goes.
- **`mk/platform/unknown.mk`** is the empty fallback for an unrecognized `uname`.

`mk/platform/windows.mk` is a **stub**: Hull already runs on Windows via the
cosmo APE (one binary, no native build), so there is no native-Windows toolchain
yet. The stub documents the seam a future native port fills - a Windows sandbox
backend (Restricted Tokens / AppContainer), `VirtualAlloc`/`VirtualProtect` for
the sealed arena (see docs/security.md §5b), and the MSVC/clang-cl link flags.

**Status: implemented.** `PLATFORM` detection + `include mk/platform/$(PLATFORM).mk`
are live; pledge moved to `linux.mk` verbatim. Validated by resolved-value parity
(PLATFORM + `PLEDGE_OBJS` identical to the old inline `ifeq` under simulated
`UNAME_S=Linux`, `COSMO=1`, and native Darwin) plus the full CI matrix.

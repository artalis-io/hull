# Build modularization: one record per feature

**Status:** design (approved approach: hybrid mk/ split + `define-feature`
macro; cross-file registry unified). Lands **after v0.9.0**.

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

### `define-feature` macro contract

`$(call define-feature,NAME)` reads `FEAT_<NAME>_*` vars and emits:

- `$(BUILDDIR)/libhull_feature-NAME.a` (ar of `CORE_OBJS`; C++/`base_group`
  variants selected by the quirk vars)
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
mechanical rule-emission only. Anything conditional or quirky stays as plain
Make in the per-feature fragment, where a misfire is legible.

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

0. **Design + PoC.** This doc + `define-feature` proven on one representative
   feature (image: core + 2 bridges + embed) with `make` green. De-risks the
   macro before touching the other 13.
1. **Physical split.** Move coarse sections into `mk/*.mk` via `include`,
   behavior-preserving. No template yet. `make check` green.
2. **Featurize.** Convert each feature to `mk/features/<name>.mk` +
   `define-feature`. One feature per commit, `make && make test` after each.
3. **build.lua.** `COMPOSE_SPECS` + generic resolver; `main()` stages.
4. **Cross-file.** `print-feature-*` helpers; release.yml consumes them;
   feature.c parity check (or codegen).

## Validation (behavior-preserving)

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

# Runtime feature publishing - design

Wiring the `lua` / `js` runtime feature archives (`libhull_feature-lua-<arch>.a`,
`libhull_feature-js-<arch>.a`, built by `make feature-lua` / `feature-js`) into
`hull feature list` and the release pipeline. Follow-on to the runtime-feature
epic ([runtime_feature_phase3.md](runtime_feature_phase3.md)).

> **DECISION (2026-07-26): make runtimes a true composable feature, DuckDB-style
> - a runtime-less base + compose exactly one runtime.** This supersedes the
> "embedded dual base + dead-strip" analysis below. The slim shipped via
> dead-strip (the base links both runtimes, the app drops one); the maintainer
> wants the principled model instead: **the base platform lib carries no
> runtime**, and a produced app **composes exactly one** runtime archive to be
> runnable (which one is free - inferred from the entry extension, or forced with
> `--with=lua` / `--with=js`). It is *like* DuckDB in the composition mechanism
> (a signed feature archive, `--with`, `hull feature install` for source builds),
> and *unlike* DuckDB in that exactly-one is **mandatory** (a runtime-less binary
> can't run an app), so the two archives are **embedded in the distributed `hull`
> + auto-composed** (a fresh `hull build app.lua` must work with no
> `hull feature install`). The archives thus become **real compose consumers**,
> which is the case for publishing them. The pivot from dead-strip to
> runtime-less base reuses nearly all the shipped machinery (empty `g_factories`,
> per-app generated registry, the archives, hook-driven resolution, weak stubs);
> see "Pivot: dead-strip -> runtime-less base" at the end. The analysis below is
> kept for the reasoning; where it says runtimes are "not fetched / not composed",
> read it as the pre-decision state.

## The reconciliation problem (read this first)

Runtimes are **not** like the other composable features, and the design turns on
one honest fact about how the slim actually shipped:

- **DuckDB / postgres / mysql / gpu / tui** are *install-on-demand*: off by
  default, fetched with `hull feature install <name>`, composed with
  `hull build --with=<name>`. The archive is the delivery vehicle.
- **`lua` / `js` are embedded and auto-composed.** The distributed `hull` binary
  carries **both** runtimes (the base platform lib is dual). `hull build app.lua`
  produces the slim by **dead-stripping** the unused runtime at app-link time
  (via the per-app generated `app_feature_registry.o` that references only the
  entry's runtime), *not* by fetching an archive. There is **no fetch consumer**
  in the common path: a user never runs `hull feature install lua`, and
  `hull build` needs no runtime archive on disk.

So the runtime archives, as the epic shipped, are **not consumed by any build
path**. That is the central design tension: why publish an archive nobody fetches?

### Why publish anyway (the case for doing this)

Three real reasons, none of them the common `hull build` path:

1. **Provenance + signature symmetry.** Every other `libhull_feature-*.a` is in
   the signed `hull.sha256` release manifest. Publishing the runtime archives puts
   the *exact runtime bits a release was built from* under the same Ed25519
   signature - auditable, reproducible-build-checkable, and consistent with the
   feature story rather than a special-cased hole.
2. **The runtime-less-base future.** The epic reached the slim via dead-strip on
   a *dual* base. A later, stronger slim - a genuinely **runtime-less base
   flavor** (`libhull_platform-runtimeless-<arch>.a`, neither interpreter linked)
   - would compose exactly one runtime by *force-loading* a runtime archive, the
   way `--with=duckdb` composes DuckDB. That flavor is the archive's real fetch
   consumer. Publishing now means the flavor lands without a new
   release-pipeline change, and the archives are already trusted.
3. **Source / custom builds.** Someone building `hull` from source, or assembling
   a bespoke single-runtime toolchain, can `make feature-lua` and verify it
   against the published, signed archive.

If none of those are wanted, the alternative is **Option 0: don't publish** -
treat the archives as a build-time artifact of `make feature-lua` and delete the
`hull feature` / release wiring entirely. That is cleaner but forfeits the
provenance + the runtime-less-base runway. **This design chooses to publish**,
with UX that never misleads a user into thinking they must install a runtime.

## Design: a second feature *kind* - `embedded`

The install-on-demand assumption is baked into `FEATURES[]`
(`src/hull/commands/feature.c`) and its `list` / `install` / `uninstall`
handlers. Add a **kind** discriminator rather than pretend runtimes install like
DuckDB.

```c
typedef enum {
    HL_FEATURE_INSTALLABLE = 0,  /* duckdb/postgres/mysql/gpu/tui: fetch on demand */
    HL_FEATURE_EMBEDDED,         /* lua/js: in hull already, auto-composed */
} HlFeatureKind;

typedef struct {
    const char   *name;
    const char   *description;
    HlFeatureKind kind;
    int has_linux_x86_64, has_linux_aarch64, has_darwin_arm64;
} HlFeatureSpec;

static const HlFeatureSpec FEATURES[] = {
    { "duckdb",   "...", HL_FEATURE_INSTALLABLE, 1,1,1 },
    /* ... postgres, mysql, gpu, tui ... */
    { "lua", "Lua 5.4 runtime (embedded; auto-composed for app.lua)",
      HL_FEATURE_EMBEDDED, 1,1,1 },
    { "js",  "QuickJS runtime (embedded; auto-composed for app.js)",
      HL_FEATURE_EMBEDDED, 1,1,1 },
};
```

### `hull feature list`

Embedded features report a distinct, non-actionable status - the exact analog of
`hull flavor list` showing the full build as `embedded`:

```
Features (platform: darwin-arm64, cache: ~/.hull/feature):
  duckdb   DuckDB embedded OLAP SQL backend (duckdb://)    not installed (hull feature install)
  gpu      GPU compute via wgpu-native (Vulkan/Metal)      installed
  lua      Lua 5.4 runtime (embedded; auto-composed)       embedded (auto-composed)
  js       QuickJS runtime (embedded; auto-composed)       embedded (auto-composed)
```

`cmd_list` branches on `kind`: an `HL_FEATURE_EMBEDDED` row prints
`embedded (auto-composed)` and never touches the cache dir.

### `hull feature install <runtime>` / `uninstall <runtime>`

Not an error - an **informative redirect**, exit 0:

```
$ hull feature install lua
hull feature: lua is a runtime - it is embedded in hull and auto-composed from
the app entry extension (app.lua). There is nothing to install. A slim,
single-runtime app is produced automatically by `hull build app.lua`.
```

`uninstall` is symmetric ("runtimes are embedded; nothing to uninstall"). This is
the one place the reconciliation is user-visible; the message must say *why*
(embedded + auto-composed) so it reads as intentional, not a missing feature.

`cmd_install` / `cmd_uninstall` gain an early `kind == HL_FEATURE_EMBEDDED`
branch before any network / cache work.

### `hull build --with=lua` / `--with=js`

Kept meaningful but re-pointed at the dead-strip model (no archive fetch):

- **Default (no `--with`):** auto-infer the runtime from the entry extension;
  the produced app keeps exactly that runtime (today's shipped behaviour).
- **`--with=lua --with=js` (both):** produce a **dual-runtime** app - the per-app
  registry references both factories, so neither interpreter dead-strips. This is
  "full" as an app property. Useful for an app that must run both (rare) or to
  opt out of the slim.
- **`--with=<the entry's own runtime>`:** a no-op (already composed); accepted
  for symmetry/scriptability.
- **`--with=<the other runtime>` only** (e.g. `--with=js` on an `app.lua`):
  reject at build time - an app has one entry; composing only the non-entry
  runtime cannot run it. Clear fix-it.

Crucially, `--with=lua/js` here drives the **generated registry** (which factory
symbols the app references), *not* an archive fetch. The `feature-lua/js`
archives are not read by this path. (A future runtime-less-base flavor is the
path that *would* force-load them; see below.) This is worth a sentence in the
`--with` help so it is not conflated with `--with=duckdb`.

## Design: release pipeline

Mirror the existing `build-feature-<name>` jobs (release.yml ~744-972). Two new
jobs, each a 3-way native matrix (`linux-x86_64`, `linux-aarch64`,
`darwin-arm64`), no cosmo (features are native-only static archives; the cosmo
APE embeds both runtimes and never slims):

```yaml
  build-feature-lua:
    name: Build feature lua (${{ matrix.name }})
    # ... same runner matrix as build-feature-tui ...
    steps:
      - ... checkout + submodules + deps ...
      - name: Build Lua runtime feature archive
        run: make feature-lua
      - run: mv build/libhull_feature-lua.a libhull_feature-lua-${{ matrix.name }}.a
      - uses: actions/upload-artifact@v4
        with:
          name: feature-lua-${{ matrix.name }}
          path: libhull_feature-lua-${{ matrix.name }}.a
  build-feature-js:
    # identical, s/lua/js/
```

Wire into the `release` job:
- Add `build-feature-lua`, `build-feature-js` to its `needs:` list.
- Add `mv artifacts/feature-{lua,js}-<arch>/*.a dist/` staging lines (mirroring
  the duckdb/gpu/tui block ~1023-1049).
- **No manifest / publish change needed:** the SHA-256 + sign step already globs
  `dist/libhull_feature-*.a` (release.yml ~1135) and the publish step already
  globs `dist/libhull_feature-*.a` (~1242). The runtime archives fall in
  automatically, under the same signature.

Size note: these are the two **largest** feature archives (`lua` ~1.8 MB, `js`
~2.2 MB, vs postgres/mysql ~48 KB) - they carry a vendored VM. Six new release
assets (2 runtimes x 3 native arches). Acceptable; they are the runtime bits,
signed.

`tests/release_smoke.sh` gains a runtime-feature check: after install of the
release, assert `hull feature list` shows `lua` / `js` as `embedded`, and (if the
runtime-less-base flavor exists) that a fetched archive verifies. Until that
flavor lands, the smoke check is just the `embedded` status + that the signed
archives are present in the release.

## Trust chain

No new keys, no new mechanism. The runtime archives ride the **same** signed
`hull.sha256` (Ed25519 release key) as every other feature/flavor/tool asset. If
`hull feature install lua` were ever wired (it is not - it redirects), it would
reuse `hl_release_io_fetch_verified_manifest` exactly like the installable
features. The runtime-less-base flavor's build-time re-verify would reuse
`hl_release_io_verify_local_asset` (the same install-to-build TOCTOU close the
flavored/featured builds already use).

## What this design deliberately does NOT do

- **Does not make runtimes install-on-demand.** They stay embedded + auto-composed.
  `hull feature install lua` informs, never fetches.
- **Does not build the runtime-less-base flavor.** That is the archive's real
  fetch consumer and a separate epic (a `libhull_platform-runtimeless-<arch>.a`
  flavor + `--with=<runtime>` force-load). This design only makes its archives
  exist, signed and listed, so that epic is pure addition.
- **Does not publish cosmo runtime archives.** The APE embeds both = full.

## Decision surface (for the maintainer)

1. **Publish at all?** This design says yes (provenance + runtime-less-base
   runway). Option 0 (don't publish, delete the `hull feature`/release wiring) is
   the honest alternative if that runway is not wanted - the slim already works
   without these archives.
2. **`embedded` status wording** in `hull feature list` and the install redirect.
3. Whether to add the runtime rows to `hull feature list` at all, or keep runtimes
   entirely out of the `feature` surface and document the slim only under
   `hull build` / `docs/features_and_flavors.md`. (Listing them is more
   discoverable; hiding them keeps `hull feature` strictly install-on-demand.)

## Implementation checklist (once approved)

1. `feature.c`: `HlFeatureKind`, two `FEATURES[]` rows, `cmd_list` /
   `cmd_install` / `cmd_uninstall` `embedded` branches.
2. `build.lua`: `--with=lua/js` semantics (both = dual app; non-entry-only =
   reject; help text noting no fetch).
3. `release.yml`: `build-feature-lua` / `build-feature-js` jobs + `needs` + dist
   staging.
4. `release_smoke.sh`: `embedded` status assertion.
5. Docs: `features_and_flavors.md` taxonomy note (runtimes = embedded feature,
   the sixth/seventh rows with a distinct kind); update the
   `docs/features_and_flavors.md` feature table.

## Pivot: dead-strip -> runtime-less base (the DECISION, concretely)

The shipped slim links both runtimes into the base platform lib and lets the app
dead-strip the unused one. The decision is to make the base **runtime-less** and
have the app **compose** its one runtime. Same slim outcome; principled model.
Nearly all the machinery already exists - this is mostly moving objects and
teaching `hull build` to force-load one runtime archive.

**What changes:**

1. **Runtime-less base platform lib.** Drop from `PLATFORM_OBJS`: `RT_OBJS`,
   `VEND_OBJS` (the VMs), `STDLIB_RT_REGISTRY_OBJS`, `manifest_lua/js.o`, and the
   toolchain registries. `libhull_platform.a` then links no runtime; a bare base
   cannot run an app (like the GPU base ships only the dispatch layer). Introduce
   `HULL_CORE_OBJS` here (phase2 doc Change 3) so `libhull.a` and the base share
   the kernel.
2. **The `hull` toolchain stays dual, unchanged.** `hull` keeps linking both
   `RT_OBJS`/`VEND_OBJS` directly + the toolchain factory/stdlib registries, so
   `hull dev/test` runs either runtime. (Equivalently it could force-load the two
   archives like `HL_TUI_TOOLCHAIN`; linking the objects directly is simpler and
   already works.)
3. **Embed both runtime archives in the distributed `hull`.** Because a runtime
   is mandatory, `hull build app.lua` must compose one with no `hull feature
   install`. Embed `libhull_feature-{lua,js}.a` in the `hull` binary the same way
   `EMBED_PLATFORM=1` embeds `libhull_platform.a` (xxd into `build_assets`), and
   extract the inferred one at build time. Native `hull` embeds runtime-less base
   + both archives = the same bytes as today's dual base; the win is the produced
   *app* is composed-slim, and the model is clean. Cosmo embeds both = full.
4. **`hull build` composes one runtime archive.** Resolve the archive (embedded
   in `hull` -> extracted to tmp; or `~/.hull/feature/` for source builds ->
   re-verified), **whole-archive** it into the app link next to the runtime-less
   base and the generated `app_feature_registry.o` (which already names the one
   runtime's factory + stdlib). Auto-inferred from the entry extension;
   `--with=lua`/`--with=js` forces it; both = a dual app; the non-entry runtime
   alone is rejected. This replaces "dead-strip a dual base" with "link only the
   one runtime" - genuinely slim, no dead-strip reliance.
5. **The weak stubs stay.** App-linked base objects still reference a few
   toolchain-only symbols (`serve.c` -> `hl_agent_api_register`; the composed
   runtime's `lua_rt_runtime.o` -> `hl_lua_tool_register`); the `app_runner.o`
   weak stubs keep those from pulling the tool VM / agent (and, now, an
   *undefined* symbol, since the base no longer carries them).
6. **`hull feature` + release** exactly as designed above, except the status is
   `embedded (auto-composed)` **and** `hull feature install lua` genuinely fetches
   for a **source build** of a runtime-less `hull` (where the archive is not
   embedded) - so the install path is real, not only a redirect. For the shipped
   embedded `hull`, install still redirects ("already embedded").

**What stays (already shipped, reused as-is):** empty `g_factories`, the per-app
`app_feature_registry` (factory + stdlib hooks), the `feature-lua/js` archives
(3c), hook-driven resolution, the slim `hl_app_run` entrypoint, the weak stubs,
`e2e_feature_runtime.sh`.

**Net:** the pivot is (1) a Makefile object-list move (base goes runtime-less +
`HULL_CORE_OBJS`), (2) embedding the two archives in `hull` + extract, (3)
`build.lua` whole-archiving the composed runtime, (4) the `feature.c` +
`release.yml` publishing wiring. The nm invariant (`e2e_feature_runtime.sh`) is
unchanged and must stay green; add an assertion that `libhull_platform.a` itself
has zero VM symbols (the base is genuinely runtime-less), which the dead-strip
build would fail and the runtime-less build passes - the objective proof the
pivot landed.

## Source-tree builds: `make` builds the runtime archives

A distributed `hull` embeds both runtime archives, so `hull build` extracts the
inferred one with no extra step. A **source build** has no embedded copy, so
`hull build` (and `hull eject`) resolve the archive from `build/` (then
`~/.hull/feature/`). To make `make && hull build` work out of the box, a native
`make` builds `RUNTIME_FEATURE_LIBS` (`libhull_feature-lua.a` +
`libhull_feature-js.a`, or the single half for a `RUNTIME=lua|js` build)
**alongside `hull`** - they are an order-only prerequisite of the `hull` target,
so every `make`/`make e2e-*` that produces `hull` also lays the archives in
`build/`. Cosmo has a dual base and builds none. This closes the gap where a
plain `make` produced a `hull` that could not `hull build` until a separate
`make feature-lua feature-js`.

**Compose logic is shared.** The archive-resolution ladder (embedded-extract ->
`build/` -> signed cache, fail-closed re-verify), the whole-archive/`-force_load`
link fragment, and the per-app `app_feature_registry` codegen live in
`stdlib/cli/lua/hull/feature_compose.lua`, used by both `hull build` and
`hull eject`, so the produced-app and ejected-app paths cannot drift.

## `hull eject` composes the runtime (native)

`hull eject` on a native `hull` produces a runtime-less `libhull_platform.a`, so
it must also bundle the app's one runtime archive + a generated
`app_feature_registry.c` and whole-archive-link them in the standalone Makefile
(GNU-ld `--start-group` around the base; `-force_load` on macOS). The output
binary is named after the app (never `app`, which would collide with the `app/`
source directory and make would treat the up-to-date dir as the target). The
ejected trampoline calls `hl_app_run` (the slim app-runner), matching
`hull build`. Cosmo eject keeps the dual base and skips runtime composition.
Covered by `tests/e2e_build.sh` step 17 (build + run the ejected native app).

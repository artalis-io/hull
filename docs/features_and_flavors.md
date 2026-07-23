# Features, flavors, and tools — the distribution model

Status: **shipped.** Introduces **features** as a third distribution concept
alongside **flavors** and **tools**, so Hull can absorb multiple large,
orthogonal optional libraries without a combinatorial explosion of published
build artifacts. Re-scopes the DuckDB side-load (roadmap "DuckDB epic" #4) from a
one-off `full-duckdb` flavor to **DuckDB as the first composable feature**. Five
features ship today: **`duckdb`** (embedded OLAP, `duckdb://`), **`postgres`**
(pure-C PostgreSQL wire backend, `postgres://`), **`mysql`** (pure-C MySQL/MariaDB
wire backend, `mysql://` / `mariadb://`), **`gpu`** (wgpu-native compute), and
**`tui`** (terminal UI) - all native-only, installed with
`hull feature install <name>` and composed with `hull build --with=<name>`. The
command surface (`hull feature install / list / uninstall`), the `make
feature-<name>` archive targets, the `--with=` build path, and the `?`
optional-module fallback are all live.

Companion docs: [build_flavors.md](build_flavors.md) (the subtractive flavor
spectrum), [duckdb_backend_design.md](duckdb_backend_design.md) (the DuckDB
backend itself).

## 1. One primitive (feature); a flavor is a preset over it

At the mechanism level there is **one** kind of thing: a **feature** — a
compile-time capability knob (`HL_ENABLE_*`). `HL_ENABLE_HTTP_SERVER`,
`HL_ENABLE_HTTP_CLIENT`, `HL_ENABLE_DUCKDB`, `HL_ENABLE_WASM` are all the same
kind of thing. **HTTP is not special; it's a feature that happens to be on by
default.** The DB connectors (`sqlite`/`postgres`/`mysql`/`duckdb`) are one
feature family behind `HlDbBackend`, chosen at runtime by DSN scheme.

A **flavor** is *not* a second primitive — it's a **named preset** over
feature-space. `full` = "all default features on"; `pure-compute` = "HTTP
features off"; a `duckdb` preset = "defaults + DuckDB." `hull build --flavor=X`
is sugar over "apply preset X" (conceptually `--feature=+duckdb,-http-server`).
There is no ontological line between flavor and feature: there is **one axis
(features)** and flavors are labels for points on it. The direction of travel is
to make that explicit — one primitive, presets over it, and the word "flavor"
eventually retiring into "preset" (§7).

The distinction earns its keep today as **distribution vocabulary**, not
architecture — because the *shipping units* differ. What a user installs to get
capability without compiling from source is one of three:

| Concept | What it is | Consumed | Published as |
|---|---|---|---|
| **tool** | a separate companion **program** Hull spawns (`wamrc`) | `hl_tool_spawn` at build time | `hull-<tool>-<platform>` binary |
| **flavor** | a named **build config of the base** (a preset of the *default* knobs) | linked into an app by `hull build --flavor` | `libhull_platform-<flavor>-<arch>.a` |
| **feature** | a large **optional subsystem** added on top of a base | linked into an app by `hull build` when the app needs it | `libhull_feature-<name>-<arch>.a` |

`tool` vs the other two is the "is it Hull, or a program Hull runs?" split.
`flavor` vs `feature` is the **subtractive vs additive** split, and that
distinction is the whole point of this document.

## 2. Why flavors don't scale to additive libraries

A flavor is a **named, published, signed build**. That works for the HTTP
spectrum because it's a *subtractive lattice*: `full ⊃ server-only ⊃
pure-compute`. It's small and finite — a handful of points, each a slim of the
default. You can pre-build, sign, and store all of them.

Large optional libraries are the opposite: **additive and orthogonal.** DuckDB
(~58 MB), a headless browser, a graphics lib, an ML runtime — any *subset* could
be wanted. If each is a `full-<lib>` flavor, then two of them means `full-duckdb`,
`full-raylib`, `full-duckdb-raylib`; N of them means **2^N** published builds. The
release matrix (CI build time, signing, storage, download size) explodes. The
`full-<lib>` naming is fine for exactly one blessed add-on and is a dead end at
three.

**So: reserve "flavor" for the subtractive spectrum, and make each large additive
library a "feature."**

## 3. The model: orthogonal axes, composed at build time

Flavors and features are **orthogonal axes**, composed at the user's `hull build`:

- **flavor** picks the *base* (which of the default knobs — the HTTP/TLS profile).
- **feature** adds *optional subsystems* on top of that base.

```
hull build --flavor=pure-compute --with=duckdb   # slim, no HTTP, + DuckDB OLAP
hull build --flavor=full         --with=duckdb   # full web app + DuckDB
hull build --flavor=full                          # today's default
```

The scaling win is the whole reason to do this:

> With **M flavors** and **N features**, you publish **M + N** signed libraries,
> and the user composes any of **M × N** effective builds at link time. Per-combo
> flavors would need M × N *published* artifacts. M + N vs M × N is the difference
> between "add a feature" being an O(1) release change and an O(2^N) one.

### 3.1 Per-feature signed libraries

Each feature ships as its own static lib, `libhull_feature-<name>-<arch>.a`,
built in CI and covered by the same Ed25519-signed `hull.sha256` manifest as
everything else. Fetched on demand:

```
hull feature install duckdb        # fetch + verify + cache to ~/.hull/feature/
hull feature list                  # embedded / installed / not installed
```

Same `hl_release_io_*` trust chain as `hull flavor install` / `hull tools
install` / `hull update` — no new key, no new verifier. **Artifact count is
linear in N**, not exponential.

### 3.2 Registration: build-time-generated const registry (not dlopen, not linker sets)

The hard part is that a feature isn't just "link another `.a`" — the cap layer
has to *know* the new subsystem exists (e.g. `db_select.c`'s backend table must
include the DuckDB backend). Hull is **static-only** (the hardening bans
`dlopen`), so a feature cannot be a runtime plugin; it must be wired in at link
time. Two ways to do that, and Hull picks the portable one:

- **Rejected — linker sets.** Have each feature lib drop a `const` descriptor
  into a named section (`__start_/__stop_` on ELF, `section$start$…` on Mach-O)
  that the base iterates. It works, but the boundary-symbol magic differs per
  platform, is unproven under cosmo's `apelink`, and a custom-named section can
  quietly land outside RELRO — reintroducing a writable function-pointer table
  (exactly what [security.md §4b](security.md) forbids).

- **Chosen — generated registry.** `hull build` already generates sorted `const`
  registries at build time (`app_registry.c`, `stdlib_registry.c`). Extend that:
  when the build composes a feature, it emits a small `feature_registry.c` that
  `extern`-declares the feature's descriptor (provided by the feature lib, e.g.
  `extern const HlDbBackend hl_db_backend_duckdb;`) and assembles the final
  `const` backend/cap table the base consumes. The base platform lib is
  refactored to read an **externally-provided** table (`hl_db_backends()`) rather
  than a compile-time `#ifdef`'d `BACKENDS[]`; the base always contributes SQLite,
  and each composed feature appends its own.

The generated registry is plain C codegen: fully portable (ELF / Mach-O / cosmo
alike), lands in `.rodata` / `.data.rel.ro` like every other Hull dispatch table
(W^X- and CFI-safe), and needs no section attributes or `--gc-sections` care. It
is the natural evolution of the `#ifdef`'d `BACKENDS[]` in `cap/db_select.c`.

### 3.3 Selection: manifest-inferred, `--with` to override

Which features a build needs is derived, not hand-listed, mirroring
`--flavor=auto` + the module resolver:

- **Module-backed features** (a future `hull/graphics` → raylib, `hull/browser`
  → a browser engine): inferred from the app's declared `modules`.
- **DB-connector features** (`duckdb`): inferred from the DSN schemes the app
  declares in `manifest.databases` (a `duckdb://` connection ⇒ the `duckdb`
  feature).
- `hull build --with=duckdb,raylib` is the explicit override / addition.

The build **validates**: if a required feature's lib isn't installed (or built
from source), the build fails with a `hull feature install <name>` hint -
exactly like the missing-flavor-lib path today. A cache-sourced feature lib is
re-verified against its signed manifest before linking (the same
`hl_release_io_verify_local_asset` TOCTOU close as flavored builds).

## 4. The dev loop: `hull dev` self-links a cached feature runtime

A pre-built lean `hull` **cannot statically compose a feature at runtime** — no
`dlopen`, no way to grow the linked set after the fact. So a feature has to be
*linked in*. The good news is that `hull` already knows how:
`libhull_platform.a` **is** the full runtime (Keel + Lua/JS + caps + sandbox —
everything except `main`/commands), and `hull build` already links it to produce
standalone binaries. "Get a runtime with the feature" is `hull build` aimed at
the dev loop.

**The mechanism.** When `hull dev` (or `hull <app>` / `hull test`) sees the app
needs a feature the running binary lacks, it: (1) fetches + verifies the signed
`libhull_feature-<name>.a` (the §3.1 trust chain); (2) **links a dev-runtime
binary** = embedded platform lib + feature lib + a dev `main` + the generated
feature registry (§3.2); (3) **caches** it in `~/.hull/` keyed by *feature-set +
hull version* (like the AOT / bytecode caches), so the link is paid once *ever*,
not per session; (4) re-execs into it. From there the dev loop is normal:
**app source hot-reloads in-process**, because the split falls along the
change-frequency line — the **feature is C** (links once, never changes mid-
session) and the **app is Lua/JS** (changes constantly, re-evals in-process).

**This does not touch the `dlopen` ban.** Hull isn't loading code into a live
process — it produces a *new statically-linked binary* and `execv`s. Static link
+ re-exec, W^X and CFI intact. It's exactly what `hull build` does, aimed at a
dev runtime instead of a shipping artifact.

**Caveats.** It needs a linker at dev time (embedded tcc or system cc) — the same
requirement as `hull build`, so "feature dev" isn't zero-toolchain the way
pure-Lua `hull dev` is today. The produced dev binary is a **local, ephemeral**
artifact (developer-owned, like any `hull build` output); its *inputs* (platform
lib + feature lib) are still release-signed + re-verified, so no trust hole
opens.

This scales to **any** feature combo with nothing extra published — strictly
better than pre-publishing a handful of "loaded" runtime binaries (which would
reintroduce the combinatorial matrix). It is a **later enhancement**, not part of
a feature's v1: v1 ships **build-then-run** (`hull build --with=…`, run the
binary), and the self-linking dev loop lands once the feature machinery exists
(see §6, Phase 2).

## 5. Distribution is uniform; integration is not

The mechanism in §3 is identical for every feature. **Integrating** each library
is not — vet each against Hull's capability + sandbox model *before* committing,
because some may not fit at all:

- **DuckDB** — easiest, and the first feature. It's a DB *connector*: fits the
  existing `HlDbBackend` vtable + DSN selection, pure compute, no new sandbox
  surface. (The one real interaction — glibc `rseq` on its worker pool under the
  pledge sandbox — is already solved; see [duckdb_backend_design.md §3.2](duckdb_backend_design.md).)
- **GPU (wgpu-native)** - **shipped as the second feature.** Unlike raylib it's
  *headless compute*, not windowing: it fits the existing `HlGpuBackend` vtable,
  needs no new cap domain, and its sandbox surface (`/dev/dri` unveil on Linux,
  `iokit-open` + `MTLCompilerService` on macOS) is gated on the manifest
  `gpu = true` flag. The base ships the generic gpu dispatch layer + a weak
  `hl_gpu_feature_backends` hook; `libhull_feature-gpu.a` fills the concrete wgpu
  backend. No symbol isolation was needed (wgpu shares no symbols with Hull).
  `hull feature install gpu` / `hull build --with=gpu`; native-only.
- **raylib / a graphics lib** — a *new capability domain*: windowing, input, a GL
  context. Needs a new `graphics.*` cap, display/GPU sandbox access (`/dev/dri`),
  and it's a GUI model unlike Hull's headless server/CLI shape. The lib links;
  the cap + sandbox design is the real work.
- **A headless browser (Chromium-class)** — the hardest, and possibly *out*. It
  is **multi-process by design** (spawns renderer/GPU child processes and
  sandboxes them), which collides head-on with Hull's pledge (`no exec / proc /
  fork`). It can't simply be linked into a single-process no-exec binary; it
  would need a separate-process architecture (the same shape as the DuckDB
  "mode B / less-restricted jail" idea) or it doesn't fit Hull's posture. That's
  a "should we even" question, decided per-feature, not a packaging one.

**Budget per-feature integration separately from distribution, and gate each new
feature on a sandbox-fit review.**

## 6. DuckDB as the first feature (re-scoped #4)

The DuckDB backend itself is already merged (backend, mode A/B, rseq fix, dialect
helpers). The remaining "packaging" item is re-scoped from *a `full-duckdb`
flavor* to *the first feature*:

1. **`libhull_feature-duckdb-<arch>.a`** - a `make feature-duckdb` target building
   the DuckDB objects (`cap/db_duckdb.o` + the isolated static libs from
   `make fetch-duckdb`) into a standalone feature archive. Native x86_64/aarch64
   + darwin-arm64; **no cosmo** (DuckDB isn't cosmo-compatible).
2. **Generated-registry refactor (§3.2)** — turn `cap/db_select.c`'s `#ifdef`'d
   `BACKENDS[]` into a base table (SQLite always) that a build-time-generated
   registry extends with composed features. This is the reusable core; every
   later feature rides on it.
3. **`hull feature install duckdb` / `list`** - a new `commands/feature.c` +
   registry, `~/.hull/feature/` signed store, reusing
   `hl_release_io_fetch_verified_manifest` (mirrors `commands/flavor.c` /
   `tools_install.c`). One `dispatch.c` line.
4. **Build wiring** — `hull build` detects a `duckdb://` connection in the
   manifest (or `--with=duckdb`), links the feature lib (local build dir →
   `~/.hull/feature/` → error with an install hint), re-verifies a cache-sourced
   lib, and emits the feature into the generated registry.
5. **Release pipeline** — CI job builds + the manifest covers
   `libhull_feature-duckdb-<arch>.a`. ~58 MB × 3 native archs.
6. **Reserved-scheme hint** — `cap/db_select.c`'s `duckdb://` "not available"
   message points at `hull feature install duckdb`.

Items 1–6 are **Phase 1** and ship the v1 story: build-then-run
(`hull build --with=duckdb`, run the binary). **Phase 2 (later):** the
self-linking dev loop from §4 — `hull dev` on a DuckDB app fetches the feature
lib, links + caches a dev runtime keyed by feature-set + hull version, re-execs,
and hot-reloads app source in-process. Phase 2 reuses everything in Phase 1 plus
`hull build`'s linker, so it's a "dev-`main` + link + cache + re-exec" wrapper,
not a new subsystem.

What we explicitly **don't** do: publish a `full-duckdb` flavor, or a per-combo
`<flavor>-duckdb` matrix. DuckDB composes onto any flavor base at build time.

## 7. Direction: collapse to one primitive (feature) + presets

The two words survive **not** because HTTP-off and DuckDB-on are different in the
compiler's eyes — they're identical `-D` flags — but because one lives in an
**enumerable, pre-published set** and the other lives in **combinatorial space
you compose per build**. That's a packaging fact, not an architectural one, and
the intended direction is to make the model reflect it.

**The unification.** There is one concept: a **feature**. A **flavor** is just a
*saved feature preset* — a named bundle. Then:

- `full` = the preset "all default features on" (db + http + wasm + tui + tcc).
- `pure-compute` = a minimal / feature-less preset (defaults minus HTTP, …).
- `duckdb` = the preset "defaults + DuckDB."
- `hull build --flavor=X` becomes sugar for `hull build --preset=X`, itself sugar
  over `--feature=+duckdb,-http-server`.

That HTTP is *itself* a feature (on by default) is exactly what proves flavors are
just presets over features. The target CLI is **one primitive (feature) + named
presets**, and the word "flavor" retires into "preset."

**The one asymmetry that keeps a distinct shipping unit (for now).** Even once
everything is "just a feature," the *artifacts* still differ by direction:

- **Subtracting** a default feature (turn HTTP off) = a **different base build** —
  a slimmer `libhull_platform-*.a`. You can enumerate these (the default set is
  small) and pre-publish them.
- **Adding** a large feature (DuckDB) = a **bolt-on separate signed lib**
  (`libhull_feature-*.a`) linked on demand - because the additive space is 2^N
  and can't be enumerated.

So the two words are really tracking the **shipping unit**, not the flag:
"flavor/preset" = an enumerable pre-published base build (subtractive); "feature"
= a composable bolt-on artifact (additive). The end state keeps *that* asymmetry
(it's real and physical) while collapsing the *vocabulary* to features + presets.

**Migration path.** (1) Land features as composable bolt-on libs (§3, §6 —
DuckDB first). (2) Re-express the existing flavors as presets over the feature
set, with `--preset=` as the surface and `--flavor=` an alias. (3) Retire
"flavor" from docs/CLI in favor of "preset," leaving one primitive (feature) and
presets as the only user-facing vocabulary. Steps 2–3 are a later cleanup; step
1 is #4 and is what unblocks everything.

## 8. Open items

- The generated-registry refactor (§3.2) is the load-bearing piece and touches a
  trust boundary (the composed table is what the cap layer dispatches through).
  It needs the same review rigor as the sealed-table work in
  [security.md §4b](security.md): the generated table must be `const`
  (`.rodata` / RELRO), the feature lib must be release-signed and re-verified at
  build, and the base must fail closed if a declared feature is absent.
- Feature + flavor interaction in `hull build --flavor=auto`: auto stays
  *subtractive* (picks the minimal base); features are additive and are inferred
  separately, never dropped by auto.
- Whether the base platform lib itself should move *all* optional DB connectors
  (postgres, mysql) to the feature model, or keep the tiny pure-C ones compiled
  into the base and reserve features for the big libs. Leaning: keep small
  connectors in the base (they're cheap), features for anything that's a heavy
  static dependency.
- A `hull doctor` / `hull feature list` view that shows, for the current app,
  which features it needs and whether each is installed.

# Hull build flavors. `hull build --flavor`

Status: **Draft / Proposed** | Tracked in: [`roadmap_next.md`](roadmap_next.md)

This is the design for making a build flavor a property of the **app
binary you ship**, not of the `hull` toolchain that builds it. Today every
app `hull build` produces inherits whatever `HL_ENABLE_*` flags the `hull`
binary itself was compiled with (full HTTP, in the released binaries). This
doc proposes `hull build --flavor=<name>` so an app author can ship a
pure-compute, server-only, client-only, or full binary regardless of which
`hull` built it.

## 1. Motivation

The four HTTP flavors (and the `HL_ENABLE_DB` / `HL_ENABLE_TUI` / ...
toggles) already exist and are CI-validated (see [CLAUDE.md "HTTP build
flavors"](../CLAUDE.md)). But they are only reachable by recompiling Hull
from source with `make HL_ENABLE_HTTP=0`. The end user who installed a
released `hull` cannot produce a flavored app.

That is backwards. The flavor describes what the **app** needs:

- a CLI tool that calls an API → client-only (no inbound listener)
- a compliance web app forbidden from outbound calls → server-only
- an offline transform / signing tool → pure-compute (no TLS at all)
- a full web app → full

The author knows this from the app's manifest. The toolchain should honor
it.

## 2. The enabling fact: the app side is flavor-agnostic

`hull build` emits two pieces of C and links them against a precompiled
platform library:

- `app_main.c` is a **thin trampoline**. Its entire body is
  `return hull_main(argc, argv);`. It does not change per flavor.
- `app_registry.c` is the embedded asset table (modules, templates,
  static, migrations, compute, shaders). Also flavor-agnostic.

Everything that differs between flavors (serve loop vs `app.main`, whether
mbedTLS/Keel are present, which `hl_cap_*` are compiled) lives **inside
`libhull_platform.a`**. serve-vs-CLI dispatch is resolved inside
`hull_main`.

> Consequence: `hull build --flavor=X` reduces to "link against the
> flavor-X platform library instead of the default embedded one." No
> per-flavor app templates, no codegen changes.

## 3. Proposed UX

```
hull build myapp                         # default flavor (today's behavior)
hull build --flavor=pure-compute myapp   # explicit flavor
hull build --flavor=server-only myapp
hull build --flavor=client-only myapp
hull build --flavor=auto myapp           # infer minimal flavor from manifest
```

### Flavor catalog

| Flavor | Flags | Drops | App must use |
|---|---|---|---|
| `full` (default) | all on | nothing | anything |
| `server-only` | `HL_ENABLE_HTTP_CLIENT=0` | outbound `http.fetch`, SMTP, `hull update` | no `hull/http-client` / `hull/smtp` / `hull/email` |
| `client-only` | `HL_ENABLE_HTTP_SERVER=0` | inbound server, routing, ws, sse, middleware | `app.main`; no `hull/http-server` / `hull/web/*` |
| `pure-compute` | `HL_ENABLE_HTTP=0` | both HTTP halves + Keel + mbedTLS | `app.main`; no HTTP/web modules |

Orthogonal toggles (`HL_ENABLE_DB`, `HL_ENABLE_TUI`, `HL_ENABLE_WASM`,
`HL_ENABLE_GPU`, `HL_ENABLE_TCC`) can compose with any flavor. Whether
`--flavor` exposes them as a free-form flag set or only ships a curated
list of named flavors is an open question (§8).

## 4. Mechanism

### 4.1 Where the per-flavor platform libraries come from

`HlEmbeddedPlatform` (in `build_assets.c`) is keyed by **arch** today
(x86_64 + aarch64 for cosmo), single-flavor. Three ways to make other
flavors available:

| Option | Cost | Verdict |
|---|---|---|
| (a) Embed all flavors × arches in `hull` | Each platform lib is multi-MB; 4 flavors × 2 arches bloats `hull` several-fold | No |
| (b) **Download signed per-flavor libs on demand** | One fetch per non-default flavor, cached in `~/.hull/platform/` | **Yes** |
| (c) Build the platform lib from source | Needs the full Hull source + toolchain; defeats the distributed-binary model | Contributor-only fallback |

Option (b) reuses machinery Hull already has:

- Release publishes `libhull_platform-<flavor>-<arch>.a` as release
  artifacts, each line in the Ed25519-signed `hull.sha256` manifest.
- `hull build --flavor=X` resolves the lib: **embedded** for the default
  flavor (no download), else `~/.hull/platform/<flavor>-<arch>.a`,
  fetching + SHA-256 + signature verifying via the existing
  `release_io.{c,h}` + platform-signature path on first use. This is the
  identical trust chain as [`hull tools install`](tools_install.md).
- The platform-signature layer (`platform_sig.c`, `hull sign-platform`,
  the inner layer of `package.sig`) already exists; each per-flavor lib is
  signed the same way.

**This is what "a pure-compute release artifact" should actually be:** not
a flavored `hull` binary (a crippled CLI that cannot update/dev/test), but
a signed `libhull_platform-pure-compute-<arch>.a` consumed by
`hull build --flavor`.

### 4.2 Resolver target-caps (the one non-trivial code change)

The module resolver (`module_resolver.c`) hard-blocks build-time gates: an
app declaring `hull/gpu@1` on a non-GPU build fails at resolve time. Today
it checks against the **running hull's** `HL_ENABLE_*` flags.

For `--flavor` it must validate against the **target** flavor's caps. So a
pure-compute build of an app that declares `hull/web/ws-server` must fail
cleanly at build time with the existing "requires HL_ENABLE_HTTP_SERVER"
message, even though the building `hull` is full-HTTP.

Change: thread a target-cap bitset (derived from the chosen flavor) into
`hl_module_resolve` instead of reading the compiled-in flags directly. The
resolver already produces a bitset; this makes its **input** a parameter.

### 4.3 `--flavor=auto`

Because the manifest already declares every first-party module, the
minimal flavor is derivable:

- declares any `hull/http-server` / `hull/web/{ws-server,ws-client,sse}` /
  `hull/web/middleware/*` → needs server
- declares `hull/http-client` / `hull/smtp` / `hull/email` → needs client
- neither, and registers only `app.main` → pure-compute
- both → full

`--flavor=auto` picks the smallest flavor that satisfies the resolved
module set. This makes flavor a derived property of the app rather than a
flag the author must reason about. Recommended as the eventual default
once the mechanism is proven; until then `full` stays the default for
backward compatibility.

### 4.4 Flavor registry (one table, four consumers)

A single table is the source of truth, mirroring `module_registry.c` and
the tool registry. Each entry maps a flavor name to its compile-time cap
set and the platform-lib asset that carries it:

```c
// proposed: src/hull/build_flavor.{c,h}
typedef struct {
    const char *name;          // "pure-compute"
    uint32_t    caps;          // bitset of HL_CAP_HTTP_SERVER | HTTP_CLIENT | DB | TUI | ...
    const char *asset;         // "libhull_platform-pure-compute" (asset/file stem)
} HlBuildFlavor;

static const HlBuildFlavor FLAVORS[] = {
  { "full",         CAP_ALL,                                       "libhull_platform" },
  { "server-only",  CAP_ALL & ~CAP_HTTP_CLIENT,                    "libhull_platform-server-only" },
  { "client-only",  CAP_ALL & ~CAP_HTTP_SERVER,                    "libhull_platform-client-only" },
  { "pure-compute", CAP_ALL & ~(CAP_HTTP_SERVER | CAP_HTTP_CLIENT), "libhull_platform-pure-compute" },
};
```

Four consumers read this one table: the module resolver validates against
`caps` (§4.2), `--flavor=auto` searches over it (§4.3), platform-lib
resolution keys off `asset` (§4.1), and `hull doctor` / `hull agent`
enumerate it. Adding a flavor (say `pure-compute-min`, which also drops DB
+ TUI) is one registry row plus one signed asset.

**Decision: named flavors, not free-form flag composition.** `--flavor=X`
selects one of a finite, signed, CI-tested set. It deliberately does **not**
expose arbitrary `--enable-db=0 --disable-tui` combos, because every
published platform lib must be signed and listed in the release manifest,
and free-form composition is combinatorial (2^N flag combos) which cannot
be signed or tested exhaustively. Free-form composition stays available on
the build-from-source path only (`make platform HL_ENABLE_*=...`), for
forks and contributors. The named set can grow; each addition is a
deliberate, signed artifact.

### 4.5 End to end: what `hull build --flavor=pure-compute myapp` does

1. **Parse.** `commands/build.c` reads `--flavor=pure-compute` and looks it
   up in `FLAVORS[]`. Unknown name errors with the valid list. No
   `--flavor` resolves to `full` (today's behavior, byte-for-byte).

2. **Resolve the manifest against the target caps.** The tool VM runs the
   module resolver with the **flavor's** `caps`, not the running hull's
   compiled-in flags. An app declaring `hull/web/ws-server` under
   `--flavor=pure-compute` fails right here with the existing message
   (`module 'hull/web/ws-server@1' requires HL_ENABLE_HTTP_SERVER
   (build-time)`). This is the one real code change (§4.2):
   `tool.modules_resolve(manifest, caps)` gains a caps parameter instead of
   reading the build's flags directly.

3. **Locate the platform library** via `hl_build_resolve_platform(flavor,
   arch)`:
   - `full` -> the embedded lib (`hl_build_get_platforms`, today's path,
     no download).
   - otherwise -> `~/.hull/platform/<asset>-<arch>.a`. Cache hit uses it.
     Miss -> **(Phase Full)** fetch `libhull_platform-pure-compute-<arch>.a`
     from the release matching this hull's version, verify SHA-256 +
     Ed25519 against the embedded signed manifest (the same `release_io` +
     `tools_install` trust path), store, use. **(MVP)** miss -> error with
     a `make platform HL_ENABLE_HTTP=0` hint.

4. **Verify the platform signature.** The fetched `.a`'s SHA-256 must match
   the signed platform manifest, exactly the `--verify-platform` cross-check
   Hull already does for the embedded lib. `--no-verify-platform` opts out
   (dev hulls / forks signing with their own platform key).

5. **Generate, compile, link.** Unchanged from today except the linked
   archive is the flavor lib: `app_main.c` (trampoline) + `app_registry.c`
   (assets) compiled and linked against `libhull_platform-pure-compute.a`.
   Because the app side is flavor-agnostic (§2), nothing else in the build
   changes; the `-DHL_ENABLE_*` defines are already baked into the `.a`, so
   the app TUs never see them.

6. **Sign the app layer** (`--sign`), unchanged, except `package.sig`
   records `flavor = "pure-compute"` in its metadata so `hull verify` /
   `hull inspect` / `hull agent` can report which flavor a binary is.

Result: a standalone `myapp` with no TLS stack and no HTTP parser, ~0.7 MB
smaller, produced by an unchanged full `hull`.

## 5. Trust and security

- **Per-flavor platform signature.** Each `libhull_platform-<flavor>-<arch>.a`
  is covered by the platform-signature layer and the release manifest.
  `hull build --verify-platform` (default on) cross-checks the fetched
  lib's SHA-256 against the embedded signed manifest, exactly as it does
  for the embedded lib today.
- **Manifest seal unaffected.** The app's runtime manifest seal
  (`hl_seal_arena`) and the two-phase kernel sandbox are inside the
  platform lib and behave identically per flavor. `--flavor` changes which
  caps are *compiled in*, not how the surviving caps are enforced.
- **Smaller flavor = smaller attack surface, honestly.** Pure-compute ships
  no TLS stack and no HTTP parser. That is a real reduction, and it is the
  app author's to claim only because the flavor is now bound to the app
  binary.
- **No new key material.** Per-flavor libs ride the existing release +
  platform keys; no rotation, no new trust root.

## 6. Phasing

- **MVP (no release-pipeline changes, CI-testable):**
  1. `--flavor` selects a **locally available** platform lib (built from
     source: `make platform HL_ENABLE_HTTP=0`, etc.) or errors with a
     clear hint.
  2. Resolver target-caps plumbing (§4.2).
  3. Per-flavor app-build e2e (build a pure-compute app with a full hull,
     run it, assert exit code + that a forbidden module fails at build).

  This proves the whole mechanism end to end without publishing anything.

- **Full:**
  4. Release pipeline builds + signs + publishes `libhull_platform-<flavor>-<arch>.a`.
  5. `hull build --flavor` auto-fetches + verifies + caches missing libs.
  6. `--flavor=auto` inference.
  7. `hull doctor` / `hull agent` report available flavors + cached libs.

## 7. Open questions

1. ~~Named flavors only, or free-form flag composition?~~ **Resolved (§4.4):
   named flavors for published libs; free-form only via build-from-source.**
2. Does `--flavor=auto` become the default, and when? (Behavior change.)
3. ~~Cosmo: per-flavor × per-arch, worth it?~~ **Resolved: cosmo flavor libs
   are now published** (dual-arch `libhull_platform-<flavor>.{x86_64,aarch64}-cosmo.a`,
   covered by the signed `hull.sha256`). `hull platform install <flavor>` on a
   cosmo hull fetches + verifies the pair into `~/.hull/platform/`, and
   `hull build --flavor` lays them out in the `.aarch64/` apelink layout.
   `make platform-cosmo-<flavor>` still works for build-from-source. The
   release producer builds each cosmo flavor on its **own fresh runner** (a
   second cosmo platform build in one job corrupts the dynamic loader), so no
   double-up in a single job.
4. Cache location: `~/.hull/platform/` as a sibling of `~/.hull/tools/`
   (signed durable store, not a prunable cache), consistent with
   [blob.md](blob.md)'s store split.

## 8. Related direction: a no-runtime embedding flavor (`libhull`)

> **Now partly shipped.** The `hl_embed_*` ABI, the `libhull.a` archive,
> and the sealed fail-closed lifecycle described below have landed (phases
> L-1..L-3). See **[libhull_flavor.md](libhull_flavor.md)** for the trust
> boundary and seal lifecycle. The rest of this section is the original
> design rationale.

A separate idea surfaced alongside this: a flavor with **`HL_ENABLE_LUA=0`
AND `HL_ENABLE_JS=0`** (no scripting runtime at all), exposing just the
kernel sandbox (pledge/unveil/seatbelt) + the capability layer so an
existing native codebase can link Hull as a **hardening library** rather
than run a Lua/JS app on it.

This is **not** the same axis as `--flavor` and is **out of scope for this
doc's implementation**, but the platform-lib decoupling here is a stepping
stone, so it is worth recording.

### Why it is compelling

- The capability layer (`hl_cap_*`) and the sandbox (`sandbox.c`,
  `manifest.c`, the seal arena, signatures, SBOM) are already
  **runtime-agnostic C**. Both runtimes are just two callers of the same
  caps. Dropping both leaves a coherent "hardened core."
- A native program would get: manifest-declared capabilities, the
  two-phase kernel sandbox, capability-mediated FS/DB/crypto, WASM/GPU
  compute isolation, and the signed-artifact/SBOM machinery, without
  rewriting its orchestration in Lua/JS.
- It generalizes Hull's hardening beyond the script-app model, which is
  strategically interesting (Hull-as-an-SDK, not just Hull-as-a-runtime).

### What it requires (and why it is bigger)

- **The host owns `main`.** Today Hull's lifecycle assumes Hull owns `main`
  and sequences phase-1 pledge → load app → phase-2 sandbox. In library
  mode the host calls these in the right order itself. Hull provides the
  primitives; the host is **trusted to sequence them correctly**. That is a
  documented, weaker contract than "Hull owns main" and needs a stable,
  public embedding API (`hl_embed_*` over the existing `hl_sandbox_*` /
  `hl_manifest_*` / `hl_cap_*`).
- **API-stability commitment.** Exposing the cap/sandbox C surface as a
  supported embedding API is a real semver promise; today those are
  internal.
- **No `app.manifest()`.** The manifest must be constructed in C by the
  host before phase-2, instead of declared in a script.

### Honest fit for a codebase like `../otto`

`otto` is a polyglot monorepo (Make + Node + Docker + service dirs), not a
single C program. Two caveats follow:

- **In-process C linking** only helps a component that is itself native
  (C/Rust/Zig) or willing to add a native bridge. A Node service cannot
  link `libhull` directly without an addon.
- **Hull cannot "wrap and launch" otto as a subprocess.** Hull's sandbox
  deliberately blocks `exec`/`fork`/`proc` to prevent escapes; a
  sandbox-supervisor model (firejail/bubblewrap-style) is antithetical to
  that design and is not what this flavor would be.

The natural Hull-shaped way to harden an external workload is often the
inverse: compile its compute-heavy or untrusted part to **WASM** and run it
under `compute.*` (gas-metered, no I/O imports). That is the existing
"sandbox an untrusted payload" story and needs no new flavor.

### Recommendation

Worth a **separate design doc** if there is a concrete native consumer.
The `--flavor` work in this doc is a prerequisite either way (it builds the
per-flavor platform-lib selection + signing the embedding flavor would also
need). Sequence: ship `--flavor` first; revisit `libhull` when a real
native embedder exists.

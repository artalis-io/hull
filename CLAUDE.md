# HULL. Development Guide

## Building an application on Hull?

If you're using Hull to build an app (rather than hacking on Hull
itself), start at **[BOOTSTRAP.md](BOOTSTRAP.md)**. It's a single-file
prompt for AI coding agents (Claude Code, Codex, OpenCode, Cursor) that
covers installation, the required-reading order through this guide, a
forced discovery / plan / implement workflow, the widget-tier reach
order, the anti-pattern table, and a `PLATFORM_GAPS.md` protocol for
flagging Hull-side gaps instead of coding around them. Same file works
for any product spec - hand the agent BOOTSTRAP.md + the spec.

The rest of THIS file is the Hull-internal development guide for
contributors hacking on the runtime, stdlib, or build pipeline.

## Distribution

### HTTPS / CA bundle

Hull embeds Mozilla's CA bundle (from curl.se, ~226KB, ~145 roots) into `libhull_platform.a` so HTTPS works without a system CA store. Apps built via `hull build` inherit the embedded bundle automatically.

Resolution order at startup (in `main.c::hl_serve_wire_and_start`):
1. `--no-ca-bundle` (or `--skip-ca-bundle` deprecated alias) → no verification (dev only, MITM-vulnerable)
2. `--ca-bundle PATH` (or `--ca-bundle=PATH`) → load that file
3. System CA store at `/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`
4. Embedded Mozilla bundle (via `hl_embedded_ca_bundle()` in `src/hull/cacert.c`)
5. Fail with a clear hint

`hull doctor` reports both system and embedded availability. The new Keel API `kl_tls_mbedtls_client_ctx_create_from_buf()` loads PEM/DER directly from memory.

Refresh the bundle with `make fetch-ca-bundle` (pulls from `curl.se/ca/cacert.pem`, verifies SHA-256). Disable embedding with `make HL_EMBED_CA_BUNDLE=0` (saves ~200KB but breaks HTTPS in stripped-down containers / Windows / air-gapped).

End-users install Hull with:

```sh
curl -fsSL https://gethull.dev/install.sh | sh
```

`install.sh` (POSIX, ~250 lines) detects OS/arch via `uname`, picks `hull-linux-x86_64` / `hull-linux-aarch64` / `hull-darwin-arm64` / `hull-cosmo` from the latest GitHub release, verifies the SHA-256 from `hull.sha256`, and installs to `~/.local/bin/hull` (or `/usr/local/bin` if root). Knobs: `HULL_VERSION`, `HULL_PREFIX`, `HULL_FLAVOR=cosmo|native`, `HULL_FORCE=1`, `HULL_DRY_RUN=1`.

Shell completions for bash, zsh, fish live in `completions/`. They cover every subcommand, `--compiler=system|cc|...`, agent subcommands, deploy targets, etc. See `completions/README.md`.

Tested by `tests/e2e_install.sh` (`make e2e-install`. Runs install.sh in dry-run mode, syntax-checks all three completion shells, exercises bash completion behavior for representative inputs).

## Release Process

Releases are tagged commits (`v0.1.0`, `v0.1.1`, …) that trigger
`.github/workflows/release.yml`. The workflow runs four platform
builds in parallel. `hull-linux-x86_64`, `hull-linux-aarch64`
(Graviton / DGX / Ampere / Pi 4+. `runs-on: ubuntu-24.04-arm`),
`hull-darwin-arm64`, and the universal `hull-cosmo` APE. Computes
`hull.sha256` over the four artifacts, signs that manifest with the
offline Ed25519 release key, and publishes a GitHub release with
all six files. End-user `hull
update` then verifies `hull.sha256.sig` against the public key
embedded at build time as `HL_RELEASE_PUBKEY_HEX` (in
`include/hull/release.h`) before atomically `rename(2)`-ing the
new binary into place.

### One-time setup (per signing-key generation)

Hull has **two independent signing keys**, each generated and stored
the same way:

- **Release key**. Signs `hull.sha256` for `hull update` to verify
  downloads. Pubkey embedded as `HL_RELEASE_PUBKEY_HEX` in
  `include/hull/release.h`.
- **Platform key**. Signs the platform `.a` library (the inner layer
  of `package.sig` when an app is built via `hull build`). Pubkey
  embedded as `HL_PLATFORM_PUBKEY_HEX` in `include/hull/signature.h`.

Keep them separate (different `.key` files, different GitHub secrets)
so one compromise doesn't taint the other.

| # | Step | Where |
|---|------|-------|
| 1 | `mkdir -p ~/.hull/keys && chmod 700 ~/.hull/keys` | local |
| 2 | `cd ~/.hull/keys && hull keygen release && hull keygen platform` | local. Writes `release.{key,pub}` and `platform.{key,pub}` |
| 3 | Paste `release.pub` into `include/hull/release.h::HL_RELEASE_PUBKEY_HEX`; paste `platform.pub` into `include/hull/signature.h::HL_PLATFORM_PUBKEY_HEX`. Commit + push. | repo |
| 4 | `gh secret set HULL_RELEASE_KEY --body "$(cat ~/.hull/keys/release.key)" --repo artalis-io/hull` (and similarly `HULL_PLATFORM_KEY` once sign-platform is wired into a workflow) | GitHub Actions secret |
| 5 | Back up `~/.hull/keys/release.key` AND `~/.hull/keys/platform.key` offline (USB stick, password-manager attachments, sealed envelope). **Losing either = no more signed v0.1.x artefacts of that kind; rotation requires a new embedded pubkey and a coordinated user-side reinstall.** | external |

### Per-release procedure

| # | Step |
|---|------|
| 1 | Confirm CI green on the commit to be tagged. The release workflow re-runs `make`, signs from the freshly built native-linux binary, and publishes. Broken CI means a broken release. |
| 2 | `git tag -a vX.Y.Z -m "Hull vX.Y.Z" && git push origin vX.Y.Z` |
| 3 | Watch `.github/workflows/release.yml`; on success the GitHub release lands with `hull-cosmo`, `hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`, `hull.sha256`, and `hull.sha256.sig`. |
| 4 | Smoke-test on a clean machine: `curl -fsSL https://gethull.dev/install.sh \| sh && hull update --check`. |

### Invariants

- The private release key **never** leaves `~/.hull/keys/release.key` and the GitHub Actions secret `HULL_RELEASE_KEY`. Not in the repo, not in any commit, not in any log.
- The public release key **is** in the repo (as the `HL_RELEASE_PUBKEY_HEX` literal). Anyone can read it; that's the point.
- Pre-v0.1.0 builds with the all-zero placeholder pubkey skip signature verification with a one-time warning. See `hl_release_pubkey_configured()`. Once a real key is embedded, that bypass disappears.
- `install.sh` stays SHA-256-only (no signature check). The signature is what `hull update` verifies on subsequent self-updates, after the user has already trusted the first install via TLS + SHA-256.

See [docs/release_signing.md](docs/release_signing.md) for the threat model, key-management rationale, and rotation plan.

## Build

```bash
make                    # build hull binary (epoll on Linux, kqueue on macOS)
make test               # build and run all unit tests
make e2e                # end-to-end tests (all examples, both runtimes)
make debug              # debug build with ASan + UBSan (recompiles from clean)
                        #   `make debug && make test` runs the tests under ASan:
                        #   debug records the sanitizer in build/.sanitizer.mk and a
                        #   following bare `make test` inherits it (else the ASan-
                        #   instrumented objects would fail to link the runtime).
                        #   `make clean` clears it; `make DEBUG=1 test` is the
                        #   equivalent single invocation CI uses.
make msan               # MSan + UBSan (Linux clang only)
make check              # full validation: clean + ASan + test + e2e
make analyze            # Clang static analyzer (scan-build)
make cppcheck           # cppcheck static analysis
make platform           # build libhull_platform.a (everything except main/build-tool code)
make platform-cosmo     # build multi-arch cosmo platform archives (x86_64 + aarch64)
make libhull            # build libhull.a: the runtime-free core for native embedders (no Lua/JS). See docs/libhull_flavor.md
make embed-smoke        # build + run the C/Rust/Zig reference embedders (rust/zig skip if toolchain absent)
make self-build         # reproducible build chain: hull → hull2 → hull3
make CC=cosmocc         # build with Cosmopolitan (APE binary)
make EMBED_PLATFORM=1   # embed platform library in hull binary (distribution mode)
make EMBED_PLATFORM=cosmo  # embed multi-arch cosmo platform (distribution mode)
make wamrc              # build WAMR AOT compiler (requires cmake + LLVM)
make bench-wasm         # WASM compute benchmark (native vs interpreter vs AOT)
make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu  # build with GPU compute (wgpu-native)
make bench-gpu HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu  # GPU vs WASM vs native benchmark
make HL_ENABLE_DB=0     # compute-only build (drop SQLite, ~1.4 MB smaller)
make clean              # remove all build artifacts
```

### Build feature flags

Hull's distribution is one binary; what's compiled into it is controlled by a small set of `HL_ENABLE_*` flags. Each is on/off at the Makefile level and surfaces as a `-D` define so the C code branches consistently.

| Flag | Default | Effect when off |
|------|---------|-----------------|
| `HL_ENABLE_LUA` | 1 | Drop the Lua 5.4 runtime; QuickJS-only build |
| `HL_ENABLE_JS` | 1 | Drop QuickJS; Lua-only build |
| `HL_ENABLE_WASM` | 1 | Compile-time drop of WAMR (`compute.*` unavailable, ~256 KB): base + no wasm archive. Orthogonal to the auto-composed axis - with the default `=1`, the native base is still **compute-less** and composes WASM only for apps that need it (see "Composable runtime + HTTP base"). Cosmo keeps WASM in-base. |
| `HL_ENABLE_GPU` | 0 | (Off by default.) On enables wgpu-native (`gpu.*`). Normally composed via the `gpu` **feature** (`hull build --with=gpu`) rather than set directly; see "Composable features" below. Native-only (no cosmo). |
| `HL_ENABLE_DUCKDB` | 0 | (Off by default.) On enables the embedded DuckDB OLAP backend (`cap/db_duckdb.c` + fetched static libs via `make fetch-duckdb`). A `duckdb://` DSN selects it. Native-only (no cosmo). Normally composed via the `duckdb` **feature** (`hull build --with=duckdb`) rather than set directly; see "Composable features" below. |
| `HL_EMBED_CA_BUNDLE` | 1 | Drop Mozilla CA bundle (~200 KB, breaks HTTPS without system store) |
| `HL_ENABLE_SQLITE` | 1 | Drop the SQLite backend (`cap/db_sqlite.c`, `cap/db_udf.c`, vendored `sqlite3.c`). A SQLite file path or `:memory:` DSN then has no backend. |
| `HL_ENABLE_IMAGE` | 1 | Drop the image codec subsystem (`cap/image.c`, `cap/image_stb.c`, per-runtime `mod_image`, and vendored `stb_image` - image's **sole** consumer, ~146 KB). **Two-mode knob:** at the default `=1` the base is IMAGE-LESS and auto-composes the image feature (`libhull_feature-image.a` + `-image-<rt>.a`, embedded in hull) back for apps that declare `hull/image` (the `needs_image` gate; see "Composable runtime + HTTP base"). `=0` is the subtractive path - drops image entirely (no feature to compose, like `HL_ENABLE_DB=0`). `hull/image` then fails module resolution unless declared optional (`"hull/image@1?"` → `require` returns nil). The GPU texture paths that take/return an `HlImage` (`gpu.texture(img)`, `gpu.texture_read`/`textureRead`) stay `#ifdef HL_ENABLE_IMAGE`-gated, so a `GPU=1 IMAGE=0` build keeps raw-byte textures but not the image bridge. See "Image-less builds" below and [docs/image_feature.md](docs/image_feature.md). |
| `HL_ENABLE_POSTGRES` | 0 | (Off by default.) On compiles the pure-C PostgreSQL wire backend (`cap/pgwire.c` + `cap/pg_conn.c` + `cap/db_postgres.c`; no libpq) into the base. A `postgres://` / `postgresql://` DSN selects it. Links the shared TLS client (`HL_LINK_TLS`) for SSL connections. Normally composed via the `postgres` **feature** (`hull build --with=postgres`) rather than set directly; the flag is the monolithic path and what `make feature-postgres` builds the archive with. See "Composable features" above and "PostgreSQL + multi-backend DB" below. |
| `HL_ENABLE_MYSQL` | 0 | (Off by default.) On compiles the pure-C MySQL / MariaDB wire backend (`cap/mysqlwire.c` + `cap/mysql_conn.c` + `cap/db_mysql.c`; no libmysql/libmariadb) into the base. A `mysql://` / `mariadb://` DSN selects it. Links the shared TLS client (`HL_LINK_TLS`). Normally composed via the `mysql` **feature** (`hull build --with=mysql`) rather than set directly; the flag is the monolithic path and what `make feature-mysql` builds the archive with. See "Composable features" above and "MySQL/MariaDB specifics" below. |
| `HL_ENABLE_DB` | 1 | **Umbrella, derived** from the three granular flags: defined iff `HL_ENABLE_SQLITE`, `HL_ENABLE_POSTGRES`, or `HL_ENABLE_MYSQL` is on. Off (all granular off) drops `db.*` + `migrate.*` + worker-DB + the connection registry + DB-backed stdlib (session, ratelimit, idempotency, outbox, inbox, rbac, search). ~1.4 MB smaller. See "Compute-only builds" below. |
| `HL_ENABLE_HTTP_SERVER` | 1 | Drop the inbound HTTP server: serve.c (KlServer setup), routing, body reader, WebSocket server (cap/ws), middleware, SSE, in-process test harness (cap/test, test_runner), and `hull dev/test/agent/mcp` commands. Apps must use `app.main(fn)` and may not declare `hull/http-server`, `hull/web/ws-server`, `hull/web/ws-client`, `hull/web/sse`, or any `hull/web/middleware/*`. See "HTTP build flavors" below. |
| `HL_ENABLE_HTTP_CLIENT` | 1 | Drop the outbound HTTP/HTTPS client: `http.fetch` (cap/http + cap/http_async), SMTP send (cap/smtp), and `hull update` (which uses Keel's HTTPS client). Apps may not declare `hull/http-client`, `hull/smtp`, or `hull/email`. |
| `HL_ENABLE_HTTP` | 1 | **Back-compat alias.** Setting `HL_ENABLE_HTTP=0` pins both `HL_ENABLE_HTTP_SERVER` and `HL_ENABLE_HTTP_CLIENT` to 0. The macro stays defined when either granular flag is on, so existing source guards continue to mean "any HTTP at all". |
| `HL_ENABLE_TUI` | 0 native / 1 cosmo | Whether the terminal UI capability (`cap/tui.c`, `cap/tui_input.c`, `cap/tui_width.c`, the runtime bindings, the `hull.tui` stdlib module) is compiled INTO the base. TUI is a **composable feature** (like gpu/duckdb): the native base is TUI-free so apps that never touch the terminal link a leaner platform lib (~80-150 KB), and an app that declares `hull/tui` composes it back via `hull build --with=tui` (auto-inferred from the manifest, so a plain `hull build` of a `hull/tui` app just works). The hull TOOLCHAIN keeps its own `--tui` commands (`hull doctor / dev / agent context / agent errors / modules available`) by force-loading `libhull_feature-tui.a` at link time (`HL_TUI_TOOLCHAIN`, default 1 on a TUI-free native base). Cosmo compiles TUI in (a fat APE can't force-load a native feature archive). See "Terminal UI module" and [docs/features_and_flavors.md](docs/features_and_flavors.md). |

Combine flags freely: `make HL_ENABLE_DB=0 HL_ENABLE_WASM=1 HL_ENABLE_HTTP=0` yields a pure compute runtime with Lua/JS orchestration but no database or HTTP.

### HTTP build flags

The two HTTP flags (`HL_ENABLE_HTTP_SERVER` / `HL_ENABLE_HTTP_CLIENT`) are independent **internal knobs**. They drive the per-runtime web-archive split; they are NOT exposed as shippable `--flavor` presets (the shipped HTTP axis is binary: `full` vs `pure-compute` - see "Build flavors for apps" below). Each combination still produces a useful binary at the `make` level (arm64 Darwin sizes for the default invocation, i.e. DB + WASM + TUI all on):

| Config | Server | Client | Binary | Notes |
|---|---|---|---|---|
| Default (full) | 1 | 1 | ~6.5 MB | Full HTTP. Web apps that serve requests and call out to APIs. This is the shipped `full` flavor. |
| Server flag only | 1 | 0 | ~6.5 MB | Internal knob. Keel + mbedTLS stay linked (no size win), so this is not a shippable flavor. |
| Client flag only | 0 | 1 | ~6.5 MB | Internal knob. Keel + mbedTLS stay linked (no size win), so this is not a shippable flavor. |
| Pure compute | 0 | 0 | ~5.8 MB | Compute / CLI binary with no HTTP, no Keel, no mbedTLS. This is the shipped `pure-compute` flavor. See "Pure-compute builds" below. |

The `full` (both on) and `pure-compute` (both off) configs are link-validated on every push by the `flavors` matrix in `.github/workflows/ci.yml` (each builds, runs `hull version`, and runs an `app.main` exit-code smoke).

**Linker dependencies.** Keel's `libkeel.a` and mbedTLS are linked whenever either HTTP flag is on (Keel ships both halves; the linker dead-strips the unused side). The compile-time `-DHL_ENABLE_HTTP` macro is defined in that same case, so existing source guards continue to work. `HL_ENABLE_HTTP_SERVER` / `HL_ENABLE_HTTP_CLIENT` are only used where the distinction matters. When **both** halves are off, mbedTLS is dropped entirely; see "Pure-compute builds" for how Hull's own hashing stays available without it.

**Migration note.** The single `HL_ENABLE_HTTP` flag is now a back-compat alias. New code targeting one half (e.g. an HTTP-server-only middleware, or a CLI tool that needs outbound HTTPS) should use the granular flags directly.

### Build flavors for apps (`hull build --flavor`)

The build flags above are compile-time properties of the `hull` binary. `hull build --flavor` makes the flavor a property of the **app binary you produce** instead: a full `hull` can build a narrower app.

`hull build --flavor=full|pure-compute [app_dir]`. Since **Phase 4.3** the flavor
axis has collapsed into the composable base: every reducible subsystem (HTTP,
TLS, Keel, SQLite, WASM, image) already drops from the distributed base and
composes back per app, so a compute app is *already* minimal without a flavor.
- **`full`** - the embedded default base; the only "real" base.
- **`pure-compute`** - now a **build.lua PRESET** (empty asset in `BUILD_FLAVORS[]`),
  **not** a pre-built per-flavor platform lib. It builds on the default composable
  base and only **validates** that the app declares no HTTP/TLS (rejects e.g.
  `hull/http-server` at build time with a clear message). The size win comes from
  the base, not the flavor. `hull flavor install pure-compute` → "preset flavor,
  nothing to install"; `hull flavor list` shows it as `preset (default base)`.
  (The former `server-only`/`client-only` were removed in #114; the pre-built
  `platform-pure-compute` / `platform-cosmo-pure-compute` libs + their release
  matrix were deleted in Phase 4.3.)

`--flavor=auto` infers the minimal flavor from the app's declared modules (via
`hl_build_flavor_auto`). Registry + resolver: `src/hull/module_resolver.c`
(`BUILD_FLAVORS[]` - a NON-empty asset stem still means "pre-built lib"; an EMPTY
stem means "preset"; `hl_build_flavor_auto`); the tool-side handling is in
`stdlib/cli/lua/hull/build.lua` (only a non-empty asset overrides the base).
Full design: [docs/build_flavors.md](docs/build_flavors.md).

*(Historical: pre-Phase-4.3, a non-default flavor linked a signed pre-built
per-flavor `libhull_platform-<flavor>.a` fetched via `hull flavor install`, with
a build-time release-signature re-verify closing the install→build TOCTOU. That
machinery still exists for any FUTURE non-preset flavor but has no user today.)*

### Composable features (`hull build --with=<name>`)

A **composable feature** is a large optional subsystem shipped as its **own**
signed static archive `libhull_feature-<name>.a` and composed into an app at
`hull build --with=<name>`. This is the **additive** axis, orthogonal to the
**subtractive** flavor axis above: `--flavor` slims a base build, `--with`
bolts a subsystem on. They compose (`M` flavors + `N` features publish `M+N`
libs but build any of `M×N` combos). Design + rationale:
[docs/features_and_flavors.md](docs/features_and_flavors.md).

Five features ship today, all **native-only (no cosmo)**, published for all
three native platforms (`linux-x86_64`, `linux-aarch64`, `darwin-arm64`):

| Feature | What | Reached via |
|---------|------|-------------|
| `duckdb` | embedded DuckDB OLAP backend (~58 MB, C++) | a `duckdb://` DSN on `hull/db` |
| `postgres` | pure-C PostgreSQL wire backend (~4 KB, no libpq) | a `postgres://` / `postgresql://` DSN on `hull/db` |
| `mysql` | pure-C MySQL / MariaDB wire backend (~4 KB, no libmysql) | a `mysql://` / `mariadb://` DSN on `hull/db` |
| `gpu` | wgpu-native GPU compute | `gpu.*` (the base ships the generic dispatch layer; the feature fills the concrete wgpu backend) |
| `tui` | terminal UI subsystem (`hull.tui`) | `hull/tui` module (auto-inferred; force-loaded - see [docs/features_and_flavors.md](docs/features_and_flavors.md)) |

`duckdb`, `postgres`, and `mysql` are **backend features**: all fill the same weak
`hl_db_feature_backends` hook (DSN-scheme-selected via the `HlDbBackend` vtable),
so `--with=duckdb --with=postgres --with=mysql` compose together into one
generated collector. `postgres` and `mysql` are pure C (no vendored engine), so
their archives are tiny (~48 KB); because a wire backend references base
`tls_client` (sslmode) + crypto (auth) that a DB-only app doesn't otherwise pull,
`hull build` wraps the platform lib + the archive in a GNU-ld `--start-group` at
compose (`base_group` in FEATURE_SPECS; Linux only - macOS ld64 rescans natively
and rejects the flag). Like DuckDB, the backend is reached by DSN and carries no
module gate, so selection is **explicit `--with=<name>`** (DSNs are often `$VAR`
env-refs, invisible at build time) rather than auto-inferred; e.g. a `mysql://`
DSN on a plain base fails with a `hull feature install mysql` hint. The kernel
sandbox grants `network_outbound` for a declared network DB connection
(`databases.named` net-DSN / env-ref, `databases.dynamic` net scheme, or a `-d`
net DSN), so a sandboxed DB-only app can reach its database.

**Install (end users):** `hull feature install <name>` / `hull feature list` /
`hull feature uninstall <name>` fetch + Ed25519-verify + cache the signed lib to
`~/.hull/feature/`, via the shared `hl_release_io_fetch_verified_manifest` - the
**same trust chain** as `hull flavor install` / `hull tools install` /
`hull update`, no new keys. The registry is the `FEATURES[]` table in
`src/hull/commands/feature.c` (the single registration point: one row per
feature + which native platforms publish it).

**Build from source:** `make feature-duckdb` / `make feature-gpu`. Each
re-invokes make with `HL_ENABLE_DUCKDB=1` / `HL_ENABLE_GPU=1` so the backend
object + the vendored static lib are in scope, then `ar`s them into one
self-contained `build/libhull_feature-<name>.a`. (The config-sentinel cleans
`build/` on the flag flip, so a feature archive build wipes the base objects -
build the base binary first if you need both.)

**Compose:** `hull build --with=duckdb` (or `--with=gpu`) resolves the lib
(local build dir → `~/.hull/feature/` → error with a `hull feature install`
hint), re-verifies a cache-sourced lib against its signed manifest offline
(closes the install-to-build TOCTOU, same as flavored builds), and generates a
`feature_registry.c` filling the base's **weak** `hl_db_feature_backends` /
`hl_gpu_feature_backends` hook with a **strong** override returning the composed
backend. `FEATURE_SPECS` in `stdlib/cli/lua/hull/build.lua` is the codegen
source of truth per feature (backend symbol, vtable type, hook name, C++ flag,
and extra link libs that can't live in a `.a` - DuckDB's `-lstdc++`, GPU's
`-lvulkan` / Metal frameworks). Selection can be **inferred** from the manifest
(a `duckdb://` connection) or **forced** with `--with=`. A `--with=` feature
falls back to the system compiler automatically (the default compiler-free emit
path only emits + links `app_registry.o`); a C++ feature (duckdb) additionally
needs a C++-capable driver, so `--compiler=system` resolves a system `cc`.

Verified end to end by `tests/e2e_feature_duckdb.sh` and
`tests/e2e_feature_gpu.sh` (each builds a base hull + the feature archive,
composes an app with `--with=`, runs it, and asserts a plain app stays
feature-free - the GPU one is build-only since `gpu.dispatch` needs a device
CI lacks).

### Composable runtime + HTTP base (the mandatory, auto-composed axis)

`--with=` features above are the **optional, additive** axis. There is a second,
**mandatory** composition axis that a plain `hull build` drives automatically:
the distributed hull's app-build base (the **SLIM** base) drops **every**
composable subsystem - it is **runtime-less, HTTP-core-less, compute-less,
image-less, SQLite-less, mbedTLS-less, AND Keel-event-loop-less** - and the
produced app composes back exactly what it uses. This is the endgame of "Hull is
completely modular and composable": the base is the minimum, and each subsystem
is whole-archived in **at build time** (static composition - *not* dynamically
loaded; no `dlopen`, no runtime plugins, so the manifest stays enforceable and
the build stays reproducible). Unlike a `--with=` feature, you never install or
flag these - they are **embedded in the distributed `hull`** and auto-composed.
(Issues #113 runtime, #114 HTTP, #118 WASM, image #138, TLS a2, Keel Phase 4;
cosmo is exempt - a fat APE can't force-load native feature archives, so its base
keeps everything compiled in.)

What the native base **drops** and what composes it back at `hull build`:

| Dropped from the base | Where it lives | Composed back |
|---|---|---|
| both interpreters (Lua VM, QuickJS) | `libhull_feature-{lua,js}.a` | exactly one, auto-inferred from the entry extension (`app.lua` → lua, `app.js` → js). Mandatory: an app must have a runtime to run. |
| the HTTP core caps (`cap/http` + async, `ws`, `smtp`, `body`) | `libhull_feature-http.a` | only when the app needs HTTP |
| the per-runtime web bindings (routes, dispatch, `res:*` helpers, `mod_http_*`/`mod_ws_*`/`sse`/`mod_smtp`, the in-process test harness, timers) | `libhull_feature-http-<rt>.a` | with the http core, only when the app needs HTTP |
| **the Keel event loop + HTTP server** (`serve.o` the KlServer loop, `async/keel.c`, `net/keel.c`, the server-only static/agent/test objects, + `libkeel.a` pulled on demand) | `libhull_feature-keel.a` | only when the app needs HTTP (the `needs_http` gate). A compute app runs `app.main` / `compute.async` on the base's Keel-free `async/poll.c` and links **zero Keel** |
| **mbedTLS + the crypto/TLS transport backends** (`cap_crypto_{hmac,asym}_mbedtls.o`, `tls_client.o`, `tls_transport.o`, Keel's `tls_mbedtls.o`, all of vendored mbedTLS) | `libhull_feature-tls.a` | when the app needs TLS: an HTTP module, or a `--with=postgres`/`mysql` net-DB backend (the `needs_tls` gate). A plaintext app links **zero mbedTLS** |
| the SQLite engine (`cap/db_sqlite`, vendored `sqlite3`, FTS5, the udf cap) | `libhull_feature-sqlite.a` + per-runtime udf bridge `libhull_feature-sqlite-<rt>.a` | only when the app uses `db` (the `needs_sqlite` gate) |
| the WASM caps + WAMR (`cap/wasm*`, `worker_wasm`, ~256 KB of vendored WAMR) | `libhull_feature-wasm.a` | only when the app needs compute (the `needs_wasm` gate below) |
| the per-runtime compute binding (`mod_compute` - `compute.*` + the `WasmBuffer` userdata) | `libhull_feature-wasm-<rt>.a` | with the wasm core, only when the app needs compute |
| the per-runtime tui bridge | `libhull_feature-tui-<rt>.a` (the tui cap core stays the installable `--with=tui` asset) | with `--with=tui`, only the app's runtime's bridge |
| the image codec caps + vendored stb (`cap/image`, `cap/image_stb`, `stb_impl`, ~146 KB) | `libhull_feature-image.a` | only when the app declares `hull/image` (the `needs_image` gate below) |
| the per-runtime image binding (`mod_image` - `image.*` + the `HlImage` userdata) | `libhull_feature-image-<rt>.a` | with the image core, only when the app declares `hull/image` |

Net for the distributed hull: a stock `hull build` of a compute-only `app.main`
links **zero Keel, zero mbedTLS, zero SQLite, zero WASM** (~2.1 MB); a full web
app composes all of them back. Every reduction is composition, not a flavor.

**The seam.** Each dropped piece leaves a **weak no-op default** in the base that
a **strong override** in the composed archive replaces (mirrors the gpu/tui
feature hooks). The HTTP seam is `include/hull/http_feature.h` (weak defaults in
`cap/http_feature.c`); a base TU `src/hull/http_weakstub.c` carries weak
real-signature stubs so the pure runtime's few references to the web bindings
link even when HTTP is not composed. The WASM seam is weak-stubs-only (no hook
header): `src/hull/wasm_weakstub.c` carries weak `hl_cap_wasm_*` / `hl_wasm_buffer_*`
defaults (referenced by `db_udf` / `mod_buffer` / `mod_image` / `mod_gpu` /
`app_context` / `serve`) plus the per-runtime compute-binding stubs - so a WASM-free
app links; a function `db.udf` still works while a WASM-backed one fails closed.
The IMAGE seam is weak-stubs-only too: `src/hull/image_weakstub.c` carries weak
`hl_image_new`/`hl_image_free` (referenced by `mod_gpu`'s `gpu.texture_read`), and
per-runtime `runtime/{lua,js}/image_stub.c` carry weak `luaopen_hull_image` /
`hl_js_init_image_module` (referenced by `modules.c`'s registration) - so an
image-free app links, and `HL_ENABLE_IMAGE` stays **defined** in the base
(resolver keeps reporting the cap; `modules.c`/`mod_gpu` need no `#ifdef` change).
The TLS seam is crypto weak-hooks (`include/hull/tls_feature.h`:
`hl_crypto_{hmac,asym}_active_backend` weak defaults → portable/fail-closed, a
composed `libhull_feature-tls.a` strong-overrides to mbedTLS) plus a transport
seam (`include/hull/tls_transport.h`: `hl_tls_*` weak in `tls_transport_stub.c`,
strong in `shared/tls_transport.c` = the sole in-Hull consumer of Keel's
`tls_mbedtls.o`). The KEEL seam is the async backend (`hl_async_backend()` weak →
Keel-free `async/poll.c`, strong override in `async/keel.c`), the app entry
(`hull_serve` weak in `serve_cli.c` = the Keel-free `app.main` runner, strong in
`serve.c` = the KlServer loop), and weak net-backend stubs; the base is compiled
Keel-free (`serve_cli.c` compiles clean under HTTP_SERVER=1 via these seams).
The archives are whole-archived at compose (no single anchor symbol), inside a
GNU-ld `--start-group` (native) / `-force_load` (ld64) with the platform lib -
`libkeel.a` stays merged in the base `.a` and is pulled on demand only by a
composed `serve.o`, so a compute app pulls none of it.

**"Needs IMAGE" is module-inferred.** `hull build` composes the image codec core +
the per-runtime image bridge only when the resolved manifest declares `hull/image`
(`req_caps & HL_MOD_CAP_IMAGE`, exposed as `needs_image` from
`tool.modules_resolve`, mirrors `needs_http`/`needs_wasm`). Unlike WASM this needs
no second signal: the only reachable `HlImage` producer is the `image` module
itself, so a declared `hull/image` is the whole gate. An image-free app links
**zero** stb (~146 KB smaller; `nm app | grep stbi_load_from_memory` → empty). The
subtractive `make HL_ENABLE_IMAGE=0` knob still exists (drops image entirely, no
feature to compose); the composable path is the default `HL_ENABLE_IMAGE=1` base.
See [docs/image_feature.md](docs/image_feature.md); covered by
`tests/e2e_feature_image.sh`.

**"Needs HTTP" is resolved, not guessed.** `hull build` composes the http core +
web bindings only when the resolved manifest trips an HTTP cap
(`hl_module_set_required_caps & HL_MOD_CAP_HTTP`, exposed as `needs_http` from
`tool.modules_resolve`). This is reliable because `app.get`/`app.post`/`app.ws`
/… are module-conditional decorations - nil unless the app declares
`hull/http-server`. A genuine `app.main` CLI / compute app with no HTTP module
links only the pure runtime and - on the distributed SLIM base - drops Keel +
mbedTLS + SQLite automatically (no flavor needed; `--flavor=pure-compute` only
adds the "reject any HTTP/TLS module" validation). The base defines **zero** HTTP
caps (verifiable: `nm libhull_platform.a | grep hl_cap_http_request` → empty).

**"Needs WASM" is a two-signal gate** (docs/wasm_feature.md). WASM is harder than
HTTP because it is not cleanly module-inferable: `db.udf` can be WASM-backed
(`cap/db_udf.c` calls `hl_cap_wasm_instance_*`) without any module declaration. So
`hull build` composes the wasm core + compute bridge iff **S1 or S2**: `S1` = a
declared WASM cap (`hull/compute`, `req_caps & HL_MOD_CAP_WASM`, exposed as
`needs_wasm` from `tool.modules_resolve`, mirrors `needs_http`); `S2` = the app
ships `compute/*.wasm` (catches the WASM-backed `db.udf`). A genuinely compute-free
app links **zero** WAMR (~256 KB smaller; verifiable: `nm app | grep
wasm_runtime_full_init` → empty). The base defines zero wasm caps (`nm
libhull_platform.a | grep hl_cap_wasm_init` → only the weak stub).

**Where it's wired.** The archives + embed live in the Makefile (`FEATURE_*_OBJS`,
`libhull_feature-*.a`, `embedded_{runtime,http,tui,wasm}.h`, `RUNTIME_FEATURE_LIBS`);
the compose + embedded-first resolve ladder is shared by `hull build` and
`hull eject` via `stdlib/cli/lua/hull/feature_compose.lua`
(`resolve_runtime_lib` / `resolve_http_lib` / `resolve_http_rt_lib` /
`resolve_tui_rt_lib` / `resolve_wasm_lib` / `resolve_wasm_rt_lib`) +
`build_assets.c` (`hl_build_extract_feature_*`). Design:
[docs/http_feature_phase1.md](docs/http_feature_phase1.md),
[docs/wasm_feature.md](docs/wasm_feature.md). Covered by
`tests/e2e_feature_runtime.sh` (runtime slim, both runtimes),
`tests/e2e_build_flavor.sh` (pure-compute × runtime, symbol-level Keel/http drop),
`tests/e2e_feature_tui.sh` (tui × runtime, both runtimes), and
`tests/e2e_feature_wasm.sh` (compute-free drops WAMR + the needs_wasm gate + a
composed `compute.call`, both runtimes).

### Extension taxonomy: feature vs flavor vs tool vs stdlib

A new capability reaches an app through exactly one of four shipping units.
Classifying it correctly is the difference between a signed static archive and
twenty lines of Lua. The four, and the single question that separates each:

| Unit | What it is | Ships as | Reached via | Axis |
|------|-----------|----------|-------------|------|
| **stdlib** | pure Lua/JS built on capabilities the base already has (no new C, no new authority) | always in the base VFS | `require("hull.X")` / `import "hull:X"` + `manifest.modules` | orchestration |
| **feature** | a large optional C subsystem, off by default, adding a new vendored engine, wire backend, or authority | signed `libhull_feature-<name>-<arch>.a` | `hull build --with=<name>` (auto-inferred or forced) | **additive** |
| **flavor** | a build.lua **preset** validating the app against a slimmer cap set (since Phase 4.3 - the base already composes; pre-built per-flavor libs are gone) | (none - a preset on the default base) | `hull build --flavor=<name>` | **subtractive** |
| **tool** | a separate companion **program** Hull spawns (never linked in) | `hull-<tool>-<platform>` binary | `hl_tool_spawn` at build time; `hull tools install` | is-it-Hull-or-a-program-Hull-runs |

**Decision procedure** (ask in order; first yes wins):

1. **Is it a separate program Hull executes, not code linked into the app?**
   (a compiler, an AOT/optimizer pass, a codegen binary) → **tool**. Version-
   coupled to the release, side-loaded, resolved via `hl_tools_lookup_path`.
   Example: `wamrc`.
2. **Can it be built entirely on existing capabilities** (`http`/`crypto`/`fs`/
   `db`/`compute`/…) **with no new C and no new authority?** → **stdlib module**.
   If the answer is "it's just HTTP + a signing scheme" or "it's composition over
   caps we already ship," it does NOT get a C archive. Example: an S3 /
   object-storage client is `http.fetch` + SigV4 (`crypto`) → stdlib, never a
   feature. Same for most webhook/integration clients.
3. **Does it add a large optional C subsystem or a new authority, off by
   default?** (a vendored engine, a pure-C wire protocol, a new hardware/OS
   surface) → **feature**. It fills a base-resident **weak hook** with a
   **strong override** from the composed archive; register one row in
   `FEATURES[]` (`src/hull/commands/feature.c`) + one `FEATURE_SPECS` entry
   (`stdlib/cli/lua/hull/build.lua`). Examples: `duckdb`, `postgres`, `mysql`,
   `gpu`, `tui`.
4. **Are you turning a default subsystem OFF to produce a smaller base?** →
   **flavor** (preset). Example: `pure-compute`.

**The sharp rules.** New vendored C or new authority → **feature** (additive).
Turning a default off → **flavor** (subtractive). A separate program → **tool**.
Pure orchestration over existing caps → **stdlib** (and this is the most common
misclassification: reach for stdlib before a feature). "On by default and you
subtract" is a flavor; "off by default and you add" is a feature. HTTP itself is
just a feature that happens to be on by default, which is why the flavor/feature
line is a distribution fact (enumerable pre-published base vs. combinatorial
bolt-on), not an architectural one. Full rationale in
[docs/features_and_flavors.md](docs/features_and_flavors.md); near-term
candidates classified against this table live in
[docs/roadmap.md](docs/roadmap.md) ("Extension taxonomy and near-term targets").

### Pure-compute builds (`HL_ENABLE_HTTP=0`)

`make HL_ENABLE_HTTP=0` turns **both** HTTP halves off (it's the back-compat alias that pins `HL_ENABLE_HTTP_SERVER=0` and `HL_ENABLE_HTTP_CLIENT=0`). This is the only flavor that drops **mbedTLS and Keel entirely**. Use it for an offline, network-free compute or signing binary: a WASM/GPU transform pipeline, a local data tool, an air-gapped batch job. Apps run via `app.main(fn)`.

What's removed:
- `vendor/mbedtls/**` (the whole TLS stack) and Keel's `libkeel.a`
- Outbound `http.fetch` (cap/http + cap/http_async), SMTP (cap/smtp), `hull update`
- The inbound HTTP server, routing, middleware, WebSocket, SSE, and the in-process test harness

What's unavailable to app code:
- `http`, `ws.*`, `sse` and every `hull/web/*` module; `hull/http-client`, `hull/http-server`, `hull/smtp`, `hull/email`
- `hull dev` / `hull test` / `hull agent` / `hull mcp` (they need the HTTP server) and `hull update` (needs the HTTPS client)

What still works:
- Lua / QuickJS runtimes, sandbox, instruction limits, audit logging, `app.main`
- `compute.*` (WASM), `gpu.*`, `fs.*`, `db.*` (unless also `HL_ENABLE_DB=0`), `time.*`, `env.*`, templates, image codecs, CSV, i18n, validation, `hull.tui`
- **`crypto.*` in full, including `crypto.hmac_*` and `hull/jwt`.** mbedTLS is gone, so Hull's own hashing falls back to in-tree implementations: SHA-256 is the cap layer's self-contained transform (HW-accelerated where available), SHA-1 is hand-rolled in `cap/crypto.c` (RFC 3174), SHA-512 comes from TweetNaCl, and HMAC routes through the portable backend `hl_crypto_hmac_backend_portable` (selected via the `HL_HMAC_BACKEND` macro only in this flavor; HTTP builds keep the mbedTLS HMAC backend byte-for-byte). Ed25519 / NaCl box / secretbox are TweetNaCl as always.
- `hull build`, `hull compute`, `hull sbom`, `hull doctor`, `hull cache`, `hull keygen`, `hull verify` / `verify-self` / `verify-release`

> **Invariant for contributors:** core C code (`cap/`, `commands/`, `shared/`) must hash via `hl_cap_crypto_sha256` / the cap HMAC entry points, **never** `mbedtls_sha*` / `mbedtls_md_hmac` directly. A direct mbedTLS hash call re-breaks this flavor at link time (the `flavors` CI job catches it).

Binary size on arm64 Darwin: ~5.8 MB vs ~6.5 MB for the default build. Combine with `HL_ENABLE_DB=0` (and optionally `HL_ENABLE_TUI=0`) for the smallest possible compute runtime.

Pure-compute is a **build flavor, not a published release artifact**. The signed `hull.sha256` release manifest covers the four standard binaries (`hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`, `hull-cosmo`), all full-HTTP. Build pure-compute from source with the flag above; publishing a signed pure-compute binary would be a separate release-pipeline decision (new matrix entry + manifest line + `install.sh` flavor).

### Compute-only builds (`HL_ENABLE_DB=0`)

`make HL_ENABLE_DB=0` produces a hull binary without SQLite. Use when the deployment is a pure compute service (REST endpoints that wrap WASM/GPU shaders, transform pipelines, signing services) and any persistent state lives elsewhere (Redis, Postgres-over-HTTP, S3).

What's removed:
- `vendor/sqlite/sqlite3.c` (~700 KB compiled)
- `src/hull/cap/db.c`, `cap/db_sqlite.c`, `cap/db_udf.c`
- `src/hull/migrate.c`, `commands/migrate.c`, `agent/db.c`, `worker_db.c`
- `runtime/{lua,js}/mod_db.c`, `runtime/{lua,js}/worker_db.c`

What's unavailable to app code:
- `db` global (`db.query`, `db.exec`, `db.batch`, `db.udf.*`, `db.async.*`)
- `hull migrate` subcommand and `hull agent db|migrate` subcommands
- The `migrate`/`outbox`/`inbox`/`session`/`idempotency`/`rbac`/`search`/`ratelimit` stdlib modules. They all assume `db`. Apps importing them will fail at module load.

What still works:
- Full HTTP routing, middleware, request/response handling
- Lua / QuickJS runtimes, sandbox, instruction limits, audit logging
- `http.fetch`, `ws.*`, `fs.*` (filesystem capability with manifest allowlist), `crypto.*`, `compute.*`, `gpu.*`
- Templates, static files, image codecs, SSE, timers, validation, CSV, i18n, CORS, ETag, health, JWT, CSRF (the stateless variant), form parsing, logger
- `hull build`, `hull dev`, `hull test`, `hull agent` (minus the `db`/`migrate` subcommands)

Binary size on arm64 Darwin: ~3.66 MB vs ~5.06 MB with DB (about 28% smaller).

### Image-less builds (`HL_ENABLE_IMAGE=0`)

`make HL_ENABLE_IMAGE=0` produces a hull binary without the image codec
subsystem. Use for a compute / CLI / signing binary that never decodes or
encodes images (a WASM/GPU transform pipeline, an air-gapped batch job, a data
tool). Image is **on by default** (web apps want avatars / thumbnails), so this
is a subtractive flavor knob like `HL_ENABLE_DB=0` - a make-level switch, not a
`hull build --with=` feature and not exposed as a `--flavor` preset.

What's removed:
- `src/hull/cap/image.c`, `cap/image_stb.c` (the codec vtable + stb backend)
- `runtime/{lua,js}/mod_image.c` (the `image` module bindings)
- `vendor/stb/stb_impl.c` (stb_image + stb_image_write - image is its **only**
  consumer, so it drops entirely; ~146 KB smaller on arm64 Darwin)

What's unavailable to app code:
- The `hull/image` module (`image.new/decode/encode/from_buffer`, the `HlImage`
  userdata). A non-optional `"hull/image@1"` declaration is a hard app-load
  error (`requires HL_ENABLE_IMAGE (build-time)`); declare it optional
  (`"hull/image@1?"`) to have `require("hull.image")` return nil / `import`
  bind null and fall back gracefully.
- On a `HL_ENABLE_GPU=1 HL_ENABLE_IMAGE=0` build only the **image bridge** of the
  GPU texture API drops: `gpu.texture(img)` no longer accepts an `HlImage` (raw
  bytes + `{width,height,format}` still work) and `gpu.texture_read` /
  `textureRead` (which return an `HlImage`) are compiled out. `gpu.buffer_read`
  and the rest of `gpu.*` are unaffected.

What still works: everything else - Lua/JS runtimes, `db.*`, `http.*`,
`compute.*`, `gpu.*` (buffers/dispatch/pipeline), `crypto.*`, `fs.*`, templates,
CSV, i18n, `hull build/dev/test/agent`. The image-less link is CI-covered by the
`flavors` matrix (`HL_ENABLE_IMAGE=0`); combine with `HL_ENABLE_DB=0` /
`HL_ENABLE_HTTP=0` for a minimal compute runtime.

### PostgreSQL + multi-backend DB

Hull's database layer is backend-agnostic behind the `HlDbBackend` vtable
(`include/hull/cap/db_backend.h`). Three backends ship: embedded **SQLite**
(default), an optional pure-C **PostgreSQL** wire client (no libpq), and an
optional pure-C **MySQL / MariaDB** wire client (no libmysql/libmariadb).
All are chosen per-connection by DSN scheme via `hl_db_backend_select`
(`cap/db_select.c`): each backend declares the `://` schemes it claims (SQLite:
`sqlite`, `file`; Postgres: `postgres`, `postgresql`; MySQL: `mysql`,
`mariadb`) and the selector matches the DSN's scheme against the compiled-in
backends (`BACKENDS[]`, the sole registration point). A scheme-less DSN (a bare
path, `:memory:`, or a single-colon `file:` URI) defaults to SQLite. A
reserved-but-uncompiled scheme (`duckdb://`, `mysql://` / `mariadb://` without
`HL_ENABLE_MYSQL`, or `postgres://` without `HL_ENABLE_POSTGRES`) fails with a
specific hint; an unknown scheme with a generic one. Adding a backend needs no
change to the selector, just a `.schemes` declaration + one `BACKENDS[]` line.

**Abstract interface vs concrete backends (§2.3).** `cap/db_backend.h` is the
pure interface: the vtable, `HlDbHandle`, the inline `hl_db_*` wrappers, and
`hl_db_backend_select` - no concrete-backend symbol. Each backend's
`extern const HlDbBackend` + backend-specific helpers live in its own header
(`cap/db_sqlite.h`, `cap/db_postgres.h`), included only by the registry
(`db_select.c`) and the few SQLite-aware consumers. Code that needs a backend's
native connection handle (udf registration, agent introspection) goes through
the generic `hl_db_backend_native_handle(h, &tag)` (tag is `HL_DB_NATIVE_SQLITE`
/ `_POSTGRES` / `_NONE`) instead of a per-backend `hl_db_<x>_raw`; SQLite keeps a
typed `hl_db_sqlite_raw` convenience in `db_sqlite.h` built on top of it.
`hl_db_sqlite_wrap`/`_unwrap` (wrap an externally-opened `sqlite3*`) also live in
`db_sqlite.h`. A new backend sets its `native_tag` + optional `native_handle`;
consumers needing its handle add a tag case, not a header symbol.

**Dialect surface - identifier quoting (§2.4).** The vtable carries a
`char identifier_quote` (`"` for SQLite / Postgres / DuckDB, `` ` `` for MySQL);
`hl_db_quote_ident(h, name, out, sz)` wraps a name in it, doubling any internal
occurrence (reserved-word- and injection-safe). Exposed to app / stdlib code as
`conn.quote_identifier(name)` (Lua) / `conn.quoteIdentifier(name)` (JS). The
stdlib uses it where an app-supplied identifier flows into multi-backend SQL
(e.g. `hull/web/auth-flows`'s `standard_users` table name), so a table named
`order` / `user` works and a MySQL backend drops in unchanged. (Blanket-quoting
the shared `insert_if_absent` / `upsert` helpers is deliberately NOT done: it
would change Postgres case-folding semantics for existing apps.)

`conn.udf` is present only on a backend that supports user-defined functions
(§2.5): a SQLite connection has it, a Postgres connection does not (checking
`conn.udf ~= nil` / `!!conn.udf` is the capability probe), so there is no
"method present but fails at call time". `conn.autoincrement_id_ddl` (§2.6)
stays exposed as an **escape hatch** for the stdlib's portable `CREATE TABLE`
(audit-log, outbox); apps should express schema in **migrations**, not by
interpolating this dialect DDL fragment.

**Handles-only API (no top-level `db.*`).** The `hull/db` module exposes only
connection acquisition; every query goes through an explicit connection object:

```lua
local db = require("hull.db").default()   -- the default connection (-d / "default")
db.query("SELECT ...", { ... })
db.exec("INSERT ...", { ... })
local cache = require("hull.db").connect("cache")  -- a named connection
cache.query(...)
local tmp = require("hull.db").open(dsn)   -- a caller-owned dynamic connection
tmp.query(...); tmp.close()                -- app owns it: close() or let GC finalize
```

```javascript
import { db as dbModule } from "hull:db";
const db = dbModule.default();            // JS: acquire the default connection
db.query("SELECT ...", [ ... ]);
const cache = dbModule.connect("cache");
const tmp = dbModule.open(dsn);           // caller-owned dynamic connection
tmp.query(...); tmp.close();
```

The connection object carries `query` / `exec` / `batch` / `last_id` (`lastId`)
/ `insert_if_absent` (`insertIfAbsent`) / `upsert` / `table_columns`
(`tableColumns`) / `quote_identifier` (`quoteIdentifier`) / `backend_name`
(`backendName`) / `autoincrement_id_ddl`
(`autoincrementIdDdl`), plus `async` (`.async.query/exec`) and `udf`
(`.udf.register/unregister`). `async` and `udf` are **per-connection**:
`db.connect("cache").async.query(...)` opens the worker pool's own per-thread
connection to that database (the worker cache is keyed by DSN), and
`db.udf.register` lands on the connection it is called on. The worker resolves
which database via the registry's DSN for the bound handle
(`hl_db_registry_dsn_for`); a `udf` on a non-SQLite connection errors at call
time (udf is SQLite-only). Covered by `tests/e2e_named_connections.sh` (both
runtimes) and the cross-backend variant in `tests/e2e_postgres.sh`.

**Named connections via the manifest.** Additional connections are declared
under `manifest.databases.named` (a name -> DSN map). A DSN value of exactly
`"$VAR"` or `"${VAR}"` is an env reference resolved at connection-open time (so
credentials never sit in app source); a value that merely CONTAINS a `$` (e.g.
a password) is literal.

```lua
app.manifest({
    modules = { "hull/db@1" },
    databases = {
        named = {
            cache   = "./cache.db",       -- SQLite file (literal)
            primary = "$DATABASE_URL",    -- postgres:// from $DATABASE_URL (env ref)
        },
        dynamic = { hosts = { "*.rds.amazonaws.com" }, schemes = { "postgres" } },
    },
})
```

(The DB connection dials its host directly; `manifest.hosts` gates
`http.fetch`, not database connections.)

**Dynamic connections (`db.open(dsn)`, roadmap §2.2).** For a DSN not known at
manifest-authoring time (a per-tenant shard, a user-supplied endpoint),
`db.open(dsn)` opens a **caller-owned** connection after validating the DSN
against `manifest.databases.dynamic`: the scheme must be in `dynamic.schemes`,
and a network backend's host must match `dynamic.hosts` (exact, `*`, `*.suffix`
subdomain glob, or a CIDR; IP-literal hosts only, no DNS) while a file backend's
path must pass the same fs-sandbox gate as `fs.read`. `hosts` / `schemes`
entries may be `"$VAR"` env refs. Both lists fail closed: no `dynamic` policy (or
an empty one) rejects every `db.open`. A process-wide cap (16) bounds concurrent
dynamic connections. Unlike `db.connect`, the app **owns** the returned handle:
call `conn.close()` (`conn.close()` in JS) when done, or let GC finalize it
(double-close and use-after-close are safe and fail closed). A dynamic handle
carries the sync methods (`query`/`exec`/`batch`/`insert_if_absent`/`upsert`/
`table_columns`) plus `async` (`conn.async.query/exec`): the connection object
reports its own DSN (`db_call_dsn` / `js_call_dsn`, symmetric with the handle
resolver) so `conn.async` targets the dynamic database through the worker pool,
which bounds its per-thread connection cache with an LRU
(`HL_WORKER_DB_MAX_CONNS`) so churning through many dynamic DSNs never grows
unbounded. `async`-after-close fails closed (the live-handle guard). `udf` on a
dynamic handle is intentionally absent (worker-side udf re-registration is keyed
off the registry a dynamic handle is not in; tracked follow-up). Note the
`db.async` + `:memory:` caveat above applies: for async use a file-backed
dynamic DSN, not `:memory:`. Enforcement lives in `cap/db_dynamic.c` +
`cap/host_match.c`; covered by `tests/e2e_dynamic_connections.sh` (both runtimes,
SQLite, incl. async) and the Postgres CIDR allow/deny phase in
`tests/e2e_postgres.sh`.

The connection named `"default"` is what `db.default()` and the stdlib target;
when an app just uses `-d <DSN>` with no `databases` map, that becomes the
`"default"` connection. Connections open **lazily** on first use and are cached
by name (per process for the sync path; per worker thread for `db.async`).
Architecturally the registry (`cap/db_registry.c`) owns every connection,
including the default -- there is no distinguished default-handle field;
consumers resolve it via `hl_db_registry_default`. `db.connect(name)` resolves
after startup (the manifest is applied post-load), so call it from `app.main`
or a handler, not at module top-level; `db.default()` works everywhere.

**`db.async` + `:memory:` (SQLite) caveat.** `db.async` runs on the worker
pool, where each thread opens its OWN connection to the target DSN (keyed by
DSN). A bare `:memory:` DSN is a connection-PRIVATE database in SQLite: every
open is a fresh empty DB, so a worker's `:memory:` never sees the rows the
sync (event-loop-thread) connection wrote. This is SQLite semantics, not a
Hull bug. For state that async workers must see, use a **file** (a normal
SQLite path, or a tmpfs path like `/dev/shm/app.db` on Linux for RAM-speed):
every worker opens the same path, so they share one database, and WAL gives
concurrent readers. Postgres is shared by nature. (A named shared-cache
in-memory DB, `file:x?mode=memory&cache=shared`, would also share, but Hull
opens via plain `sqlite3_open`, which does not parse `file:` URIs today; see
roadmap §2.9.)

**PostgreSQL specifics** (`HL_ENABLE_POSTGRES=1`):
- **Auth:** SCRAM-SHA-256 (the postgres:16 default) and `trust` / cleartext.
  MD5 is rejected. Reuses `cap/crypto` (SHA-256 / HMAC / PBKDF2).
- **TLS:** `sslmode=disable|prefer|require|verify-ca|verify-full` in the DSN
  (`?sslmode=...`), default `prefer`. `verify-full` checks the chain +
  hostname against the embedded CA bundle via the shared `shared/tls_client.c`
  helper (the same KlTls handshake SMTP uses). `HL_LINK_TLS` links Keel +
  mbedTLS whenever an HTTP half OR Postgres is enabled.
- **Types:** typed decode by OID (bool/int/float/text/bytea); `?` placeholders
  are rewritten to `$n`; params bound in text format (SQL injection
  impossible). A Lua/JS params array with trailing nils binds the tail as NULL,
  matching SQLite.
- **Migrations** run through the vtable; multi-statement migration files use
  the Postgres simple-query protocol. The `_hull_migrations` tracking table is
  dialect-portable (name PK, host-generated ISO-8601 `applied_at`).
- **SQLite-only features under Postgres:** `db.udf` and `hull/search` (FTS5)
  are SQLite-only and fail with a clear error on a Postgres connection.

**MySQL/MariaDB specifics** (`HL_ENABLE_MYSQL=1`). One backend serves both
`mysql://` and `mariadb://` (MariaDB is a MySQL fork on the same wire
protocol). Codec (`cap/mysqlwire.c`) is little-endian, 3-byte-length +
1-byte-sequence framed, with length-encoded ints/strings; all server-message
parsing is bounds-checked over untrusted input (mirrors `cap/pgwire.c`).
- **Auth:** `mysql_native_password` (SHA-1 challenge-response) and
  `caching_sha2_password` (MySQL 8 default: SHA-256 fast path always;
  full-auth sends the cleartext password over TLS; the plaintext RSA
  public-key exchange is deferred with a "set `sslmode=require`" hint). The
  server-named handshake plugin drives which is used, and AuthSwitchRequest
  re-dispatches through the same path. `client_ed25519` (MariaDB) is not yet
  supported and fails with a clear hint pointing at the two supported plugins
  (it needs ed25519 group-ops TweetNaCl keeps private; tracked follow-up).
  Reuses `cap/crypto` (SHA-1 / SHA-256).
- **TLS:** `sslmode=disable|prefer|require|verify-ca|verify-full` (same names +
  default `prefer` as Postgres). When TLS is wanted and the server advertises
  `CLIENT_SSL`, the client sends an SSLRequest, runs the blocking handshake
  over the shared `shared/tls_client.c` (embedded CA bundle; `verify-*` checks
  chain + hostname), then sends the credentialed HandshakeResponse41 over TLS.
  All post-handshake I/O tunnels through `KlTls`.
- **Queries / types:** param-less statements use the text `COM_QUERY` protocol;
  **parameterized** statements use the binary prepared-statement protocol
  (`COM_STMT_PREPARE` / `EXECUTE` / `CLOSE`) so values never touch the SQL text
  (injection impossible). Binary rows decode by column type to `HlValue`
  (int / double / borrowed text; `DATE`/`DATETIME`/`TIMESTAMP`/`TIME` are
  formatted to ISO-8601-ish strings). A Lua/JS params array with trailing nils
  arrives short (Lua's `#` drops the tail), so `COM_STMT_EXECUTE` pads to the
  statement's declared `num_params`, binding the tail NULL, matching
  SQLite/Postgres.
- **Dialect:** `insert_if_absent` builds `INSERT IGNORE`; `upsert` builds
  `INSERT ... ON DUPLICATE KEY UPDATE c = VALUES(c)` (MariaDB / MySQL 5.7+);
  `last_id` returns the OK packet's `last_insert_id`; `table_columns` queries
  `information_schema.columns` scoped to `DATABASE()`; identifier quoting is
  backtick (`` ` ``). MySQL 8 has no `CREATE INDEX ... IF NOT EXISTS`, so the
  backend transparently rewrites it to a plain `CREATE INDEX` and treats a
  duplicate-index error as success (the stdlib's idempotent index DDL works
  unchanged). Multi-statement migration files run as one `COM_QUERY`
  (`CLIENT_MULTI_STATEMENTS`), draining every result set.
- **Portable stdlib DDL:** MySQL rejects a `TEXT`/`BLOB` primary key or index
  without a prefix length, so every keyed / indexed / foreign-key text column
  in the DB-backed stdlib (and the `_hull_migrations` tracking table) is
  `VARCHAR(255)`, not `TEXT` (TEXT affinity on SQLite, varchar on Postgres,
  indexable on MySQL); data-only columns stay `TEXT`.
- **SQLite-only features under MySQL:** `db.udf` and `hull/search` (FTS5) are
  SQLite-only and fail with a clear error on a MySQL connection (checking
  `conn.udf ~= nil` is the capability probe).

**Flag combinations.** `HL_ENABLE_SQLITE` (default 1), `HL_ENABLE_POSTGRES`
(default 0), and `HL_ENABLE_MYSQL` (default 0) are independent; `HL_ENABLE_DB`
is the derived umbrella (on iff any is). `make HL_ENABLE_POSTGRES=1` /
`make HL_ENABLE_MYSQL=1` builds SQLite plus that backend;
`make HL_ENABLE_SQLITE=0 HL_ENABLE_POSTGRES=1` (or `HL_ENABLE_MYSQL=1`) builds a
single-backend binary (SQLite dropped -- so `db.udf` and `hull/search` are
absent, and the SQLite-file agent introspection
`hull agent db|migrate|schema-diff|sql` is compiled out). CI covers the
`sqlite + postgres`, `postgres-only`, `sqlite + mysql`, and `mysql-only` link
flavors, the pg + mysql fuzzers, and full `e2e_postgres` / `e2e_mysql` jobs
(real Postgres 16 / MySQL 8 in Docker: auth + TLS + migrations + `db.async` +
stdlib). Design + rationale:
[docs/postgres_backend_design.md](docs/postgres_backend_design.md) and the
`§2.10` MySQL epic in [docs/roadmap_next.md](docs/roadmap_next.md).

### Lua/JS orchestration overhead for compute-heavy workloads

The runtime sandboxes are designed so the hot path of a compute request is dominated by the C/WASM/GPU work, not the script glue:

- Request entry → C dispatcher → Lua/JS handler is a single C→script call (~µs).
- Inside the handler, `compute.call(name, input)` / `gpu.dispatch(...)` is one C call that hands the request off to WAMR (interpreter or AOT) or wgpu-native. The script suspends until completion.
- For large inputs use the unified buffer protocol (`fs.mmap`, `WasmBuffer`, `ArrayBuffer`) so bytes never round-trip through a Lua string / JS typed array copy.
- `compute.async.call` / `gpu.async.dispatch` dispatch to the thread pool and yield to the event loop. Other requests are served while the GPU/WASM job runs.
- Per-request instruction limits (default 100M) cap script-side cost; the bytecode interpreters themselves run at hundreds of MIPS.

Empirically, when a request executes a non-trivial AOT WASM or GPU dispatch, script overhead is sub-millisecond on top of multi-millisecond compute work. If a workload becomes orchestration-bound (very small jobs, hot loop dispatching to GPU), the right fix is usually to batch in the shader (e.g. one `gpu.pipeline` covering multiple stages) rather than to drop the script layer. The C `hl_cap_*` boundary is what makes the build reproducible and the manifest enforceable.

### Dependencies

All vendored. No external dependencies:

| Library | Location | Purpose |
|---------|----------|---------|
| Keel | `vendor/keel/` (git submodule) | HTTP server library (async primitives, thread pool) |
| Lua 5.4 | `vendor/lua/` | Application scripting |
| QuickJS | `vendor/quickjs/` | ES2023 JavaScript runtime |
| SQLite | `vendor/sqlite/` | Embedded database |
| mbedTLS | `vendor/mbedtls/` | TLS client |
| TweetNaCl | `vendor/tweetnacl/` | Ed25519 + NaCl crypto |
| pledge/unveil | `vendor/pledge/` | Linux kernel sandbox polyfill |
| log.c | `vendor/log.c/` | Logging |
| sh_arena | `vendor/sh_arena/` | Arena allocator |
| sh_json | `vendor/sh_json/` | Streaming JSON writer + arena-based parser |
| WAMR | `vendor/wamr/` (git submodule) | WebAssembly Micro Runtime (compute plugins) |
| wgpu-native | `vendor/wgpu/` | GPU compute backend (optional, `HL_ENABLE_GPU=1`) |
| utest.h | `vendor/utest.h` | Unit test framework |

## Project Structure

```
include/hull/           # Public headers
  cap/                  #   Capability module headers (db.h, fs.h, crypto.h, etc.)
  commands/             #   Command headers (build.h, test.h, verify.h, etc.)
  runtime/              #   Runtime headers (lua.h, js.h)
src/hull/               # Core source
  cap/                  #   Capability implementations (db.c, fs.c, crypto.c, http.c, tool.c, etc.)
  commands/             #   Subcommand implementations (build.c, test.c, verify.c, etc.)
  runtime/lua/          #   Lua 5.4 runtime (bindings.c, modules.c, runtime.c)
  runtime/js/           #   QuickJS runtime (bindings.c, modules.c, runtime.c)
  utils/                #   Generic leaf utilities, no Hull-domain knowledge
                        #   (alloc, path_normalize, compress, csp, plus the
                        #   header-only parse_size/macros/buffer/limits)
  shared/               #   Cross-cutting infrastructure used across cap/,
                        #   commands/, and runtime/ (cache_dir, cache_registry,
                        #   blob_store, thread_affinity, log_lock, async)
  vfs.c                 #   Unified Virtual Filesystem (O(log n) binary search over HlEntry arrays)
  static.c              #   Static file serving middleware (/static/* convention)
stdlib/                 # Embedded standard library
  lua/hull/             #   User-facing Lua modules apps may require: template,
                        #   jwt, csrf, cookie, csv, email, form, i18n, json,
                        #   search, validate, plus middleware/*
  js/hull/              #   User-facing JS modules (parallel to Lua side)
  cli/lua/hull/         #   CLI plugins invoked only by the C dispatcher
                        #   (`hull build`, `hull deploy`, `hull init`, etc.)
                        #   via hull_tool. Never imported by app code.
                        #   Same `hull.X` require name as user-facing modules
                        #   (Makefile strips both prefixes); the split exists
                        #   so the user-facing directory honestly reflects
                        #   what apps can import. `hull new`'s modular
                        #   templates (templates_rest.lua, etc.) also live
                        #   here. They're embedded scaffolds, not user code.
vendor/                 # Vendored libraries (do not modify)
tests/                  # Unit tests (test_*.c) and E2E scripts (e2e_*.sh)
  fixtures/             #   Test fixtures (null_app, etc.)
  hull/                 #   Hull-specific test suites
examples/               # 11 example apps (hello, rest_api, auth, jwt_api, todo, compute, etc.)
docs/                   # Architecture, security, roadmap, audit documentation
templates/              # Build templates (app_main.c, entry.h)
```

## Architecture

### System Layers

```
Application Code (Lua/JS)  →  Standard Library (stdlib/)
        ↓
Runtimes (Lua 5.4 / QuickJS)  →  Sandboxed interpreters
        ↓
Capability Layer (src/hull/cap/)  →  C enforcement boundary
        ↓
Hull Core (main.c, manifest.c, sandbox.c, signature.c, static.c, vfs.c)
        ↓
Keel HTTP Server (vendor/keel/)  →  Event loop + routing + async + thread pool
        ↓
Kernel Sandbox (pledge/unveil/seatbelt)  →  OS enforcement
```

Each layer talks only to the one below it. Application code cannot bypass capabilities.

### Orchestration vs Compute

Hull separates control-plane orchestration from data-plane computation:

- **Orchestration (Lua/JS):** Request handling, routing, middleware, database queries, template rendering. Runs in sandboxed Lua 5.4 or QuickJS interpreters with capability-mediated system access.
- **Compute (WASM, planned):** CPU-intensive data processing (scoring, transformation, deduplication). Runs in WAMR's isolated linear memory with no I/O imports, gas-metered execution, and configurable memory caps. See `docs/wamr_architecture.md`.

This separation means orchestration code has full capability access (mediated by the C layer), while compute plugins are pure functions with no side effects.

### Dual-Runtime Design

Hull supports Lua 5.4 and QuickJS (ES2023). Only one is active per application. Selected by entry point extension (`.lua` or `.js`). Both runtimes implement the same polymorphic vtable (`HlRuntimeVtable`) and call the same C capability functions.

### Capability Layer (`hl_cap_*`)

All system access is mediated by C capability functions. Neither runtime touches SQLite, filesystem, or network directly.

| Module | File | Key Functions |
|--------|------|---------------|
| Database | `cap/db.c` | `hl_cap_db_query()`, `hl_cap_db_exec()`, `hl_cap_db_begin/commit/rollback()` |
| Filesystem | `cap/fs.c` | `hl_cap_fs_read()`, `hl_cap_fs_write()`, `hl_cap_fs_exists()`, `hl_cap_fs_delete()` |
| Crypto | `cap/crypto.c` | SHA-256/512, HMAC, PBKDF2, Ed25519, secretbox, box, random |
| HTTP client | `cap/http.c` | `hl_cap_http_request()` with host allowlist |
| Environment | `cap/env.c` | `hl_cap_env_get()` with manifest allowlist |
| Time | `cap/time.c` | `hl_cap_time_now()`, `_now_ms()`, `_clock()`, `_date()`, `_datetime()` |
| Tool (build mode) | `cap/tool.c` | `hl_tool_spawn()`, `hl_tool_find_files()`, `hl_tool_copy()`, `hl_tool_mkdir()` |
| Test | `cap/test.c` | In-process HTTP dispatch, assertions |
| Body | `cap/body.c` | Request body handling |
| WASM compute | `cap/wasm.c` | `hl_cap_wasm_init()`, `_load()`, `_call()`. WAMR compute plugins |
| GPU compute | `cap/gpu.c`, `cap/gpu_wgpu.c` | `hl_cap_gpu_init()`, `_compile()`, `_dispatch()`. Wgpu-native compute shaders |
| TUI | `cap/tui.c`, `cap/tui_input.c`, `cap/tui_width.c` | `hl_cap_tui_acquire/release()`, `_size()`, `_move/print/style/flush()`, `_poll()`, `_clipboard_set()`. Terminal UI w/ cell-diff rendering, ANSI parser, OSC 11 theme detect |
| Audit | `cap/audit.c` | Structured capability audit logging (JSON to stderr) |

### Request Flow

```
Client → Keel HTTP → Route Match → hl_{lua,js}_dispatch() → Handler → KlResponse
                                           ↓
                                    hl_cap_* API (shared C)
                                           ↓
                                    SQLite / FS / Crypto / HTTP
```

### App Lifecycle

One unified flow; the runtime picks the behavior from what the app
registers.

```
1. Process start; argv parsed
2. Init runtime; kernel sandbox phase 1
3. Load app.{lua,js}  ── top-level runs once:
     app.manifest({...})
     // any combination of:
     app.main(fn)
     app.get(...), app.use(...), app.ws(...), app.sse(...),
     app.every(...), app.daily(...)
4. Extract manifest; run module resolver; sandbox phase 2
5. Run migrations (HL_ENABLE_DB + ./migrations/ + not --no-migrate)
6. If app.main is registered, invoke it once on the event-loop thread.
     ctx = { args, env, stdin, stdout, stderr }
     async ops (compute.async / gpu.async / http.fetch / hull.sleep)
     yield via coroutine/Promise. Main awaits naturally.
7. After main returns:
     - non-zero return → exit with that code, skip the serve loop
     - zero/nil return + handlers registered → enter the serve loop
     - zero/nil return + no handlers → exit 0
   If app.main is NOT registered: go straight to the serve loop
   (today's pure web-app behavior).
8. Serve loop: accept connections → dispatch to handler → respond
   until SIGINT / SIGTERM.
9. Graceful shutdown → drain mmap/WASM/GPU caches → scrub keys →
   close DB → exit.
```

Three useful patterns this covers:

| App registers | Behavior |
|---|---|
| `app.main` only | Run main; exit with its return code. CLI tool. |
| `app.get`/etc only | Serve forever. Web app. Today's default. |
| Both | Run main as a startup hook (run migrations, warm caches, prefetch config). Then serve. |

`app.main` and route registration are **no longer mutually exclusive**
. Register them in any order. `app.main`'s return value short-circuits
the serve loop if non-zero, matching shell exit-code conventions.

On `HL_ENABLE_HTTP_SERVER=0` builds (CLI flavor) the route-registration
bindings drop out entirely; only the `app.main` path is reachable.

### App Layout Conventions

Hull's runtime supports both single-file apps and modular trees. Pick
the layout that matches the app's scope:

**Flat** (default `hull new myapp`). One `app.lua` (or `app.js`) holds
the manifest, requires, and all routes. Best for small services, demos,
single-resource APIs, and one-shot CLI tools. The scaffolder emits:

```
myapp/
  app.lua             . Manifest + routes inline
  migrations/001_init.sql
  tests/test_app.lua
```

**Modular REST** (`hull new --type rest myapp`). `app.lua` is the
bootstrap; resources, models, validation, and middleware live in
sibling directories. Best for apps that will grow past one resource.

```
myapp/
  app.lua             . Manifest + bootstrap (requires each route group)
  routes/             . One file per resource; exports register(app)
    users.lua         . Calls app.get/post/put/delete for /users
    posts.lua
  middleware/         . App-specific middleware (wraps stdlib's hull.middleware.*)
    require_auth.lua
  models/             . DB access functions per resource (no SQL in routes/)
    user.lua          . Exports { create, find_by_id, list, delete_by_id }
    post.lua
  lib/                . Shared helpers (validators, formatters, domain types)
    validate_user.lua
  migrations/, tests/, static/, templates/, locales/ . Same as flat
  tests/routes/test_users.lua   . Mirror the source tree under tests/
```

The shape:

- **`app.lua`** is bootstrap only. It declares the manifest (every module
  any file in the tree imports must be listed, per Hull's import
  tracker), `require`s cross-cutting middleware (logger, session.init,
  etc.), and calls each route group's `register(app)`. No route handlers
  inline.
- **`routes/X.lua`** registers HTTP verbs for one resource. Exports a
  single function: `function M.register(app) ... end`. Returns `M`.
  Routes call into models; they don't touch `db` directly.
- **`models/X.lua`** holds the SQL for one resource. Exports CRUD
  functions: `create`, `find_by_id`, `list`, etc. Each function is a
  pure function of its arguments; no dependency on `req`/`res`.
- **`middleware/X.lua`** wraps stdlib middleware with app-specific
  policy (login redirect target, role checks, etc.). Stays thin.
- **`lib/X.lua`** is for anything that doesn't fit a route/model/
  middleware: schemas, formatters, helpers shared across resources.

**Relative `require` is supported.** `routes/users.lua` does
`require("./../models/user")` and Hull's path normalizer (shared by
both runtimes via `src/hull/path_normalize.c`) collapses `./`/`../`
segments safely. Escape past the app root fails closed.

**Both runtimes coexist.** Generating with `--runtime js` produces the
parallel JS scaffold (same dirs, `.js` extension, `export { register }`
instead of Lua's `M.register = ...`). When `app.lua` and `app.js` both
exist, `hull test` runs each runtime's tests separately (each gets its
own runtime, router, and test discovery); `hull` picks one entry based
on filename.

**JS test bodies may be `async`.** The runner detects a returned Promise
and pumps both QuickJS microtasks (for pure `await`-chain tests) and
the async backend (for tests that do real I/O via `http.fetch` /
`compute.async`) until the promise settles or a per-test timeout
elapses. Default timeout is 5 seconds; override per-test via
`test("slow", { timeout: 30000 }, async () => { ... })`. A rejected
promise (which is how a failing `await test.eq(...)` surfaces in an
async body) marks the test as FAIL with the rejection reason. Sync
test bodies (`() => { ... }`) work too. Non-exception return → PASS,
thrown error → FAIL. Pre-May-2026, the runner only checked
`JS_IsException(ret)` and silently passed every async test regardless
of the awaited assertions; the regression is covered by
`tests/hull/runtime/js/test_js.c::js_test_runner.*`.

**`hull build`** walks subdirectories. `routes/`, `models/`, `lib/`,
etc. are all picked up by `tool.find_files(dir, "*.lua")` and embedded
in the produced binary. No build-config change needed. **`hull deploy`**
treats the modular app as one bundle; same Dockerfile / systemd /
fly.toml output regardless of layout.

**Modular CLI** (`hull new --type cli mytool`). `app.lua` is a
dispatcher: `ctx.args[1]` names a subcommand; `commands/<name>.lua`
exports `M.run(ctx)`. `lib/` holds shared output formatters.

```
mytool/
  app.lua            . Manifest + app.main(ctx) → require("./commands/" .. ctx.args[1]).run(...)
  commands/
    greet.lua        . Exports M.run(ctx); ctx.args is shifted (subcommand argv)
    count.lua
  lib/
    fmt.lua          . Shared helpers
  tests/commands/test_greet.lua
```

`hull test` for `app.main`-based apps is a known limitation today (the
runner expects a registered HTTP server context. Emits "no routes
registered" otherwise). The scaffolded `tests/` files are placeholders
until a CLI-mode test harness lands. The commands themselves are
plain Lua/JS, so they're easy to unit-test in any external harness.

**Modular TUI** (`hull new --type tui mydash`, Lua-only). `app.lua`
holds the `tui.run({ draw, on_event })` loop and a single `state`
table threaded through views. Each view in `views/<name>.lua` exports
`render(ctx, state)` and `handle_event(state, ev) → (new_state | nil,
exit_token | nil)`. Routing is `state.view = "X"`. Because views are
pure functions of state, they unit-test cleanly without a terminal.
see the scaffolded `tests/views/test_menu.lua`.

```
mydash/
  app.lua            . Manifest + tui.run loop; dispatches to views by state.view
  views/
    menu.lua         . Render() + handle_event(); sets state.view to route
    detail.lua
  lib/
    state.lua        . Initial state factory + helpers
  tests/views/test_menu.lua
```

The canonical examples live at `examples/rest_api_modular/`,
`examples/cli_modular/`, and `examples/tui_modular/`.

### Command Dispatch

**Tool mode is Lua-only by design.** The C dispatcher delegates most
non-trivial subcommands (`hull build`, `hull deploy`, `hull init`,
`hull new`, `hull verify`, `hull migrate`, `hull inspect`, `hull
analyze`, `hull sign-platform`, all `--tui` variants, etc.) to a
sandboxed Lua VM via `hull_tool("hull.X", argc, argv, hull_exe)`.
The "tool VM" is the same Lua 5.4 runtime that user apps use, but
loaded with the unveil + spawn-allowlist caps and never with HTTP /
DB / network. **There is no JS counterpart.** When Hull was first
built the tool layer was written in Lua because of its smaller
sandbox surface; adding a parallel JS dispatch path would double
the tool-mode VM count without removing the Lua one (tool plugins
in `stdlib/cli/lua/hull/` would still need maintenance). JS users
can still write applications in JS without restriction. Only the
CLI tool plugins are Lua-only. See `stdlib/cli/lua/hull/` for the
plugin source and `src/hull/tool.c` / `src/hull/tool_orchestration.c`
for the C binding surface the tool VM exposes.

Table-driven dispatcher in `src/hull/commands/dispatch.c`. 25 commands:

```
hull keygen | build | verify | inspect | manifest | test | new | init | dev | eject | sign-platform | migrate | agent | mcp | check | compute | deploy | version | doctor | update | tools | flavor | feature | cache | sign-release | verify-release | help
Runtime flags: --audit (capability audit logging), --agent (sidecar files), --no-migrate, --no-sandbox, --no-ca-bundle, --ca-bundle PATH
Global flags: --version / -v (equivalent to hull version), --help / -h (equivalent to hull help)
```

Each command is a separate `.c`/`.h` under `src/hull/commands/`. Adding a new command = one line in the table + one source file.

**`hull init [dir] [--runtime lua|js]`**. Initialize a hull project in-place. Like `git init`: creates missing files (`app.lua`, `tests/`, `migrations/`, `.gitignore`) without touching existing ones. Detects existing runtime from `app.lua`/`app.js` presence. Implemented as a Lua tool module (`stdlib/cli/lua/hull/init.lua`).

**`hull doctor [--json]`**. Environment check for distribution readiness. Reports hull version/runtime/platform, whether the platform library is embedded (none / single-arch / multi-arch), which system C compilers (`cc`, `gcc`, `clang`, `cosmocc`) are found in PATH (used only for `--compiler=system`, `--with=` features, and cosmo/APE targets - the default `hull build` emit path needs no compiler), and a **Caches** section listing every registered cache kind with status / entries / size / on-disk path (sourced from `hl_cache_registry()` - the same registry that powers `hull cache list`, so doctor and the cache subcommand always agree). Surfaces an active `HULL_CACHE_DIR` override when set. Exits 0 only when `hull build` is fully ready (platform embedded AND at least one compiler available). Pure C implementation (`src/hull/commands/doctor.c`). `--json` includes a `"caches"` array and a `"hull_cache_dir"` field for machine-readable output.

**`hull build --compiler=<backend>`**. Select how `hull build` produces the app binary. **By default `hull build` is compiler-free**: it emits `app_registry.o` directly via the object emitter (`obj_emit`) and links, needing no C compiler at all. `--compiler=system` (or an explicit compiler path) opts into a system `cc`/`gcc`/`clang` instead; `--with=` features and cosmo/APE targets fall back to the system compiler automatically (they can't go through the emit path). Passing `--compiler=<name>` (e.g. `--compiler=tcc`) treats `<name>` as a plain system compiler resolved from `$PATH` - a user's own `tcc` on `$PATH` still works this way, but tcc is no longer a Hull-provided, vendored, or installable tool. The compiler abstraction uses `HlCompilerVtable` (`include/hull/compiler.h`); the system backend lives in `src/hull/compiler.c`.

**`hull update [--check] [--force] [--channel=stable|beta] [--repo=ORG/NAME]`**. Self-update from GitHub releases. Fetches the latest release metadata via `api.github.com`, picks the asset matching this binary's OS/arch (`hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`, or `hull-cosmo` fallback), downloads via HTTPS using the embedded Mozilla CA bundle (Phase D4), verifies the manifest's Ed25519 signature against the embedded `HL_RELEASE_PUBKEY_HEX` (when configured), verifies SHA-256 against `hull.sha256` from the same release (constant-time compare), and atomically replaces the running binary via `rename(2)`. `--check` exits after the version compare without installing. Pure C implementation in `src/hull/commands/update.c`; shared HTTPS / SHA-256 / manifest plumbing lives in `src/hull/release_io.{c,h}`.

**`hull tools install <name> [--all]` / `tools list [--json]` / `tools uninstall <name>`** (Side-load optional Hull-native tools from GitHub releases into `$HOME/.hull/tools/`. Registered tools: `wamrc` (WAMR AOT compiler, single binary), the `libc-musl-<arch>` static-link floors (data-only bundles for `hull build --linker=lld-static`), and the toolchain-free LINKER `zig` (a self-contained multi-file bundle for `hull build --linker=zig` — a static driver + its libc tree + its own bundled lld, so it runs anywhere). A standalone `lld` bundle is **not** shipped: every binary lld (Homebrew/apt/LLVM-release) is dynamically linked against libLLVM (an absolute-path libLLVM.dylib on macOS), so it can't be bundled flat and run portably — a runnable lld bundle needs a static LLVM source build (tracked follow-up); `hull build --linker=lld` still works against a system/PATH lld. The trust chain is identical to `hull update`) same Ed25519-signed `hull.sha256` manifest covers tool assets, no new keys. Install is version-coupled: it pulls from the SAME release as the running hull binary (not "latest"), so e.g. wamrc stays at the WAMR commit hull was compiled against. Tool registry is a compile-time-constant static table in `src/hull/tools_install.c`; adding a tool means one entry in the registry + a matching release asset in `hull.sha256`. **Three asset shapes:** a single-binary tool ships as `hull-<name>-<platform>` (wamrc); a **bundle** (`.is_bundle`) ships as a `.tar` that extracts to a DIRECTORY `$HOME/.hull/tools/<name>/` and is either single-platform with the arch baked into its name (`hull-<name>.tar`, the floors) or per-platform (`.bundle_per_platform` → `hull-<name>-<platform>.tar`, zig). A bundle's `.bundle_entry` is the exec driver inside the dir that `hl_tools_lookup_path` resolves (`zig`) or a sentinel for a data-only bundle (`crt1.o`); `hl_tar_extract` mkdir-p's the dest so a bundle installs into a fresh `$HOME`. Cosmo unsupported for native-toolchain tools (a fat APE can't drive a native zig tree; cosmo users build from source). The download cap (`release_io.c` `max_response_size`) is 512 MB to fit the ~330 MB zig bundle. Pure C; consumers locate installed tools via `hl_tools_lookup_path()` (or `tool.find_tool()` from build-tool Lua) which checks `~/.hull/tools/<name>/<bundle_entry>` (bundles) → `~/.hull/tools/<name>` → `dirname(hull_exe)/` → `$PATH`. Release producer: `scripts/build_zig_bundle.sh` (repacks the official ziglang.org tree). Full design: [docs/tools_install.md](docs/tools_install.md).

The live install path is intentionally not tested in CI (it would need the just-published release to exist before publishing). The post-release validation step is `tests/release_smoke.sh`: install hull, run `sh tests/release_smoke.sh`, watch `hull tools install wamrc` actually fetch the asset, verify SHA-256, exercise `wamrc --help`, and uninstall cleanly. Run it manually after every `gh release create`. (Phase 4.3 removed the pre-built pure-compute flavor lib, so there is no longer a `hull flavor install pure-compute` to smoke-test.)

**`hull flavor install <flavor> [--repo=ORG/NAME]` / `flavor list`**. Since Phase 4.3 the only flavors are `full` (embedded) and `pure-compute` (a preset on the default composable base), and neither is a fetchable per-flavor platform lib: `hull flavor install pure-compute` reports "preset flavor, nothing to install", and `hull flavor list` shows `full` as `embedded` and `pure-compute` as `preset (default base)`. The fetch/verify machinery (the shared `hl_release_io_fetch_verified_manifest`, atomic-write to `$HOME/.hull/platform/`, build-time re-verify) still exists in `src/hull/commands/flavor.c` for any FUTURE non-preset flavor with a non-empty asset stem, but has no user today (`flavor.c` treats an empty-asset flavor as a preset). Pure C, HTTP-client-gated. Full design: [docs/build_flavors.md](docs/build_flavors.md).

**`hull feature install <name> [--repo=ORG/NAME]` / `feature list` / `feature uninstall <name>`**. Fetch the per-feature composable library (`libhull_feature-<name>-<arch>.a`) for `hull build --with=<name>` so end users do not build it from source. Same trust chain as `hull flavor install` (the shared `hl_release_io_fetch_verified_manifest`: verify the Ed25519 signature on `hull.sha256` against the embedded `HL_RELEASE_PUBKEY_HEX`, then per asset look up its SHA-256 in the verified manifest, download, constant-time-compare, atomic-write to `$HOME/.hull/feature/`). Five features today: `duckdb`, `postgres`, `mysql`, `gpu`, `tui`, all native-only (features are static archives; cosmo is never published). `feature list` shows each as `not installed` / `installed` for this platform. Registry is the `FEATURES[]` table in `src/hull/commands/feature.c` (one row per feature). Pure C, HTTP-client-gated. See "Composable features" above and [docs/features_and_flavors.md](docs/features_and_flavors.md).

**`hull cache list|prune|clear|verify`**. Inspect and manage the runtime cache pool (`$HOME/.hull/blobs/runtime/` by default - set `HULL_CACHE_DIR=/abs/path` to redirect for per-app isolation). Full reference: [docs/cache.md](docs/cache.md). Six registered kinds today: `lua-bytecode` (Lua source → bytecode), `js-bytecode` (QuickJS module bytecode), `compute-aot` (WASM AOT artifacts), `templates` (compiled Lua template render functions), `js-templates` (compiled JS template render functions), `tools` (signed side-loaded tool binaries; system store, not pruned by default). `list` enumerates every kind with entry count + total size + on-disk path; `--json` for machine output. `prune [--kind=K] [--max-size=N] [--max-age=N] [--strategy=lru|fifo] [--dry-run]` runs LRU/FIFO eviction over runtime caches; system stores are preserved unless `--kind=tools` is named. `--max-size` accepts `K`/`M`/`G` suffixes (binary, 1024-based; trailing `B` optional, so `100M` and `100MB` are equivalent); `--max-age` accepts `s`/`m`/`h`/`d`/`w`/`y` suffixes (bare numbers = seconds for back-compat). Bad units are rejected with an example. `clear --yes` wipes runtime caches entirely (iter + delete, not policy-based - so even sub-second-old files go). `verify [--kind=K] [--repair] [--json]` walks every entry and flags corruption: for CAS-mode kinds (`tools`) it recomputes `sha256(contents)` and compares to filename; for keyed-mode runtime caches it does a structural check (regular file, non-empty, readable). `--repair` unlinks corrupt entries - safe because the next compile/install repopulates from source. Exits non-zero on corruption unless `--repair` was able to fix it. Cache registry (`include/hull/cache_registry.h`) is the single source of truth for cache kinds, also consumed by `hull doctor` (Caches section), `hull inspect` (runtime-caches disclosure), and `hull cache verify` (CAS vs keyed mode dispatch via `is_cas` field). Adding a new cache kind = one entry in `REGISTRY[]` and the new consumer auto-shows up in list / prune / clear / verify / doctor / inspect.

**Cache eviction is manual.** No automatic TTL / background sweep / on-write cap. Stale entries are harmless (content-keyed → orphans never serve incorrect data), and the caches are tiny per entry, so correctness doesn't depend on freshness. The only automatic hygiene is `hl_blob_store_open`'s `tmp_max_age_sec` sweep (default 1 hour) which removes abandoned `tmp/.blob-*.tmp` files from crashed writers. `hull doctor` surfaces a `⚠ large` mark next to any cache kind that passes 250 MB or runtime total past 1 GB and prints an actionable `hull cache prune --max-age=30d --strategy=lru` hint - but does not act on it. Disk-pressure users who want fully automatic eviction wire `hull cache prune` into cron / a systemd timer.

**`HULL_CACHE_DIR` (per-app cache isolation).** `HULL_CACHE_DIR=/absolute/path` redirects the entire runtime cache pool from `$HOME/.hull/blobs/runtime/` to the given directory. Use on multi-tenant boxes or under systemd / k8s / Docker so each deployment has its own cache and never reads / writes another deployment's blobs. Must be an absolute path; the sandbox auto-allows the resolved path. All `HULL_NO_*_CACHE` opt-outs (below) still apply on top. The tools store (`$HOME/.hull/blobs/tools/`) is intentionally NOT redirected - those are signed durable downloads with a stable system home, not per-app caches. Layer C (automatic per-app isolation derived from app identity) is a planned follow-up; the override here is the manual / deployment-controlled equivalent. See [docs/blob.md §"Per-app cache isolation"](docs/blob.md).

### Cache environment variables (full table)

Every runtime cache exposes a granular opt-out plus the global kill-switch. All variables are checked on every cache call (not memoized at process start), so flipping them mid-process takes effect on the next access. `hull cache list` shows the resolved status in a Status column (`ok` / `off (env)` / `off (all)` / `n/a` for system stores) and surfaces an active variable in a footer line.

| Variable | Effect when truthy | Default |
|---|---|---|
| `HULL_NO_CACHE` | Disables every runtime cache (lua-bytecode, js-bytecode, compute-aot, templates). Tools store still works (not a cache). | unset = caches active |
| `HULL_NO_LUA_BYTECODE_CACHE` | Disables only the Lua bytecode cache (`hl_lua_load_cached` falls through to `luaL_loadbuffer`). | unset |
| `HULL_NO_JS_BYTECODE_CACHE` | Disables only the QuickJS bytecode cache (`hl_js_compile_module_cached` falls through to `JS_Eval`). | unset |
| `HULL_NO_AOT_CACHE` | Disables the compute AOT cache (`hull build` runs `wamrc` every time). | unset |
| `HULL_NO_TEMPLATE_CACHE` | Disables the template render-function cache (`hl_lua_template_compile_cached` falls through to parse + pcall). | unset |
| `HULL_CACHE_DIR` | **Path override**, not an opt-out. Redirects the entire runtime cache pool to an absolute path for per-app isolation. Tools store stays at `$HOME/.hull/blobs/tools/`. | unset = `$HOME/.hull/blobs/runtime/` |

Truthiness rule (in `hl_hull_cache_disabled`, `src/hull/cache_dir.c`): unset / empty / `0` / `false` / `FALSE` / leading `f`/`F` → off. Anything else (including `1`, `true`, `yes`) → on.

The naming pattern is `HULL_NO_<KIND>_CACHE` where `<KIND>` is the registry's `env_kind` field uppercased (kebab-case display names map to snake_case env kinds so the SCREAMING_SNAKE result reads naturally). Adding a new cache kind to `REGISTRY[]` with an `env_kind` value automatically gets the matching env var and the Status column in `hull cache list` without any per-surface code.

**`hull help` / `hull --help` / `hull -h`**. Print top-level usage grouping every registered subcommand by purpose (Scaffolding, Build & ship, Develop & test, Diagnostics, Compute / WASM, Database, Deployment, Self-management). Implementation in `src/hull/commands/help.c`. Build-time-gated commands (HL_ENABLE_HTTP_SERVER, HL_ENABLE_DB, HL_ENABLE_HTTP_CLIENT) are suppressed when the corresponding flag is off so help only advertises what this binary can do. The output ends with a "for AI agents" pointer at `hull agent context --task=orientation --level=minimal`, matched by similar breadcrumbs in `hull`'s bare-mode usage and the `install.sh` postscript.

**`hull agent tools`** (Generic JSON dump of the tool registry crossed with the install state on this host (`{platform, tools: [{name, available_for_platform, installed, path, asset_name, install_hint}, ...]}`). Independent of `HL_ENABLE_HTTP_CLIENT`) even CLI-flavor builds without the installer can list what's registered. The compute-specific wamrc state is also surfaced inside `hull agent compute` under a `wamrc` block so agents reading per-subsystem panels see the actionable hint without making a second call.

**`hull agent context --list [--json]`**. Enumerate every context: task in the embedded platform stdlib registry plus which level markers each one populates. Cold-start agents call this first to discover what `--task=NAME` values are valid. The bare `hull agent context` form (no `--task`, no `--list`) errors with a usage message that points at `--list`. Topic docs live in `stdlib/context/*.md` and are auto-discovered by the Makefile + xxd embedding pipeline.

**`hull agent overview [app_dir]`**. Single-shot composite project summary an agent can read when dropped into an unfamiliar app dir. Composes runtime detection, route stats (count + methods + ws/sse flags), compute modules + AOT readiness + wamrc state, GPU shaders, migrations, declared modules, tests, and a `build_ready` flag. No DB connection; agents needing pending-migration counts call `hull agent migrate` separately.

**`hull sign-release <manifest> --key <secret_key>`**. Sign a release manifest (typically `hull.sha256`) with an Ed25519 secret key. Writes `<manifest>.sig` (128 hex chars). Used by the GitHub Actions release workflow; never invoked by end users. See [docs/release_signing.md](docs/release_signing.md).

**`hull verify-release <manifest> <signature> [--pubkey <hex>]`**. Verify an Ed25519 signature over a release manifest using the embedded release public key (or an explicit override). Exit 0 = valid, 1 = invalid / placeholder. For offline auditing of a downloaded release.

**`hull compute new <name> [--lang c]`**. Scaffold a new WASM compute module under `compute/<name>/`. Writes `<name>.c` (with a 30-line `hull_process` skeleton), `hull_compute.h` (the freestanding ABI header with libc shim + bump allocator + UDF wire format), and `test_fixtures.json`. Errors if the directory already exists. Names must match `[A-Za-z0-9_-]+`. C is the only supported language today.

**`hull compute build [name]`**. Compile `compute/<name>/<name>.c` → `compute/<name>.wasm` via `clang --target=wasm32-unknown-unknown -nostdlib -O2 -flto`. With no name, builds every discovered module. Toolchain lookup prefers Homebrew `llvm@18` then falls back to system `clang` with `wasm-ld` in PATH. The same logic runs automatically as part of `hull build` for stale sources. `hull compute build` is the manual rebuild entry point. Implementation: `stdlib/cli/lua/hull/compute.lua` + the shared helper at `stdlib/cli/lua/hull/compute_build.lua`.

**`hull compute test <name>`**. Run JSON fixtures from `compute/<name>/test_fixtures.json` against `compute/<name>.wasm`. Each fixture has `{name, input, expect_status}`; the runner generates a tempdir app with a `GET /call?input=...` route that calls `compute.call("<name>", input)`, then exercises it via `hull test`. Fixtures use the exact same `compute.call` codepath that production handlers use.

**`hull compute check <name>`**. Validate that `compute/<name>.wasm` has the correct WASM magic, has version 1, and actually loads in WAMR with a trivial input. The "yes, this module can run inside Hull" gate.

**`hull compute refresh-header [name]`**. Overwrite the per-module `compute/<name>/hull_compute.h` with the canonical version embedded in the `hull` binary. With no name, refreshes every discovered module. Run after upgrading Hull when a new ABI helper has been added.

### `hull build` and compute modules

`hull build` makes compute modules first-class artifacts:

- **Step 1. Auto-rebuild from source.** Before the existing discovery pass, `build.lua` calls `cbuild.build_all(app_dir, "stale")`. Any `compute/<name>/<name>.c` whose mtime is newer than the matching `.wasm` is recompiled inline using the same clang invocation as `hull compute build`. Reports as `hull build: compiled N compute source(s): <names>`. If clang is missing AND something is stale, errors with a fix-it hint. Default ON; `--no-build-compute` opts out (for hermetic CI builds shipping pre-committed `.wasm` artifacts).
- **Step 2. AOT compilation.** When `wamrc` is available, every `compute/*.wasm` is AOT-compiled to `compute/*.aot.<arch>`. Cosmocc builds produce both `x86_64` and `aarch64` AOT files. `--no-aot` skips this.
- **Step 3. Embed both.** `.wasm` (interpreter fallback) and `.aot.<arch>` (preferred at runtime) are embedded into the unified app VFS alongside templates, static files, and migrations.

`hull agent deploy` reports a `compute_modules` array with per-entry `{name, wasm_size, has_aot, has_source, source_stale}` plus a recommendation when any source is newer than its `.wasm`. Cosmocc/Linux/macOS coverage is in `tests/e2e_compute_dev.sh` (the dev workflow) and `tests/e2e_compute.sh` (runtime semantics).

### Agent Tooling (`hull agent`)

Machine-readable introspection for AI coding agents. All output is JSON to stdout.

```bash
hull agent routes [app_dir]              # list routes + middleware
hull agent db schema [app_dir] [-d path] # introspect DB schema
hull agent db query "SQL" [app_dir]      # run read-only SQL query
hull agent request METHOD PATH [opts]    # HTTP request to dev server
hull agent status [app_dir] [-p port]    # check dev server status
hull agent errors [app_dir]              # structured errors from last reload
hull agent test [app_dir]                # run tests with JSON output
hull agent context --task=T --level=L    # task-relevant documentation
hull agent migrate [app_dir] [-d path]   # migration status
hull agent deploy [app_dir]              # deployment readiness analysis
```

`hull dev --agent` enables sidecar files: `.hull/dev.json` (port, PID) on start, `.hull/last_error.json` on load failure. See [AGENTS.md](AGENTS.md) for the full agent development guide.

### Migration System

SQL migrations provide versioned schema management for SQLite databases.

| Component | File | Purpose |
|-----------|------|---------|
| Migration runner | `src/hull/migrate.c`, `include/hull/migrate.h` | Core migration execution engine (uses VFS prefix query) |
| CLI command | `src/hull/commands/migrate.c` | `hull migrate` subcommand |
| Scaffolding | `stdlib/cli/lua/hull/migrate.lua` | `hull migrate new` template generation |
| Auto-run (dev) | `main.c` | Runs pending migrations on startup |
| Auto-run (test) | `test.c` | Runs migrations against `:memory:` database |
| Embedding | `build.lua` | Embeds `migrations/*.sql` in built binaries |

**Convention:** `migrations/*.sql` files numbered `001_`, `002_`, etc. Each runs in `BEGIN IMMEDIATE` / `COMMIT`. The `_hull_migrations` table tracks applied migrations (name + checksum + timestamp). Opt out with `--no-migrate`.

**Commands:**
- `hull migrate [app_dir]`. Run pending migrations
- `hull migrate status`. Show applied/pending
- `hull migrate new <name>`. Create numbered migration file

### Virtual Filesystem (VFS)

All embedded file lookups go through a unified VFS module (`src/hull/vfs.c`, `include/hull/vfs.h`). Two VFS instances are created at startup:

| Instance | Entries | root_dir | Used by |
|----------|---------|----------|---------|
| `app_vfs` | `hl_app_entries[]` | `app_dir` | templates, static, migrations, app modules, signature |
| `platform_vfs` | `hl_stdlib_entries[]` | NULL | Lua/JS stdlib module loading |

Both are stored in `HlRuntime` and accessible to all consumers.

**API:**
- `hl_vfs_find(vfs, name)`. O(log n) exact lookup (binary search)
- `hl_vfs_prefix(vfs, prefix, &first)`. O(log n) prefix query (returns count + pointer to first match)
- `hl_vfs_has_prefix(vfs, prefix)`. O(log n) prefix existence check
- `hl_vfs_path(vfs, name, buf, size)`. Filesystem path construction (`root_dir/name`)

**Build-time requirement:** Entry arrays must be sorted by name in C `strcmp` order (the Makefile uses `LC_ALL=C sort`). `hl_vfs_init()` debug-asserts sorted order.

**Consumers:**
- Static serving: `hl_vfs_find(vfs, "static/style.css")`
- Migrations: `hl_vfs_prefix(vfs, "migrations/", &first)`
- Templates: `hl_vfs_find(vfs, "templates/base.html")`
- Module loading: `hl_vfs_find(vfs, "hull:web:cookie")` (JS), `hl_vfs_find(vfs, "hull.json")` (Lua)
- Signature: `hl_vfs_find(vfs, "./app.lua")` for hash verification

## Platform Builds

### Standard Build (Linux/macOS)

```bash
make                    # builds build/hull
make platform           # builds build/libhull_platform.a
make EMBED_PLATFORM=1   # embeds platform in hull for distribution
```

### Cosmopolitan APE Build

Cosmopolitan produces fat APE binaries that run on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single file.

**How cosmocc works:**
- `cosmocc` runs two separate link passes (x86_64 + aarch64), then combines with `apelink`
- Uses `.aarch64/` directory convention: for every `foo.o`, a `.aarch64/foo.o` exists
- Arch-specific tools: `x86_64-unknown-cosmo-cc`, `aarch64-unknown-cosmo-cc`

**Multi-arch platform build:**

```bash
# Build both x86_64 and aarch64 platform archives
make platform-cosmo

# This creates:
#   build/libhull_platform.x86_64-cosmo.a
#   build/libhull_platform.aarch64-cosmo.a
#   build/platform_cc  (contains "cosmocc")

# Then build hull with cosmocc
make CC=cosmocc
```

`platform-cosmo` internally:
1. `make clean && make platform CC=x86_64-unknown-cosmo-cc` → copies to staging
2. `make clean && make platform CC=aarch64-unknown-cosmo-cc` → copies to staging
3. Cleans build artifacts, copies both archives to `build/`

**Keel Cosmo detection:**
- Keel's Makefile detects the cosmo toolchain via `ifneq ($(findstring cosmo,$(CC)),)`
- Sets `COSMO=1`: forces poll backend, omits `-fstack-protector-strong`
- Sets `COSMO_FAT=1` only when `CC=cosmocc`: creates `.aarch64/libkeel.a` counterpart
- Uses plain `ar` (not `cosmoar`. Cosmoar fails with recursive `.aarch64/` lookups)

**hull build with cosmo:**
- `build.lua` detects `is_cosmo = cc:find("cosmocc")`
- Searches for both arch-specific archives in `build/` or hull binary directory
- Copies `x86_64-cosmo.a` → `tmpdir/libhull_platform.a`
- Copies `aarch64-cosmo.a` → `tmpdir/.aarch64/libhull_platform.a`
- `cosmocc` automatically finds the `.aarch64/` counterpart during linking

**Embedding for distribution:**
```bash
make platform-cosmo
make CC=cosmocc EMBED_PLATFORM=cosmo  # embeds both arch archives
```

### CI Configuration

The Cosmo CI job in `.github/workflows/ci.yml`:
1. Installs cosmocc from `cosmo.zip/pub/cosmocc/cosmocc.zip`
2. `make platform-cosmo`. Builds both arch platform archives
3. `make CC=cosmocc`. Builds hull as APE binary
4. `make test CC=cosmocc`. Runs unit tests
5. E2E smoke test + sandbox tests

## Security

### Manifest & Sandbox

Two-phase sandbox in `sandbox.c`:

**Phase 1** (`hl_sandbox_apply_pledge()`: Called before `load_app()`. On Linux/Cosmo, pledges `stdio inet rpath wpath cpath flock dns unveil`) blocks `exec`, `proc`, `fork` during module loading. On macOS, phase 1 is a no-op (Seatbelt's `sandbox_init` is irreversible, so the full profile is applied in phase 2).

**Phase 2**. `hl_sandbox_apply()`: Called after manifest extraction. Platform-specific enforcement:
- **Linux/Cosmo:** Unveils specific paths, seals filesystem, applies pledge syscall filter
- **macOS:** Builds dynamic SBPL profile from manifest, applies via `sandbox_init_with_parameters()`. Deny-default with selective allows for app_dir, db files, manifest paths, network.

Violation = SIGABRT on OpenBSD, SIGKILL on Linux/Cosmo, EPERM on macOS. `--no-sandbox` flag disables kernel enforcement for debugging.

### Capability Enforcement Invariants

- **SQL injection impossible:** All DB access uses `sqlite3_bind_*` parameterized binding. SQL is always a literal string.
- **Internal tables protected:** `hl_cap_db_check_namespace()` blocks user code from accessing `_hull_*` tables. Enforcement uses call-stack inspection (Lua checks `ar.source` for `hull.` prefix, JS checks module name for `hull:` prefix) so stdlib modules transparently bypass the check via normal `db.exec`/`db.query`. No internal API is exposed. Tables: `_hull_outbox`, `_hull_inbox_processed`, `_hull_idempotency_keys`, `_hull_sessions`.
- **Path traversal blocked:** `hl_cap_fs_validate()` rejects absolute paths, `..` components, symlink escapes via `realpath()` ancestor check. Plus kernel unveil.
- **Host allowlist enforced:** `hl_cap_http_request()` validates target host against manifest's `hosts` array. Since §2.8 the check delegates to the shared matcher `hl_host_match_any_env` (`src/hull/utils/host_match.c`), so `hosts` entries may be an exact hostname (case-insensitive), `"*"` (any), a `"*.suffix"` subdomain glob, a CIDR (matches only IP-literal hosts, never a DNS name), or a `"$VAR"` / `"${VAR}"` env reference resolved at match time. The same matcher gates `ws.connect` (shares the http config) and `smtp.send` (`hl_smtp_check_host`), and `databases.dynamic.hosts` - one convention across every outbound host allowlist.
- **Env allowlist enforced:** `hl_cap_env_get()` checks against manifest's `env` array (max 32 entries).
- **No shell invocation:** Tool mode uses `hl_tool_spawn()` with compiler allowlist. No `system()`/`popen()`.
- **Key material zeroed:** `hull_secure_zero()` (volatile memset) scrubs crypto material from stack buffers.
- **Instruction limits:** Both Lua and JS runtimes enforce per-request instruction limits (default 100M). Lua uses `lua_sethook(LUA_MASKCOUNT)`, JS uses `JS_SetInterruptHandler`. Override with `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` env var.
- **Audit logging:** `--audit` flag or `HULL_AUDIT=1` env var enables structured JSON logging of all capability calls to stderr. Off by default (zero overhead. Single branch on `hl_audit_enabled` global). Uses `ShJsonWriter` for streaming output with proper escaping. No heap allocation.

### Module Declaration System

Apps declare which first-party Hull stdlib modules they import via `manifest.modules`. Three principles:

> **v0.2.0 namespace note.** Strictly-web modules live under `hull/web/*`. The 20 affected modules are `hull/web/{cookie,form,htmx,sse,ws-client,ws-server}` plus the 14 `hull/web/middleware/*` modules. Cross-cutting modules stay flat: `hull/jwt`, `hull/http-server`, `hull/http-client`, `hull/template`, `hull/email`, `hull/smtp`, plus all the runtime-agnostic utilities. Apps using pre-v0.2.0 names get an explicit fix-it message from the resolver pointing at the new path.


1. **Every external capability is declared.** Language intrinsics (Lua: `string/table/math/utf8/coroutine`; JS: `Object/Array/Math/JSON`) and Hull's intrinsic core are always available; everything else must appear in `manifest.modules`. The intrinsic core is the minimum needed to bootstrap an app: **`hull/app`** alone, providing `app.manifest`, `app.get/post/use`, `app.router`, `app.ws/sse`, and `app.main`. `app` stays intrinsic because the manifest itself is expressed via `app.manifest(...)` (it must exist before the manifest is parsed. **Module-conditional decoration:** some declared modules don't just enable imports, they add methods to the `app` intrinsic. Today: `"hull/timers@1"` decorates `app` with `app.every(ms, fn)` and `app.daily(hhmm, fn)`. Without the declaration those methods don't exist on `app` at all (calling them raises "attempt to call a nil value" / "is not a function")) the C# partial-class metaphor. `hull/log` and `hull/json` are also declared modules; apps that call `log.X` or `json.X` directly must put `"hull/log@1"` / `"hull/json@1"` in `manifest.modules`. Response helpers (`res:json(...)`) and internal JSON marshalling work without either declaration. They bypass user-visible imports at the C layer.
2. **Import-only exposure.** Declared modules are reached via `require("hull.X")` (Lua) / `import "hull:X"` (JS). They are NOT exposed as globals. The two-level `hull.web.X` / `hull:web:X` form (also `hull.web.middleware.X` / `hull:web:middleware:X`) works identically - segment depth is unrestricted, the resolver just translates separator-to-`/` for canonical lookup.
3. **Capability + module are independent gates, and deps auto-resolve.** Declaring `hull/http-client@1` makes `require("hull.http-client")` resolve; it does not open the network. The per-call cap layer (`hl_cap_http_request`, `hl_cap_fs_validate`, `hl_cap_env_get`) fails closed against an empty allowlist, so an unused module is harmless. The resolver only hard-blocks build-time gates (`HL_ENABLE_*`); manifest `fs/env/hosts` sections are validated at call time. **Transitive deps are auto-admitted**. Declaring `hull/web/middleware/session@1` implicitly admits `hull/db`, `hull/crypto`, `hull/json`, `hull/time`, `hull/http-server` (and triggers the matching `app` decorations). The dep graph lives in the registry; `hull modules list` shows the resolved set. Apps don't need to re-declare every transitive utility. **Top-of-file imports/requires are tracked** during the pre-manifest window (before `app.manifest()` runs the resolver) and validated against the resolved set immediately after; an undeclared `import { db } from "hull:db"` at the top of an app.js fails app load synchronously with a clear message instead of silently slipping through (the gate isn't wired yet at import time). See `hl_import_tracker_record` / `_validate` in `module_resolver.c`.

```lua
app.manifest({
    modules = {
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/web/middleware/auth@1",
        "hull/web/middleware/session@1",     -- needs db, crypto, time (declare each)
    },
    hosts = {"api.stripe.com"},          -- required if http is declared
})

-- require/import are standard Lua/JS. Choose any local binding name:
local crypto = require("hull.crypto")
local fetcher = require("hull.http-client")
```

**Manifest syntax**: each entry is a canonical spec `"<vendor>/<name>@<major>"` in an array. First-party modules use `hull/`; future third-party would use `"acme/widgets@2"`. The manifest declares *what's in scope*; the require/import call site picks *what to call it locally*. The legacy keyed form (`crypto = "hull/crypto@1"`) is still parsed for back-compat but the array form is canonical.

**Optional modules (`?` suffix) - graceful fallback**: a trailing `?` on a spec (`"hull/gpu@1?"`, `"hull/duckdb@1?"`) marks the module **optional**. When the build lacks the required capability (a compiled-out `HL_ENABLE_*` subsystem AND no matching composed `--with=` feature), the resolver **skips** it (records it in an `optional_absent` bitset) instead of failing app load, and the require/import returns a soft absent value: `require("hull.gpu")` returns `nil` (Lua) / `import { gpu } from "hull:gpu"` binds `null` via a synthesized stub module (JS). So an app can use a capability when present and fall back when not:

```lua
local gpu = require("hull.gpu")          -- nil on a base binary
if gpu and gpu.available() then ... else cpu_path() end
```
```javascript
import { gpu } from "hull:gpu";           // null on a base binary
if (gpu && gpu.available()) { ... } else { cpuPath(); }
```

Only the **absent** case changes: a present optional module is gated exactly as a normal declaration (full resolver + per-call cap layer - zero new authority), and a **non-optional** spec for an absent capability stays a hard app-load error. Generalizes to every build-cap module (`db`/`wasm`/`gpu`/`http`/`tui`). Impl: `hl_module_set_optional_absent_*` + `hl_module_needs_absent_build_cap` in `module_resolver.c`; Lua nil path in `runtime/lua/mod_fs.c`; JS null-export stub in `runtime/js/runtime.c`.

**Architecture (`include/hull/module_registry.h`, `include/hull/module_resolver.h`):**

| Component | File | Purpose |
|-----------|------|---------|
| Canonical registry | `src/hull/module_registry.c` | Sorted `HlModuleSpec` table. Name, api_major, intrinsic, deps, required_caps. O(log n) lookup. |
| Resolver | `src/hull/module_resolver.c` | Validates `manifest.modules` against registry; auto-seeds intrinsics; produces `HlResolvedModuleSet` bitset stored on `HlRuntime`. |
| Lua gate | `src/hull/runtime/lua/mod_fs.c` (`hl_lua_require`) | Per-require check against the set. |
| JS gate (native) | `src/hull/runtime/js/runtime.c` (`hl_js_check_module_declared`) | Called inside each native module's QuickJS init callback. |
| JS gate (stdlib `.js`) | `src/hull/runtime/js/runtime.c` (`hl_js_module_loader`) | VFS-resolved `.js` modules checked before load. |
| Build-time persistence | `stdlib/cli/lua/hull/build.lua` | Resolver output written to `package.sig` as `modules_resolved`. Covered by the signature. |
| Tool exposure | `src/hull/runtime/lua/mod_tool.c` (`tool.modules_resolve`) | Lua binding so `hull build` and similar tools can run the resolver. |

**Failure-mode summary:**

| Error | Cause | Fix |
|-------|-------|-----|
| `module 'hull.X' is not declared in app.manifest. Add to modules: X = "1"...` | App requires a known module not in the modules table | Add to `modules` (the error includes the exact line, plus deps if any) |
| `module 'hull/jwt@1' transitively requires 'hull/gpu', which needs HL_ENABLE_GPU but it is disabled in this hull build` | A declared module's auto-admitted dep needs a compile-time subsystem this binary doesn't have | Rebuild with the required `HL_ENABLE_*` flag, or remove the top-level module declaration |
| `http.fetch: host 'api.example.com' not in manifest hosts allowlist` | Module is declared and loaded fine, but the per-call cap layer rejects the URL | Add the host to `manifest.hosts` (or use `fs.read = {...}` / `env = {...}` for the corresponding modules. Capability sections are checked at call time, not at module load) |
| `module 'hull/gpu@1' requires HL_ENABLE_GPU (build-time)` | The build wasn't compiled with the subsystem and no `gpu` feature was composed | Compose the feature: `hull build --with=gpu` (after `hull feature install gpu`), or rebuild hull with `make HL_ENABLE_GPU=1 …`, or mark the module optional with a `?` suffix (`"hull/gpu@1?"`) to fall back gracefully, or remove the declaration |
| `module 'hull/http-client@1' requires HL_ENABLE_HTTP_CLIENT (build-time)` | App declares an outbound HTTP module (`hull/http`, `hull/smtp`, `hull/email`) on a build with `HL_ENABLE_HTTP_CLIENT=0` | Rebuild with `HL_ENABLE_HTTP_CLIENT=1` (the default) or remove the module declaration. |
| `module 'hull/http-server@1' requires HL_ENABLE_HTTP_SERVER (build-time)` | App declares an inbound HTTP module (`hull/server`, `hull/ws`, `hull/web/sse`, any `hull/middleware/*`) on a build with `HL_ENABLE_HTTP_SERVER=0` | Rebuild with `HL_ENABLE_HTTP_SERVER=1` (the default) or remove the module declaration. See [docs/cli_mode.md](docs/cli_mode.md). |
| `unknown module 'X' in app.manifest.modules` | Typo or non-existent module | Run `hull modules available` for the canonical list |

**CLI surface:**

| Command | Output |
|---------|--------|
| `hull modules available [--json]` | Full first-party registry. Names, deps, capability requirements |
| `hull modules list [APP_DIR]` | What the app declares |
| `hull modules explain <NAME>` | One spec |
| `hull agent modules [APP_DIR]` | `{declared, intrinsic, build_caps, registry_count}` JSON |
| `hull doctor` | Reports which `HL_ENABLE_*` subsystems the build supports |
| `hull check` | Validates the app's manifest before tests fire |

See [docs/security.md §5b](docs/security.md) for the full design and design principles.

### Signature System

Three independent Ed25519 layers:
- **Platform layer (inner, in `package.sig`):** Signed by gethull.dev key. Proves platform library is authentic.
- **App layer (outer, in `package.sig`):** Signed by developer key. Proves app hasn't been tampered with.
- **Release layer (`hull.sha256.sig`):** Signed by Hull release key. Proves the `hull` binary you just downloaded via `hull update` matches the SHA-256 manifest signed by the release authority. Embedded pubkey: `HL_RELEASE_PUBKEY_HEX` in `include/hull/release.h`.

**Composed-feature attestation (`package.sig.gethull.composed`).** The native base is composed (every optional subsystem whole-archived at `hull build`), so the platform layer above, which only covers `libhull_platform.a`, is no longer the whole trusted surface. `hull build` records **every** archive it composes into `package.sig.gethull.composed`, in two trust domains: `platform_domain` (the archives EMBEDDED in hull: runtime `lua`/`js`, HTTP core + per-runtime web bindings, WASM core + per-runtime compute bridge, image core + per-runtime bridge, SQLite engine + per-runtime udf bridge, the TLS feature (`tls`), the Keel event loop (`keel`), and the tui bridge) attested by the **platform** key, and `release_domain` (`--with` backend features: duckdb/postgres/mysql/gpu) attested by the **release** key. Each entry is `{name, sha256}` keyed by the composed asset name `libhull_feature-<stem>.<arch>.a`. At runtime, `--verify-sig` (`src/hull/signature.c` §5c, right after the base §5b) verifies the `platform_domain` block against the embedded `HL_PLATFORM_PUBKEY_HEX` via `hl_platform_sig_verify_composed` and **refuses to boot** on any tamper - presence-gated (absent on pre-#114 apps and on cosmo, where §5b alone anchors trust). `release_domain` is recorded + app-sig-sealed for provenance; its trust is already anchored at `hull feature install` (release-key fetch) and at `hull build --with` (compose re-verify), so it is not runtime-re-anchored. The whole block sits inside the developer-signed payload, so it cannot be stripped without breaking the app signature. Release wiring: `release.yml` signs every embedded feature-archive hash (the `platform_domain` stems, incl. `tls` and `keel`) into the platform manifest and stage 3 embeds those exact bytes (`TRUST_FEATURE_LIBS=1`, mirroring `TRUST_PLATFORM_LIB`). Big composed archives (~127 MB DuckDB, wgpu) are hashed with the streaming C binding `tool.sha256_file` so the tool VM's 64 MB Lua allocator never sees them. Full design: [docs/composed_feature_signing.md](docs/composed_feature_signing.md); covered by `tests/e2e_composed_sig.sh` (a throwaway test-key chain, because a test cannot use the production platform key). **§5c is LIVE:** `HL_PLATFORM_PUBKEY_HEX` is a real key (restored at v0.1.3; `hl_platform_pubkey_is_placeholder()` matches ONLY all-zeros), so §5c enforces on every released binary - validated end-to-end by the v0.9.0 keel dry-run (the Platform-sig E2E smoke test runs `--verify-sig`, which verified the composed archives against the real-key-signed embedded manifest).

See [docs/security.md](docs/security.md) for the full attack model and [docs/release_signing.md](docs/release_signing.md) for the release-signing design.

### Keel Audit

Run `/c-audit` to perform a comprehensive C code audit on the Keel HTTP server library. The audit checks for memory safety, input validation, resource management, integer overflow, network security, dead code, and build hardening. Keel lives in a separate repository ([github.com/artalis-io/keel](https://github.com/artalis-io/keel)); its own audit history is maintained there.

Key findings to be aware of:
- WebSocket and HTTP/2 upgrade code has partial-write issues (C-2, H-3, H-4)
- kqueue event_mod doesn't support READ|WRITE bitmask (C-1). Affects HTTP/2 on macOS
- Private key material should be zeroed before free in tls_mbedtls.c (H-2)

## Key Types

| Type | Header | Purpose |
|------|--------|---------|
| `HlValue` | `cap/types.h` | Runtime-agnostic value (nil, int, double, text, blob, bool) |
| `HlColumn` | `cap/types.h` | Named column + value (query results) |
| `HlRowCallback` | `cap/types.h` | Per-row callback for db_query() |
| `HlManifest` | `manifest.h` | Declared capabilities (fs paths, env vars, hosts) |
| `HlRuntime` | `runtime.h` | Polymorphic runtime context |
| `HlRuntimeVtable` | `runtime.h` | Runtime interface (init, load, wire_routes, extract_manifest, destroy) |
| `HlLua` | `runtime/lua.h` | Lua 5.4 context (VM, config, capabilities) |
| `HlJS` | `runtime/js.h` | QuickJS context (VM, config, capabilities) |
| `HlVfs` | `vfs.h` | Unified VFS: sorted HlEntry array with O(log n) find, prefix query, path construction |
| `HlGpuCtx` | `cap/gpu.h` | GPU compute context: backend vtable, device array, pipeline/buffer caches |
| `HlEmbeddedPlatform` | `build_assets.h` | Multi-arch embedded platform entry (arch, data, len) |

## Git

- When committing, do NOT add any Co-Authored-By trailers.
- Do NOT add "Generated with Claude Code" or similar attribution to PRs.

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`
- Unused functions and variables are errors (`-Werror=unused-function -Werror=unused-variable`). Dead code must be deleted, not left to accrue. Unused parameters stay a warning (vendored static-inline headers like QuickJS leak the diagnostic into Hull TUs); silence them in Hull code with `(void)x;`.
- `-fstack-protector-strong` for buffer overflow detection (not Cosmo)
- Vendor code compiled with `-w` (relaxed warnings, no `-Werror`)
- Integer overflow guards: check against `SIZE_MAX/2` before arithmetic
- Error handling: return `-1` on failure, `0` on success (or positive value)
- Resource cleanup: every `_init` has a corresponding `_free`
- All SQLite access through `hl_cap_db_*`. Never call sqlite3 directly from bindings
- All filesystem access through `hl_cap_fs_*`. Never call open/read/write directly from runtimes
- Public Hull functions prefixed with `hl_` (capabilities: `hl_cap_*`, tools: `hl_tool_*`, commands: `hl_cmd_*`)
- Keel functions prefixed with `kl_` (see vendor/keel/CLAUDE.md)

## Stdlib Middleware

### Middleware Factory Pattern

All middleware modules follow the same contract:

```lua
local mod = require("hull.<module>")
local mw = mod.middleware(opts)   -- factory returns a middleware function
-- mw signature: function(req, res) -> 0 | 1
--   0 = continue to next middleware / handler
--   1 = short-circuit (response already sent)
```

Register with `app.use(method, pattern, mw)`:
- `"*"` method = match any method
- `"/*"` pattern = prefix match all paths
- `"/api/*"` = prefix match under `/api/`

### Module Reference

| Module | Lua | JS | Purpose |
|--------|-----|-----|---------|
| `cors` | `hull.web.middleware.cors` | `hull:web:middleware:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.web.middleware.ratelimit` | `hull:web:middleware:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.web.middleware.csrf` | `hull:web:middleware:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.web.middleware.auth` | `hull:web:middleware:auth` | Session-based and JWT-based authentication middleware |
| `oauth` | `hull.web.middleware.oauth` | `hull:web:middleware:oauth` | OIDC / OAuth 2.0 Authorization Code + PKCE (Google, Microsoft, generic IdPs) |
| `totp` | `hull.web.middleware.totp` | `hull:web:middleware:totp` | RFC 6238 TOTP 2FA with QR enrollment, recovery codes, replay-protection, optional at-rest encryption |
| `auth-flows` | `hull.web.auth-flows` | `hull:web:auth-flows` | Registration, email-verify, login, password-reset, magic-link, email-change. HMAC-signed single-use tokens, PBKDF2, app-provided storage + templates |
| `envelope` | `hull.crypto.envelope` | `hull:crypto:envelope` | HMAC-signed JSON-payload stateless tokens (`base64url(payload) "." hex(HMAC)`). Used internally by `hull/web/auth-flows` and `hull/web/middleware/oauth`; available standalone for any app that wants a tamper-detectable signed envelope without DB state |
| `pwned` | `hull.web.pwned` | `hull:web:pwned` | k-anonymity pwned-password check via HIBP range API. Hashes the password SHA-1 client-side, sends only the first 5 hex chars over the wire, scans the returned suffix list locally. Apps must add `api.pwnedpasswords.com` to `manifest.hosts`. Fail-open on HIBP outage. Used internally by `hull/web/auth-flows` when `check_pwned_passwords = true` |
| `audit-log` | `hull.web.middleware.audit-log` | `hull:web:middleware:audit-log` | Append-only sign-in / auth event log + per-device grouping. `record(user_id, kind, req, opts)`, `list(user_id, opts)`, `list_devices(user_id, opts)`, `is_new_device(user_id, req, opts)`. Fingerprint = `sha256(family_os|ip_prefix)[:16]`. Owns `_hull_audit_log`. Composes with auth-flows (emits events when `sign_in_log = true`), with session (per-device summary via `list_devices`), or standalone for app-recorded kinds (`api_token_issued`, `admin_impersonate`, etc.) |
| `session` | `hull.web.middleware.session` | `hull:web:middleware:session` | Server-side sessions backed by SQLite |
| `logger` | `hull.web.middleware.logger` | `hull:web:middleware:logger` | Request logging with logfmt output and request IDs |
| `transaction` | `hull.web.middleware.transaction` | `hull:web:middleware:transaction` | Wraps handlers in `db.batch()` (BEGIN IMMEDIATE..COMMIT) |
| `idempotency` | `hull.web.middleware.idempotency` | `hull:web:middleware:idempotency` | Idempotency-Key middleware with response caching |
| `outbox` | `hull.web.middleware.outbox` | `hull:web:middleware:outbox` | Transactional outbox for reliable side-effect delivery |
| `inbox` | `hull.web.middleware.inbox` | `hull:web:middleware:inbox` | Inbox deduplication for incoming events/webhooks |
| `cookie` | `hull.web.cookie` | `hull:web:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
| `template` | `hull.template` | `hull:template` | HTML template engine with inheritance, includes, filters |
| `validate` | `hull.validate` | `hull:validate` | Declarative input validation with schema rules |
| `form` | `hull.web.form` | `hull:web:form` | URL-encoded form body parsing |
| `i18n` | `hull.i18n` | `hull:i18n` | Internationalization: locale detection, translations, formatting |
| `csv` | `hull.csv` | `hull:csv` | CSV parse/encode (RFC 4180) |
| `tar` | `hull.archive.tar` | `hull:archive:tar` | ustar archive parse/create/extract/pack (extract/pack compose the fs capability) |
| `qrcode` | `hull.qrcode` | `hull:qrcode` | QR Code generator (ISO/IEC 18004), pure Lua/JS |
| `search` | `hull.search` | `hull:search` | Full-text search (SQLite FTS5) |
| `rbac` | `hull.web.middleware.rbac` | `hull:web:middleware:rbac` | Role-based access control |
| `health` | `hull.web.middleware.health` | `hull:web:middleware:health` | Health check + readiness endpoints |
| `etag` | `hull.web.middleware.etag` | `hull:web:middleware:etag` | ETag response helpers with 304 Not Modified |
| `db.udf` | `db.udf.register/unregister` | `db.udf.register/unregister` | User-defined SQL functions (Lua/JS callbacks or WASM) |
| `image` | `hull.image` | `hull:image` | Image decode/encode (stb_image), raw pixel buffers |
| `ws-server` | `hull.web.ws-server` | `hull:web:ws-server` | WebSocket server (`app.ws`, `broadcast`, `connections`) |
| `ws-client` | `hull.web.ws-client` | `hull:web:ws-client` | WebSocket client (`ws.connect`) |
| `sse` | `hull.web.sse` (decorates `app.sse`) | `hull:web:sse` (decorates `app.sse`) | Server-Sent Events |
| `json` | `hull.json` | (built-in) | JSON encode/decode |

### Module APIs

**cors.middleware(opts)**. CORS headers + OPTIONS preflight.
- `opts.origins`. List of allowed origins (default: `{"*"}`)
- `opts.methods`. Allowed methods string (default: `"GET, POST, PUT, DELETE, OPTIONS"`)
- `opts.headers`. Allowed headers string (default: `"Content-Type, Authorization"`)
- `opts.credentials`. Boolean, send `Access-Control-Allow-Credentials` (default: `false`)
- `opts.max_age`. Preflight cache seconds (default: `86400`)
- Returns `1` on OPTIONS preflight (sends 204), `0` otherwise.

**ratelimit.middleware(opts)**. Per-key request rate limiting (in-memory, resets on restart).
- `opts.limit`. Max requests per window (default: `60`)
- `opts.window`. Window in seconds (default: `60`)
- `opts.key`. String or `function(req) -> string` (default: `"global"`)
- Sets `X-RateLimit-Limit`, `X-RateLimit-Remaining`, `X-RateLimit-Reset` headers.
- Returns `1` on limit exceeded (sends 429 + JSON), `0` otherwise.

**csrf.middleware(opts)**. Stateless CSRF protection using HMAC tokens.
- `opts.secret`. HMAC secret (required)
- `opts.session_key`. Key in `req.ctx` for session ID (default: `"session_id"`) [Lua]
- `opts.max_age`. Max token age in seconds (default: `3600`)
- `opts.header_name`. Header to read token from (default: `"x-csrf-token"`)
- `opts.field_name`. Form field name (default: `"_csrf"`)
- Safe methods (GET/HEAD/OPTIONS): generates token → `req.ctx.csrf_token`.
- Unsafe methods: verifies token from header or form field.
- Returns `1` on verification failure (sends 403 + JSON), `0` otherwise.
- Helpers: `csrf.generate(session_id, secret)`, `csrf.verify(token, session_id, secret, max_age)`.
- **Body size caps (bounded work per request):** when reading the CSRF
  token from a url-encoded form body, the middleware caps total body at
  **1 MiB**, max **256** form pairs, and (Lua) max **4 KiB** per individual
  pair. Requests exceeding the body cap get **413**. Large multipart
  uploads should use a multipart parser BEFORE `csrf.middleware` in the
  stack. By the time CSRF sees the body, it should already be the
  pre-parsed url-encoded form, not the raw upload.

**auth.session_middleware(opts)**. Session cookie authentication.
- `opts.cookie_name`. Session cookie name (default: `"hull_session"`)
- `opts.optional`. Continue without session (default: `false`)
- `opts.login_path`. Redirect here on failure instead of sending 401
- Sets `req.ctx.session` and `req.ctx.session_id`.
- Returns `1` on auth failure (sends 401 or redirect), `0` on success.

**auth.jwt_middleware(opts)**. JWT Bearer token authentication.
- `opts.secret`. HMAC-SHA256 secret (required)
- `opts.optional`. Continue without token (default: `false`)
- Reads `Authorization: Bearer <token>` header.
- Sets `req.ctx.user` (decoded payload).
- Returns `1` on auth failure (sends 401 + JSON), `0` on success.

**auth.login(req, res, user_data, opts)**. Creates session, sets cookie. Returns `session_id`.

**auth.logout(req, res, opts)**. Destroys session, clears cookie.

**oauth**. OIDC / OAuth 2.0 Authorization Code flow with PKCE. Owns three
routes (`/auth/:provider/login`, `/auth/:provider/callback`, `/auth/logout`);
verifies ID token signatures against the IdP's JWKS (x5c → SPKI PEM via
`crypto.x509_pubkey_pem`); HMAC-signs the per-request state + nonce + PKCE
verifier into an HttpOnly cookie so the callback can't be replayed
cross-provider or by a CSRF.

- `oauth.init(opts)`. Call once at app startup. Required: `state_secret`
  (≥16 bytes), `providers = { name = {...}, ... }`. Optional: `state_cookie`,
  `state_ttl`, `find_user(provider, claims) -> user` (required when
  `on_login` is set), `on_login(req, res, user, ctx) -> path?` (signature
  matches `hull/web/auth-flows` so a single `session.login_handler(cookie)`
  wires both; `ctx = { provider, claims, tokens }` exposes OIDC-specific
  data for callers that need it), `on_logout(req, res) -> path?`.
- `oauth.routes(app)`. Mounts the three routes on the given app.
- Provider config. Either `preset = "google" | "microsoft"` (plus
  `client_id`, optional `client_secret`, `scopes`, `tenant` for Microsoft)
  or fully-explicit `{ authorization_endpoint, token_endpoint, jwks_uri,
  issuer, client_id, client_secret?, scopes? }`.
- Allowed signing algs: `RS256 / RS384 / RS512 / PS256 / ES256 / ES384`
  (HS256 excluded - OIDC IdPs don't sign ID tokens with HMAC).
- App must declare `hull/web/middleware/oauth@1` in `manifest.modules` plus
  add the IdP host (e.g. `accounts.google.com`, `login.microsoftonline.com`)
  to `manifest.hosts`.
- JS API: same shape with camelCase keys (`stateSecret`, `clientId`,
  `clientSecret`, `onLogin`, `onLogout`).
- See `tests/fixtures/oauth_client_lua/app.lua` for a complete worked
  example, exercised end-to-end against a Python mock IdP via
  `tests/e2e_oauth.sh`.

**totp**. RFC 6238 Time-based One-Time Password 2FA. Composes with
the existing `auth` + `session` modules - after password verify, the
app sets `req.ctx.session.pending_2fa = true`; this module's
middleware gates sensitive routes until a valid TOTP code (or recovery
code) is presented. Algorithm is fixed at HMAC-SHA1 (RFC 6238 default,
what every mainstream authenticator app - Google Authenticator,
Authy, 1Password - supports).

- `totp.init(opts)`. Required at app startup. Creates `_hull_totp` +
  `_hull_totp_recovery` tables.
  - `opts.issuer` (default `"Hull"`) - label shown in authenticator.
  - `opts.digits` (default `6`; also accepts `8`).
  - `opts.period` (default `30s`, RFC default).
  - `opts.window` (default `±1` step → ~90s clock-skew tolerance).
  - `opts.recovery_codes` (default `10`).
  - `opts.encryption_key` - optional 32-byte string. When set,
    secrets are NaCl-secretbox-encrypted at rest with a fresh nonce
    per enrollment. Caller manages the key (env, fs.read, etc.).
- `totp.enroll(user_id)` → `{ secret_base32, otpauth_url, qr_svg,
  recovery_codes }`. Recovery codes are returned ONCE; only PBKDF2
  hashes persist. Re-enrolling overwrites - intentional, it's the
  recovery path when both authenticator and codes are lost.
- `totp.confirm(user_id, code)` → bool. Pairs the authenticator;
  flips the row's `confirmed` flag.
- `totp.verify(user_id, code)` → bool. Bare boolean for the common
  `if not totp.verify(...) then deny() end` shape; matches JS. Use
  `totp.verify_with_kind(user_id, code)` → `(ok, "totp"|"recovery"|nil)`
  when you need the kind for audit metadata. TOTP path first;
  replay-protected via `last_used_step` (atomic compare-and-set in
  the UPDATE WHERE). Recovery-code path scans unused rows; consumed
  codes get `used_at` stamped.
- `totp.disable(user_id)` - deletes secret + recovery codes.
- `totp.enrolled(user_id)` → bool. Confirmed enrollment check.
- Login-time 2FA gating happens via `hull/web/auth-flows`
  (enable_totp + user_totp_enrolled + totp_verify callbacks). There
  is no `totp.middleware` / `pending_2fa` mechanism - the auth-flows
  envelope path is the single supported way.
- JS API mirrors the Lua surface with camelCase
  (`verifyWithKind` / `secretBase32` / `otpauthUrl` / `qrSvg` /
  `recoveryCodes` in the enroll return).
- Local-first note: TOTP needs no network at verify time (works
  air-gapped). Clock skew matters more off-cloud - raise `opts.window`
  on devices without NTP.

**auth-flows**. Transactional auth-flow recipes. Bundle of routes that
cover registration / email-verify / login / password-reset / magic-link
/ email-change. The module owns the auth-internal bookkeeping tables
(`_hull_auth_used_tokens`, `_hull_auth_pending_email_changes`) but
does NOT own the users table - the app provides `user_*` callbacks so
existing user models drop in. Optional TOTP composition wedges a 2FA
verify step between successful first-factor auth and `on_login` when
`enable_totp = true` is set (see below).

- `authflows.init(opts)`. Required at app startup. Validates all
  callbacks up front + creates internal tables.
  - `opts.state_secret` (≥32 bytes). HMAC key for the signed tokens.
  - `opts.email_send(to, subject, html, text)`. App-provided.
  - `opts.templates = { welcome, verify, magic_link, password_reset,
    email_change }`. Each is `function(ctx) → { subject, html?, text? }`.
  - `opts.user_*` callbacks (find_by_email, get, create, set_password,
    set_email, set_email_verified). All required; missing ones are
    reported together at init time. **Shortcut:** pass
    `opts.users = authflows.standard_users({ table = "users" })`
    to bulk-fill all six against the standard schema in one line;
    any explicit `opts.user_X` still overrides. The adapter is
    DB-backend-agnostic (works on whatever backend `hull/db` is
    wired to). Apps with a custom schema either pass the 6
    callbacks directly or post-process the adapter table.
  - `opts.on_login(req, res, user)` / `opts.on_logout(req, res)`. App
    issues its own session (cookie, JWT, whatever) here. Module is
    session-agnostic. **Shortcut:** wire
    `opts.on_login = session.login_handler(cookie)` and
    `opts.on_logout = session.logout_handler(cookie)` to get
    session creation + session-fixation defense + Set-Cookie + JSON
    response in one line each. The same factories work for OAuth
    after the find_user split (below).
  - `opts.require_verified_email` (default `true`). Block login until
    email is verified. Opt-out for apps that gate per-route on the
    `email_verified` flag instead.
  - `opts.magic_link_auto_signup` (default `false`). Silent no-op
    when magic-link is requested for an unknown email (enumeration-
    safe). Opt-in to auto-create a passwordless user instead.
  - `opts.enumeration_safe` (default `true`). Register / reset /
    magic-link / email-change return identical success shapes
    regardless of whether the email exists.
  - `opts.verify_ttl` (86400s), `opts.reset_ttl` (3600s),
    `opts.magic_link_ttl` (600s), `opts.email_change_ttl` (86400s).
  - `opts.prefix` (default `"/auth"`).
  - `opts.enable_totp` (default `false`). Opt in to TOTP-as-second-
    factor on successful password login OR magic-link click. Requires
    `opts.user_totp_enrolled(user_id) -> boolean` and
    `opts.totp_verify(user, code) -> boolean`. Apps typically delegate
    to `hull/web/middleware/totp` from these two callbacks; the
    recovery-code path flows transparently because `totp.verify`
    accepts both 6-digit and recovery codes.
  - `opts.totp_pending_ttl` (default `300`). Lifetime of the pending-
    2FA token issued between first-factor success and `/totp-verify`.
  - `opts.totp_pending_redirect`. When set, magic-link clicks that
    require 2FA redirect to `<redirect>?token=<totp_token>` instead
    of rendering the module's default minimal HTML form. POST-based
    `/login` always responds with JSON
    `{ ok: true, pending_2fa: true, totp_token: "…" }`.
  - **Hardening options** (all opt-in via init):
    - `opts.max_failed_logins` (default `5`) + `opts.lockout_duration`
      (default `900` = 15 min). After N consecutive failed-password
      attempts, the user's row in `_hull_auth_login_attempts` trips
      a `locked_until` window; `handle_login` responds 429 +
      `Retry-After` during the window regardless of whether the
      submitted password is right. Counter clears on successful
      login OR `password-reset/confirm`.
    - `opts.check_pwned_passwords` (default `false`). Routes
      register + password-reset-confirm through `hull/web/pwned`
      (HIBP k-anonymity). Apps must add `api.pwnedpasswords.com`
      to `manifest.hosts`. Fail-open on HIBP outage. Tests can
      override the endpoint via `opts.pwned_endpoint`.
    - **Email-change notify+revoke** activates implicitly when the
      app provides a `templates.email_change_notify` template. A
      revoke link is sent to the OLD address on every email-change
      request; the OLD-address holder can click it to delete the
      pending change (`/email-change/revoke?token=…`) within
      `email_change_ttl` even if the attacker holds a valid
      session cookie.
    - `opts.sign_in_log` (default `false`). Routes every login /
      password-reset-completed / email-changed / email-change-
      revoked into `hull/web/middleware/audit-log` so apps can
      surface a per-user device list, recent events, and "you
      signed in from a new device" UX. Requires
      `hull/web/middleware/audit-log` to be in scope (it's a
      transitive dep of auth-flows so the resolver auto-admits).
    - `opts.on_new_device(req, res, user)` (optional). Fires
      from inside the login path when `audit_log.is_new_device`
      returns true for this user + request fingerprint. App
      typically sends a "you signed in from a new device" email.
    - `opts.on_password_reset(req, res, user)` (optional). Fires
      after a successful `password-reset/confirm` updates the
      hash. Recommended implementation:
      `function(req, res, user) session.destroy_all(user.id) end`
      - revokes every existing session because a reset is the
      standard recovery move after a suspected compromise.
- `authflows.routes(app)`. Mounts the routes under `prefix`:
  POST `/register`, GET `/verify`, POST `/verify/resend`,
  POST `/login`, POST `/logout`, POST `/magic-link`,
  GET `/magic-link/consume`, POST `/password-reset/request`,
  POST `/password-reset/confirm`, POST `/email-change`,
  GET `/email-change/confirm`, GET `/email-change/revoke`,
  POST `/totp-verify` (404s when `enable_totp` is off).
  `/verify/resend` is enumeration-safe - always returns `{ok:true}`
  whether the user exists, is unverified, or is already verified.
  Apps SHOULD rate-limit it (per-email key) to bound mail volume.
- `authflows.send_verify_email(user, url_prefix)`,
  `authflows.send_password_reset(email, url_prefix)`,
  `authflows.send_magic_link(email, url_prefix)`. Standalone helpers
  for admin/programmatic triggers (resend, etc.).
- Token format: `base64url(JSON{sub, action, exp, nonce}) "." hmac_hex`.
  Signature framing comes from `hull/crypto/envelope` (shared with
  the OAuth state cookie); auth-flows layers single-use enforcement
  via `_hull_auth_used_tokens` (sha256 of full token as PK, atomic
  INSERT OR IGNORE → 0 rowcount = replay) plus action-tag + expiry
  checks. The TOTP-pending flow uses the underlying envelope.verify
  directly so the token stays usable across retry-on-typo attempts
  and is only burned on a successful code verify.
- JS API: camelCase keys (`stateSecret`, `emailSend`, `userFindByEmail`,
  `onLogin`, `magicLinkAutoSignup`, `requireVerifiedEmail`, `enableTotp`,
  `userTotpEnrolled`, `totpVerify`, `totpPendingTtl`, `totpPendingRedirect`).
  `totp.verify(userId, code)` returns a bare boolean (the historical
  `[ok, kind]` tuple lives behind `totp.verifyWithKind` now - see
  the TOTP section), so a `totpVerify: (user, code) => totp.verify(
  user.id, code)` delegate is safe by default.
- Email-change flow re-verifies on the NEW address - old email stays
  active until the user clicks the link sent to the new one.

**session**. Server-side sessions backed by SQLite. Requires `session.init()` at startup.
- `session.init(opts)`. Creates `hull_sessions` table; runs PRAGMA-checked additive migrations (adds `user_id`, `ip`, `user_agent` columns for the device-management helpers). `opts.ttl` = lifetime in seconds (default: `86400`).
- `session.create(data, opts?)` → 64-char hex session ID. `opts.req` lets it capture ip + ua + user_id columns at create time. `data.user_id` is auto-populated into the column.
- `session.load(session_id)` → data table or nil. Auto-extends expiry.
- `session.update(session_id, data)`. Updates session data.
- `session.destroy(session_id)`. Deletes session.
- `session.cleanup()` → count of deleted expired sessions.
- **Device management** - `session.list_for_user(user_id)` → array of `{id, created_at, last_accessed, ip, user_agent}`; `session.destroy_others(current_sid, user_id)` → "sign out everywhere else"; `session.destroy_all(user_id)` → "sign out everywhere" (used by auth-flows on password reset cascade).
- **Login/logout factories** - `session.login_handler(cookie, opts?)` returns a turnkey `on_login(req, res, user, ctx?)` callback that creates a session, sets the cookie, and responds. Defaults to session-fixation defense (`session.rotate(prior_sid, ...)`). `opts.name` (cookie name, default `"hull_session"` - same as `auth.session_middleware`), `opts.cookie_opts` (forwarded to `cookie.serialize`), `opts.extract_data(user) -> data`, `opts.respond(res, user, sid)`, `opts.rotate` (default `true`), `opts.audit_log` (module ref - when set, records a login event after the session is set), `opts.audit_kind` (default `"login"`), `opts.audit_metadata(user, ctx) -> table` (default derives `{ factors = ctx.factors }` for auth-flows or `{ factors = "oauth:" .. ctx.provider }` for oauth), `opts.on_new_device(req, res, user)` (requires `audit_log` - called before record when `audit_log.is_new_device` returns true). `session.logout_handler(cookie, opts?)` is the matching `on_logout`. Same factories work for `hull/web/auth-flows` AND `hull/web/middleware/oauth` (the audit + new-device seam covers both for free).
- `session.rotate(old_sid, data, opts)` - destroy + recreate, session-fixation defense primitive. Used by `login_handler`; apps doing custom on_login can call it directly.

**The `on_login(req, res, user, ctx?)` contract.** Both `hull/web/auth-flows` and `hull/web/middleware/oauth` hand off through this single shape. Guarantees:
- `user` is the **app's** user object (whatever `find_user` / `standard_users` / a custom adapter produced). `user.id` MUST be a non-empty string - `session.login_handler` enforces it and throws otherwise. `user.email` is conventionally present but not required.
- `ctx` is `nil` for the simplest call sites, or a table carrying source-specific metadata. auth-flows passes `{ factors = "password" | "magic_link" | "password+totp" }`. oauth passes `{ provider, claims, tokens }` (the IdP claims + raw tokens, for apps that want to capture them).
- Returning a string overrides the post-login redirect target (oauth honors this; auth-flows uses the redirect from `login_redirect` opt).
- `on_logout(req, res)` is the matching shape - no `user` arg because the session row is the source of truth there.

**Audit-metadata scrub at the session.login_handler seam.** When `audit_log` is wired, the factory calls your `audit_metadata(user, ctx)` and then **strips** these keys from the result before passing it to `audit_log.record`: `tokens`, `token`, `access_token`, `refresh_token`, `id_token`, `claims`, `password`, `password_hash`, `pwhash`, `secret`. This is defense in depth - the **OAuth ctx already contains `claims` and `tokens`** (the raw IdP tokens), so a `audit_metadata = function(_,c) return c end` override would otherwise persist access_token + refresh_token in `_hull_audit_log.metadata` for `retain_days` (default 365). The scrub is top-level only; if you need to log claim details, pull them out by name in your custom `audit_metadata` (e.g. `return { factors = "oauth:" .. ctx.provider, sub = ctx.claims.sub }`) - never pass the raw `ctx` through.

**cookie**. Cookie helpers (not middleware).
- `cookie.parse(header)` → table `{ name = value, ... }`.
- `cookie.serialize(name, value, opts)` → `Set-Cookie` header string.
  - `opts.path` (default: `"/"`), `opts.httponly` (default: `true`), `opts.secure`, `opts.samesite` (default: `"Lax"`), `opts.max_age`, `opts.domain`.
- `cookie.clear(name, opts)` → `Set-Cookie` header with `Max-Age=0`.

**jwt**. JWT sign/verify (HS256 only, not middleware).
- `jwt.sign(payload, secret)` → token string. Auto-sets `iat`.
- `jwt.verify(token, secret)` → payload table, or `nil, "error reason"`.
- `jwt.decode(token)` → payload table or nil (no signature check).

**logger.middleware(opts)**. Request logging with logfmt output and auto-assigned request IDs.
- `opts.skip`. List of paths to skip (exact match, e.g. `{"/health"}`)
- `opts.include_headers`. List of header names to include in log line
- Sets `X-Request-ID` response header and `req.ctx.request_id`.
- Helpers: `logger.generate_id()`, `logger.format_line(entries)`, `logger.should_skip(path, skip_list)`.
- Returns `0` (always continues).

**validate.check(data, schema)**. Declarative input validation.
- `schema` maps field names to rule tables.
- Rules: `required`, `trim`, `type` (`"string"`, `"number"`, `"integer"`, `"boolean"`), `min`, `max`, `pattern`, `oneof`, `email`, `fn` (custom validator), `message` (custom error).
- `min`/`max` apply to string length or numeric value depending on field type.
- Returns `(ok, errors)` where `errors` maps field names to error strings.

**form.parse(body)**. URL-encoded form body parsing.
- Decodes `application/x-www-form-urlencoded` format.
- Handles `+` → space and `%XX` percent-encoding. Last value wins for duplicates.
- Returns table `{ field_name = value, ... }` (empty table for nil/empty input).

**i18n**. Internationalization: locale detection, message bundles, formatting.
- `i18n.load(name, tbl)`. Register a locale with translations and format rules.
- `i18n.locale(name?)`. Get or set the active locale.
- `i18n.t(key, params?)` → translated string. Supports `${variable}` interpolation and dot-path keys.
- `i18n.number(n)` → formatted number (locale-specific decimal/thousands separators).
- `i18n.date(timestamp)` → formatted date string.
- `i18n.currency(amount, code)` → formatted currency string (symbol + locale rules).
- `i18n.detect(accept_language_header)` → best matching locale name or nil.

**transaction**. Wraps handlers in SQLite transactions.
- `transaction.middleware()`. Post-body middleware that sets `req.ctx._txn = true` for downstream use.
- `transaction.run(fn)`. Wraps `fn` in `db.batch()` (BEGIN IMMEDIATE → fn() → COMMIT, ROLLBACK on error).
- `transaction.try(fn)` → `(ok, err)`. Like `run` but returns error instead of throwing.

**idempotency**. Idempotency-Key middleware with response caching.
- `idempotency.init(opts)`. Creates `_hull_idempotency_keys` table. `opts.ttl` = key lifetime in seconds (default: `86400`).
- `idempotency.middleware(opts)`. Post-body middleware intercepting POST (configurable via `opts.methods`).
  - `opts.header_name`. Header to read key from (default: `"idempotency-key"`).
  - `opts.get_principal`. `function(req) -> string` for per-user scoping (default: `"__anon"`).
  - Cache hit + same fingerprint → returns cached response (handler skipped).
  - Cache hit + different fingerprint → returns 409 Conflict.
  - Fingerprint: `SHA-256(method + path + body)`.
- `idempotency.respond(req, res, status, data)`. Sends response and caches it for replay.
- `idempotency.complete(req)`. Marks key as processed without caching response body.
- **Replay-header allowlist (security):** when `idempotency.respond` is
  called with `extra_headers`, only headers on a strict allowlist are
  emitted AND persisted to SQLite. Allowed: `Content-*`, `Location`,
  `ETag`, `Last-Modified`, `Cache-Control`, `Vary`, the stdlib's own
  `X-Request-ID` / `X-RateLimit-*` / `X-Idempotency-Replay`, plus
  `X-Content-Type-Options`, `X-Frame-Options`, `Strict-Transport-Security`,
  `Content-Security-Policy`, `Referrer-Policy`, `Permissions-Policy`.
  Anything else. Especially credential headers like `Set-Cookie`,
  `Authorization`, `X-Auth-*`, `X-API-Key`, `X-CSRF-*`,
  `X-Forwarded-Authorization`, `X-Amz-Security-Token`, etc.. Is dropped
  silently on BOTH the cache-write and replay paths. Set credential
  headers via a separate middleware (e.g. session.create) instead of
  passing them through `respond()`'s `extra_headers`.
- `idempotency.cleanup()` → count of deleted expired keys.

**outbox**. Transactional outbox for reliable side-effect delivery.
- `outbox.init(opts)`. Creates `_hull_outbox` table. `opts.max_attempts` (default: `5`).
- `outbox.enqueue(opts)`. Enqueue a delivery (call inside a transaction).
  - `opts.kind`. Delivery type (e.g. `"webhook"`, `"email"`).
  - `opts.destination`. Target URL or address.
  - `opts.payload`. Payload string.
  - `opts.headers`. JSON-encoded headers (optional).
  - `opts.idempotency_key`. Dedup key for delivery (optional).
- `outbox.flush(opts)`. Deliver pending items. Exponential backoff (`2^attempt * 10s`, capped at 1hr).
- `outbox.middleware()`. Sets `req.ctx._outbox_flush = true` for auto-flush.
- `outbox.stats()` → `{ pending, delivered, failed }` counts.
- `outbox.cleanup(max_age)`. Delete old delivered items.

**inbox**. Inbox deduplication for incoming events/webhooks.
- `inbox.init(opts)`. Creates `_hull_inbox_processed` table. `opts.ttl` = record lifetime (default: `86400`).
- `inbox.is_duplicate(message_id, source?)` → boolean. Default source: `"default"`.
- `inbox.mark_processed(message_id, source?, opts?)`. Record as processed.
- `inbox.check_and_mark(message_id, source?, opts?)` → boolean (true = duplicate, false = new + marked).
- `inbox.cleanup()` → count of deleted expired records.

**template**. HTML template engine with compile-once, render-many caching.

```lua
local template = require("hull.template")
template.render("pages/home.html", data)       -- load + compile + render (cached)
template.render_string(source, data)            -- compile from string + render
template.compile("pages/home.html")             -- returns compiled function
template.clear_cache()                          -- clear compiled function cache
```

Template syntax:
- `{{ var }}`. HTML-escaped output
- `{{ var.path }}`. Dot path lookup (nil-safe)
- `{{ var | filter }}`. Pipe filter (`upper`, `lower`, `trim`, `length`, `default: value`, `json`, `raw`)
- `{{{ var }}}`. Raw (unescaped) output
- `{% if var %}` / `{% elif var %}` / `{% else %}` / `{% end %}`. Conditionals
- `{% for item in list %}` / `{% for key, val in obj %}`. Iteration
- `{% block name %}` / `{% extends "parent.html" %}`. Template inheritance
- `{% include "partial.html" %}`. Include partials
- `{# comment #}`. Stripped from output

Templates are loaded from `app_dir/templates/` in dev mode. In built binaries, templates are embedded as byte arrays via `hull build` or `make APP_DIR=...`.

**JS API** (camelCase):
```javascript
import { template } from "hull:template";
template.render("pages/home.html", data);       // load + compile + render (cached)
template.renderString(source, data);             // compile from string + render
template.compile("pages/home.html");             // returns compiled function
template.clearCache();                           // clear compiled function cache
```

**Template engine details:**
- **Compilation:** Templates are parsed (lexer → recursive-descent parser → AST), then code-generated to native Lua/JS source and compiled via `luaL_loadbuffer` (Lua) or `JS_Eval` (JS). Compiled functions are cached. Compile once, render many.
- **XSS safety:** All `{{ }}` output is HTML-escaped by default (`& < > " '` → entities). Only `{{{ }}}` and `| raw` bypass escaping.
- **Dot paths are nil-safe:** `{{ user.address.city }}` returns empty string if any intermediate is nil/undefined. No errors.
- **For-loop variables are scoped:** Inside `{% for item in items %}`, `item` refers to the loop variable, not `data.item`.
- **Lua truthiness caveat:** In Lua, empty tables `{}` and `0` are truthy. Use a boolean flag like `has_items = #items > 0` when checking emptiness in `{% if %}`.
- **Filters:** `upper`, `lower`, `trim`, `length`, `default: "value"`, `json`, `raw`. Filters chain: `{{ name | trim | upper }}`.
- **Inheritance:** `{% extends "base.html" %}` loads parent, child overrides `{% block name %}` content. Multi-level inheritance supported. Circular extends detected.
- **Includes:** `{% include "partials/nav.html" %}` inlines the partial's AST. Included templates share the same data context.
- **Template directory:** Place templates in `app_dir/templates/`. Names are relative paths (e.g. `"pages/home.html"`, `"partials/nav.html"`, `"base.html"`).
- **CSP nonce:** No engine magic needed. Pass nonce as data: `template.render("page.html", { csp_nonce = nonce })`, use `<script nonce="{{ csp_nonce }}">` in template.

**csv.parse(text, opts?)**. Parse CSV text (RFC 4180).
- `opts.headers`. First row is header; returns objects (default: `false`)
- `opts.separator`. Field delimiter (default: `","`)
- Returns array of row arrays, or row objects if `headers = true`.

**csv.encode(rows, opts?)**. Encode rows as CSV text.
- `opts.headers`. Rows are objects; emit header row (default: `false`)
- `opts.separator`. Field delimiter (default: `","`)
- Returns CSV string.

**tar** (`hull.archive.tar` / `hull:archive:tar`). ustar (`.tar`) archive
handling backed by the shared C core (`cap/tar.c`) - the SAME core `hull tools
install` uses for signed bundles. Namespaced under `hull/archive/` as the
container-format family (a future `zip` would be a sibling; stream codecs like
gzip belong under a separate `hull/compress/`). `parse`/`create` are pure
byte<->table transforms (no authority); `extract`/`pack` COMPOSE the fs
capability, so they enforce `manifest.fs.write` / `fs.read` exactly like
`fs.write`/`fs.read` (path validation + allowlist), NOT the trusted install-path
extractor. `parse`/`create`/`extract` accept any buffer type (string /
`MappedBuffer` / `WasmBuffer` / `ArrayBuffer`) for the archive bytes.
- `tar.parse(bytes)` -> array of `{ name, data, size, mode, is_dir }` (JS:
  `isDir`; `data` is a Lua string / JS `ArrayBuffer`). Malformed / truncated /
  traversal-bearing archives return `nil, err` (Lua) / throw (JS).
- `tar.create(entries)` -> archive bytes (Lua string / JS `ArrayBuffer`). Each
  entry is `{ name, data?, mode?, is_dir? }` (JS: `isDir`; `mode` defaults 0644).
  An absolute or `..`-bearing member name is refused.
- `tar.extract(bytes, dest_dir)` -> `true` | `nil, err` (JS: throws on error).
  Writes each member under `dest_dir` via the fs capability (needs
  `manifest.fs.write`). Directory entries are skipped (file writes create their
  parents; empty dirs are dropped).
- `tar.pack(files)` -> archive bytes. `files` is an array of path strings or
  `{ path, name? }` tables; each is read via the fs capability (needs
  `manifest.fs.read`) and added as a member (name defaults to the path).

**qrcode**. QR Code generator (ISO/IEC 18004). Pure Lua / JS, byte
mode, all four EC levels, versions 1-40, all 8 mask patterns scored
per spec. Algorithm structure adapted from Project Nayuki's
MIT-licensed QR Code generator library; tables transcribed from
ISO/IEC 18004:2015 Annex E + Table 9 and cross-verified against
Python's `qrcode` library on 48 input / EC / mask combinations.

- `qrcode.encode(text, opts?)` → `{ matrix, size, version, ec_level, mask }`.
  Matrix is 1-indexed; cells are 0 (light) or 1 (dark).
  - `opts.ec_level` (Lua) / `opts.ecLevel` (JS). `"L"|"M"|"Q"|"H"`, default `"M"`.
  - `opts.mask`. `0..7` to force a specific mask; omitted runs the
    8-mask score-and-pick selector per spec 8.8.2.
- `qrcode.svg(text, opts?)` → SVG string.
  - `opts.scale`. Pixel size per module (default 4).
  - `opts.margin`. Quiet-zone modules around the QR (default 4).
  - `opts.dark` / `opts.light`. Colors (default `"#000"` / `"#fff"`).
    Pass `light: "none"` for transparent background.
- Input is treated as bytes (Latin-1 / ASCII). For non-ASCII payloads,
  encode to UTF-8 bytes at the call site before passing to `encode`.

Used by `hull/web/middleware/totp` for enrollment QR rendering;
also useful for WiFi codes, contact cards, payment links, etc.

**search**. Full-text search backed by SQLite FTS5.
- `search.create_index(name, columns, opts?)`. Create FTS5 virtual table.
- `search.index(name, id, fields)`. Insert/replace document.
- `search.remove(name, id)`. Delete document.
- `search.query(name, query, opts?)`. Full-text search. Returns `{id, rank}` array.
  - `opts.limit` (default: 20), `opts.offset` (default: 0)
- `search.reindex(name, source_table, opts?)`. Bulk re-index from table.
- `search.drop_index(name)`. Drop FTS5 table.

**rbac**. Role-based access control backed by SQLite.
- `rbac.init()`. Creates `_hull_roles`, `_hull_permissions`, `_hull_role_permissions`, `_hull_user_roles` tables.
- `rbac.define_role(name, permissions?)`. Create role with optional permissions.
- `rbac.assign(user_id, role)` / `rbac.revoke(user_id, role)`. Manage user roles.
- `rbac.roles(user_id)` → array of role names.
- `rbac.has_role(user_id, role)` → boolean.
- `rbac.has_permission(user_id, permission)` → boolean.
- `rbac.require_role(role)` → middleware function (403 on denial).
- `rbac.require_permission(perm)` → middleware function (403 on denial).

**health**. Liveness (`/health`) and readiness (`/ready`) endpoints with DB ping, custom checks, and server stats.
- `health.register(name, fn)`. Register a custom health check. `fn()` returns `true` or `false`.
- `health.unregister(name)`. Remove a registered check.
- `health.run_checks(opts)` → `{ checks, all_ok }`. `opts.db_check` (default: `true`).
- `health.middleware(opts)`. Returns middleware that intercepts `/health` and `/ready`.
  - `opts.path_health`. Liveness path (default: `"/health"`). Returns `{ status: "ok", uptime }`.
  - `opts.path_ready`. Readiness path (default: `"/ready"`). Returns status, checks, uptime, server stats.
  - `opts.db_check`. Include DB ping (default: `true`).
  - Returns `1` on health/ready paths, `0` otherwise (passes through to next handler).
  - Readiness returns 503 if any check fails.
- **JS only:** `health.setDb(dbModule)`. Pass the db module explicitly (ES modules can't conditionally import). Also accepts `opts.db` in middleware options.

**etag** (ETag response helpers. Not a middleware) provides wrapper functions for route handlers.
- `etag.json(req, res, data, status?)`. Send JSON response with ETag. Sends 304 if `If-None-Match` matches.
- `etag.text(req, res, text, status?)`. Same for text responses.
- `etag.html(req, res, html, status?)`. Same for HTML responses.
- `etag.compute(body)` → `W/"<first 16 hex chars of SHA-256>"` or `nil`.
- `etag.matches(req, tag)` → boolean. Checks `If-None-Match` header (comma-separated, `*` wildcard).
- Only computes ETags for GET/HEAD requests. Skips bodies > 1 MB.

**db.udf**. User-defined SQL functions backed by Lua/JS callbacks or WASM modules.
- `db.udf.register(name, fn, opts?)`. Register scalar UDF (Lua/JS function).
- `db.udf.register(name, {step, finalize}, opts?)`. Register aggregate UDF.
- `db.udf.register(name, "module_name", opts?)`. Register WASM-backed UDF.
- `db.udf.unregister(name)`. Remove a registered UDF.
- `opts.deterministic`. Boolean, enables SQLite optimizer (default: false)
- `opts.aggregate`. Boolean, WASM aggregate mode (default: false)
- `opts.args`. Number of arguments (-1 = variadic, default: 1)
- `opts.gas`. Per-row gas limit for WASM UDFs (default: 100K)
- Names must start with `hull_` to prevent shadowing SQLite built-ins.
- Lua/JS UDFs work with `db.query()` only (sync). WASM UDFs work with both `db.query()` and `db.async.query()`.

**image**. Image creation, encoding, and decoding via pluggable codec vtable (stb_image default).
- `image.new(w, h, format, pixels)` → HlImage. Formats: `"rgba8"`, `"r8"`, `"rgba16float"`, `"r32float"`.
- `image.from_buffer(buf, w, h, format)` → HlImage. Zero-copy borrow from a `MappedBuffer`/`WasmBuffer` (the source's bytes back the image directly). The borrow is refcounted: closing the source (`buf:close()`) while the image is alive is safe and defers the source's actual munmap/free until the last borrowing image is freed (no dangling pixels). Other sources (string, `ArrayBuffer`/typed array, another image) have no refcountable object to pin and are copied.
- `image.decode(data, format?)` → HlImage. Auto-detects PNG/JPEG/BMP from magic bytes.
- `image.encode(img, format, opts?)` → bytes. `opts.quality` for JPEG (default 90).
- `img:width()`, `img:height()`, `img:format()`, `img:size()`. Properties.
- `img:pixels()`. Raw pixel bytes.
- `img:close()`. Explicit free (GC handles it otherwise).

### WebSocket Endpoints

Register WebSocket endpoints with `app.ws(path, callbacks)`. Server-side connections are managed via the `HlWsRegistry`.

**Lua:**
```lua
app.ws("/ws/chat", {
    on_open = function(conn)
        log.info("connected: " .. conn:id())
    end,
    on_message = function(conn, msg, is_binary)
        ws.broadcast("/ws/chat", msg)
    end,
    on_close = function(conn, code, reason)
        log.info("disconnected: " .. conn:id())
    end,
})
```

**JavaScript:**
```javascript
import { wsServer } from "hull:web:ws-server";
app.ws("/ws/chat", {
    onOpen(conn) { log.info("connected: " + conn.id); },
    onMessage(conn, msg, isBinary) { ws.broadcast("/ws/chat", msg); },
    onClose(conn, code, reason) { log.info("disconnected: " + conn.id); },
});
```

**Connection object:**
- `conn:id()` / `conn.id`. Monotonic connection ID (getter)
- `conn:path()` / `conn.path`. Endpoint path (getter)
- `conn:send(text)` / `conn.send(text)`. Send text frame
- `conn:send_binary(data)` / `conn.sendBinary(data)`. Send binary frame
- `conn:close(code?, reason?)` / `conn.close(code?, reason?)`. Initiate close
- `conn:ping(data?)` / `conn.ping(data?)`. Send ping
- `conn.data`. Per-connection storage (table/object, lazy-created)

**Module functions:**
- `ws.broadcast(path, data [, binary])`. Broadcast to all connections on path, returns count sent
- `ws.connections(path)`. Count active connections on path
- `ws.connect(url, handlers [, opts])`. Connect to remote WebSocket server (see below)

**Client WebSocket (`ws.connect`):**

```lua
local client = ws.connect("ws://other:8080/feed", {
    on_open = function(conn) conn:send("hello") end,
    on_message = function(conn, msg) log.info(msg) end,
    on_close = function(conn, code, reason) end,
    on_error = function(conn, err) log.error(err) end,
})
```

```javascript
const client = ws.connect("ws://other:8080/feed", {
    onOpen(conn) { conn.send("hello"); },
    onMessage(conn, msg) { log.info(msg); },
    onClose(conn, code, reason) {},
    onError(conn, err) { log.error(err); },
});
```

- Client conn has same `send`/`sendBinary`/`close`/`ping` methods as server conn
- Host allowlist enforced (same as `http.fetch`. Must be in manifest `hosts`)
- Requires running server (`hull` with `-p` port)
- Callbacks fire on the event loop thread (same as server WS callbacks)

### SSE Endpoints

Register Server-Sent Events endpoints with `app.sse(path, handler)`. The handler receives a request object and a stream object.

**Lua:**
```lua
app.sse("/sse/events", function(req, stream)
    stream:event("welcome", json.encode({ time = time.datetime() }))
    for i = 1, 5 do
        hull.sleep(1000)
        stream:event("tick", tostring(i), tostring(i))
    end
    stream:close()
end)
```

**JavaScript:**
```javascript
app.sse("/sse/events", async (req, stream) => {
    stream.event("welcome", JSON.stringify({ time: time.datetime() }));
    for (let i = 1; i <= 5; i++) {
        await hull.sleep(1000);
        stream.event("tick", String(i), String(i));
    }
    stream.close();
});
```

**Stream object:**
- `stream:event(name, data [, id])` / `stream.event(name, data, id?)`. Send SSE event. `name` = event type (null/nil to omit), `data` = event data (multiline auto-split), `id` = event ID (optional)
- `stream:comment(text)` / `stream.comment(text)`. Send SSE comment (keep-alive)
- `stream:close()` / `stream.close()`. End the stream

**Implementation:** Uses Keel's `kl_sse_begin` / `kl_sse_event` / `kl_sse_end` over chunked transfer encoding. The handler runs as a coroutine (Lua) or async function (JS) that can yield with `hull.sleep()` between events.

### Streaming Multipart Uploads

Routes can opt into streaming `multipart/form-data` parsing via `opts.multipart` on `app.<verb>(...)`. The handler runs before the body is buffered and pulls bytes out of the socket on demand via an iterator. There is no `req.body` for these routes - `req:multipart()` / `req.multipart()` is the only way to read the body.

```lua
app.post("/upload", function(req, res)
    for part in req:multipart() do
        if part.filename then
            for chunk in part:chunks() do
                -- handle binary chunk (Lua byte string, #chunk = bytes)
            end
        else
            local value = part:read()
        end
    end
    res:json({ ok = true })
end, { multipart = { max_part_size = 64 * 1024 * 1024, max_total_size = 256 * 1024 * 1024, max_parts = 32 } })
```

```javascript
app.post("/upload", async (req, res) => {
    for await (const part of req.multipart()) {
        if (part.filename) {
            for await (const chunk of part.chunks()) {
                // chunk is an ArrayBuffer; binary-safe (.byteLength = bytes)
            }
        } else {
            const buf = await part.read();   // ArrayBuffer
        }
    }
    res.json({ ok: true });
}, { multipart: { maxPartSize: 64 * 1024 * 1024, maxTotalSize: 256 * 1024 * 1024, maxParts: 32 } });
```

**Caps** (all default to `0` = unlimited): `max_part_size` / `maxPartSize`, `max_total_size` / `maxTotalSize`, `max_parts` / `maxParts`, `max_headers_size` / `maxHeadersSize`, `max_input_buffer` / `maxInputBuffer`. Exceeding any cap raises a parser error which the handler can `pcall` / try-catch to write a structured 4xx response; uncaught errors → 500. Works for both single-read and multi-read bodies - Keel v2.2.0's streaming-async dispatch invokes the handler BEFORE feeding leftover body bytes, so the handler is alive when the cap trips in `on_data`. JS accepts both naming conventions; snake_case wins if both appear.

**Part fields:** `name` (always), `filename` (`nil`/`null` for text fields), `content_type` (Lua) / `contentType` (JS).

**Binary safety:** Lua chunks/`read()` return byte-clean Lua strings. JS chunks/`read()` return `ArrayBuffer` (never JS strings - `JS_NewStringLen` would UTF-8-mangle binary input). To decode text fields in JS, use `new TextDecoder().decode(buf)` (BYOP - QuickJS doesn't bundle it; ASCII can use a manual loop).

**Implementation:**
- Route is registered via `kl_server_route_streaming_async` (Keel v2.2.0+) + a per-runtime factory shim (`hl_{lua,js}_multipart_factory` in `runtime/{lua,js}/routes.c`) that wraps Keel's `kl_body_reader_multipart` with the parkable `hl_cap_multipart_factory` wrapper (`src/hull/cap/body.c`). The async variant means Keel invokes the handler BEFORE feeding leftover body bytes; the handler parks on `NEED_DATA` immediately, and `on_data` resumes it for both leftover and subsequent socket reads.
- The iterator drives `kl_multipart_next()` and on `NEED_DATA` parks the handler via `hl_cap_multipart_park`, sets `c->state = KL_CONN_READING_BODY`, and yields (Lua coroutine / pending JS Promise). The body reader's `on_data` callback fires the park and resumes.
- Handlers that respond without iterating (e.g. auth rejection): Keel forces `keep_alive=0` so stranded body bytes don't bleed into a re-used connection's next request.
- Bindings live in `src/hull/runtime/lua/mod_request.c` and `src/hull/runtime/js/mod_request.c`.

**Known limitations:**
- Live connection required - in-process `hull test` dispatch raises on first `NEED_DATA`. End-to-end coverage is in `tests/e2e_multipart.sh` (run via `make e2e-multipart`).
- `Part` is invalidated after the next iter step (parser is forward-only - don't stash parts past their iteration).
- `chunks(n)` accepts a min-bytes hint that's currently advisory (each parser event = one chunk; coalescing is a follow-up).
- Mid-stream connection close leaks the parked continuation - production deployments should run behind a reverse proxy with request timeouts.

See [docs/multipart.md](docs/multipart.md) for the full API + `examples/multipart_upload/` for a runnable Lua + JS demo.

### Static File Serving

Convention-based: place files in `app_dir/static/`, they're served at `/static/*`.

- **Dev mode:** Reads from disk via `kl_response_file()` (zero-copy sendfile). `Cache-Control: no-cache`.
- **Build mode:** Files are embedded in the binary via `hl_app_entries[]` (with `static/` prefix) and looked up via VFS (`hl_vfs_find`). `Cache-Control: public, max-age=86400`.
- **ETag/304:** `W/"<size_hex>"` for embedded, `W/"<mtime_hex>-<size_hex>"` for filesystem. Returns 304 on `If-None-Match`.
- **MIME types:** Extension-based lookup (21 types: html, css, js, json, png, jpg, svg, woff2, etc.). Default: `application/octet-stream`.
- **Security:** Rejects `..` path traversal, null bytes, leading `/` in relative paths.
- **Auto-detection:** Middleware is registered only when `hl_vfs_has_prefix(app_vfs, "static/")` returns true or `static/` directory exists on disk. User routes take priority (registered first).

Implementation: `src/hull/static.c` + `include/hull/static.h`. Uses `HlVfs` for O(log n) embedded lookup. Registered as a Keel pre-body middleware via `kl_server_use()`.

Embedding paths:
- `make APP_DIR=myapp`. Makefile discovers all app files, generates sorted `app_registry.c` with single `hl_app_entries[]`
- `hull build myapp`. `build.lua` discovers all files, generates sorted `app_registry.c`
- All file types share one `HlEntry` array, sorted by name (`LC_ALL=C sort`), disambiguated by naming convention: `./` (modules), `templates/`, `static/`, `migrations/`
- At runtime, consumers use `HlVfs` for O(log n) lookups instead of O(n) linear scans

### Recommended Middleware Stack

Order matters. Each middleware runs before the next:

```lua
local cors        = require("hull.web.middleware.cors")
local ratelimit   = require("hull.web.middleware.ratelimit")
local auth        = require("hull.web.middleware.auth")
local csrf        = require("hull.web.middleware.csrf")
local session     = require("hull.web.middleware.session")
local logger      = require("hull.web.middleware.logger")
local health      = require("hull.web.middleware.health")
local etag        = require("hull.web.middleware.etag")
local transaction = require("hull.web.middleware.transaction")
local idempotency = require("hull.web.middleware.idempotency")

session.init()       -- create hull_sessions table
idempotency.init()   -- create _hull_idempotency_keys table

-- Pre-body middleware (runs before body is read)
-- 0. Health checks. /health (liveness) and /ready (readiness)
app.use("GET", "/*", health.middleware())
-- 1. Logging. Assign request ID, log method + path
app.use("*", "/*", logger.middleware({ skip = {"/health"} }))
-- 2. Rate limiting. Reject abusive traffic before doing any work
app.use("*", "/api/*", ratelimit.middleware({ limit = 60, window = 60 }))
-- 3. CORS. Must run before auth so preflight doesn't require credentials
app.use("*", "/api/*", cors.middleware({ origins = { "https://myapp.com" } }))
-- 4. Authentication. Session or JWT
app.use("*", "/api/*", auth.session_middleware({}))

-- Post-body middleware (runs after body is read)
-- 5. CSRF. Needs body for form token (session-based apps only, not JWT)
app.use_post("*", "/*", csrf.middleware({ secret = "change-me" }))
-- 6. Transaction. Wrap mutations in BEGIN IMMEDIATE..COMMIT
app.use_post("POST", "/api/*", transaction.middleware())
-- 7. Idempotency. Cache POST responses by Idempotency-Key header
app.use_post("POST", "/api/*", idempotency.middleware())
-- 8. Route handlers. Use etag.json() instead of res:json() for ETag support
app.get("/api/items", function(req, res)
    local items = db.query("SELECT * FROM items")
    etag.json(req, res, { items = items })
end)
```

### Best Practices

- **Middleware order matters:** Rate limit before auth (reject early, save work). CORS before auth (preflight must not require credentials).
- **Scope middleware to paths:** Use `"/api/*"` not `"/*"` for CORS and rate limiting. Public routes (health checks, static assets) shouldn't be rate limited or require auth.
- **Use `req.ctx` for data passing:** Middleware stores data in `req.ctx` (e.g. `session_id`, `user`, `csrf_token`) for downstream handlers.
- **CORS origins:** Always list explicit origins in production. Never use `"*"` with `credentials = true`.
- **Rate limiting keys:** Default `"global"` key rate-limits all clients together. Use a key function for per-user limits: `key = function(req) return req.ctx.user_id or req.headers["x-forwarded-for"] or "anon" end`.
- **CSRF is for cookies only:** Session/cookie auth needs CSRF protection. JWT Bearer auth does not (tokens aren't sent automatically by browsers).
- **Session init at startup:** Call `session.init()` before registering routes. It creates the SQLite table.
- **Lua vs JS differences:** The Lua and JS APIs are functionally equivalent but differ in naming conventions (snake_case vs camelCase) and some defaults. See the JS stdlib source for JS-specific option names.

### Background Timers

`app.every()` and `app.daily()` register repeating timer callbacks that run on the event loop thread. Timer callbacks support the full async runtime (`hull.sleep()`, `http.fetch()`, `db.*`).

**Lua:**
```lua
-- Repeating interval (milliseconds, minimum 100ms)
app.every(5000, function()
    session.cleanup()
end)

-- Async operations work inside timers
app.every(30000, function()
    outbox.flush()  -- makes HTTP requests
end)

-- Return false to self-cancel
app.every(1000, function()
    local pending = outbox.flush()
    if pending == 0 then return false end
end)

-- Daily at wall-clock time (UTC by default)
app.daily("02:00", function()
    outbox.cleanup(86400 * 30)
end)

-- Daily at local time
app.daily("02:00", function()
    inbox.cleanup()
end, { localtime = true })
```

**JavaScript:**
```javascript
app.every(5000, () => { session.cleanup(); });

app.every(30000, async () => { await outbox.flush(); });

app.every(1000, () => {
    const pending = outbox.flush();
    if (pending === 0) return false;  // self-cancel
});

app.daily("02:00", () => { outbox.cleanup(86400 * 30); });
app.daily("02:00", () => { inbox.cleanup(); }, { localtime: true });
```

**Constraints:**
- Minimum interval: 100ms (enforced, prevents tight loops)
- No `req`/`res`. These are background tasks, not request handlers
- Errors are logged but don't stop the timer (re-schedules regardless)
- One invocation at a time. If a callback is still running (async yield), the next tick is deferred
- Return `false` to stop the repeating timer
- `app.daily("HH:MM")` defaults to UTC. Pass `{ localtime = true }` for local time

**Implementation:** Timer callbacks fire via Keel's `kl_timer_add` min-heap. Self-re-adding callbacks give repeating behavior. Async operations use "detached" mode. `HlAsyncCtx` with `detached=1` resumes via `hl_async_ctx_resume_detached()` instead of `kl_async_complete()`.

### WASM Compute Plugins

Hull supports compute-only WASM plugins for CPU-intensive pure functions. Plugins have no I/O. They transform input bytes to output bytes inside isolated WASM linear memory with gas-metered execution.

**Directory convention:** Place `.wasm` files in `app_dir/compute/`. Module name = filename without extension (e.g. `compute/score.wasm` → `"score"`).

**Lua API:**
```lua
compute.available()                     -- boolean (WASM runtime initialized?)

-- Synchronous call (gas-limited, blocking)
local output, err = compute.call("score", input_bytes, {
    max_input  = 64 * 1024,    -- 64 KB (optional, has defaults)
    max_output = 64 * 1024,    -- 64 KB
    gas        = 10000000,     -- 10M instructions
    heap       = 256 * 1024,   -- 256 KB WASM heap
})
if err then
    -- err: "not_found", "gas_exhausted", "output_too_small",
    --       "input_too_large", "call_failed", "internal_error"
end

-- Async call (yields to event loop, request handler only)
-- Dispatches WASM execution to thread pool. Other requests served while running.
local r = compute.async.call("score", input_bytes, opts)
-- r.result = output string on success, r.error = error string on failure

-- Preload module into cache
compute.load("score")

-- Create a WasmBuffer from a string (for zero-copy chaining)
local buf = compute.buffer("input data")
```

**JavaScript API:**
```javascript
import { compute } from "hull:compute";

compute.available()                    // boolean

// Sync: Input: string or ArrayBuffer. Output: ArrayBuffer.
const output = compute.call("score", inputBytes, {
    maxInput: 64 * 1024,
    maxOutput: 64 * 1024,
    gas: 10000000,
});

// Async: returns Promise. Dispatches to thread pool.
const buf = await compute.async.call("score", inputBytes, opts);

compute.load("score");

// Create a WasmBuffer from a string (for zero-copy chaining)
const buf = compute.buffer("input data");
```

**Sync vs Async:** Use `compute.call()` for fast/small computations (sub-ms) and in tests/timers. Use `compute.async.call()` in request handlers for expensive computations. It yields to the event loop so other requests are served concurrently. The async variant follows the same pattern as `db.async.query()`.

**Shared data segments**. `compute.segment()` loads named read-only data segments that all instances of a module can read at native speed via WAMR shared heaps:

```lua
-- Lua: load named segments for a module
compute.segment("routing", "graph", graph_bytes)       -- segment 0
compute.segment("routing", "landmarks", fs.mmap("landmarks.bin"))  -- zero-copy
compute.segment("routing", "grid", nil)                -- remove segment
compute.segment("routing", nil)                        -- remove all segments
-- Use normally. Segments auto-attached to every instance
local out = compute.call("routing", query)
```

```javascript
// JS
compute.segment("routing", "graph", graphBytes);
compute.segment("routing", null);                      // remove all
```

WASM plugins query segments via `host_call(0x02, segment_id, sub)`:
- `host_call(0x02, seg_id, 0)` → WASM address of segment (0 if not loaded)
- `host_call(0x02, seg_id, 1)` → size of segment
- `host_call(0x02, -1, 0)` → total segment count

Segments are page-aligned mmap regions in the high end of WASM32 address space. Up to 16 segments per module, 3 GB total. Adding/removing segments drains the instance pool.

**Plugin ABI:** Plugins must export `hull_process(in_ptr, in_len, out_ptr, out_max) -> bytes_written` and optionally `hull_version() -> int`. Single import: `env.host_call(opcode, ptr, len) -> int` (LOG=0x01, DATA_INFO=0x02, CALLBACK=0x10).

**Build & AOT:** `hull build` embeds `compute/*.wasm` files and auto-compiles them to AOT if `wamrc` is available. AOT modules are embedded alongside `.wasm` files; at runtime, AOT is preferred over interpreter.

```bash
make wamrc                         # build AOT compiler (one-time, requires cmake + LLVM)
hull build myapp                   # auto-AOT compiles compute/*.wasm during build
hull build myapp --no-aot          # skip AOT compilation
hull build myapp --target=x86_64   # cross-compile AOT for different arch
```

For cosmocc builds, both x86_64 and aarch64 AOT files are generated automatically.

**Dev mode AOT:** In `hull dev`, place pre-compiled `.aot.<arch>` files next to `.wasm` files in `compute/` and they'll be loaded automatically (four-tier lookup: VFS AOT → VFS WASM → filesystem AOT → filesystem WASM).

**wamrc build:** `make wamrc` builds the WAMR AOT compiler from `vendor/wamr/wamr-compiler`. Requires cmake and LLVM (`brew install llvm` on macOS, `apt install llvm` on Linux). Override LLVM path: `make wamrc WAMRC_CMAKE_FLAGS="-DLLVM_DIR=/path/to/llvm/cmake"`. Output: `build/wamrc`.

**Configuration:** Controlled by `HL_ENABLE_WASM` (default: 1). Disable with `make HL_ENABLE_WASM=0`. WAMR adds ~256 KB to the binary.

**SIMD128:** Enabled (`-DWASM_ENABLE_SIMD=1`). Compile plugins with `-msimd128` (C) or `#[target_feature(enable = "simd128")]` (Rust). AOT maps to native SSE4.1/NEON. Interpreter cannot load v128 modules (graceful error).

**Instance pooling:** Reuses WASM instances across `compute.call()` invocations (pool max 8 per module, heap ≤ 4 MB). Reduces per-call overhead from ~2.5ms to near-zero.

**Persistent instances:** `compute.instance(name, opts?)` creates a long-lived WASM instance that retains linear memory across calls. Not pooled. Exclusively owned until `close()` or GC. Supports sync (`inst:call`/`inst.call`), async (`inst.async:call`/`inst.async.call`), and buffer mode. Gas resets per call; heap/stack are immutable. Use for stateful workloads (ML weights, pre-built indexes) where per-call instantiation cost is too high.

**Memory limits:** Configurable at three tiers. Per-call opts, CLI flags (`--wasm-heap 512M`), and compile-time (`make HL_WASM_MAX_HEAP_MB=512`). Default: 2 MB heap, 1 MB I/O. Max: ~4 GB heap, 256 MB I/O (WASM32) / 16 GB I/O (Memory64).

**Memory64:** Modules compiled with 64-bit memory (`(memory i64 N)`) are detected automatically. Memory64 modules **require AOT compilation**. The fast interpreter does not support Memory64. `hull build` passes `--enable-memory64` to wamrc when it detects a Memory64 module. The `hull_process` ABI changes to `(i64, i64, i64, i64) -> i32` for Memory64 modules; the runtime dispatches the correct calling convention based on the module's `is_memory64` flag.

**Streaming I/O:**
```lua
-- Buffer → buffer
local result = compute.stream("module", input_data, nil, { chunk_size = 65536 })

-- File → file (never fully in memory)
compute.stream("module", { file = "input.csv" }, { file = "output.json" }, { chunk_size = 65536 })

-- Buffer → callback
compute.stream("module", data, function(chunk, index, is_last) end, { chunk_size = 65536 })
```

- Input: string, WasmBuffer, MappedBuffer, or `{ file = "path" }`
- Output: nil (return buffer), `{ file = "path" }`, or callback function
- Uses persistent instance internally. State preserved between chunks
- Modules can query chunk metadata via `host_call(0x03)`: `hull_stream_is_first()`, `hull_stream_is_last()`, `hull_stream_chunk_index()`

**Architecture:** See `docs/wamr_architecture.md` for the full design document.

### GPU Compute (wgpu-native)

Hull supports GPU compute shaders via wgpu-native v27 (Vulkan/Metal/DX12). Disabled by default. Enable with `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`.

**Manifest declaration:** Apps must declare `gpu: true` in their manifest to access the `gpu` global. Apps without a manifest get GPU access by default (backward compat).

```lua
app.manifest({ gpu = true })
```

**Lua API:**
```lua
gpu.available()                        -- boolean
gpu.devices()                          -- { {id=0, name="Apple M1"}, ... }
gpu.compile(name, wgsl)                -- compile WGSL shader (cached, idempotent)
gpu.load(name)                         -- load + compile shaders/<name>.wgsl from disk/VFS
gpu.dispatch(name, opts)               -- run shader, return output (string)
gpu.dispatch(name, { output = false }) -- fire-and-forget (no readback, returns true)
gpu.pipeline(stages, opts)             -- multi-stage dispatch, single submission
gpu.pipeline(stages, { output = false }) -- fire-and-forget pipeline
gpu.async.dispatch(name, opts)         -- async dispatch (yields to event loop)
gpu.async.pipeline(stages, opts)       -- async pipeline
gpu.buffer(name, data)                 -- create/write persistent buffer (string or MappedBuffer)
gpu.buffer(name, nil)                  -- destroy buffer
gpu.buffer_read(name)                  -- read buffer back to host
gpu.buffer_copy(src, dst, opts?)       -- GPU-side buffer copy (no CPU roundtrip)
```

**JavaScript API:**
```javascript
import { gpu } from "hull:gpu";
gpu.available()                        // boolean
gpu.devices()                          // [{id, name}, ...]
gpu.compile(name, wgsl)                // compile WGSL shader
gpu.load(name)                         // load + compile shaders/<name>.wgsl
gpu.dispatch(name, opts)               // ArrayBuffer output
gpu.dispatch(name, { output: false })  // fire-and-forget (returns true)
gpu.pipeline(stages, opts)             // multi-stage, ArrayBuffer or Array<ArrayBuffer>
gpu.pipeline(stages, { output: false })// fire-and-forget pipeline
gpu.async.dispatch(name, opts)         // Promise<ArrayBuffer>
gpu.async.pipeline(stages, opts)       // Promise
gpu.buffer(name, data)                 // create/write (ArrayBuffer, MappedBuffer, or string)
gpu.buffer(name, null)                 // destroy
gpu.bufferRead(name)                   // ArrayBuffer
gpu.bufferCopy(src, dst, opts?)        // GPU-side buffer copy
```

**Dispatch options:**
```lua
gpu.dispatch("shader_name", {
    uniforms = packed_binary,          -- binding 0 (16-byte aligned)
    buffers = {
        { data = bytes, usage = "read" },       -- binding 1
        { name = "persistent", usage = "read" }, -- binding 2 (named buffer)
        { size = N, usage = "readwrite" },       -- binding 3 (output)
    },
    textures = {                               -- optional texture bindings
        { name = "input" },                    -- sampled (paired: texture + sampler)
        { name = "output", storage = true },   -- storage texture (single binding)
    },
    workgroups = { x = 64, y = 1, z = 1 },
    output = 3,                        -- 1-indexed buffer to read back (Lua)
    output_texture = 2,                -- 1-indexed texture to read back as HlImage (Lua)
    device = -1,                       -- -1 = default device
})
```

**Pipeline (multi-stage dispatch):**
```lua
-- Single command buffer submission, single poll, single readback.
-- Named buffers are shared across stages (max declared size allocated).
local out = gpu.pipeline({
    { shader = "normalize", buffers = {{ name = "data", data = input }}, workgroups = {x=64} },
    { shader = "score",     buffers = {{ name = "data" }, { name = "results", size = N*4 }},
                            uniforms = params, workgroups = {x=64} },
    { shader = "top_k",     buffers = {{ name = "results" }}, workgroups = {x=1} },
}, {
    outputs = { { stage = 3, buffer = 1 } },  -- 1-indexed (Lua)
    device = -1,
})
-- Single output: returns string. Multiple outputs: returns table of strings.
```
```javascript
// JS: 0-indexed outputs
const out = gpu.pipeline([
    { shader: "normalize", buffers: [{ name: "data", data: buf }], workgroups: {x:64} },
    { shader: "score",     buffers: [{ name: "data" }, { name: "results", size: N*4 }],
                           uniforms: paramsBuf, workgroups: {x:64} },
    { shader: "top_k",     buffers: [{ name: "results" }], workgroups: {x:1} },
], { outputs: [{ stage: 2, buffer: 0 }] });
// Single output: ArrayBuffer. Multiple outputs: Array<ArrayBuffer>.
```

**Fire-and-forget dispatch:** Set `output = false` to skip readback. The shader executes and persistent buffers are updated in-place, but no data is returned to the host. Returns `true` on success. Works with both `gpu.dispatch()` and `gpu.pipeline()`.

```lua
-- Update embeddings in-place on GPU (no readback)
gpu.dispatch("normalize", {
    buffers = {{ name = "embeddings" }},
    workgroups = { x = 1024 },
    output = false,
})
-- Pipeline fire-and-forget: double → triple in-place
gpu.pipeline({
    { shader = "double", buffers = {{ name = "data" }}, workgroups = {x=64} },
    { shader = "triple", buffers = {{ name = "data" }}, workgroups = {x=64} },
}, { output = false })
```

**GPU-side buffer copy:** Copy between persistent GPU buffers without CPU roundtrip.

```lua
gpu.buffer_copy("source", "dest")                           -- full copy
gpu.buffer_copy("source", "dest", { size = 1024 })          -- partial
gpu.buffer_copy("source", "dest", {
    src_offset = 0, dst_offset = 512, size = 256,           -- with offsets
})
```

**GPU Textures:**
- `gpu.texture(name, img)`. Create persistent texture from HlImage.
- `gpu.texture(name, data, opts)`. Create from raw bytes with `opts.width`, `opts.height`, `opts.format`, `opts.storage`.
- `gpu.texture(name, nil)`. Destroy persistent texture.
- `gpu.texture_read(name)` → HlImage. Read back texture pixels.
- Dispatch with textures:
  ```lua
  gpu.dispatch("shader", {
      textures = {
          { name = "input" },                        -- sampled (binding N, N+1)
          { name = "output", storage = true },       -- storage (binding M)
      },
      output_texture = 2,                            -- readback as HlImage
  })
  ```
- Sampled textures get paired bindings (texture view + sampler). Storage textures get single binding.
- Binding convention: uniforms → buffers → sampled textures → storage textures (all `@group(0)`).

**Shader loading from files:** `gpu.load(name)` reads `shaders/<name>.wgsl` from disk (dev mode) or VFS (built binaries) and compiles it. Enables shader iteration without modifying app code.

```lua
-- shaders/score.wgsl on disk (dev mode) or embedded (built binary)
gpu.load("score")                     -- reads + compiles shaders/score.wgsl
-- equivalent to: gpu.compile("score", <file contents>)
```

**Shader embedding in builds:** `hull build` and `make APP_DIR=` automatically discover and embed `shaders/*.wgsl` files into the binary via the VFS, just like `templates/`, `static/`, `compute/`, and `migrations/`. `gpu.load()` checks VFS first, then falls back to disk. So shaders work identically in dev mode and built binaries.

**Directory convention:**
```
myapp/
  app.lua
  shaders/             ← WGSL compute shaders (gpu.load)
    normalize.wgsl
    score.wgsl
  compute/             ← WASM plugins (compute.call)
    echo.wasm
  templates/           ← HTML templates
  static/              ← Static assets
  migrations/          ← SQL migrations
```

**Buffer sharing in pipelines:** Named buffers are created once and reused across stages. When multiple stages reference the same buffer name with different sizes, the maximum declared size is allocated. First stage with `data` uploads initial content; subsequent stages reuse the existing buffer. Persistent buffers (created via `gpu.buffer()`) participate by name.

**Memory-mapped file input (fs.mmap → GPU):**
```lua
-- Zero-copy: disk → mmap → GPU buffer (no Lua string intermediary)
app.manifest({ gpu = true, fs = { read = {"embeddings.bin"} } })

local mapped = fs.mmap("embeddings.bin")  -- mmap'd pointer
gpu.buffer("vectors", mapped)              -- pointer → wgpuQueueWriteBuffer directly
mapped:close()

-- Also works inline in dispatch/pipeline buffers:
local out = gpu.dispatch("search", {
    buffers = {{ data = mapped, size = mapped:len() }},
    workgroups = { x = 1024 },
    output = 1,
})
```

`gpu.buffer()`, `gpu.dispatch()`, and `gpu.pipeline()` all accept `MappedBuffer` (from `fs.mmap()`) as buffer data in both Lua and JS. This avoids copying large datasets through the scripting runtime.

**Binding layout:** Uniforms at binding 0 (if present), storage buffers at binding 1..N. WGSL shader `@binding()` annotations must match this auto-layout.

**Async dispatch:** `gpu.async.dispatch()` and `gpu.async.pipeline()` submit GPU work to the thread pool and yield to the event loop (Lua coroutine / JS Promise). Other requests are served while the GPU is working. Deep-copies all buffer data for thread safety.

**Sandbox:** When `manifest.gpu` is set:
- macOS: allows `iokit-open` and `com.apple.MTLCompilerService` mach-lookup
- Linux: unveils `/dev/dri` (rw) and `/proc/self` (r)

**Performance (Apple M1 Max, cosine similarity on 128-dim vectors):**

| Vectors | Native C | WASM AOT | GPU | GPU vs AOT |
|---------|----------|----------|-----|------------|
| 64 | 7 µs | 7 µs | 2,630 µs | 0.0x |
| 1K | 118 µs | 108 µs | 2,630 µs | 0.0x |
| 16K | 1,830 µs | 2,534 µs | 2,629 µs | 1.0x |
| 64K | 7,270 µs | 10,969 µs | 2,653 µs | **4.1x** |

GPU latency is constant ~2.6ms (dominated by submit+poll overhead). Crossover vs AOT at ~16K vectors. Use GPU for large parallel workloads; use WASM AOT for small sequential ones.

**GPU timeout:** Dispatches time out after 5 seconds (configurable via `HL_GPU_TIMEOUT_MS` at compile time). Returns `HL_GPU_ERR_TIMEOUT`. Prevents infinite hangs from shader bugs. Applies to `dispatch`, `pipeline`, and `buffer_copy`.

**Build:** `make fetch-wgpu` downloads and SHA-256 verifies wgpu-native. Then `make HL_ENABLE_GPU=1` auto-detects `vendor/wgpu/`. macOS links Metal + QuartzCore + CoreGraphics + Foundation; Linux links `-lvulkan`. Not compatible with Cosmopolitan builds.

### Unified Buffer Protocol

All compute and GPU functions accept any buffer type as input via the unified buffer protocol (`HlBufferView` in `include/hull/buffer.h`):

| Type | Source | Lua | JS |
|------|--------|-----|-----|
| String | Literals, `string.pack` | Default | N/A |
| ArrayBuffer | JS typed arrays | N/A | Default |
| MappedBuffer | `fs.mmap(path)` | Userdata | Object |
| WasmBuffer | `compute.call(name, input, { buffer = true })` | Userdata | Object |

All four types are accepted by `compute.call()`, `compute.segment()`, `gpu.buffer()`, `gpu.dispatch()` buffer data, and `gpu.pipeline()` buffer data. This enables zero-copy data flow:

```lua
-- Disk → GPU (zero-copy via mmap)
local mapped = fs.mmap("embeddings.bin")
gpu.buffer("vectors", mapped)
mapped:close()

-- WASM → GPU (zero-copy via WasmBuffer)
local processed = compute.call("preprocess", raw_data, { buffer = true })
gpu.buffer("features", processed)  -- WasmBuffer accepted directly
```

C helper functions:
- **Lua:** `lua_get_buffer(L, idx, &view)`. Extracts `HlBufferView` from any buffer type at stack index
- **JS:** `js_get_buffer(ctx, val, &view, &str, &needs_free)`. Same for JS values

### Compute API Harmonization

WASM and GPU compute share symmetric naming where the concepts align:

| Concept | WASM (`compute.*`) | GPU (`gpu.*`) |
|---------|-------------------|---------------|
| Availability | `compute.available()` | `gpu.available()` |
| Load from file | `compute.load(name)` | `gpu.load(name)` |
| Execute | `compute.call(name, input)` | `gpu.dispatch(name, opts)` |
| Async execute | `compute.async.call(...)` | `gpu.async.dispatch(...)` |
| Persistent state | `compute.instance(name)` | `gpu.buffer(name, data)` |
| Shared data | `compute.segment(mod, seg, data)` | `gpu.buffer(name, data)` |
| Input types | string, WasmBuffer, MappedBuffer | string, WasmBuffer, MappedBuffer |
| Execution limits | Gas metering (per-instruction) | Timeout (5s wall clock) |

Intentionally different: `call` vs `dispatch` (function call vs hardware dispatch), `instance` vs `buffer` (retained linear memory vs GPU storage), `segment` vs `buffer` (read-only shared heap vs read/write GPU buffer).

## Terminal UI module

Hull ships a built-in `hull.tui` module for interactive terminal apps. The design lives in [docs/tui_mode.md](docs/tui_mode.md); the short version:

- **One canonical entry point**: `tui.run({ draw, on_event, tick_ms })`. Raw primitives (`tui.move`, `tui.print`, `tui.poll`, …) are exposed but the immediate-mode loop is what apps lead with.
- **CLI mode only**: TUI requires `app.main`. Server apps (`app.get/post/...`) cannot also call `tui.run`. Same rationale as CLI mode itself.
- **Manifest gate**: `app.manifest({ tui = true, modules = { "hull/tui@1" } })`. The resolver enforces both. The build flag at compile time, the manifest field at app-load time.
- **Per-process singleton**: one `HlTuiCtx` per process (the controlling tty is singleton). Second `acquire` returns `-EBUSY`.
- **Cell-diff rendering**: shadow + pending buffers in the cap layer; only changed cells are emitted on flush. Flicker-free over ssh / mosh without app-side work. Unicode width comes from an embedded data table at `vendor/unicode/eaw.h`. Identical behavior across glibc / musl / cosmo / macOS. Refresh via `make fetch-unicode`.
- **Async-integrated `tui.poll`**: yields to the runtime's event loop while waiting for input. Background `tui.async` coroutines / Promises keep ticking, so an app can `http.fetch` or `db.async.query` while the main coroutine awaits a keystroke.
- **Lone-ESC commit**: bare `\x1b` is committed as a synthetic `"escape"` event after a 50 ms quiet window. Resolves the classic "ESC vs. start of CSI" ambiguity without making the user wait for a follow-up byte.

### API surface

```lua
local tui = require("hull.tui")

-- Canonical entry point.
tui.run({
    draw     = function(t) ... end,        -- required, called every tick + on event
    on_event = function(ev) return nil end,-- nil = keep going; non-nil = exit token
    tick_ms  = -1,                         -- -1 = block until event; 100 = 10Hz
    mouse    = false,                      -- opt-in SGR mouse (CSI <btn>;<x>;<y> M)
    paste    = false,                      -- opt-in bracketed paste
    focus    = false,                      -- opt-in focus in/out events
    kitty_kbd= false,                      -- opt-in Kitty keyboard protocol
})

-- Helpers.
tui.list(items, opts?)        -- scrollable picker; returns picked index or nil
tui.confirm(msg)              -- y/N prompt; returns boolean
tui.input(prompt, opts?)      -- single-line editor with cursor + editing keys
tui.frame(opts, fn)           -- bordered area; opts.border ∈ single/double/round/ascii
tui.progress(pct, opts?)      -- "[████░░] 67%"
tui.spinner(state)            -- ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏. Returns frame + next state
tui.theme()                   -- "dark" | "light" | "unknown" (cached at acquire)
tui.caps()                    -- { truecolor, color256, color, mouse, focus, kitty_kbd, clipboard }
tui.clipboard_set(text)       -- OSC 52 write to system clipboard
tui.async(fn)                 -- spawn a detached coroutine on the event loop

-- Escape-hatch primitives (use tui.run instead).
tui.enter() / tui.leave()
tui.size() / tui.theme() / tui.caps()
tui.clear() / tui.invalidate() / tui.move(x, y) / tui.style(opts)
tui.print(x, y, s) / tui.write(s) / tui.flush()
tui.poll(timeout_ms)          -- yields to event loop; returns event or nil
```

JS API is the same shape (`import { tui } from "hull:tui"`) in camelCase: `tui.enableMouse`, `tui.clipboardSet`, `tui.poll` returns a `Promise<event|null>`.

### First-party `--tui` tools

These ship as Lua tool modules under `stdlib/cli/lua/hull/`; the C dispatchers in `src/hull/commands/` accept a `--tui` (or `--interactive`) flag and delegate via `hull_tool`. Each refuses cleanly when stdin/stdout isn't a real terminal.

| Command | What | Module |
|---------|------|--------|
| `hull doctor --tui` | Live readiness check w/ ✓/✗ glyphs, sections for platform/compilers/subsystems/compute/CA-bundle, summary. `r` reprobes, `c` copies JSON via OSC 52, `q` quits. | `hull/doctor_tui.lua` |
| `hull dev --tui` | Live request log streamed from child's stderr/stdout into a ring buffer, status line (pid, reloads, lines, app_dir), inline filter prompt, file-watch auto-reload, manual `r` reload. | `hull/dev_tui.lua` (+ `src/hull/dev_state.h`) |
| `hull agent context --interactive` | Two-pane task picker w/ live preview; ←/→ cycles level (minimal/compact/full); Enter prints chosen context as JSON to stdout for shell pipelines. | `hull/agent_context_tui.lua` |
| `hull agent errors --tui` | Scrollable error list + detail panel. Normalizes varied error shapes. Empty-state shows clean "✓ No errors". | `hull/agent_errors_tui.lua` |
| `hull modules available --tui` | Two-pane searchable registry; `/` opens filter prompt; right pane shows caps + deps + manifest snippet. | `hull/modules_available_tui.lua` |

### Architecture conventions for `--tui` dogfood

The pattern, verified by all five commands above:

1. C command parses `--tui`, checks `isatty()`, delegates to `hull_tool("hull.X_tui", argc, argv, env->hull_exe)`. Non-tty path prints a helpful message + exits non-zero.
2. The Lua tool module accesses data via `tool.*` accessors (registered in `src/hull/runtime/lua/mod_tool.c`). Examples: `tool.doctor_json()`, `tool.agent_context(task, level)`, `tool.dev_drain()`, `tool.modules_available()`.
3. Heavy data goes through JSON strings parsed with `hull.json.decode`. Lighter data (registry walks) goes through Lua tables directly. No data is duplicated between the JSON path and the TUI path. Both call the same C helpers.

### Testing

- **Cap layer**: 72 tests across `test_tui_width.c` (Unicode width, UTF-8 decode), `test_tui_parser.c` (CSI/SS3/OSC parser, mouse, paste, focus, flush_idle), `test_tui_lifecycle.c` (PTY-driven acquire/release/render/termios). Skips gracefully on platforms without `forkpty`.
- **Resolver**: 3 tests for the build/manifest gate (`hull/tui@1` admitted, rejected without `tui = true`, rejected without `HL_ENABLE_TUI`).
- **E2E**: `tests/e2e_tui.sh` + the PTY harness `tests/e2e_tui_drive.c` cover 30+ cases including all five dogfood tools, the async-yield proof, and ENOTTY refusals. Drive script supports `%d`/`%u`/`%r`/`%e`/`%q`/`%sN` and literal bytes; the e2e helper builds on every `HL_ENABLE_TUI=1` build via `make e2e-tui`.

### Adding a new `--tui` command

1. Expose any in-process data the TUI needs by adding a `tool.X()` binding in `src/hull/runtime/lua/mod_tool.c` (either returning a Lua table directly or calling `open_memstream` + a JSON writer for heavier payloads).
2. Add a `--tui` flag to the C dispatcher in `src/hull/commands/*.c`. Check `isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)`; refuse cleanly otherwise.
3. Write `stdlib/cli/lua/hull/X_tui.lua`. `require "hull.tui"` + a `tui.run` loop calling your new `tool.X()`.
4. Add an e2e case in `tests/e2e_tui.sh` using the PTY driver (e.g. `"%q"` to send 'q' immediately after the first frame).

## Testing

Tests use Sheredom's utest.h. Each `tests/hull/*/test_*.c` is a standalone executable.

```bash
make test                           # run all unit tests
make debug && make test             # run under ASan + UBSan
make e2e                            # run all E2E tests (examples + build + sandbox)
./build/test_hull_cap_db            # run a single test suite
```

### Test Suites

| Suite | Tests | What it covers |
|-------|------:|----------------|
| `test_db` | 22 | SQLite query, exec, params, null, error handling |
| `test_db_backend` | 31 | `HlDbBackend` vtable: open, query, exec, transactions, native-handle tag, identifier quoting |
| `test_time` | 8 | Timestamps, date formatting, buffer bounds |
| `test_env` | 7 | Allowlist enforcement, null safety |
| `test_crypto` | 31 | SHA-256, random, PBKDF2, Ed25519, NaCl box/secretbox, null safety |
| `test_fs` | 19 | Path validation, read/write, traversal rejection |
| `test_http` | 5 | Host allowlist, URL parsing |
| `test_smtp` | 37 | SMTP client send/auth/TLS |
| `test_smtp_e2e` | 8 | End-to-end SMTP over real socket |
| `test_image` | 15 | Decode/encode/raw pixels |
| `test_audit` | 3 | Audit log JSON output |
| `test_body` | 3 | Body reader limits + types |
| `test_ws` | 7 | WebSocket frame parse |
| `test_wasm` | 55 | WAMR init/destroy, module load, echo call, gas exhaustion, limits, pools, persistent instances, shared data segments |
| `test_wasm_buffer` | 12 | WasmBuffer protocol + zero-copy paths |
| `test_gpu` | 1 | GPU init/destroy (real GPU tests skip if no adapter) |
| `test_tool` | 52 | Tool-spawn allowlist, dangerous-flag validation |
| `test_js` | 81 | QuickJS init, eval, sandbox, modules, GC, async, host bindings |
| `test_lua` | 99 | Lua init, eval, sandbox, modules, GC, async, host bindings |
| `test_static` | 23 | MIME detection, path traversal, embedded VFS lookup, stdlib platform-VFS fallback (app shadows stdlib, miss returns 0, NULL stdlib_vfs is safe), HEAD-on-GET-middleware acceptance (regression: Keel routes HEAD→GET middleware; pre-fix our middleware strict-rejected non-GET and dropped HEAD to 404) |
| `test_vfs` | 19 | Binary search find, prefix queries, path construction, empty VFS |
| `test_signature` | 20 | Ed25519 sign/verify round-trips, dual-layer signature |
| `test_release` | 20 | Ed25519 release-manifest sign/verify, tamper detect, hex edge cases, secret-key file IO |
| `test_parse_size` | 9 | Size string parser ("1m", "1g", etc.) |
| `test_compiler` | 16 | Compiler vtable: system backend, allowlist, end-to-end on Linux |
| `test_cacert` | 6 | Embedded Mozilla CA bundle: presence, NUL-termination, mbedTLS parse |
| `test_dispatch` | 4 | Command dispatch table, unknown-command handling |
| `test_csp` | 7 | CSP preset registry: `htmx` preset expansion, literal passthrough, NULL passthrough, reverse name lookup |
| `test_db_select` | 5 | DSN-scheme backend routing (sqlite / postgres / mysql / reserved / unknown) |
| `test_db_registry` | 7 | Named-connection registry: lazy open, cache, `$VAR` env-ref DSN, seeded default |
| `test_db_dynamic` | 5 | `db.open` validation: policy gate, scheme/host allowlist, fs gate, concurrent cap |
| `test_pgwire` / `test_pg_conn` | 40+ | PostgreSQL v3 codec (framing/cursor) + DSN parse + handshake/SCRAM over a socketpair |
| `test_mysqlwire` | 19 | MySQL/MariaDB codec (LE framing, lenenc, OK/ERR parse, untrusted-input safety, SSLRequest build, native + caching_sha2 scramble) + DSN parse |
| `test_mysql_conn` | 10 | MySQL handshake/auth/query over a socketpair: native + caching_sha2 fast-auth, COM_QUERY + prepared (binary) result decode, DATETIME decode, trailing-nil param padding, multi-statement exec, ed25519-unsupported |
| `test_host_match` | 7 | Host-allowlist matcher (exact / `*` / `*.suffix` glob / CIDR) shared by db/http/smtp |

~58 suites, ~1280 test cases total (this table is representative, not exhaustive).
Plus libFuzzer harnesses (sh_json, path_normalize, mime_sniff, host_match, pgwire,
pg_dsn, pg_rewrite, mysqlwire, mysql_dsn) run 60s each in CI.

\+ E2E suites (`e2e_build.sh`, `e2e_examples.sh`, `e2e_http.sh`, `e2e_sandbox.sh`, `e2e_install.sh`, `e2e_ca_bundle.sh`, `e2e_update.sh`)

### E2E Tests

| Script | What it tests |
|--------|---------------|
| `e2e_build.sh` | Build pipeline: platform build, app compilation, signing, self-build chain |
| `e2e_examples.sh` | All 9 examples in both Lua and JS runtimes |
| `e2e_http.sh` | HTTP routing, middleware, error handling |
| `e2e_sandbox.sh` | Kernel sandbox enforcement (OpenBSD + Linux + macOS + Cosmo) |
| `e2e_templates.sh` | Template engine: 20 tests per runtime (text, vars, escaping, conditionals, loops, filters, inheritance, includes, XSS) |
| `e2e_migrate.sh` | Migration system: apply, status, idempotency, embedding |
| `e2e_compute.sh` | WASM compute: compute.call() from Lua + JS, preload, error handling |
| `e2e_deploy.sh` | Deploy config generator: Dockerfile, systemd, fly.toml, agent deploy |
| `e2e_install.sh` | `install.sh` dry-run across platform/flavor/prefix; shell-completion syntax + behavior |
| `e2e_ca_bundle.sh` | Doctor output; real HTTPS handshake to `example.com` via embedded CA bundle (sandbox-active) |
| `e2e_update.sh` | `hull update --check` against real public repo; full GitHub-API + JSON parse + version compare via embedded CA bundle |
| `e2e_auth_flows.sh` | Auth flows: register → verify → login → logout → magic-link → password-reset → email-change, with replay/tamper assertions, against `tests/fixtures/auth_flows_{lua,js}` (24 assertions per runtime) |
| `e2e_auth_flows_2fa.sh` | Auth flows + TOTP composition: enroll → confirm → login (returns `pending_2fa` + `totp_token`) → wrong-code retry → right-code completes → totp_token single-use-on-success → recovery code path → magic-link with 2FA rendering default form, against `tests/fixtures/auth_flows_2fa_{lua,js}` (15 assertions per runtime) |
| `e2e_auth_flows_hardening.sh` | Auth flows hardening: re-send verify (incl. enumeration-safe silence post-verify) → account lockout after N failed logins (429 + `Retry-After`; correct password during window still locked; auto-clears after window) → email-change notify+revoke (revoke link aborts pending change; subsequent confirm fails 400) → pwned-password check via a localhost HIBP mock (rejects "password", accepts random). 20 assertions per runtime; fixtures at `tests/fixtures/auth_flows_hardening_{lua,js}` |
| `e2e_sign_in_events.sh` | Sign-in events + device management: login from "browser A" emits one new-device alert; login from "browser B" (different UA + IP) emits a second; re-login from A doesn't re-fire; email-change recorded as `email_changed`; `session.list_for_user` + `audit_log.list_devices` surface 2 devices; `destroy_others` kills B leaves A; password reset cascade kills A via `on_password_reset`. 20 assertions per runtime; fixtures at `tests/fixtures/sign_in_events_{lua,js}` |
| `e2e_htmx_playwright.sh` | Browser-side E2E for `examples/htmx_widgets_register` (every §1.5.g widget) + `examples/hypermedia_photos` (Lua AND JS runtimes) via headless Chromium driven by Playwright. Catches things curl can't: CSS actually applies, htmx swaps fire, widget JS runs under `csp = "htmx"`, confirm dialog opens only for `hx-confirm` elements, sort widget Enter/Space keyboard activation, full CRUD round-trip with CSRF+session, and `@axe-core/playwright` WCAG scan (FAILs on `critical`/`serious`, logs `moderate`/`minor`). Runs in two MODE-controlled flavors against an identical 31-assertion suite: **dev** (`make e2e-htmx-playwright`) launches `hull <app.lua>` so files come off disk; **build** (`make e2e-htmx-playwright-build`) runs `hull build` on each example first then launches the standalone binary, exercising the embedded-VFS code path. On failure: writes playwright traces + final-page screenshots to `build/playwright-artifacts/` and CI uploads via `actions/upload-artifact@v4`. Skips cleanly when node/npm absent; first run downloads ~150 MB to `tests/.playwright/` (gitignored, cached in CI). |

## Runtime Sandboxes

### QuickJS Sandbox
1. `eval()` removed (C-level `JS_Eval` still works for host code)
2. `std`/`os` modules NOT loaded
3. Memory limit via `JS_SetMemoryLimit()` (64 MB default)
4. Stack limit via `JS_SetMaxStackSize()` (1 MB default)
5. Instruction-count interrupt handler for gas metering
6. Only `hull:*` modules available

### Lua Sandbox
1. `io`/`os` libraries NOT loaded
2. `loadfile`, `dofile`, `load` globals removed
3. Memory limit via custom allocator with tracking (64 MB default)
4. Instruction-count hook for gas metering (`lua_sethook(LUA_MASKCOUNT)`, 100M default)
5. Only safe libs: base, table, string, math, utf8, coroutine
6. Custom `require()` resolves only from embedded stdlib registry
7. `hull.*` modules registered as globals

## Adding a New Capability Module

### 1. C Capability Layer
- Create `src/hull/cap/<name>.c` and `include/hull/cap/<name>.h`
- Implement `hl_cap_<name>_*()` functions with input validation
- Add to Makefile `HULL_CAP_SRC` and `HULL_CAP_OBJ`

### 2. Lua Bindings
- Add bindings in `src/hull/runtime/lua/modules.c`
- `luaL_Reg` array + `luaopen_hull_<name>()` opener
- Register in `hl_lua_register_modules()`

### 3. JavaScript Bindings
- Add bindings in `src/hull/runtime/js/modules.c`
- Init function + register in `hl_js_register_modules()`

### 4. Tests
- Unit tests in `tests/hull/cap/test_<name>.c`
- Add to Makefile test discovery

## Adding a New Subcommand

1. Create `src/hull/commands/<name>.c` and `include/hull/commands/<name>.h`
2. Implement `int hl_cmd_<name>(int argc, char **argv, const char *hull_path)`
3. Add one line to the command table in `src/hull/commands/dispatch.c`
4. Add Lua implementation in `stdlib/cli/lua/hull/<name>.lua` if tool-mode command (CLI plugin); user-facing modules live in `stdlib/lua/hull/`

## Debugging

```bash
make debug              # clean + rebuild with -fsanitize=address,undefined -g -O0
make msan               # clean + rebuild with -fsanitize=memory,undefined (Linux clang)
make test               # run tests under whichever sanitizer was built
```

ASan catches: heap/stack buffer overflow, use-after-free, double-free, memory leaks.
UBSan catches: signed overflow, null dereference, misaligned access, shift overflow.
MSan catches: use of uninitialized memory.

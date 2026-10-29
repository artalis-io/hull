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

Resolution order at startup (`hl_ca_trust_resolve` in `src/hull/ca_trust.c`, shared by the server and `app.main` entry points):
1. `--no-ca-bundle` (or `--skip-ca-bundle` deprecated alias) → no verification (dev only, MITM-vulnerable)
2. `--ca-bundle PATH` (or `--ca-bundle=PATH`) → load that file
3. System CA store at `/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`
4. Embedded Mozilla bundle (via `hl_embedded_ca_bundle()` in `src/hull/cacert.c`)
5. Fail with a clear hint

The same anchor reaches every outbound TLS client, not only `http.fetch` and
the SSH tunnel: the resolver publishes it (`hl_ca_bundle_set_active`), and the
clients that build a TLS context per connection - the Postgres / MySQL / Valkey
wire backends (`shared/tls_client.c`) and the async SMTP workers - read it back
with `hl_ca_bundle_active`. It is resolved whenever the app may dial out (http
`hosts`, `ssh.tunnel`, or a network database / KV connection, the same test the
sandbox's `network_outbound` uses), and file anchors are read into memory at
startup, before the sandbox narrows file access - so the sandbox grants no CA
path at all (audit 29: the `(embedded)` label was unveiled as a relative path,
and a planted `(embedded) -> /` symlink widened the grant). A `--ca-bundle` that fails to
load leaves NO anchor, so those clients fail closed rather than falling back to
the embedded bundle (with one ERROR saying so). `--no-ca-bundle` publishes
nothing: it turns verification off for `http.fetch`, the SSH tunnel and
synchronous SMTP only. The clients that read the published anchor - a DSN
asking for `sslmode=verify-full`, and the async SMTP workers - still verify,
against the embedded bundle. `shared/tls_client.c` fails a verifying handshake
whose server name could not be set (it would check the chain but not the name;
audit 9), as Keel's own HTTP client does. A context that verifies nothing is
built only by name - `hl_tls_client_ctx_create_insecure()`, called by the
`--no-ca-bundle` branch of `hl_ca_trust_resolve` - and
A verified handshake with no server name is refused (audit 29).
`hl_tls_client_ctx_create(NULL, ...)` fails closed (returns NULL), as Keel's
constructor does since 3.3.0 (audit 11).

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
net DSN - in both serve.c and the `app.main` runner, via
`hl_sandbox_policy_add_default_dsn`; audit 25), so a sandboxed DB-only app can
reach its database.

**Install (end users):** `hull feature install <name>` / `hull feature list` /
`hull feature uninstall <name>` fetch + Ed25519-verify + cache the signed lib to
`~/.hull/feature/`, via the shared `hl_release_io_fetch_verified_manifest` - the
**same trust chain** as `hull flavor install` / `hull tools install` /
`hull update`, no new keys. The registry is the `FEATURES[]` table in
`src/hull/commands/feature.c` (the single registration point: one row per
feature + which native platforms publish it). The verified manifest must also
NAME the release asked for (its signed `hull.version` entry,
`hl_release_io_check_release_tag`, which the shared fetch runs for every caller
- before audit 9 only `hull update` checked, so an older release's signed
manifest + assets served under the current tag installed); the install caches
`hull.version` beside the manifest (`<asset>.version`), and the `hull build`
re-verify (`tool.platform_verify` → `hl_release_io_verify_local_asset_release`)
requires the cached manifest to name THIS hull's release - so a cache-sourced
feature works only with a release-build hull (a source build composes from its
own `make feature-<name>` archive), and a feature installed before audit 9 is
reinstalled once.

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

A new capability reaches an app through exactly one of five shipping units.
Classifying it correctly is the difference between a signed static archive and
twenty lines of Lua. The five, and the single question that separates each:

| Unit | What it is | Ships as | Reached via | Axis |
|------|-----------|----------|-------------|------|
| **stdlib** | pure Lua/JS built on capabilities the base already has (no new C, no new authority) | always in the base VFS | `require("hull.X")` / `import "hull:X"` + `manifest.modules` | orchestration |
| **base cap module** | a SMALL always-in-base C capability (a codec / sniffer / store) built on stdio + the caps already shipped, with no vendored engine and no off switch | compiled into the base (`CAP_OBJS`; NO `HL_ENABLE_*` gate + NO `--with` archive) | `require`/`import` + `manifest.modules` (like stdlib, but C-backed) | in-base |
| **feature** | a large optional C subsystem, off by default, adding a new vendored engine, wire backend, or authority | signed `libhull_feature-<name>-<arch>.a` | `hull build --with=<name>` (auto-inferred or forced) | **additive** |
| **flavor** | a build.lua **preset** validating the app against a slimmer cap set (since Phase 4.3 - the base already composes; pre-built per-flavor libs are gone) | (none - a preset on the default base) | `hull build --flavor=<name>` | **subtractive** |
| **tool** | a separate companion **program** Hull spawns (never linked in) | `hull-<tool>-<platform>` binary, OR a `.tar` **bundle** for a multi-file tool (`hull-<name>.tar` / `hull-<name>-<platform>.tar` extracting to a dir) | `hl_tool_spawn` at build time; `hull tools install` | is-it-Hull-or-a-program-Hull-runs |

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
3. **Does it need a LITTLE new C (a codec, a sniffer, a store) but no vendored
   engine, no new OS/hardware authority, and no reason to be optional** (it's
   small and every app might want it)? → **base cap module**. Add
   `src/hull/cap/<name>.c` (it rides `CAP_OBJS` in the base automatically - the
   Makefile globs `cap/*.c` and only *subtracts* the reducible ones) + the two
   runtime bindings + one `HlModuleSpec` row. No `HL_ENABLE_*` gate, no `--with`
   archive, no weak seam. Examples: `mime` (content sniffer), `blob` (cache
   store), `tar` (archive codec). `image` is the boundary case: it is a base cap
   module by nature but is ALSO auto-composed for size (see T4b / "Composable
   runtime + HTTP base"), so it carries both an `HL_ENABLE_IMAGE` knob and a weak
   seam - do NOT copy that dual role for a plain codec.
4. **Does it add a large optional C subsystem or a new authority, off by
   default?** (a vendored engine, a pure-C wire protocol, a new hardware/OS
   surface) → **feature**. It fills a base-resident **weak hook** with a
   **strong override** from the composed archive; register one row in
   `FEATURES[]` (`src/hull/commands/feature.c`) + one `FEATURE_SPECS` entry
   (`stdlib/cli/lua/hull/build.lua`). Examples: `duckdb`, `postgres`, `mysql`,
   `gpu`, `tui`.
5. **Are you turning a default subsystem OFF to produce a smaller base?** →
   **flavor** (preset). Example: `pure-compute`.

**The sharp rules.** SMALL new C, always wanted, no new authority → **base cap
module** (in-base, rides `CAP_OBJS`). LARGE new vendored C or a new authority,
off by default → **feature** (additive). Turning a default off → **flavor**
(subtractive). A separate program → **tool**. Pure orchestration over existing
caps → **stdlib** (and this is the most common misclassification: reach for
stdlib before a feature). The base-cap-module ↔ feature line is SIZE + optionality:
a self-contained codec everyone might use rides the base; a vendored engine or a
new authority that most apps never touch is a composed feature. "On by default
and you subtract" is a flavor; "off by default and you add" is a feature. HTTP
itself is just a feature that happens to be on by default, which is why the
flavor/feature line is a distribution fact (enumerable pre-published base vs.
combinatorial bolt-on), not an architectural one. Full rationale in
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

Drops SQLite (`vendor/sqlite/sqlite3.c`), `cap/db.c` / `db_sqlite.c` / `db_udf.c`,
`migrate.c`, `commands/migrate.c`, `agent/db.c`, `worker_db.c`, and the per-runtime
`mod_db.c` / `worker_db.c` (~28% smaller: ~3.66 MB vs ~5.06 MB, arm64 Darwin). Apps
lose the `db` module, `hull migrate`, `hull agent db|migrate`, and the DB-backed
stdlib (migrate, outbox, inbox, session, idempotency, rbac, search, ratelimit),
which fail at module load. Everything else (HTTP, middleware, `http.fetch`, `ws`,
`fs`, `crypto`, `compute`, `gpu`, templates, stateless CSRF, JWT,
`hull build/dev/test/agent`) still works. For stateless compute services.

### Image-less builds (`HL_ENABLE_IMAGE=0`)

Subtractive make knob (not a `--with` feature or `--flavor` preset). Drops
`cap/image.c`, `cap/image_stb.c`, `runtime/{lua,js}/mod_image.c`, and
`vendor/stb/stb_impl.c` (image is stb's only consumer; ~146 KB). A non-optional
`"hull/image@1"` is then a hard app-load error (`requires HL_ENABLE_IMAGE
(build-time)`); `"hull/image@1?"` makes `require` return nil / `import` bind null.
On `HL_ENABLE_GPU=1 HL_ENABLE_IMAGE=0` only the GPU image bridge drops:
`gpu.texture(img)` rejects an `HlImage` (raw bytes + `{width,height,format}` still
work) and `gpu.texture_read` / `textureRead` are compiled out. Link covered by the
CI `flavors` matrix. See [docs/image_feature.md](docs/image_feature.md).

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
`require("hull.db").default()` (the `-d` / `"default"` connection), `.connect(name)`
(a named connection), or `.open(dsn)` (a caller-owned dynamic connection the app
must `close()` or let GC finalize). Lua + JS examples and the manifest shape:
[docs/app_api_reference.md](docs/app_api_reference.md#database-connections-hulldb).

The connection object carries `query` / `exec` / `batch` / `last_id` (`lastId`)
/ `insert_if_absent` (`insertIfAbsent`) / `upsert` / `table_columns`
(`tableColumns`) / `quote_identifier` (`quoteIdentifier`) / `in_transaction`
(`inTransaction`, whether the connection is inside a transaction) / `backend_name`
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

**Internal database (`databases.internal`).** An optional DSN (or `"$VAR"`)
for the stdlib's own `_hull_*` tables - session, auth-flows, totp, audit-log,
rbac, auth-health, idempotency, attachment - so they can live under a database
role the app's connection has no grants on: the database, not the SQL-text
check, then keeps app SQL away from them. The stdlib reaches it through the
stdlib-only `hull.db._internal` / `hull:db:_internal` proxy (a lazily resolved
connection: a module takes it at load, before the manifest is wired, and a
stdlib `init()` at app top level reads `databases.internal` from the app's
manifest on the spot); `db.connect` cannot name it. Undeclared, everything
stays on the default connection. Outbox, inbox and jobs stay on the APP
connection by design - they must commit atomically with the app's own writes
- as do search (its FTS index reads app tables) and the kv SQL store, so those
tables keep only the SQL-text check. Covered by `tests/e2e_db_internal.sh`.

**Named connections via the manifest.** Additional connections are declared
under `manifest.databases.named` (a name -> DSN map). A DSN value of exactly
`"$VAR"` or `"${VAR}"` is an env reference resolved at connection-open time (so
credentials never sit in app source); a value that merely CONTAINS a `$` (e.g.
a password) is literal. The variable must be DECLARED, in `manifest.secrets`
(readable only through `$VAR` references, never by `env.get`) or in `env`;
`hl_manifest_check_env_refs` (`manifest.c`) refuses app load otherwise, for every
field that resolves references (`hosts`, `databases.named`,
`databases.dynamic.hosts`, `kv.dynamic.hosts`, `ssh.connect.hosts`,
`ssh.tunnel.hosts`). Before this a reference could read ANY variable - an SQLite
DSN's value is readable back through `PRAGMA database_list`. The app context
behind `hull test` / `hull agent` runs the same check, and so does the stdlib's
internal-connection lookup before a top-level `init()` uses a `"$VAR"`
`databases.internal` (audit 17: both read an undeclared variable).

(The DB connection dials its host directly; `manifest.hosts` gates
`http.fetch`, not database connections.)

**Dynamic connections (`db.open(dsn)`, roadmap §2.2).** For a DSN not known at
manifest-authoring time (a per-tenant shard, a user-supplied endpoint),
`db.open(dsn)` opens a **caller-owned** connection after validating the DSN
against `manifest.databases.dynamic`: the scheme must be in `dynamic.schemes`,
and a network backend's host must match `dynamic.hosts` (exact, `*`, `*.suffix`
subdomain glob, or a CIDR; IP-literal hosts only, no DNS) while a file backend's
path must pass the same fs-sandbox gate as `fs.read` and run through no symbolic
link, leaf and SQLite sidecars included (audit 29). `hosts` entries may be
`"$VAR"` env refs; `schemes` are compared literally. Both lists fail closed: no `dynamic` policy (or
an empty one) rejects every `db.open`. A process-wide cap (16) bounds concurrent
dynamic connections. `kv.open` has the same cap (`HL_KV_OPEN_MAX`, in `cap/kv.c`; every Valkey store
opens through it), and a Valkey receive buffer past 1 MiB is freed once its reply is used
(audit 30). Unlike `db.connect`, the app **owns** the returned handle:
call `conn.close()` (`conn.close()` in JS) when done, or let GC finalize it
(double-close and use-after-close are safe and fail closed). A dynamic handle
carries the sync methods (`query`/`exec`/`batch`/`insert_if_absent`/`upsert`/
`table_columns`) plus `async` (`conn.async.query/exec`): the connection object
reports its own DSN (`db_call_dsn` / `js_call_dsn`, symmetric with the handle
resolver) so `conn.async` targets the dynamic database through the worker pool,
which bounds its per-thread connection cache with an LRU
(`HL_WORKER_DB_MAX_CONNS`); a dynamic DSN's worker connection is not cached at
all - it closes after the op (`HlWorkerDbOp.no_cache`), so worker threads never
hold dynamic connections past the 16-connection cap or the app's `close()`. `async`-after-close fails closed (the live-handle guard). `udf` on a
dynamic handle is intentionally absent (worker-side udf re-registration is keyed
off the registry a dynamic handle is not in; tracked follow-up), and JS refuses a `db.open` handle as `this` of another connection's `udf.register` / `unregister` (audit 29). Note the
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

**SQLite connection replaced or broken (audit 11/12).** When the guard cannot
roll a SQLite connection's transaction back, it reopens the file in place
(`sqlite_replace_conn`, same ctx so handles stay valid) - never while one of
its statements is stepping (a UDF callback). An in-memory database, a wrapped
connection or a failed reopen cannot be replaced: the handle is marked BROKEN
and every call fails ("stuck inside a transaction ... restart the app"), its
raw `sqlite3*` / statement cache are withheld, and it reports no transaction
(so waits are not refused for it); a later entry whose quiet rollback
succeeds puts it back. A reopen drops the old connection's `db.udf`
registrations (re-register them). No raw `sqlite3*` is cached across app code:
`hl_app_context_db` and the udf bindings resolve it on each use.

**Stale transactions.** Every OPEN registry connection (default, named,
internal) and every open `db.open` handle has a transaction an entry left open
rolled back (`hl_db_registry_guard_stale_txns`): SQLite by autocommit state,
Postgres by `tx_status`, MySQL by `SERVER_STATUS_IN_TRANS`, DuckDB by an
unconditional `ROLLBACK`; the handle's `db.batch` depth goes with it. The
runtimes run it when an entry (request, middleware, SSE event, timer,
ws-server / ws-client callback, Lua `hull._task` / JS `hull:_task` task) starts, when it
returns, raises or parks, and before a parked continuation (Lua `hl_lua_async_resume`, JS
`hl_js_async_resume` / the multipart pump) resumes - audit 6 M1: run only at
the start, a resumed handler or a WebSocket callback joined the transaction a
failed entry had left. A new entry or resume path must call it at both ends. Before audit 4 only SQLite's default connection was guarded, so a Postgres
handler that raised between `BEGIN` and `COMMIT` left every later request
inside that transaction (or, after a failed statement, failing with "current
transaction is aborted"). The SQLite guard checks its ROLLBACK took
(`sqlite3_get_autocommit`), retries once after resetting the connection's
statements, and when the connection is still inside the transaction replaces
it in place (same ctx, so every handle stays valid) with a new connection to
the same file - closing the old one rolls it back. An in-memory database is
not replaced (that would drop it), and `db.udf` registrations do not carry
over (audit 11).

**No transaction across a wait.** The guard cannot tell a dead owner from one
parked on `http.fetch` / `db.async` / `hull.sleep`, and a parked handler's
transaction is on a connection other entries are using meanwhile: the guard
rolled it back under the handler, whose remaining statements then autocommitted
and whose COMMIT "succeeded" (audit 5 M1). So every Hull wait refuses while a
registry connection or `db.open` handle is in a transaction
(`hl_db_registry_open_txn`, the backend's `in_txn` vtable method; Lua:
`hl_lua_check_can_wait`, called AFTER the binding has read its arguments and
options and with no app code between it and the arm - an opts `__index`, a
`__tostring` or a `__gc` finalizer (any Lua allocation can run one) is app code
that could `BEGIN` after an earlier check (audit 15); a binding that must free
copies before raising tests `hl_lua_can_wait` first; JS:
`hl_js_db_refuse_wait` in `runtime/js/db_wait.h`, called by each parking
operation before it arms anything - a new JS wait primitive must call it).
In JS the check runs again where the run actually yields (`hl_js_run_yield_check`), since a
transaction can open after the op was made (`const p = http.fetch(..); conn.exec("BEGIN");
await p`): the transaction is rolled back there and the run is failed at its next resume (500;
`app.main` exits 1) without being continued. The run's TAG is failed too, and stays failed while
any pending job or reaction on a pending promise carries it (`HlJS.txn_dead_tags`, swept with
QuickJS `JS_ForEachLiveJobTag`): a reaction the run left on a promise Hull does not drive is
queued when that promise settles, maybe from a later request, and is dropped unrun - kept in a
32-slot ring and cleared once the request was answered, it ran the code after the await in
autocommit (audit 33). A handler left waiting only on such a promise is failed at once (500,
`budget_txn_failed`), not answered an empty 200. Every entry - dispatch, SSE, timers, ws-server and
ws-client callbacks, `app.main` - parks through `hl_js_entry_park` (`runtime/js/async.c`): it
first runs the jobs the handler queued, with the entry still active (its code after `await
null` is the entry's own: an op it starts belongs to the run, and a `BEGIN` it runs is seen),
then wires every continuation into one run and checks; an async or multipart resume that waits
again checks too - whether or not it started a new op (`await pa; BEGIN; ...; await pb` with
`pb` made before the BEGIN waits across it all the same, audit 8 M7) - and so does an entry
left waiting on a promise Hull does not drive (audit 8 c_db L1). Code no entry waits on is
checked too (audit 31): the end of a resume whose continuation is unwired (`hull.sleep(5).then(async
() => ...)` from a synchronous handler) fails that chain's run record (its continuations are
wired with no handler promise, so each comes back to the check), and in `hl_js_run_jobs` the end
of another run's stretch of jobs (QuickJS `JS_GetPendingJobTag`, HULL PATCH 0008) with a
transaction open rolls it back before the active entry's next job runs and marks that run's tag
failed (`HlJS.txn_failed_tags`, `hl_js_run_tag_failed`): its later resumes are not continued.
Before, the other run's transaction swallowed the active request's acknowledged write. The
reverse holds too: while the active run is inside a transaction across a microtask yield (`BEGIN;
await null; ...`), the drain runs its own jobs first (`JS_ExecutePendingJobTagged`) and holds other
runs' jobs back until the transaction closes; if the active run runs out of jobs with it still
open, its stretch ended holding it (`hl_js_run_txn_hold_end`: rolled back, logged, tag failed) and
the drain goes on. A failed tag's jobs are dropped unrun at the top of every drain
(`JS_DiscardPendingJob`, HULL PATCH 0008; audit 32: the run's own jobs queued later in the same
drain ran on, writing in autocommit and answering 200), and the run ends failed where its
completion is decided: `hl_js_entry_park` (continuations wired into one `txn_held` record and the
run waits, or, with none, a trip with `budget_txn_failed` so the caller answers 500 / fails
app.main / ends the timer or ws callback as a tripped run), and the post-drain checks of
`hl_js_async_resume` and `mp_js_pump` (forced to failure, toString skipped). The tag list is a
fixed ring of 32; a slot is freed (`hl_js_run_tag_clear`) once a wired run's record stops its
continuations. Audit 7 H1 / M3 / M6: run after
the entry returned, those jobs ran with no request active (an op they started was detached and
never waited for: an empty 200, then `res.json` into a recycled connection slot), or inside
another entry's drain. The same holds for the toString of a failed run's error or rejection,
app code that runs after the last drain: a resume (async or multipart) and a timer run it with
no request or timer active and drain its jobs right after (audit 9 M1). An op belongs to a request only while the request's live life is the
active one (`hl_js_async_cont_create`; `hl_js_op_suspend` refuses otherwise): a resume makes
its whole entry active before it settles the op's promise, dispatch and SSE before `req` is
built, middleware sets `in_middleware` before it, and `req` is built with defined (not set)
properties - app code run in any of those windows (an `Object.prototype` `then` getter, an
inherited setter) made an op that suspended the connection uncounted (audit 8 H1). The drain
also runs OTHER runs' jobs (a handler that awaited a promise Hull does not drive, resumed by the
entry that settled it), so each run has a tag (`HlJS.entry_tag`: fresh at every
`hl_js_budget_arm` and app.main's arm, re-entered by a resume from `HlJsRunLink.run_tag`) and
QuickJS runs a promise job with the tag of the code that awaited it (HULL PATCH 0008): an op made
under another tag is detached and part of no run (app.main's own excepted), and a multipart read
is refused there - it was the active request's op and held that client's response (audit 30). A
nested run (`hull:_task`, `test.get`) puts the outer tag back. As defence
in depth a resume never sends while Keel still has the connection suspended by an op
(`hl_js_conn_held_elsewhere`, over `hl_net_op_holder`).
Under that invariant every transaction the guard finds is orphaned. JS
`db.batch(fn)` refuses an async fn before BEGIN (prototype check against the
intrinsic AsyncFunction / AsyncGeneratorFunction) and a returned thenable
after (TypeError, rolled back) - in a `worker.dispatch` VM too. The prototype
check can run a Proxy trap that closes a `db.open` handle, so the handle is
resolved again after it and a closed one refused (`hl_db_batch_enter` /
`_leave` return -1 on NULL; audit 25). A sync fn that
returns an async function's promise (`() => saveAll()`) is caught only by the
second check: the batch is rolled back, but `saveAll` keeps running and its
statements after its first `await` commit one by one. The backends that read transaction state from
the SQL text (DuckDB tracking, the Postgres / MySQL ROLLBACK and COMMIT
checks) share `cap/db_sql_kw.h`, which skips comments and knows every
spelling (`COMMIT WORK`, `END TRANSACTION`, `ABORT`, ...); a Postgres COMMIT
in state `E` is judged by the reply (it left the transaction and was not a
rollback), not by its spelling.

**`db.async` never leaves a transaction on a worker connection** (audit 6
M3). Worker connections are per-thread and reused by every later op, so after
each op `worker_end_txn` (`worker_db.c`) rolls back a transaction the op left
open (`BEGIN` through `db.async`) and fails it with "a transaction cannot span
db.async operations"; a connection whose rollback did not take is dropped. A
`worker.dispatch` job's `db` runs on the same pooled connection, so the job's
end does the same (`hl_worker_db_end_job`, after the worker VM is closed):
a transaction the job left open - forgotten, or raised / over budget between
`BEGIN` and `COMMIT` - is rolled back and the dispatch fails with "a
transaction cannot outlive a worker.dispatch job" (audit 7 M4). A worker
VM's `db.batch` goes through the same `hl_db_batch_enter/leave` as the event
loop's (a nested batch is a savepoint; audit 8 M2 - raw BEGIN / COMMIT let a
nested batch commit the outer's writes on Postgres / MySQL), and the job's
end and `worker_end_txn` reset the handle's batch depth. A
WAIT_NOTIFY on a `db.open` handle keeps its LISTEN connection only while the
handle is open: the op carries the handle's id (`hl_db_dynamic_id`) and every
op sweeps the thread's connections whose handle was closed
(`hl_db_dynamic_id_live`). A NotificationResponse a query or a script reads on
a LISTEN connection marks its channel pending (`HlPgConn.on_notify`), as one
for another channel read during a wait does - it was dropped, and the next
wait slept to its timeout (audit 20). Every connection to one DuckDB file - the
event loop's and each pool thread's - shares one database instance (refcounted
in `db_duckdb.c`, the security configuration applied and locked by the first
open): each open was an instance of its own, so a pool thread read what its
instance had replayed at open, and both could write the file (audit 20).
`hl_db_batch_enter/leave` detect a transaction that ended under an open batch
(a raw COMMIT / ROLLBACK in fn, a nested in-process dispatch): leave fails with
a clear message, a nested batch starts a fresh transaction.

**`db.async` results are bounded (audit 15).** A query's rows are copied
(`worker_db.c`, `MaterializeCtx`) into plain malloc on the worker thread before
the VM converts them, so the copy keeps a running byte total and fails the op
with `result too large` past the submitting VM's heap limit
(`HlWorkerDbOp.max_result_bytes`) or `HL_WORKER_DB_RESULT_MAX_BYTES` (256 MB),
and charges the copied bytes to the op's budget at 1 unit per
`HL_DB_ALLOC_UNIT_BYTES` - only the 100k-row cap stood in the way. A row takes
at least one cell (`db_row_stride`), counted and charged: a zero-column row
(Postgres `SELECT FROM t`) grew the store by `realloc(p, 0)`, which glibc frees,
and the result was freed twice (audit 32). A copy that
fails (too large, too many rows, rows of another width, out of memory) also
trips the op's budget, so the Postgres / MySQL backends close the connection
at the next row instead of reading the rest of the reply, uncharged and with
no deadline (audit 16; `hl_worker_db_materialize`); the op keeps the copy's
own message. A TEXT / BLOB cell whose copy cannot be allocated fails
the op (`out of memory`) instead of reading as NULL in a result that
succeeded (audit 24). That cap is per op, and a run keeps up to the queue plus the
workers (~68) in flight, so every result copy - and a `worker.dispatch` job's
return value, which had no bound at all (a table of 500k keys naming one
30 MB string) - also takes its bytes from one per-VM total, HlLua / HlJS
`async_result_bytes`, updated with atomics from the pool threads
(`HlOffheapShared`, `shared/offheap.h`) and capped at the heap limit (1 GiB
with none); past it the op fails with `result too large`, and the op gives
its share back when it is freed (audit 23). Async GPU read-backs and async compute outputs take the same total, on the worker before the copy (`HlGpuDispatchOpts` / `HlGpuPipelineOpts` / `HlWasmCallOpts.result_hold`; audit 29). Both bindings resolve the bound handle again after converting
their arguments (app code there - a JS getter, a Lua `__gc` run by the
conversion's allocation - could close a `db.open` handle; Lua audit 16).
`conn.wait_notify` refuses a channel name over 63 bytes before it copies it
(`HL_WORKER_DB_CHANNEL_MAX`; audit 23) and clamps its
timeout to 0..`HL_WORKER_DB_NOTIFY_MAX_MS` (60 s; `hl_worker_db_notify_timeout`)
and raises on a backend without LISTEN/NOTIFY instead of taking a pool thread
(audit 14).

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
  MD5 is rejected. A cleartext password is sent only over TLS whose server
  certificate was VERIFIED (`verify-ca` / `verify-full`) or under an explicit
  `sslmode=disable` (`hl_pg_cleartext_allowed`): `prefer` / `require` verify
  nothing, so a MITM could finish TLS with a self-signed cert and ask (audit 5
  M4). The same rule admits a SCRAM iteration count below 4096, and the server
  nonce must extend the client's (audit 29). Reuses `cap/crypto` (SHA-256 / HMAC / PBKDF2).
- **Aborted transactions:** a COMMIT in state `E` (an earlier statement
  failed) gets a "ROLLBACK" tag and no error from the server; the backend
  reports it as a failure, so `db.batch` no longer returns normally with every
  write discarded.
- **TLS:** `sslmode=disable|prefer|require|verify-ca|verify-full` in the DSN
  (`?sslmode=...`), default `prefer`. `verify-full` checks the chain +
  hostname against the resolved trust anchor (`--ca-bundle`, the system store,
  or the embedded bundle; see "HTTPS / CA bundle") via the shared `shared/tls_client.c`
  helper (the same KlTls handshake SMTP uses). `HL_LINK_TLS` links Keel +
  mbedTLS whenever an HTTP half OR Postgres is enabled.
- **Types:** typed decode by OID (bool/int/float/text/bytea); `?` placeholders
  are rewritten to `$n`; params bound in text format (SQL injection
  impossible). A Lua/JS params array with trailing nils binds the tail as NULL,
  matching SQLite.
- **Migrations** run through the vtable; multi-statement migration files use
  the Postgres simple-query protocol. The `_hull_migrations` tracking table is
  dialect-portable (name PK, host-generated ISO-8601 `applied_at`). The whole
  run holds a session lock on the network backends - `pg_advisory_lock` /
  MySQL `GET_LOCK('hull_migrate')` - so instances starting together neither
  race the first `CREATE TABLE` nor re-run a migration (audit 5 L3). While it
  is held the handle is `session_pinned`: a connection lost mid-run is not
  transparently reconnected (the lock went with the session), so the run
  fails instead of going on unlocked (audit 6 L5).
- **SQLite-only features under Postgres:** `db.udf` and `hull/search` (FTS5)
  are SQLite-only and fail with a clear error on a Postgres connection.
- **ROLLBACK shortcut is single-statement (Postgres and MySQL):** on a
  connection lost (or, MySQL, deadlock-aborted) inside a transaction, a
  ROLLBACK is answered "done" without being sent - so it must be the whole
  text: `hl_sql_txn_kind` makes trailing text `HL_SQL_TXN_OTHER`, and MySQL
  also requires `my_sql_single_statement` (it refuses a `--` MySQL would read
  as minus signs); `ROLLBACK; DELETE ...` is refused like any other statement
  rather than reported done (audit 12).
- **Lost connections (Postgres and MySQL):** every read is bounded by the
  DSN's `?read_timeout=<ms>` (default 300000, `0` = none), from the startup
  exchange on. So is every send (`hl_db_transport_send_sliced`, audit 23):
  non-blocking writes, one `read_timeout` for the whole message, the run asked
  after each chunk and each quiet `HL_DB_WAIT_SLICE_MS` slice, and the
  connection closed and marked broken on a stop - a blocking `send()` held the
  event loop for a peer that stopped reading, for ever with `read_timeout=0`.
  A TLS close never waits on the peer (audit 24): a sliced send or read that
  stopped or failed marks the transport broken, and `hl_db_transport_abort`
  (`pg_abandon` / `conn_teardown`, `hl_my_conn_close` on a broken connection,
  `hl_valkey_conn_close` on a dead one) frees the session with no
  close_notify - the blocking close_notify flushed the part-sent record to a
  server that had stopped reading and held the loop for `read_timeout`. A
  healthy close sends close_notify once, non-blocking.
  A connection whose reply was left part-read - a failed or
  timed-out read, a malformed packet, a dropped socket - refuses further use
  rather than hand the next query this one's reply. The backend keeps the DSN
  (scrubbed on close) and reconnects on the next call when the connection was
  idle. Lost inside a transaction it does not: the server rolled the
  transaction back, and statements on a new connection would each commit alone,
  so calls refuse until a `ROLLBACK` (the batch wrapper's, or the app's own),
  which then succeeds. Covered by the `/reconnect` phase of `e2e_postgres` /
  `e2e_mysql` (each kills its own session). A run whose budget or deadline
  trips while the reply arrives does not read the rest of it: the connection
  is closed and marked broken (`pg_abandon_if_tripped` /
  `reply_frame`) - the server stops the query at its next write, and
  the same reconnect / refuse-until-ROLLBACK rule applies; simpler than a
  CancelRequest / KILL QUERY on a side connection (audit 15). Every frame is
  charged 1 unit plus 1 per `HL_DB_ALLOC_UNIT_BYTES` of its body - on
  Postgres every frame type but ReadyForQuery (a `LOOP RAISE NOTICE` stream of
  NoticeResponses), on MySQL every packet of a reply (`reply_frame`: rows,
  and since audit 18 the OK / column-count / column-definition / EOF packets
  too, so a stream of empty result sets is charged) - where a 0-unit charge
  only asked, which a `db.async` op's budget never tripped on (audit 17). A
  wait for the server is polled in `HL_DB_WAIT_SLICE_MS` (100 ms) slices
  (`hl_db_transport_wait_readable`, bounded in all by `read_timeout`) that ask
  the run's budget with a 0-unit charge, so a `pg_sleep` / `SLEEP()` / lock
  wait is stopped by a tripped budget or the run deadline, through the same
  abandon path, instead of holding the loop in recv for the whole read timeout
  (audit 18). So are the waits around it (audit 20): the TCP connect
  (`hl_db_transport_connect_stop`, asked between pump steps), the TLS handshake
  (`hl_tls_client_handshake_stop`), the Postgres SSLRequest answer, and a TLS
  read once its record's first byte arrived (`hl_db_transport_recv_sliced`:
  the read decrypts whole records and blocked for the rest under SO_RCVTIMEO,
  for ever with `read_timeout=0`); Valkey's connect and handshake likewise,
  which waited up to `connect_timeout` (10 min). The run is asked after
  every chunk of a frame that is still incomplete, not only after a quiet
  slice, the TLS read is non-blocking so each chunk returns, and the reads of
  one frame share one `read_timeout` (`hl_db_transport_time_left`): a peer
  that sent a big frame a byte at a time, each sooner than a slice, was never
  stopped, since every wait restarted the timeout (audit 21). A Postgres trip on the CommandComplete of a COMMIT (since audit 29 on any frame of a lone COMMIT's reply, or the wait for it: `commit_sent`) is not
  abandoned: the reply is read on to its ReadyForQuery, so the commit the
  server made is reported, not a "lost" transaction; likewise a MySQL trip on
  the packet that ends a reply (its OK / ERR, or the EOF of the last result
  set) finishes the call, nothing being left to read - and since audit 31 a
  trip in the WAIT for a lone COMMIT's OK (`HlMyConn.commit_sent`, set once the
  COMMIT is wholly sent; `commit_next` is set by db_mysql.c's `sql_is_commit`)
  switches to finishing too, held to `HL_MY_FINISH_MAX_MS` (2 s, or a shorter
  `read_timeout`, writes included) and abandoned past it as before (it
  abandoned at once, IN_TRANS kept: the commit read as lost, the next ROLLBACK
  was answered done unsent, and a retry applied it twice). The trip is sticky, so
  the run raises it at its next poll (audit 18). That finishing read asks no
  budget, so it is held to `HL_PG_FINISH_MAX_MS` (2 s, or a shorter
  `read_timeout`) in all; past it the connection is abandoned with tx_status
  still 'T' (refuse until ROLLBACK) - a stall before ReadyForQuery held the
  loop for `read_timeout`, for ever with 0 (audit 26). The bound covers the
  TLS read after each wait and any write too (`hl_pg_conn_io_left`; the read
  took `read_timeout` alone, audit 27). Postgres refuses `COPY ... TO STDOUT` by dropping the
  connection at the CopyOutResponse (there is no CopyFail for that direction;
  COPY FROM STDIN still gets a CopyFail). The Postgres startup exchange is
  charged the same way, frame by frame, and a SCRAM iteration count (the
  server's) is charged 16 units a round before the key derivation runs
  (audit 17); the MySQL auth exchange is charged per packet and takes at most
  `HL_MY_AUTH_MAX_PACKETS` (16) - it had no bound (audit 17). A failed Postgres
  `hl_pg_wait_notify` (the LISTEN wait) marks the connection broken the same
  way (audit 9 L5).
- **`table_columns` on Postgres** reads `information_schema.columns` of
  `current_schema()` only: a same-named table in another schema the role can
  see no longer adds its columns (audit 9 L3).

**MySQL/MariaDB specifics** (`HL_ENABLE_MYSQL=1`). One backend serves both
`mysql://` and `mariadb://` (MariaDB is a MySQL fork on the same wire
protocol). Codec (`cap/mysqlwire.c`) is little-endian, 3-byte-length +
1-byte-sequence framed, with length-encoded ints/strings; all server-message
parsing is bounds-checked over untrusted input (mirrors `cap/pgwire.c`).
- **Auth:** `mysql_native_password` (SHA-1 challenge-response) and
  `caching_sha2_password` (MySQL 8 default: SHA-256 fast path always;
  full-auth sends the cleartext password, only over TLS whose server
  certificate was VERIFIED - `sslmode=verify-ca` / `verify-full` - since
  `prefer` / `require` would hand it to any MITM (audit 5 M4); otherwise it
  fails with a hint. The RSA public-key exchange is not implemented. Once the
  server has cached the account's hash, the fast path works under any
  sslmode).
- **Deadlocks:** an `ER_LOCK_DEADLOCK` (1213) inside a transaction means
  InnoDB rolled the WHOLE transaction back; the backend then refuses every call
  but `ROLLBACK` (as for a connection lost mid-transaction), so later
  statements cannot autocommit and the COMMIT cannot "succeed". A deadlock
  in a later statement of a multi-statement `exec` counts too (its ERR code
  is recorded while the reply is drained, audit 6 L2). So does any other
  failed non-DDL statement after which `COM_PING` finds the transaction gone
  (a lock-wait timeout under `innodb_rollback_on_timeout=ON`): only a DDL
  statement's implicit commit is resumed (audit 7 L1). The classifier
  (`my_sql_commits_implicitly`) follows MySQL 8's implicit-commit list and
  reads past the first keyword: `CREATE` / `DROP TEMPORARY TABLE` and
  `LOAD DATA` commit nothing, so they are refused, not resumed (audit 8 L2);
  an unrecognised statement is always refused. A statement with an
  executable comment (slash-star-bang, MariaDB's slash-star-M-bang - MySQL
  runs it, the shared `cap/db_sql_kw.h` reader skips it) or a `#` comment
  (MySQL skips it, the reader does not) is unrecognised in every MySQL
  classifier: refused, never resumed, and a transaction it ended is reported
  lost (audit 9 L1). The connection runs multi-statement texts
  (`CLIENT_MULTI_STATEMENTS`) and the classifiers read the first statement,
  so only ONE statement (`my_sql_single_statement`: a trailing `;` and
  comments allowed, quotes / backticks skipped, a backslash in a string
  unreadable) is ever an implicit commit, and the resume after a transaction
  ended follows only such a statement - `CREATE TABLE t (x INT); COMMIT` or
  `SELECT 1; COMMIT` no longer get a new transaction opened under them. MySQL
  (and SQLite) block comments do not nest, Postgres's do: `cap/db_sql_kw.h`
  takes the dialect's rule (`hl_sql_txn_kind_ex(sql, nest)`), MySQL reads
  without nesting (audit 10). A `--` comment ends at CR as well as LF, as
  Postgres's lexer ends it: `ROLLBACK--\r ;DELETE ...` read as a lone
  ROLLBACK (audit 28, found by `fuzz_sql_kw`).
- **Implicit commits:** MySQL commits the open transaction around DDL, and
  before a DDL statement that then fails. An ERR carries no status flags, so
  after a failed statement inside a transaction (and after the `CREATE INDEX`
  shim swallows a duplicate-index error) the backend sends `COM_PING`
  (`hl_my_conn_ping`) to learn whether the transaction survived (audit 6 L1).
  A top-level `db.batch` whose transaction a DDL statement ended opens a new
  one for the rest of it; a NESTED one does not (the outer savepoints went
  with the commit), so the batches report the transaction lost (audit 6 L3).
  The server-named handshake plugin drives which is used, and AuthSwitchRequest
  re-dispatches through the same path. `client_ed25519` (MariaDB) is not yet
  supported and fails with a clear hint pointing at the two supported plugins
  (it needs ed25519 group-ops TweetNaCl keeps private; tracked follow-up).
  Reuses `cap/crypto` (SHA-1 / SHA-256).
- **TLS:** `sslmode=disable|prefer|require|verify-ca|verify-full` (same names +
  default `prefer` as Postgres). When TLS is wanted and the server advertises
  `CLIENT_SSL`, the client sends an SSLRequest, runs the blocking handshake
  over the shared `shared/tls_client.c` (the resolved trust anchor; `verify-*` checks
  chain + hostname), then sends the credentialed HandshakeResponse41 over TLS.
  All post-handshake I/O tunnels through `KlTls`.
- **Connection character set:** utf8mb4 (`HL_MY_DEFAULT_CHARSET`, 45) unless
  the server's default already is a utf8mb4 collation (`hl_my_client_charset`).
  The client announced the server's default whatever it was: MySQL 5.7 /
  MariaDB's latin1 stored UTF-8 text double-encoded, and a gbk / sjis / cp932
  / big5 / gb18030 default - whose trail byte can be 0x60 - let a backtick
  `hl_db_quote_ident` doubled be read as part of a character, closing the
  identifier (`conn.quote_identifier` was not injection-safe there; audit 20).
  The Postgres and MySQL DSN parsers match the scheme without case, as the
  selector does.
- **Queries / types:** param-less statements use the text `COM_QUERY` protocol;
  **parameterized** statements use the binary prepared-statement protocol
  (`COM_STMT_PREPARE` / `EXECUTE` / `CLOSE`) so values never touch the SQL text
  (injection impossible). Binary rows decode by column type to `HlValue`
  (int / double / borrowed text; `DATE`/`DATETIME`/`TIMESTAMP`/`TIME` are
  formatted to ISO-8601-ish strings). An `UNSIGNED` integer column (the
  column definition's flag) is zero-extended, not sign-extended (audit 9 M3:
  `TINYINT UNSIGNED` 200 read as -56); a `BIGINT UNSIGNED` above `INT64_MAX`
  arrives as its decimal text, in both protocols. A Lua/JS params array with trailing nils
  arrives short (Lua's `#` drops the tail), so `COM_STMT_EXECUTE` pads to the
  statement's declared `num_params`, binding the tail NULL, matching
  SQLite/Postgres. MORE values than placeholders is refused (`parameter count
  mismatch`, the statement closed), as SQLite and Postgres refuse it; the
  surplus used to be dropped silently (audit 17).
- **Dialect:** `insert_if_absent` builds a plain `INSERT` and takes its
  `ER_DUP_ENTRY` (1062) as "already there" (0 rows, no COM_PING), not
  `INSERT IGNORE` (audit 31): IGNORE downgraded every strict-mode error to a
  warning and stored the value TRUNCATED (invalid utf8mb4 1366, too long
  1406: `'evt_1\xff'` stored as `'evt_1'`, a false inbox duplicate). Strict
  mode rolls the failed statement back, so no truncated row persists; a FK /
  NOT NULL violation IGNORE skipped now fails too. Relies on the server's
  strict `sql_mode` (the default). The inbox also refuses an id / source that
  is not valid UTF-8 (Lua `utf8.len`; JS a lone surrogate). `upsert` builds
  `INSERT ... ON DUPLICATE KEY UPDATE c = VALUES(c)` (MariaDB / MySQL 5.7+;
  no IGNORE, so strict-mode errors surface);
  `last_id` returns the OK packet's `last_insert_id`; `table_columns` queries
  `information_schema.columns` scoped to `DATABASE()`; identifier quoting is
  backtick (`` ` ``). MySQL 8 has no `CREATE INDEX ... IF NOT EXISTS`, so the
  backend transparently rewrites it to a plain `CREATE INDEX` and treats a
  duplicate-index error (by its code, `ER_DUP_KEYNAME` 1061, never its
  message text - audit 9 L2) as success (the stdlib's idempotent index DDL works
  unchanged). Only a text that is exactly ONE statement whose leading tokens are
  `CREATE [UNIQUE|FULLTEXT|SPATIAL] INDEX IF NOT EXISTS` is rewritten (audit
  14): app SQL that merely contains the words (a procedure body, a literal) or
  a multi-statement text goes to the server as written. Multi-statement migration files run as one `COM_QUERY`
  (`CLIENT_MULTI_STATEMENTS`), draining every result set.
- **Portable stdlib DDL:** MySQL rejects a `TEXT`/`BLOB` primary key or index
  without a prefix length, so every keyed / indexed / foreign-key text column
  in the DB-backed stdlib (and the `_hull_migrations` tracking table) is
  `VARCHAR(255)`, not `TEXT` (TEXT affinity on SQLite, varchar on Postgres,
  indexable on MySQL); data-only columns stay `TEXT`.
- **Binary collation on `_hull_*` tables (existing deployments: migrate by
  hand).** `db_mysql.c` appends `DEFAULT CHARSET=utf8mb4 COLLATE=<c>` to every
  `CREATE TABLE _hull_*` it runs, `<c>` being the server's NO PAD binary
  collation (`hl_my_bin_collation`, from the handshake's version):
  `utf8mb4_0900_bin` on MySQL 8.0.17+, `utf8mb4_nopad_bin` on MariaDB
  10.2.2+, `utf8mb4_bin` on MySQL before 8.0.17 and MariaDB before 10.2.2
  (neither has a NO PAD binary collation; naming nopad_bin on an old MariaDB
  failed every `_hull_*` CREATE TABLE - audit 23 - and a WARN now says the
  keys there compare without trailing spaces). So role names,
  inbox ids, dedup keys, user ids and job / cron names compare exactly (under
  MySQL 8's default `utf8mb4_0900_ai_ci`, `'Admin' = 'admin'` and
  `'josé' = 'jose'`; under the PAD SPACE `utf8mb4_bin` Hull used before
  audit 22, `'alice' = 'alice '`, so a user id with a trailing space shared
  another's roles, TOTP row and sessions). That only
  reaches tables created after the change: `CREATE TABLE IF NOT EXISTS` leaves
  an existing table as it was, and nothing converts or warns. An existing
  deployment converts them once - to `utf8mb4_0900_bin` below, or
  `utf8mb4_nopad_bin` on MariaDB, including tables an earlier Hull made
  `utf8mb4_bin` (converting to a binary NO PAD collation only makes keys
  stricter, so no unique key can newly collide; FK checks are off for the
  duration because the rbac join tables reference `_hull_roles` /
  `_hull_permissions`):
  ```sql
  SET FOREIGN_KEY_CHECKS = 0;
  -- generate one statement per Hull table in this schema:
  SELECT CONCAT('ALTER TABLE `', table_name,
                '` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;')
    FROM information_schema.tables
   WHERE table_schema = DATABASE() AND table_name LIKE '\_hull\_%';
  -- run the statements it prints, e.g.:
  ALTER TABLE `_hull_roles` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  ALTER TABLE `_hull_permissions` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  ALTER TABLE `_hull_user_roles` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  ALTER TABLE `_hull_role_permissions` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  ALTER TABLE `_hull_inbox_processed` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  ALTER TABLE `_hull_jobs` CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin;
  SET FOREIGN_KEY_CHECKS = 1;
  ```
  Check the current state with `SELECT table_name, table_collation FROM
  information_schema.tables WHERE table_schema = DATABASE() AND table_name
  LIKE '\_hull\_%';`.
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
| Lua 5.4 | `vendor/lua/` | Application scripting. **Patched in tree** (five fixes, marked `HULL PATCH`: the instruction budget stays on inside `__gc` finalizers and the `__close` handlers of a reset thread, pattern matching is charged to it per byte scanned, work done inside one instruction - allocation, string compares and long-string table keys, hash-chain walks (colliding integer / float keys), table shifts, bulk value copies, number coercion, the collector's own work, and the unreported part of a coroutine's run - is charged too, and an embedder stop flag (the run watchdog's) is polled at every hooked instruction); re-apply on upgrade, see [docs/lua_patches.md](docs/lua_patches.md) |
| QuickJS | `vendor/quickjs/` | ES2023+ JavaScript runtime, Bellard release 2026-06-04 (its regexp engine polls the interrupt handler, so backtracking is charged to the instruction limit). **Patched in tree** (seven fixes, 0002-0008, each marked `HULL PATCH`; 0001 was dropped when upstream fixed it); re-apply on upgrade, see [docs/quickjs_patches.md](docs/quickjs_patches.md) |
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
                        #   via hull_tool. Never imported by app code
                        #   (embedded as "cli/<module>" entries that only
                        #   the tool VM loads).
                        #   Same `hull.X` require name as user-facing modules
                        #   (in the tool VM); the split exists
                        #   so the user-facing directory honestly reflects
                        #   what apps can import. `hull new`'s modular
                        #   templates (templates_rest.lua, etc.) also live
                        #   here. They're embedded scaffolds, not user code.
    source/             #     Pure-Lua Lua-5.4 source analysis (lexer/parser/
                        #     ranges/diagnostics/annotations/scope/lint/discover)
                        #     + hull.source.analyze (`hull analyze`). Never load().
    project/            #     Frontend-neutral project source discovery on top of
                        #     source/: registry, frontend_lua, model, analyze,
                        #     projection, inspect, publish (`hull agent inspect`,
                        #     `hull dev --agent` generations). See docs/project_discovery_design.md
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
| Filesystem | `cap/fs.c`, `cap/fs_resolve.c`, `cap/fs_policy.c` | `hl_cap_fs_read()`, `hl_cap_fs_write()`, `hl_cap_fs_mmap()`, `hl_cap_fs_stat()`, `hl_cap_fs_list()`, all through the descriptor-relative virtual-root resolver + compiled `fs.read`/`fs.write` authorization policy; READ/WRITE/MMAP leaves must be regular files (`not_a_regular_file`); the write open (`hl_cap_fs_open_write_fd`, also compute.stream's output) clears setuid / setgid or fails (audit 25), opening without `O_TRUNC` and truncating only after (`HL_FS_OPEN_WRITE_KEEP`; a refused write had emptied the file - audit 27) - except EPERM for a non-root writer of a file it does not own, whose write the kernel strips anyway (audit 26). `hl_cap_fs_exists()`/`hl_cap_fs_delete()` exist at the cap layer but are NOT app-exposed |
| Crypto | `cap/crypto.c` | SHA-256/512, HMAC, PBKDF2, Ed25519, secretbox, box, random |
| HTTP client | `cap/http.c`, `cap/http_async.c` | `hl_cap_http_request()` / `hl_async_http_start()` with host allowlist and a whole-request timeout |
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

**Teardown order (audit 14 G1 / audit 15 / audit 16).** Everything that calls
back into the runtime is stopped while the runtime is alive, and the runtime
goes last: (1) the thread pool (`rt->thread_pool` cleared and
`rt->shutting_down` set first - a timer handler a cancel_fn or done_fn ends
is not rescheduled, `hl_{lua,js}_timer_reschedule` refuses while shutting
down; audit 16): a finished op's done_fn runs, a queued one's cancel_fn
releases its continuation - through
`hl_async_ctx_drop_unrun`, which takes an attached op off the server's list
before its ctx is freed; the poll backend's `pool_free` delivers its finished
items' done_fns too, as Keel's does (they used to be dropped by `poll_free`);
(2) `rt->vt->stop_async`: sets `rt->shutting_down`, stops every detached op
and WebSocket client on the runtime's teardown list (`HlRuntime.async_live`,
`HlAsyncTeardown` in `shared/async.h`: http.fetch / Lua hull.sleep / tui.poll /
Lua task + ssh parks from a timer, a WebSocket callback or app.main, each with
an `on_shutdown` that stops its driver, plus JS's `sleep_timers`), and clears
the runtime's Keel server pointer (`HlLua` / `HlJS` `server`), so nothing -
a Lua `__gc` that `lua_close` runs in step 4 calling `ws.connect`, which
refuses while `shutting_down` - reaches the loop once it is freed (audit 16);
(3) serve.c only: the client pool, then `kl_http_server_free` (`hl_serve_free_server`),
which cancels the ops still suspended on connections and closes every
connection - a WebSocket that is still open runs NO on_close then (the loop is
gone; the conn is just invalidated) - after the async / net wraps are orphaned
(`hl_*_backend_keel_orphan`: later calls are no-ops); (4) the app context:
the DB connections (`hl_db_registry_close_all`, while the runtime is alive for
the udf xDestroy callbacks), then the runtime, then the registry's memory and
the VFS - a Lua `__gc` that `lua_close` runs may still hold a connection
object pointing into the registry, and its `conn.query` / `db.default()` /
`db.connect` / `db.open` must fail, not read a freed page (audit 17); (5) the client TLS context (a stream the runtime held does
its close_notify through it); (6) the backend loop (serve_cli). `hl_js_free` itself sets `shutting_down` and cancels its pending `hull.sleep` waits before it frees the timers and the WebSocket registry (a cancelled sleep completes its run since audit 19, which touches both; audit 20), so it is safe without `stop_async` first; `hl_lua_free` sets it before `lua_close`, and while it is set every Lua wait (`hl_lua_check_can_wait` / `hl_lua_can_wait`) and `hull.async` (`lua_hull_spawn`) refuse, so a finalizer cannot arm one (audit 29). A new detached
async op must `hl_async_ctx_track` itself (with an `on_shutdown` if it holds a
timer or a request); a new loop-bound object, an `HlAsyncTeardown` entry.
A cancelled continuation ends its handler in both runtimes: it runs the
completion hook (a ws on_close's deferred conn teardown) and clears a timer's
`in_flight` - in JS once per run (`hl_js_run_claim`). JS's cancel skipped both,
so a ws on_close parked across a teardown kept its conn object alive and
QuickJS aborted in `JS_FreeRuntime` (audit 19). Only a continuation wired into a run does this: an op a synchronous entry left un-awaited (`http.fetch(x)` in a sync ws on_close) carried the entry's hook, and its cancel tore down the conn the entry had already freed (audit 21). A detached run (a timer, a ws on_close) whose last Hull op resumes while it still awaits a promise Hull does not drive is ended at that resume - completion hook run, the timer's `in_flight` cleared and rescheduled - as a request's is: nothing else would finish it, so the timer never fired again and the conn leaked into `JS_FreeRuntime` (audit 22).
In Lua, a wait the runtime gives up on - a cancel (`hl_lua_async_cancel`) or a
result it could not build (the `push_failed` path of `hl_lua_async_resume`) -
leaves the coroutine suspended inside the binding's `lua_yieldk` with the op
freed, so the coroutine is marked ABANDONED (a sticky value in its extra space)
and `coroutine.resume` / `close` refuse it for good; merely unparked, a later
request resumed it and `lua_db_async_k` read the freed op (audit 33). Every
binding continuation (a `lua_yieldk` kfunction) calls `hl_lua_async_k_ok` first
and raises without touching its kctx unless this is the runtime's own resume:
a new one must too. (JS needs no twin: an op settles a promise whose resolve
functions the app never holds.)
A binding never resumes another coroutine's wait inline from app code: a
resume is loop-level (it runs the stale-transaction guard, re-arms the budget
and clears `active_co`), so a wake made inside a run goes through a 0 ms
backend timer that a request cancel or a teardown cancels (`hull._wake`, and
the Lua ssh stream's `close` of a stream another coroutine waits on; audit 18).
**The server TLS context belongs to Keel once `kl_http_server_init` succeeds**
(it copies the `KlTlsConfig`, `ctx_destroy` included, and destroys it in
`kl_http_server_free` after every connection's close_notify); Hull destroys it
only when init fails. Destroying it in Hull too was a double free on every
HTTPS shutdown (audit 15). Covered by `tests/e2e_teardown.sh` (`make
e2e-teardown`: SIGTERM over HTTP and HTTPS with a parked handler, a detached
fetch, a held WebSocket and a Lua finalizer calling `ws.connect`,
`--drain-timeout 100` and `0`; app.main leaving unawaited work) and the
`unawaited` legs of `tests/e2e_cli_poll.sh`.

In JS, app.main's pending promise is watched through the intrinsic
`Promise.prototype.then` (`js_cli_main_watch`), and only when it has the
intrinsic prototype and no own properties - else main fails: Hull called
`p.then` as read, so an own `then` ran app code after the entry parked and
could settle main with any exit code (audit 24).

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
Every runtime flag also takes the spelling --hull-<name> (--hull-d PATH = -d PATH). In a BUILT binary the flags that weaken
the process (--no-sandbox, --allow-degraded-sandbox, --no-ca-bundle/--skip-ca-bundle, --ca-bundle, --no-verify-platform,
--agent, --agent-api, --max-instructions, --max-connections, --read-timeout, --workers, --queue-capacity, --drain-timeout,
-b, -d, -m, -M, -s, --tls-cert, --tls-key, --wasm-gas/-heap/-stack/-timeout-ms/-max-input/-max-output, --body-max-size)
are taken ONLY as --hull-<name>; a bare one is refused, since a built binary cannot tell an operator's option from one of
its app's arguments (include/hull/runtime_flags.h, docs/cli_mode.md). Only the flags the RUNNER implements are reserved:
the app.main runner (serve_cli.c, a built app without HTTP) has no -s/-m/-M/-b/TLS/body/connection/timeout/worker/agent
options, so there those are the app's own arguments; it does take (and reserve) the --wasm-* ceilings. ONE table,
hl_runtime_flags() (runner bits + a DOWNGRADE bit per option), drives both: each parser asks hl_runtime_flag_takes()
before any branch, so it cannot parse an option the table does not give it, and the reservation is derived from the same
rows (test_runtime_flags also scans both parsers' option literals against it). A new runtime flag = a table row. A --hull-<name> the
parser does not take (unknown, or missing its value) is an error, never an app argument, and a one-letter name takes its
value as the next argument (--hull-d PATH; --hull-d=PATH is refused).
--agent-api additionally requires a loopback bind (its endpoints are unauthenticated), and answers only a request whose
Host is localhost / 127.0.0.1 / [::1] (any port) and that carries no Origin - DNS rebinding let a web page read it
through a name resolved to 127.0.0.1 (audit 9; 403 otherwise). /routes answers from the server's own app context
(HlAgentApiCtx.app): it loaded the app again, top level and migrations included, on every request (audit 18).
Global flags: --version / -v (equivalent to hull version), --help / -h (equivalent to hull help), --verbose, --json, --app-dir
```

**Host-aware output (`src/hull/shared/host.c`).** One leaf module owns every
per-host fact a user-facing string depends on: `hl_host_is_windows()` (compile-
time on a native build; an environment probe on a cosmo APE, which is the only
build that reaches Windows), `hl_host_exe_suffix()` (`".com"` on Windows, `""`
elsewhere), `hl_host_render_exec()` (`./app` vs `.\app.com`), and
`hl_host_find_in_path()` (splits PATH on the separator the LIST itself uses,
not the host's - on Windows a Cosmopolitan APE is handed a POSIX-shaped
`/C/a:/C/b`, an MSYS2 shell exports the same, and a native shell gives Win32
`C:\a;C:\b` - joins each hit with that component's own separator, and tries the
`.exe`/`.com` forms on Windows - never a `.bat`/`.cmd`, which cmd.exe would run
and re-parse; `hl_host_is_batch_file`, also refused as a named path by every
spawn, audit 18). It is the ONE PATH walker: a second private
copy in `tools_install.c` was why `hull doctor` and `hull tools list` could
disagree about the same tool on the same box. `hl_driver_resolve_name()`
(`compiler.c`) resolves a bare toolchain name through it before spawning,
because an APE's own exec does not search the PATH Windows hands it; a miss is
-1, not the bare name. Every tool spawn (`cap/tool.c`) resolves a slash-less
`argv[0]` (and a shell-driver's `$0`) the same way and runs the absolute path
(`execv` / `posix_spawn`), refusing a name it does not find: exec's own search
took `.` and empty entries as the working directory, so a repo shipping `nm`
ran it during `hull build` (audit 17). `hull dev` re-runs hull by its own
path (`hl_release_io_self_path`, else a pathed `argv[0]`, else the walker) with
`execv` / `posix_spawn`, for the same reason. Exposed to
the tool VM as
`tool.host_os()` / `tool.exe_suffix()` / `tool.render_exec()`. Every "now run
it" string routes through `render_exec` rather than hard-coding `./x`: neither
PowerShell nor a POSIX shell searches the current directory, so a bare relative
name is never a runnable instruction.

**A `hull` exit status is unusable from a POSIX shell on Windows.** Every
Cosmopolitan APE there reports its status **shifted left by 8** (the raw
`wait()` status, not the exit code), so MSYS2 / Git Bash / Cygwin - which keep
only the low byte - see `0` for every failure: `&&` does not short-circuit and
`set -e` does not abort. Measured with cosmocc 4.0.2 on Windows 11; a two-line
C program does the same, so this is the toolchain, not Hull, and Hull cannot
work around it (`exit(1)` is called correctly). Upstream:
[jart/cosmopolitan#1521](https://github.com/jart/cosmopolitan/issues/1521).
PowerShell (`$LASTEXITCODE -ne 0`, NOT `$?` - measured `True` after a failure)
and cmd (`if errorlevel 1`) still detect failure. **Invariant for contributors:**
a Windows CI step or script must assert the ARTIFACT or the printed output, never
`hull`'s status - `.github/workflows/windows-source-build.yml` follows this in
both jobs, and it is why `make test` on Windows is gated on utest's printed
summary rather than on the binaries' exit codes. User-facing writeup:
[docs/windows_install.md](docs/windows_install.md#exit-codes-on-windows-read-this-before-scripting-hull).

**CLI logging (`src/hull/shared/cli_log.c`).** `hull <subcommand>` and the
`hull <app>` serve path have different audiences, so they configure rxi/log.c
separately. `hl_cli_log_init()` is installed by the dispatcher for subcommands
only (serve.c owns its own callback in `hl_serve_init_logging`). Default
policy: Hull-internal records (a C `__FILE__`) print only at WARN and above and
never with `file:line`; script records (a `.lua`/`.js` chunk name) keep INFO so
`hull test` still shows what the app logged; records emitted inside the
build-time app-evaluation window are suppressed. `--verbose` restores every
level, `file:line`, and tags app-phase records `[build-eval]`. `--verbose` and
`--json` are recognised BOTH before and after the subcommand (`hull build
--verbose` is what people type) and are detected without being consumed, so
handlers still see their own argv unchanged. App text a subcommand prints outside the log -
`hull test`'s file names, test names and error messages, `hull migrate status`'s migration
names - goes through `hl_log_fputs_escaped` (the same escaping as `hl_log_escape_line`): raw,
an ESC / OSC sequence or a newline in a test name forged PASS lines or wrote the clipboard
(OSC 52) (audit 31).

Each command is a separate `.c`/`.h` under `src/hull/commands/`. Adding a new command = one line in the table + one source file.

**`hull init [dir] [--runtime lua|js]`**. Initialize a hull project in-place. Like `git init`: creates missing files (`app.lua`, `tests/`, `migrations/`, `.gitignore`) without touching existing ones. Detects existing runtime from `app.lua`/`app.js` presence. Implemented as a Lua tool module (`stdlib/cli/lua/hull/init.lua`).

**`hull doctor [--json] [--tui] [--fix]`**. Environment check for distribution readiness, and Hull's ONBOARDING surface. Reports hull version/runtime/platform, whether the platform library is embedded (none / single-arch / multi-arch), which system C compilers (`cc`, `gcc`, `clang`, `cosmocc`) are found in PATH (used only for `--compiler=system`, `--with=` features, and cosmo/APE targets - the default `hull build` emit path needs no compiler), and a **Caches** section listing every registered cache kind with status / entries / size / on-disk path (sourced from `hl_cache_registry()` - the same registry that powers `hull cache list`, so doctor and the cache subcommand always agree). Surfaces an active `HULL_CACHE_DIR` override when set. Pure C implementation (`src/hull/commands/doctor.c`). `--json` includes a `"caches"` array and a `"hull_cache_dir"` field for machine-readable output.

Doctor distinguishes **four** states rather than collapsing everything absent into a failure: `✓` present, `✗` REQUIRED and missing (has a fix), `○` optional and absent (nothing is broken), `↳` a system facility is absent but a working FALLBACK is active. The last one exists because an absent SYSTEM CA store is the normal, healthy state on Windows - the embedded Mozilla bundle is the designed fallback, so it is reported as such, and only "neither store" is an error.

**Readiness is compiler-SPECIFIC, not "any compiler".** A cosmo hull (the build that reaches Windows) embeds cosmo-format platform archives, so only `cosmocc` links a working APE; `cc`/`gcc`/`clang` would emit ELF/Mach-O. A native hull is the mirror image. `doctor_build_compiler()` mirrors the preference order in `hl_driver_resolve_native()` (`src/hull/compiler.c`), so doctor's verdict and `hull build`'s behaviour agree; irrelevant compilers render as `○ not used by this hull` instead of three red crosses. Exit 0 iff the platform library is embedded AND the compiler THIS binary can link with is present.

**Hints are platform-aware.** When a Hull-managed fix exists, doctor recommends it over an external package manager - on a cosmo hull that is `hull tools install cosmocc`, which is what makes Windows self-sufficient. `--fix` runs exactly that: it only ever executes a command doctor would have printed, through the ordinary `hull tools install` path (same Ed25519-signed-manifest trust chain, same `~/.hull/tools/` destination), never touches PATH/registry/system directories, and never installs OPTIONAL capabilities (wamrc, gpu) - those are choices, not repairs. `--fix` is mutually exclusive with `--json`. Optional features name Hull-native routes first (`hull feature install gpu` + `hull build --with=gpu`); a `make ...` line appears only labelled as the developer/source-build route, never as a release user's only instruction.

`--json` additionally carries `host_os` (the host this binary is RUNNING on, distinct from `platform`, which is `"cosmo"` for every cosmo build), `exe_suffix`, `build_compiler`, `build_compiler_required`, `fix_command`, and `ca_bundle.effective` / `ca_bundle.ok`.

**`hull build --compiler=<backend>`**. Select how `hull build` produces the app binary. **By default `hull build` is compiler-free**: it emits `app_registry.o` directly via the object emitter (`obj_emit`) and links, needing no C compiler at all. `--compiler=system` (or an explicit compiler path) opts into a system `cc`/`gcc`/`clang` instead; `--with=` features and cosmo/APE targets fall back to the system compiler automatically (they can't go through the emit path). Passing `--compiler=<name>` (e.g. `--compiler=tcc`) treats `<name>` as a plain system compiler resolved from `$PATH` - a user's own `tcc` on `$PATH` still works this way, but tcc is no longer a Hull-provided, vendored, or installable tool. The compiler abstraction uses `HlCompilerVtable` (`include/hull/compiler.h`); the system backend lives in `src/hull/compiler.c`.

**`hull update [--check] [--force] [--channel=stable|beta] [--repo=ORG/NAME]`**. Self-update from GitHub releases. Fetches the latest release metadata via `api.github.com`, picks the asset matching this binary's OS/arch (`hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`, or `hull-cosmo` fallback), downloads via HTTPS using the embedded Mozilla CA bundle (Phase D4), verifies the manifest's Ed25519 signature against the embedded `HL_RELEASE_PUBKEY_HEX` (when configured), verifies SHA-256 against `hull.sha256` from the same release (constant-time compare), and atomically replaces the running binary via `rename(2)`. `--check` exits after the version compare without installing. Pure C implementation in `src/hull/commands/update.c`; shared HTTPS / SHA-256 / manifest plumbing lives in `src/hull/release_io.{c,h}`. A buffer from `hl_release_io_get` (or the verified manifest / signature / version built on it) is freed with `hl_release_io_free`, which frees Keel's `len + 1` allocation (audit 12).

**`hull tools install <name> [--all]` / `tools list [--json]` / `tools uninstall <name>`** (Side-load optional Hull-native tools from GitHub releases into `$HOME/.hull/tools/`. Registered tools: `wamrc` (WAMR AOT compiler, single binary), the `libc-musl-<arch>` static-link floors (data-only bundles for `hull build --linker=lld-static`), and the toolchain-free LINKER `zig` (a self-contained multi-file bundle for `hull build --linker=zig` - a static driver + its libc tree + its own bundled lld, so it runs anywhere). A standalone `lld` bundle is **not** shipped: every binary lld (Homebrew/apt/LLVM-release) is dynamically linked against libLLVM (an absolute-path libLLVM.dylib on macOS), so it can't be bundled flat and run portably - a runnable lld bundle needs a static LLVM source build (tracked follow-up); `hull build --linker=lld` still works against a system/PATH lld. The trust chain is identical to `hull update`) same Ed25519-signed `hull.sha256` manifest covers tool assets, no new keys. Install is version-coupled: it pulls from the SAME release as the running hull binary (not "latest"), so e.g. wamrc stays at the WAMR commit hull was compiled against. Tool registry is a compile-time-constant static table in `src/hull/tools_install.c`; adding a tool means one entry in the registry + a matching release asset in `hull.sha256`. **Three asset shapes:** a single-binary tool ships as `hull-<name>-<platform>` (wamrc); a **bundle** (`.is_bundle`) ships as a `.tar` that extracts to a DIRECTORY `$HOME/.hull/tools/<name>/` and is either single-platform with the arch baked into its name (`hull-<name>.tar`, the floors) or per-platform (`.bundle_per_platform` → `hull-<name>-<platform>.tar`, zig). A bundle's `.bundle_entry` is the exec driver inside the dir that `hl_tools_lookup_path` resolves (`zig`) or a sentinel for a data-only bundle (`crt1.o`); `hl_tar_extract` mkdir-p's the dest so a bundle installs into a fresh `$HOME`. The install record beside the bundle keeps the release's signed `hull.version` (`.hull-bundle.version`), and `tool.bundle_verify` requires the cached manifest to name THIS hull's release, as `tool.platform_verify` does for feature libs (audit 17; a bundle installed earlier is reinstalled once, and a non-release hull verifies no cached bundle). Cosmo unsupported for native-toolchain tools (a fat APE can't drive a native zig tree; cosmo users build from source). The download cap (`release_io.c` `max_response_size`) is 512 MB to fit the ~330 MB zig bundle. Pure C; consumers locate installed tools via `hl_tools_lookup_path()` (or `tool.find_tool()` from build-tool Lua) which checks `~/.hull/tools/<name>/<bundle_entry>` (bundles) → `~/.hull/tools/<name>` → `dirname(hull_exe)/` → `$PATH`. Release producer: `scripts/build_zig_bundle.sh` (repacks the official ziglang.org tree). Full design: [docs/tools_install.md](docs/tools_install.md).

The live install path is intentionally not tested in CI (it would need the just-published release to exist before publishing). The post-release validation step is `tests/release_smoke.sh`: install hull, run `sh tests/release_smoke.sh`, watch `hull tools install wamrc` actually fetch the asset, verify SHA-256, exercise `wamrc --help`, and uninstall cleanly. Run it manually after every `gh release create`. (Phase 4.3 removed the pre-built pure-compute flavor lib, so there is no longer a `hull flavor install pure-compute` to smoke-test.)

**`hull flavor install <flavor> [--repo=ORG/NAME]` / `flavor list`**. Since Phase 4.3 the only flavors are `full` (embedded) and `pure-compute` (a preset on the default composable base), and neither is a fetchable per-flavor platform lib: `hull flavor install pure-compute` reports "preset flavor, nothing to install", and `hull flavor list` shows `full` as `embedded` and `pure-compute` as `preset (default base)`. The fetch/verify machinery (the shared `hl_release_io_fetch_verified_manifest`, atomic-write to `$HOME/.hull/platform/`, build-time re-verify) still exists in `src/hull/commands/flavor.c` for any FUTURE non-preset flavor with a non-empty asset stem, but has no user today (`flavor.c` treats an empty-asset flavor as a preset). Pure C, HTTP-client-gated. Full design: [docs/build_flavors.md](docs/build_flavors.md).

**`hull feature install <name> [--repo=ORG/NAME]` / `feature list` / `feature uninstall <name>`**. Fetch the per-feature composable library (`libhull_feature-<name>-<arch>.a`) for `hull build --with=<name>` so end users do not build it from source. Same trust chain as `hull flavor install` (the shared `hl_release_io_fetch_verified_manifest`: verify the Ed25519 signature on `hull.sha256` against the embedded `HL_RELEASE_PUBKEY_HEX`, then per asset look up its SHA-256 in the verified manifest, download, constant-time-compare, atomic-write to `$HOME/.hull/feature/`). Five features today: `duckdb`, `postgres`, `mysql`, `gpu`, `tui`, all native-only (features are static archives; cosmo is never published). `feature list` shows each as `not installed` / `installed` for this platform. Registry is the `FEATURES[]` table in `src/hull/commands/feature.c` (one row per feature). Pure C, HTTP-client-gated. See "Composable features" above and [docs/features_and_flavors.md](docs/features_and_flavors.md).

**`hull cache list|prune|clear|verify`**. Inspect and manage the runtime cache pool (`$HOME/.hull/blobs/runtime/` by default - set `HULL_CACHE_DIR=/abs/path` to redirect for per-app isolation). Full reference: [docs/cache.md](docs/cache.md). Six registered kinds today: `lua-bytecode` (Lua source → bytecode), `js-bytecode` (QuickJS module bytecode), `compute-aot` (WASM AOT artifacts), `templates` (compiled Lua template render functions), `js-templates` (compiled JS template render functions), `tools` (signed side-loaded tool binaries; system store, not pruned by default). `list` enumerates every kind with entry count + total size + on-disk path; `--json` for machine output. `prune [--kind=K] [--max-size=N] [--max-age=N] [--strategy=lru|fifo] [--dry-run]` runs LRU/FIFO eviction over runtime caches; system stores are preserved unless `--kind=tools` is named. `--max-size` accepts `K`/`M`/`G` suffixes (binary, 1024-based; trailing `B` optional, so `100M` and `100MB` are equivalent); `--max-age` accepts `s`/`m`/`h`/`d`/`w`/`y` suffixes (bare numbers = seconds for back-compat). Bad units are rejected with an example. `clear --yes` wipes runtime caches entirely (iter + delete, not policy-based - so even sub-second-old files go). `verify [--kind=K] [--repair] [--json]` walks every entry and flags corruption: for CAS-mode kinds (`tools`) it recomputes `sha256(contents)` and compares to filename; for keyed-mode runtime caches it does a structural check (regular file, non-empty, readable). `--repair` unlinks corrupt entries - safe because the next compile/install repopulates from source. Exits non-zero on corruption unless `--repair` was able to fix it. Cache registry (`include/hull/cache_registry.h`) is the single source of truth for cache kinds, also consumed by `hull doctor` (Caches section), `hull inspect` (runtime-caches disclosure), and `hull cache verify` (CAS vs keyed mode dispatch via `is_cas` field). Adding a new cache kind = one entry in `REGISTRY[]` and the new consumer auto-shows up in list / prune / clear / verify / doctor / inspect.

**Cache eviction is manual.** No automatic TTL / background sweep / on-write cap. Stale entries are harmless (content-keyed → orphans never serve incorrect data), and the caches are tiny per entry, so correctness doesn't depend on freshness. The only automatic hygiene is `hl_blob_store_open`'s `tmp_max_age_sec` sweep (default 1 hour) which removes abandoned `tmp/.blob-*.tmp` files from crashed writers. `hull doctor` surfaces a `⚠ large` mark next to any cache kind that passes 250 MB or runtime total past 1 GB and prints an actionable `hull cache prune --max-age=30d --strategy=lru` hint - but does not act on it. Disk-pressure users who want fully automatic eviction wire `hull cache prune` into cron / a systemd timer.

**`HULL_CACHE_DIR` (per-app cache isolation).** (Refused when it is `/`, `$HOME` or above, `~/.hull` or above, or inside `~/.hull` - checked before the directory is made, through a path's nearest existing ancestor, and matching a symlinked `~/.hull` through its target as well as by name; audit 12. A `.` or `..` component is refused outright, before anything is made, and a path or home that cannot be resolved counts as covering / inside `~/.hull`; audit 14. With neither `HOME` nor `USERPROFILE` set to an absolute path (a relative value is ignored: it resolved against the app directory), the home is the password database's (`hl_host_home`, off Windows), and a home that cannot be found makes every tool-mode grant too broad (`hl_tool_path_too_broad`; the app sandbox's app-directory check still accepts an ordinary directory); audit 15.) `HULL_CACHE_DIR=/absolute/path` redirects the entire runtime cache pool from `$HOME/.hull/blobs/runtime/` to the given directory. Use on multi-tenant boxes or under systemd / k8s / Docker so each deployment has its own cache and never reads / writes another deployment's blobs. Must be an absolute path; the sandbox auto-allows the resolved path (read-write-create, in the app AND tool sandboxes), so `/`, a drive root, `$HOME` or above, `~/.hull` or above (the cache keys), and anything inside `~/.hull` (the keys, the tools store; audit 10, `hl_host_path_under_home`) are refused with the caches off and one WARN (audit 9; `hl_host_path_too_broad` / `hl_host_path_covers_home`). All `HULL_NO_*_CACHE` opt-outs (below) still apply on top. The tools store (`$HOME/.hull/blobs/tools/`) is intentionally NOT redirected - those are signed durable downloads with a stable system home, not per-app caches. Layer C (automatic per-app isolation derived from app identity) is a planned follow-up; the override here is the manual / deployment-controlled equivalent. See [docs/blob.md §"Per-app cache isolation"](docs/blob.md).

### Cache environment variables

`HULL_NO_CACHE` disables every runtime cache (the tools store is unaffected);
`HULL_NO_<KIND>_CACHE` disables one kind (`LUA_BYTECODE`, `JS_BYTECODE`, `AOT`,
`TEMPLATE`, `JS_TEMPLATE`), derived from the registry's `env_kind` field, so a new
`REGISTRY[]` entry gets its variable and `hull cache list` Status column for free.
Checked on every access (not memoized). Truthiness (`hl_hull_cache_disabled`,
`src/hull/cache_dir.c`): unset / empty / `0` / `false` / leading `f`/`F` → off,
anything else → on. `HULL_CACHE_DIR` is a path override, not an opt-out. Full
table: [docs/cache.md](docs/cache.md).

**`hull help` / `hull --help` / `hull -h`**. Print top-level usage grouping every registered subcommand by purpose (Scaffolding, Build & ship, Develop & test, Diagnostics, Compute / WASM, Database, Deployment, Self-management). Implementation in `src/hull/commands/help.c`. Build-time-gated commands (HL_ENABLE_HTTP_SERVER, HL_ENABLE_DB, HL_ENABLE_HTTP_CLIENT) are suppressed when the corresponding flag is off so help only advertises what this binary can do. The output ends with a "for AI agents" pointer at `hull agent context --task=orientation --level=minimal`, matched by similar breadcrumbs in `hull`'s bare-mode usage and the `install.sh` postscript.

**`hull agent tools`** (Generic JSON dump of the tool registry crossed with the install state on this host (`{platform, tools: [{name, available_for_platform, installed, path, asset_name, install_hint}, ...]}`). Independent of `HL_ENABLE_HTTP_CLIENT`) even CLI-flavor builds without the installer can list what's registered. The compute-specific wamrc state is also surfaced inside `hull agent compute` under a `wamrc` block so agents reading per-subsystem panels see the actionable hint without making a second call.

**`hull agent context --list [--json]`**. Enumerate every context: task in the embedded platform stdlib registry plus which level markers each one populates. Cold-start agents call this first to discover what `--task=NAME` values are valid. The bare `hull agent context` form (no `--task`, no `--list`) errors with a usage message that points at `--list`. Topic docs live in `stdlib/context/*.md` and are auto-discovered by the Makefile + xxd embedding pipeline.

**`hull agent overview [app_dir]`**. Single-shot composite project summary an agent can read when dropped into an unfamiliar app dir. Composes runtime detection, route stats (count + methods + ws/sse flags), compute modules + AOT readiness + wamrc state, GPU shaders, migrations, declared modules, tests, and a `build_ready` flag. No DB connection; agents needing pending-migration counts call `hull agent migrate` separately.

**`hull agent inspect [app_dir]`**. Emit the **project source-discovery** model (annotated declarations) as versioned JSON. Delegates to the Lua tool VM (`hull.project.inspect` → `hull.project.analyze`) - the one canonical, host-owned analyzer; it never re-scans or parses source itself. Standalone by default; when a live `hull dev --agent` session has published a generation, the C dispatcher (`cmd_inspect`) streams that generation instead (bound to the session by `session_pid` + `kill(pid,0)` liveness + a full `sh_json_parse` envelope validation). At most one positional root (`inspect a b` → exit 2). Exit 0 whenever a discovery is produced - validity is data in the JSON (`valid` / `complete`). This is a CLI-only agent subcommand (not MCP-wired). See "Project source discovery" below and [docs/project_discovery_design.md](docs/project_discovery_design.md).

### Project source discovery (`hull.source.*` + `hull.project.*`)

Hull statically analyzes an app's **source WITHOUT executing it**, producing a canonical
representation of annotated declarations that a future codegen step (Query/Compute IR) can
consume. Both frontends ship: the Lua analysis brain is pure Lua in the sandboxed tool VM;
the JavaScript frontend runs in a bundled QuickJS tooling session (it never executes app JS).
Two layers:

- **`hull.source.*`** (`stdlib/cli/lua/hull/source/`) - the language-analysis layer: a
  pure-Lua recursive-descent Lua 5.4 lexer/parser (NEVER `load()`), half-open byte ranges,
  diagnostics, the generic `---@` annotation attach (to `local_declaration` /
  `function_declaration` only), a scope/binding resolver, and the `hull analyze` lint engine.
  Public contract `hull.source.lua` (`parse(source, {path?}) -> (unit, err)`, never raises).
  `hull.source.discover` is the shared hardened bounded walker (extracted from `hull analyze`;
  exclude-dirs pruned during traversal, canonical containment, deterministic/regular/no-symlink)
  - the ONE recursive source walker in Hull. Design: [docs/lua_source_analysis_design.md](docs/lua_source_analysis_design.md), [docs/hull_analyze_design.md](docs/hull_analyze_design.md).
- **`hull.project.*`** (`stdlib/cli/lua/hull/project/`) - the frontend-neutral project layer
  on top: `registry` (extension → frontend map; `lua` and `js`/`mjs`/`cjs` are both analyzable
  frontends, `analyzable` computed per build from `tool.frontend_available(engine)` - JS is on
  by default and reported unsupported only on a `RUNTIME=lua` build; a `.js` is never parsed as
  Lua), `frontend_lua` (knows Lua AST layouts) + the JS frontend (`stdlib/cli/js/hull/source/`,
  run in a QuickJS tool session via `HL_FRONTEND_JS`), `model` (the `ProjectDiscovery` -
  deterministic textual IDs, per-name facts sharing
  a `group_id` with each annotation carrying `target_group_id` + `frontend`, annotated-only
  public `declarations[]`, independent `valid` / `complete` axes, `by_annotation`/`by_source`/
  `by_language`/`by_id`/`annotated` indexes), `analyze` (the single canonical host analyzer:
  root canonicalization, a `pcall` boundary that converts any internal defect into a
  `project.internal` invalid discovery, a generation-unique handle table + `resolve_handle`),
  `projection` (the one side-effect-free wire-schema, dropping all generation-internal state),
  `inspect` (the `hull agent inspect` read/standalone entry), and `publish` (the internal
  dev-generation publisher). Design: [docs/project_discovery_design.md](docs/project_discovery_design.md).

`hull dev --agent` publishes a `.hull/discovery.json` **generation** per (re)spawn (fresh
analysis, atomic tmp+rename, tagged with the supervisor `session_pid`); `hull agent inspect`
serves that live generation or, absent a live session, runs one standalone analysis - same
schema either way (`source: "dev"` vs `"standalone"`). The build seam is documented + tested
but **abstraction-only**: `build.lua` is unchanged and does no per-build parse; a future
lowering consumer calls `hull.project.analyze(app_dir)` in `main()` (after `parse_args()`,
before `discover()`). ~180 lines of hardened C (in `agent.c` + `dev.c`) handle only the
live-read/sidecar/publish glue; Lua source parsing stays in Lua and JavaScript parses in the
bundled QuickJS tool session (never the app runtime), not in C. Tests:
`stdlib/cli/lua/hull/project/tests/test_project.lua` (UTEST leg `project_discovery`) +
`tests/e2e_project_discovery.sh` (`make e2e-project-discovery`, incl. a live backgrounded
`hull dev --agent`).

**`hull sign-release <manifest> --key <secret_key>`**. Sign a release manifest (typically `hull.sha256`) with an Ed25519 secret key. Writes `<manifest>.sig` (128 hex chars). Used by the GitHub Actions release workflow; never invoked by end users. See [docs/release_signing.md](docs/release_signing.md).

**`hull verify-release <manifest> <signature> [--pubkey <hex>]`**. Verify an Ed25519 signature over a release manifest using the embedded release public key (or an explicit override). Exit 0 = valid, 1 = invalid / placeholder. For offline auditing of a downloaded release.

**`hull compute new <name> [--lang c]`**. Scaffold a new WASM compute module under `compute/<name>/`. Writes `<name>.c` (with a 30-line `hull_process` skeleton), `hull_compute.h` (the freestanding ABI header with libc shim + bump allocator + UDF wire format), and `test_fixtures.json`. Errors if the directory already exists. Names must match `[A-Za-z0-9_-]+`; a module found under `compute/` with any other name is refused by `hull compute build` / `refresh-header` and `hull build`, nothing compiled (audit 18). C is the only supported language today.

**`hull compute build [name]`**. Compile `compute/<name>/<name>.c` → `compute/<name>.wasm` via `clang --target=wasm32-unknown-unknown -nostdlib -O2 -flto`. With no name, builds every discovered module. Toolchain lookup prefers Homebrew `llvm@18` then falls back to system `clang` with `wasm-ld` in PATH. The same logic runs automatically as part of `hull build` for stale sources. `hull compute build` is the manual rebuild entry point. Implementation: `stdlib/cli/lua/hull/compute.lua` + the shared helper at `stdlib/cli/lua/hull/compute_build.lua`.

**`hull compute test <name>`**. Run JSON fixtures from `compute/<name>/test_fixtures.json` against `compute/<name>.wasm`. Each fixture has `{name, input, expect_status}`; the runner generates a tempdir app with a `GET /call?input=...` route that calls `compute.call("<name>", input)`, then exercises it via `hull test`. Fixtures use the exact same `compute.call` codepath that production handlers use.

**`hull compute check <name>`**. Validate that `compute/<name>.wasm` has the correct WASM magic, has version 1, and actually loads in WAMR with a trivial input. The "yes, this module can run inside Hull" gate.

**`hull compute refresh-header [name]`**. Overwrite the per-module `compute/<name>/hull_compute.h` with the canonical version embedded in the `hull` binary. With no name, refreshes every discovered module. Run after upgrading Hull when a new ABI helper has been added.

### `hull build` output naming and artifact hygiene

**The produced binary is named for the host that must run it.** The default
output is `app_dir/app` + `hl_host_exe_suffix()`, i.e. `app` on Linux/macOS
(unchanged) and **`app.com` on Windows**. Windows will not execute an
extensionless file, and `.com` is the Cosmopolitan APE convention - the same
reason `install.ps1` installs Hull itself as `hull.com`. An explicit `-o` /
`--output` is honoured verbatim, so cross-producing an artifact for another
host stays under the caller's control.

**Asset directories are the app's own.** `hull build` refuses an app whose
`templates/`, `static/`, `migrations/`, `compute/` or `shaders/` is a symbolic
link (`tool.is_symlink`): `find_files` skips links below its root but followed
the root, so `static -> ..` embedded and served the invocation directory's
other projects, and a linked `migrations/` ran another tree's SQL - the dev
server already refused the layout (audit 20). An error raised inside the
`--sign` step (a malformed `platform.sig`) now goes through the same cleanup
as every other signing failure, so no unsigned binary is left at the output.

**One obvious shippable executable in the app root.** cosmocc emits debug
sidecars beside the APE (`<base>.com.dbg`, an x86_64 ELF with symbols, and
`<base>.aarch64.elf`). Neither is shippable and nothing reads them after the
build, so `hull build` moves them into `<app_dir>/.hull/build/`. The move runs
AFTER the post-link `nm` symbol verification (which reads the `.dbg` ELF in
place) and is non-fatal; `--keep-build-artifacts` restores the old layout. The
shippable binary itself never moves, so signing, `hull verify` and `hull
inspect` - all of which address the output path - are unaffected.

**"dual-arch" vs "single-arch" name two different layers.** `hull doctor`'s
*Platform library* line counts the platform ARCHIVES this hull carries to link
apps against; `hull build`'s message describes the APE LINK, which fuses an
x86_64 and an aarch64 image into one file. Both now say which layer they mean.
(A related bug made them look contradictory: `hl_build_get_platforms(NULL)`
returned 0 for a NULL `out`, so a genuine multi-arch cosmo build reported as
`single-arch`. `out` is now optional and the count is always returned.)

### `hull build` and compute modules

`hull build` makes compute modules first-class artifacts:

- **Step 1. Auto-rebuild from source.** Before the existing discovery pass, `build.lua` calls `cbuild.build_all(app_dir, "stale")`. Any `compute/<name>/<name>.c` whose mtime is newer than the matching `.wasm` is recompiled inline using the same clang invocation as `hull compute build`. Reports as `hull build: compiled N compute source(s): <names>`. If clang is missing AND something is stale, errors with a fix-it hint. Default ON; `--no-build-compute` opts out (for hermetic CI builds shipping pre-committed `.wasm` artifacts). A module may read no file from outside its own directory (a cloned app's auto-rebuild could otherwise copy `~/.ssh/id_rsa` into the signed `.wasm`): `compute_build.lua` first runs `clang -E -MD -MF`, refuses any dependency that does not resolve (realpath) inside `compute/<name>/` and any `#embed`'ed file (a dependency with no line marker in the preprocessed text), scans the preprocessed text - string escapes decoded, adjacent literals joined - for `incbin` / an asm `.include` and for any inline assembly at all (an `asm` / `__asm` / `__asm__` token outside a literal: GNU-as macros build `.incbin` at assembly time; audit 29), and then compiles that checked text (`-x cpp-output`). A lexical scan of the sources was bypassable (an `#include`d `.txt`, the `%:embed` digraph, `".inc\x62in"`; audit 15). The preprocessed text may hold no directive but line markers and a short allowlist of loop / diagnostic / pack pragmas (and no `_Pragma`), since the `-x cpp-output` compile still obeys directives and a macro can make one (`#define E # embed "/abs/secret"`, then a line `E`); and that compile step writes its own dependency list (`-Xclang -dependency-file`: the driver ignores `-MD` for cpp-output input), so anything it read besides `pre.i` deletes the output (audit 16).
- **Step 2. AOT compilation.** When `wamrc` is available, every `compute/*.wasm` is AOT-compiled to `compute/*.aot.<arch>`. Cosmocc builds produce both `x86_64` and `aarch64` AOT files. `--no-aot` skips this.
- **Step 3. Embed both.** `.wasm` (interpreter fallback) and `.aot.<arch>` (preferred at runtime) are embedded into the unified app VFS alongside templates, static files, and migrations. Only AOT this build compiled with its own wamrc is embedded: a committed `compute/*.aot.*` is left out (named in the output) and AOT code saved as a `.wasm` fails the build, unless `--trust-prebuilt-aot` - committed AOT is native code from whoever wrote the tree, and its stamp only says it claims to come from Hull's wamrc (audit 19). The runtime never loads AOT from disk: in `hull dev` / `hull test` / `hull agent` a module runs its `.wasm` in the interpreter (so a Memory64 module, AOT-only, runs only in a built binary), and AOT bytes under a `.wasm` name on disk are refused. That development fallback opens `compute/<name>.wasm` under the app root (`hl_fs_fopen_read`: re-rooted symlinks, an O_NONBLOCK leaf, regular files only); a plain fopen followed a symlink anywhere and blocked on a FIFO (audit 24).

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
hull agent inspect [app_dir]             # analyzed project model: annotated declarations (JSON)
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

**Convention:** `migrations/*.sql` files numbered `001_`, `002_`, etc. Each runs in `BEGIN IMMEDIATE` / `COMMIT`. The `_hull_migrations` table tracks applied migrations (name + checksum + timestamp; the checksum column is added to an older table on first use and backfilled). An applied migration whose SQL has changed is NOT re-run - startup logs a loud WARN naming it (put schema changes in a new migration). Opt out with `--no-migrate`. Migration SQL is app SQL: a file that names a `_hull_*` table (or uses a Postgres `U&"..."` identifier) is refused by the same `hl_cap_db_check_namespace` check `conn.exec` applies, and the run fails (audit 9 M1); Hull's own tracking statements do not go through it. Migrations are not app runs: they are not charged to an instruction budget (`hl_db_budget_swap(NULL, NULL)`). `hull migrate` names a database it cannot open by scheme and host only (`hl_db_dsn_redact`), never with the DSN's password (audit 9 L4); when an `@` follows the authority (a password with an unencoded `/`, `?` or `#`, or an `@` in the path / query) it prints the scheme alone (audit 10). The kernel sandbox is handed the FILE a `-d` DSN names (`hl_sandbox_db_path`, NULL for a network or in-memory database) by both entry points - the `app.main` runner (`serve_cli.c`) passed the raw DSN, so a network DSN's password reached unveil and the sandbox log (audit 10). A built binary runs only the migrations embedded in it: the `<app_dir>/migrations` filesystem fallback (and the static-file, template, and Lua `require` / JS `import` module ones - audit 17) is for development, when the app VFS is empty (`hl_vfs_embedded`, `include/hull/vfs.h`, the one test every disk loader uses), so SQL, a template or a module placed beside a built binary never runs. Only top-level `migrations/*.sql` are migrations, embedded or on disk: `hull build` skips (and names) a nested `.sql` and `hl_migrate_run` ignores an embedded name with a `/` past the prefix - `migrations/down/` ran only in the built binary (audit 25); the agent's schema-diff takes the same set (`hl_migrate_embedded_name`, audit 27). In development `migrations/` is opened from the app root without following a symlink, and each `.sql` with `O_NOFOLLOW` (regular files only): a symlinked `migrations/` (or `.sql`) fails the run - dev, `hull migrate`, `hull test` and the agent ran a cloned app's `migrations -> /elsewhere` before the sandbox applied, while `hull build` already refused it (audit 26).

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

**Interrupted fat builds repair themselves.** Only the x86_64 half of each pair
(`foo.o`, `libkeel.a`) is ever named as a make target - nothing names the
`.aarch64/` counterpart - so make cannot tell that half a pair is missing. A
build stopped part-way therefore used to leave an orphan that every later `make`
skipped as up to date, dying at the fat link on `linker input missing
concomitant .aarch64/libkeel.a` until someone ran `make clean`. When `CC` is
exactly `cosmocc`, a parse-time hook runs `scripts/cosmo_fat_repair.sh` over
`build/` and `vendor/keel/` (plus the two paired archives, named explicitly -
`build/libhull_platform.a` is deliberately single-arch even under cosmocc) and
deletes any orphan so the ordinary rules rebuild both halves. In steady state it
is a no-op. Its blast radius is gated by `make check-cosmo-fat-repair`.

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
1. Installs cosmocc 4.0.2 with `make fetch-cosmocc` (`mk/fetch.mk`): `cosmo.zip`, falling back to the
   upstream GitHub release, the zip's SHA-256 pinned either way. The Windows workflows fetch the same
   pinned zip from the GitHub release first, `cosmo.zip` second (#850); the hermetic job bakes it into its image.
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

Under `network_outbound` the kernel sandbox unveils only the named resolver files (`/etc/resolv.conf`, `hosts`, `nsswitch.conf`, `services`, `gai.conf`, `host.conf`, the systemd-resolved / nscd sockets); it also unveiled all of `/etc` (`shadow`, SSH host keys; audit 24). CA bundles are read into memory before the sandbox applies. Every `gpu.devices` entry counts as listed whatever its type, and a `devices` value that is not a list disables gpu, so a list with no valid index fails closed instead of granting every device (audit 24).

**`--verify-sig` enforces the signed policy.** The runtime re-derives its manifest by running
app code, so after load it compares the manifest the app actually declared (`app.manifest()`
keeps a plain deep copy - no metatables in Lua, frozen and non-writable in JS; in Lua app code
cannot reach it, `app.get_manifest()` returns another copy) with the signed `manifest`,
structurally, and the resolved module set with `modules_resolved`. Both that JSON and the
JSON `hull build` signs are encoded in C from the stored copy, never by app-reachable code: in Lua
with raw accessors (`runtime/lua/manifest_json.c`), not the app-replaceable `json` module; in JS
from own data properties (`hl_manifest_json_js`, `manifest_js.c`), not `JSON.stringify` (which
follows `Object.prototype.toJSON`). In JS the stored copy is held in C (`HlJS.manifest`), and the
policy extractor and the encoder both read that one value, as own properties only (an array where
an object belongs counts as absent, and an array manifest is refused) - never
`globalThis.__hull_manifest`, which is a non-configurable, setter-less getter onto it reserved
before app code runs, so an app cannot define its own (a Proxy showing the encoder one policy and
the extractor another) (`hl_sig_check_runtime_policy`); any difference refuses to start. The JS
runtime's other internal tables (`__hull_routes`, `__hull_route_defs`, `__hull_middleware`,
`__hull_post_middleware`, `__hull_timers`, `__hull_timer_defs`, `__hull_ws_defs`, `__hull_sse_defs`,
`__hull_main`, `__hull_app_ref`, `__hull_test_state`) live on a store held in C as well
(`hl_js_internals`, `HlJS.internals`), not on `globalThis`: as globals, app code could define them
and have routes or timers wired without declaring `hull/http-server` or `hull/timers` (audit 15).
The Lua runtime keeps them in the registry. The signature is verified,
and the phase-1 sandbox applied, before the app context runs migrations. §5b also requires the
per-arch `arch_hashes` (the platform archive the build cross-checked) to be present and to match
the signed manifest - a `--no-verify-platform` build carries no `gethull` block and needs
`--no-verify-platform` at run time too. Every embedded VFS entry (compute WASM, AOT code,
shaders, templates, static files, migrations) must be signed - matched by name the way
`build.lua` embeds them (a `./` entry is a module: its rest, or its rest + `.lua`; a bare
entry such as `templates/base.html` matches exactly) and hashed. Whatever the runtime still
reads from DISK under `--verify-sig` must be signed too: `hl_verify_startup` arms the loaders'
signed-file gate (`hl_vfs_disk_gate_*`, `include/hull/vfs.h`), and every disk loader - Lua /
JS modules, templates, static files, migrations, shaders, compute `.wasm` / `.aot.*` - re-hashes
the bytes it read against the signature and refuses anything else (a module planted beside
the app, a file swapped after the startup check). It loads no bytecode or compiled template
from the shared code cache (`hl_runtime_cache_code_off`, audit 31), since what the checks hashed
is the source, not the cached code. The filesystem-mode startup check also
refuses an unsigned file in `migrations/`, `compute/`, `shaders/`, `templates/` and
`static/`. A built binary never loads compute modules or shaders from its working directory
(as for migrations and static files). `hull verify` also checks `binary_hash` against the
built binary (`--binary PATH`, default `<app_dir>/app`; hashed by streaming, `tool.sha256_file`,
as `hull build --sign` computes it, so a binary larger than the tool VM's 64 MB heap is
fine), checks AOT entries only through that binary (a committed `compute/*.aot.*` on disk is not what the build signed; audit 25), fails when AOT entries could only be checked through a binary it did not find, refuses
an unsigned file in `migrations/`, `compute/`, `shaders/`, `templates/` or `static/` (the scan
`--verify-sig` runs), and applies the §5b / §5c gethull checks. Without
`--developer-key` the app layer is only self-consistent (the key is the one in
`package.sig`), and it says so in a warning (audit 17; exit status unchanged). Like `hl_sig_read`, it fails a
legacy signature (no `binary_hash`) that carries `platform` / `modules_resolved` /
`trampoline_hash` / `build`, which that signature does not cover (audit 15). `--verify-sig`
treats a malformed `platform.public_key`, or a `composed` / `composed.platform_domain` that is
present but not an object, as an error rather than a reason to skip the check (audit 15), and
a dev-mode static file's ETag 304 is answered only after the disk gate passed it.

**The app directory may not be `/`, or `$HOME` or above.** A built binary takes its app
directory from its working directory; `hl_sandbox_apply` refuses `/` (that unveiled the whole
filesystem) and the user's home or any ancestor of it (every file the user owns) - run it from
its own directory (`WorkingDirectory=`, `WORKDIR`; the generated Dockerfile sets `WORKDIR /srv`).
One rule with the tool sandbox (`hl_host_path_too_broad`, `shared/host.c`): home is `$HOME`, else
`$USERPROFILE`, compared without case on Windows, and a bare drive root (`/C`) is a root (audit 9).
A top-level `fs.write` glob (`"*.log"`) can only be granted as the whole app directory at the kernel
level; that is warned about, as for a top-level file that does not exist yet - put written files in a
subdirectory.
Without `-d`, the database is `<app_dir>/data.db` - the one `hull migrate` opens. That implicit default (and its `-wal` / `-shm` / `-journal`) must be absent or a regular file, not a symlink (`hl_default_db_path_check`, every entry point that computes it: serve, `hull migrate`, the agent, `migrate status --tui`); a cloned app's `data.db -> ../other.sqlite` was migrated. An explicit `-d` is followed (audit 25).

**Tool-mode sandbox (`hl_tool_sandbox_init`).** Kernel unveil only where it enforces (OpenBSD,
Linux with Landlock, a cosmo APE on those two hosts), and a failed unveil there is fatal,
not logged-and-ignored. Pledge applies on OpenBSD only: the Linux polyfill refuses `exec`
without execpromises, which would be a seccomp filter on every spawned compiler/linker, so
on Linux tool mode rests on Landlock unveil plus the spawn allowlist. Elsewhere
(macOS, Windows, the other BSDs) only the userspace allowlist the tool bindings check applies,
and the log says so. That allowlist reaches the bindings through `HlLua.tool_unveil_ctx`,
which `hl_lua_init` keeps for the tool VM (it used to zero it, so every binding saw NULL and
the allowlist was never checked). Both lists come from ONE plan (`hl_tool_sandbox_plan`,
applied to the userspace ctx and as kernel unveil), so they cannot drift - see
[docs/security.md](docs/security.md) "Tool mode" for the grant table. The app directory is
the first positional argument that names an existing directory and is not a symlink
(`hull deploy dockerfile <dir>`, not "dockerfile"; option values such as `--install-dir`
skipped; a planted `build -> ~/.ssh` is not taken), granted read-only, and never when it is
`/` or `$HOME` or above. The invocation directory is never granted when it is `/`, nor when
it is `$HOME` or above; the output directory (`-o`'s, else the named app dir) that is `/` or
`$HOME` or above is refused rather than made writable, and neither is hull's own directory
granted when it is that broad. `hull new <name>` / `hull init [dir]` get their target, which
`hull_tool` creates before the sandbox applies (exposed as `tool.scaffold_dir`), so
`cd ~ && hull new myapp` grants `~/myapp` and never `~`. Read-only grants also cover
`~/.hull/feature`, `~/.hull/platform`, `~/.hull/blobs/tools`, and each FILE named by
`--sign` / `--platform-sig` / `--platform-key` / `--developer-key` / `--gethull-key` /
`--binary` (a key in `~/.hull/keys` is readable; its directory is not). `$HOME` comparisons
ignore case on Windows; the unveil check refuses `..` in any spelling (backslash pieces too)
in the not-yet-existing tail of a path; `tool.rename` needs write + create on both ends; and
the PATH walk (`hl_host_find_in_path_ex`) skips `.` and relative components. A spawned tool's own PATH is reduced the same way (`hl_host_path_absolute_only`, applied by `tool_child_env` in `cap/tool.c` to every spawn path, cosmo's `posix_spawn` included): the child searches PATH itself (gcc runs `as` / collect2 runs `ld` through it, an empty entry meaning `./`; clang honours `.`), so a repo shipping `as` or `ld` ran during `hull build` (audit 24). hull's own directory is in the
kernel list as well as the userspace one (the manifest-extraction re-exec), and the cache seal
keys are loaded before the sandbox applies. A cosmo APE on a Linux kernel without Landlock
(unveil fails ENOSYS) runs with no kernel tool sandbox, as a native build there does. Tool writes
(`tool.write_file`, `tool.copy`, the embedded-archive extraction `write_blob`) do not follow a
symlink at the destination. Every tool binding that writes or reads a path checks the allowlist -
since audit 10 that includes `tool.extract_platform*` / `extract_feature_*` (the dir),
`compiler.compile`'s object, `linker.link`'s output, `tool.tmpdir`'s base and
`extract_manifest_lua` / `_js`'s entry - and the tool VM has no `load` / `loadfile` / `dofile`
(app code is only run through `extract_manifest_*`, in a fresh sandboxed runtime). The unveil
check canonicalises a not-yet-existing path through its nearest existing ancestor (a raw
`/tmp/../x` no longer passes the `/tmp` prefix), and refuses a path whose not-yet-existing
tail holds a symbolic link, leaf included: a dangling `app -> ~/.local/bin/git` passed as its
nominal spelling and the linker wrote through it (audit 32; `hull build` also refuses an output
that is a link). The tool VM's writers - `print`, `tool.stderr`, `tool.stdout`, and the
dispatcher's report of a plugin's error (`hull_tool`) - escape every control byte but LF and tab
(`hl_term_fwrite_escaped`, `include/hull/shared/term_escape.h`: CR as `\r`, ESC / NUL / the other
C0 and DEL as `\xNN`, UTF-8 unchanged), so a file name, manifest string or `package.sig` field of
a cloned tree cannot rewrite the terminal or set the clipboard through any command (audit 33:
`hull verify` / `inspect` / `deploy` / `modules` / `check` / `analyze` / `compute build` / `eject`
printed them raw; audit 32 had escaped call sites one by one). `tool.stderr_raw` /
`tool.stdout_raw` write verbatim, only for text the plugin encoded itself (json.encode output,
which escapes C0 but keeps DEL raw, legal in JSON). `tool.escape` still puts app text on one line
(LF as `\n`); its output holds no control byte, so the writers leave it unchanged. Nothing a build or eject executes or links is
taken from the working directory (`./build/wamrc`, `./build/libhull_platform*.a`,
`./build/platform.sig`); `hull build --platform-sig PATH` names a platform.sig explicitly.

**A cosmo APE only gets a kernel sandbox on Linux and OpenBSD.** Cosmopolitan's
`pledge()`/`unveil()` enforce where the host gives them a mechanism (seccomp-bpf
+ Landlock on Linux, the native syscalls on OpenBSD); on Windows, macOS and the
other BSDs both calls return 0 and do nothing. Measured with cosmocc 4.0.2 on
Windows 11: `pledge("stdio", NULL)` returns 0 and a following
`socket(AF_INET, SOCK_STREAM, 0)` succeeds, though `stdio` grants no `inet`.
`sb_supported()` therefore reports true only for those two hosts, and anywhere
else startup logs one WARN - `[sandbox] NO kernel sandbox on this host` - naming
the capability layer as the only boundary. W^X is not enforced there either;
this does NOT refuse startup, because there is no partial sandbox to opt into
and no setting that would produce one (an INCOMPLETE backend, such as Linux
without Landlock, is the opposite case and still fails closed unless
`--allow-degraded-sandbox`). Note the cosmo branch is selected before the
`__APPLE__` one, so an APE on macOS does not reach Seatbelt - giving it one is a
tracked follow-up, and would add protection rather than only honesty.

### Capability Enforcement Invariants

- **SQL injection impossible:** All DB access uses `sqlite3_bind_*` parameterized binding. SQL is always a literal string. A JS param is a string, number, boolean, BigInt in the int64 range, null / undefined, or an ArrayBuffer / typed array (a BLOB, its bytes copied at once); anything else throws (`hl_js_db_param_other`, both the event loop's and the worker VM's binding - audit 23: an object, a Date or a buffer bound NULL, silently).
- **Internal tables protected:** `hl_cap_db_check_namespace()` blocks user code from accessing `_hull_*` tables. Enforcement uses call-stack inspection (Lua checks `ar.source` for `hull.` prefix, JS checks module name for `hull:` prefix) so stdlib modules transparently bypass the check via normal `db.exec`/`db.query`. No internal API is exposed. Tables: `_hull_outbox`, `_hull_inbox_processed`, `_hull_idempotency_keys`, `_hull_sessions`. A stdlib frame alone does not grant the bypass: the call must also NAME the method (Lua: `namewhat` `method`/`field` with the method's own name; JS: `this` is a real connection object), so a stdlib helper that invokes an app-supplied function (`retry.run`'s `retryOn`, `hull.map`'s `fn`) cannot be handed `conn.exec` itself; a JS bound function gets its own stack frame (QuickJS HULL PATCH 0002) so `conn.exec.bind(conn, sql)` is app code too, and JS connection objects (with their `async` / `udf` / `dialect` sub-objects) are sealed at creation - every method non-writable and non-configurable, no new properties - so `c.retryOn = c.exec` cannot turn an options bag that IS a connection into a stdlib-frame call (and `dialect` has no prototype, so a key a backend leaves out cannot be supplied through `Object.prototype`, audit 9 L2). `insert_if_absent` / `upsert` / `table_columns` check the table AND every column name. Known limit: the check is lexical, so on Postgres / MySQL dynamic SQL that assembles the name at run time (`DO $$ ... EXECUTE`, `PREPARE` from `CONCAT`) is not caught - real separation there needs a database role the app's connection cannot reach, which `databases.internal` provides (below; a startup WARN names it when the default connection is Postgres / MySQL / DuckDB and none is declared). DuckDB's dynamic-SQL table functions (`query(sql)`, `query_table(name)`, `json_execute_serialized_sql`) did the same; its backend refuses any statement that calls one (`hl_cap_db_sql_calls_dynamic_sql`, read conservatively: string literals are not skipped, and every byte is scanned for a bare name, so a `"` inside a literal or comment cannot open a quoted name that hides the call; audit 25 / 28 - a quoted name is read only as far as one of those names could reach, so a run of quotes stays linear, audit 30). Its statements that run SQL from a file or copy every table without naming one - `IMPORT DATABASE` (runs the directory's `schema.sql` / `load.sql`, which an app with `fs.write` can author), `EXPORT DATABASE`, `COPY FROM DATABASE`, and the `import_database` / `export_database` / `copy_database` pragma names - are refused too, under every file policy (`hl_cap_db_sql_reads_whole_database`, read the same conservative way, keywords in any case with comments between them, any statement of the text; audit 32). The same holds for session state on a shared Postgres connection (audit 18): app SQL can `SET search_path` to a schema of its own and `CREATE OPERATOR` / `FUNCTION` there, so the stdlib's later `_hull_*` queries on that connection resolve an unqualified operator or function to the app's (the CVE-2018-1058 pattern) - no SQL-text check sees it; `databases.internal` (a separate connection and role) is the remedy. Internal underscore modules (`hull._template`, `hull.kv._native`, `hull.db._internal_conn`, `hull:web:_request`, ...) are importable only by the stdlib, from the app's first line on (they are not in the registry, so the pre-manifest import tracker never sees them): in Lua the caller must be a `hull.*` chunk that calls `require` by name, so a stdlib helper handed `require` itself does not lend its identity, and `pcall(require, ...)` does not count. An app `require` of a `hull.*` name that is neither a registry module nor part of one is refused, and the CLI plugins (`stdlib/cli/lua`, embedded as `cli/<module>` entries) are loaded only into the tool VM: in the app VM they were stdlib chunks app code could require, and `hull.project.registry.load` required a module name its caller passed, which reached `hull._template` and `hull.db._internal_conn`; in JS the importer must be a `hull:` module, and a relative specifier may never normalize to a `hull:` name (`./hull:_template` from `./app.js` collapsed to `hull:_template` and reached the native module); an app import of a `hull:` name outside the registry is gated by its nearest registered parent, and refused when it has none (audit 10 - the loader served `hull:verify`, an unused stdlib file now dropped from the embed, with no declaration). A stdlib module's test-only helpers (inbox's `expire`, totp's blob readers and `decrypt_secret`, auth-flows' handlers and state, jobs' live config and its clock-injectable cron tick / cron math / subscription drain - audit 17: `jobs._tick(1e12)` pushed every `next_run_at` decades out, a drain at `now = 1e15` held a lease for ever) are not exported on the module: they register with the internal `hull._test_hooks` / `hull:_test_hooks`, which the C unit-test harness reads through `hl_lua_require_trusted` / a `hull:`-named module of its own (the `__test_hooks` / `__testHooks` globals of `test_lua.c` / `test_js.c`) - exported as `_test`, app code ran them against `_hull_*` tables from a stdlib frame (audit 16). A new test hook registers there, never on the module; an app-level e2e that needs one drives the public API at the real clock instead (the jobs fleet gate and Postgres / MySQL legs run the drain and cron tick through `jobs.work`). The app JS runtime has no `WeakRef` / `FinalizationRegistry` (a cleanup callback runs in whatever run a GC happens in; the worker.dispatch VM deletes them too - audit 10). A cached first-party module is not returned to app code that did not declare it, and every entry point (serve.c, and the app context behind the CLI runner, `hull test` and `hull agent`) validates the pre-manifest import tracker, which holds the whole registry.
- **Path traversal blocked:** `hl_cap_fs_validate()` rejects absolute paths, `..` components, symlink escapes via `realpath()` ancestor check. Plus kernel unveil. A database file SQLite holds open is not the fs capability's to touch (audit 33): SQLite maps a WAL database's `-shm` `MAP_SHARED` outside the SIGBUS guard, so truncating it in place (compute.stream's file output, an `fs.write` through an in-root symlink) killed the process at the next query. The SQLite backend registers every connection's file (`hl_db_live_file_add`, `cap/db_live_files.c`, refcounted by path), and `hl_cap_fs_open_write_fd`, `fs_write_atomic` (rename-replace) and `hl_cap_fs_delete` refuse (`database_file_in_use`) a target whose device and inode are a live database's or its `-wal` / `-shm` / `-journal`'s, checked before any truncate; an in-place write to a leaf named like a sidecar is refused outright (`hl_db_sidecar_name`). A new cap that opens a file for write without the temp-and-rename path must go through `hl_cap_fs_open_write_fd` or make the same check. `mmap_size` is unchanged: the main database is mapped too, but nothing in Hull can truncate it any more, and an outside process that truncates a live database corrupts it with or without the mapping. `blob.init` refuses a store root that runs through a symbolic link below the app directory: the store opens its root by absolute path, following links, so a planted `data/blobs -> ../migrations` put it outside the `data/` grant (audit 20). Every `fs.mmap` / window mapping is registered with the SIGBUS guard (a page past a shrunk file's new end reads as zeros instead of killing the process); past its 256 slots a mapping is refused (`too_many_mappings`) rather than made unguarded (audit 20). The bindings collect garbage once and retry on that refusal: a buffer dropped without `close()` holds its slot until it is collected (audit 21).
- **Host allowlist enforced:** `hl_cap_http_request()` validates target host against manifest's `hosts` array. Since §2.8 the check delegates to the shared matcher `hl_host_match_any_env` (`src/hull/utils/host_match.c`), so `hosts` entries may be an exact hostname (case-insensitive), `"*"` (any), a `"*.suffix"` subdomain glob, a CIDR (matches only IP-literal hosts, never a DNS name), or a `"$VAR"` / `"${VAR}"` env reference resolved at match time. The same matcher gates `ws.connect` (which hands Keel the client TLS config `http.fetch` uses, so `wss://` works, and refuses `wss://` up front when the process has none - audit 24: a NULL config failed every `wss://`) (shares the http config) and `smtp.send` (`hl_smtp_check_host`), and `databases.dynamic.hosts` - one convention across every outbound host allowlist. The check is on the URL's host, so the request may not name another: a caller header that Hull's client writes - `Host`, `Content-Length` / `Transfer-Encoding` / `TE` / `Trailer`, `Connection` / `Keep-Alive` / `Upgrade`, `Proxy-*` - or a name that is not an RFC 9110 token is refused (`hl_http_reserved_header`, in the bindings and the cap entry points; audit 22: a caller Host went out as a second Host line, and a `TE: chunked` beside Keel's Content-Length smuggled a request). The SSH tunnel's request line likewise takes an origin-form `tunnel.path` only, and `tunnel.headers` only `Name: value` lines: an absolute-form target is routed by its own authority, not Host.
- **Outbound HTTP timeout is one whole-request deadline:** connect, TLS, send, receive and every redirect hop count against it. Keel restarts its timer per redirect hop, so Hull bounds the chain itself: the async path arms the op's deadline (attached: `hl_net_op_suspend`; detached - timers, ws callbacks, `app.main` - a Keel timer in `http_async.c`), and the sync path hands Keel only the remaining time before each hop (`hl_http_chain_remaining_ms`, refusing the hop at zero). At most 5 hops (`HL_HTTP_MAX_REDIRECTS`), and a chain that began on https may not continue on http (`HlHttpRedirectGuard`; Keel re-sends a 307/308 body and every header but Authorization / Cookie / Proxy-Authorization; audit 25). A request with a body or a header outside the redirect-safe list (`hl_http_header_redirect_safe`: content negotiation, cache validators, User-Agent, and the three Keel drops itself) may not leave its first origin, even for an allowed host - same-host http -> https on the default ports excepted (audit 26). An http:// `http.fetch` keeps the TLS config so it can follow a redirect to https (audit 26). `ws.connect` has the same deadline over its connect + upgrade (a backend timer; on expiry `on_error` gets "connect timeout"; audit 25), and `conn:close()` re-arms it as a close-handshake deadline (`HL_WS_CLIENT_CLOSE_TIMEOUT_MS`, 5 s; on expiry the Keel client is freed and `on_close` runs once with 1006; audit 26). The connection is marked closed before `on_close` / `on_error` run, so a `close()` there arms nothing, and the JS finalizer cancels a deadline still armed (audit 30: one armed from `onClose` fired into the freed client). A close made before the upgrade completes is sent once it does, under that deadline, and the app gets no `onOpen` (JS; audit 27). A guest's `HL_WASM_OP_LOG` message is escaped (`hl_cap_wasm_log_escape`, `cap/wasm_log.c`: CR / LF as `\r` / `\n`, other controls as `\xNN`, truncated with `...` after a whole escape; audit 26). DNS is a blocking `getaddrinfo` on both paths (async uses `system_dns`, on the loop thread): its time counts, but the deadline cannot interrupt it. Default 30 s; manifest `http = { timeout_ms = N }` (JS `http: { timeoutMs: N }`) sets the app default, a per-call `opts.timeout_ms` / `opts.timeoutMs` overrides it, and the cap layer (`hl_http_timeout_resolve`, `include/hull/limits/http.h`) clamps the result to 10 min for the async `http.async.*` calls (`http.fetch`) and 60 s for the sync calls (they block the loop). A per-call value that is not a positive number raises; an invalid manifest value is ignored, like the `wasm` limits.
- **Env allowlist enforced:** `hl_cap_env_get()` checks against manifest's `env` array (max 32 entries). A `$VAR` reference elsewhere in the manifest must name a variable in `env` or `secrets` (checked at load by `hl_manifest_check_env_refs`).
- **SQL cannot reach other files (SQLite):** `hl_cap_db_init` installs an authorizer that refuses `ATTACH` of any real file (and so `VACUUM INTO`, which SQLite runs as an internal ATTACH), `writable_schema` and the `*_store_directory` pragmas, INSERT / UPDATE of `sqlite_sequence` and `sqlite_stat*` (a forged AUTOINCREMENT high-water mark failed every `_hull_*` insert; an ALTER TABLE RENAME's own update stays legal - audit 29) and an app DELETE of either (a reset sequence reused `_hull_job_events` ids under a jobs subscriber's cursor), in any database and from a trigger, while the DELETEs SQLite's DROP TABLE / DROP INDEX / `ANALYZE t` run themselves pass: a DELETE is admitted only in a statement that issued a DROP or ANALYZE callback, and since `ANALYZE t` deletes before its callback, a `sqlite_stat*` DELETE is decided when the prepare ends - every app statement is prepared through `hl_cap_db_prepare` (the statement cache, and `hl_cap_db_exec_script` for scripts instead of `sqlite3_exec`), which also clears the per-thread state before and after; a new prepare of app SQL must go through it, plus `SQLITE_DBCONFIG_DEFENSIVE` (`hl_cap_db_guard`, which the read-only `hull agent db query` / MCP `hull_db_query` connection gets too; that query also refuses any statement `sqlite3_stmt_readonly` does not call read-only, and transaction control, since without `-d` it runs on the app's own connection - audit 9 M2). Transaction control is refused by the authorizer at prepare time (`hl_cap_db_refuse_txn_control`) as well as by the text, read with SQLite's non-nesting comment rule: `/* /* */ BEGIN` got past a nesting reader. `hull agent sql named` queries (from `queries.json`, run on the same warm connection) go through the same gate (`hl_agent_prepare_readonly`): named queries are read-only. Every agent SQL path (`db schema` / `query`, `migrate status`, `schema-diff`, `sql named`) runs with the thread's budget binding swapped out (audit 10).
- **SQL is charged to the run's budget (SQLite):** `hl_cap_db_guard` installs a progress handler that charges `HL_DB_PROGRESS_OPS` (100) units per call to the budget bound on the calling thread (`include/hull/cap/db_budget.h`, `cap/db_common.c`) and interrupts the statement once it is exhausted - a recursive CTE in one `conn.query` held the event loop for good (audit 9 H4). Each runtime binds its budget when it arms a run (`hl_lua_budget_arm`, `hl_js_budget_arm`; a worker dispatch for its duration) and unbinds it when the VM goes; a tripped query raises the limit (`hl_lua_raise_copy`, `hl_js_budget_throw`), never a SQL error to catch. A `db.async` op has a budget of its own (`HlDbOpBudget`, the submitting VM's limit) that also trips, on a 0-unit charge too, past its wall-clock deadline (app.main's run limit: 10 min by default, `--max-run-ms` when set), once its continuation is cancelled, and while the pool is being freed (`hl_db_op_shutdown`, serve.c / serve_cli.c) - a wait charges no units, so a `pg_sleep` or a silent server under `?read_timeout=0` pinned a pool thread and the SIGTERM join (audit 27); a Postgres LISTEN wait asks it every slice. While the pool is freed every other bound charge interrupts too (`hl_db_budget_charge`), so a `worker.dispatch` job's SQL wait, charged to its VM's budget, does not hold the join either (audit 28). No binding never interrupts: migrations and agent queries swap it out. DuckDB statements run task by task (`duckdb_pending_prepared` + `duckdb_pending_execute_task`, `duck_execute` in `cap/db_duckdb.c`), each task this thread runs charging `HL_DB_DUCK_TASK_OPS` (10000) and an idle step (DuckDB's own threads hold the work) asking with a 0-unit charge; a trip calls `duckdb_interrupt` and fails the statement - one `duckdb_execute_prepared` held the event loop for as long as the query ran (audit 15). The same guard caps `SQLITE_LIMIT_LENGTH` at the largest VM heap a runtime reported (`hl_db_note_heap_limit`), so `randomblob` / `zeroblob` cannot build a value of up to 1 GB outside it. One opcode can still do a lot of work off the heap (`randomblob`, `replace`, `printf`, a sorter run, a temp b-tree), so SQLite's allocator is wrapped too (audit 10 H3): `hl_cap_db_sqlite_setup` (called before every Hull SQLite open, `pthread_once`; `sqlite3_config` only works before `sqlite3_initialize`) installs a `SQLITE_CONFIG_MALLOC` wrapper that charges each allocation and each realloc's growth to the thread's bound budget at 1 unit per `HL_DB_ALLOC_UNIT_BYTES` (64, as Lua's allocator charge), and fails the allocation once that budget is exhausted, so the statement stops at once (SQLITE_NOMEM, surfaced as the limit). No binding is never charged. The rollbacks Hull runs for a run (`hl_cap_db_rollback`, so the stale-transaction guard, a batch's rollback - a nested one's `ROLLBACK TO` / `RELEASE` in `hl_db_batch_leave` - and the end of a `db.async` op or `worker.dispatch` job - the network backends' end-of-job ROLLBACK under a cleanup budget that stops after 2 s or at pool shutdown, audit 29) and `hl_cap_db_shutdown` run unbound, so a tripped budget cannot leave a transaction open; DuckDB's `rollback` does the same, and a ROLLBACK that failed or was interrupted leaves the transaction tracked as open (audit 16). **SQLite memory model (audit 11).** Connections run with `temp_store=FILE`: a sort, `CREATE INDEX`, `GROUP BY` / `DISTINCT` / `UNION` and `VACUUM`'s copy spill to temp files once they outgrow the page cache, so what one connection holds is bounded by its page cache (`cache_size`, 16 MiB) plus a sorter run of the same size - not by the size of the data. (Under the old `temp_store=MEMORY` the sorter never spilled, so every big sort had to fit in the process-wide hard limit: a migration's `CREATE INDEX` on a big table stopped the app starting, `VACUUM` of a >1 GiB database always failed, and one request's `ORDER BY` could fill the limit for every connection in the process.) The temp files go to `hl_hull_sqlite_temp_dir()` (`shared/cache_dir.c`): a fresh per-process `hull-sqlite-<euid>-XXXXXX` made with `mkdtemp` (0700, checked through an `O_DIRECTORY | O_NOFOLLOW` descriptor - `fstat` owner, `fchmod` - canonical, removed at exit) under the first of `SQLITE_TMPDIR` / `TMPDIR` / `/var/tmp` / `/usr/tmp` / `/tmp` where one can be made: every candidate is tried, and a base must be absolute, writable and searchable (`access(W_OK|X_OK)`), and not world-writable without the sticky bit (audit 12: the first EXISTING candidate was taken, writable or not, the fixed `hull-sqlite-<euid>` name could be squatted by another account or was shared by every app of the user, and any failure fell back to MEMORY silently). `hl_cap_db_sqlite_setup` sets it as `sqlite3_temp_directory` and the kernel sandbox grants it `rwc` (unveil, and a Seatbelt `subpath`) - made before the first unveil, so the two always agree, and only when this binary has a SQLite backend at all (`hl_db_backend_select("sqlite://")`, base or composed feature). With no usable dir a WARN says so and `hl_cap_db_temp_on_disk()` is 0: connections keep `temp_store=MEMORY` (on Windows by design - SQLite's unix VFS under Cosmopolitan finds no temp path at all, `SQLITE_IOERR_GETTEMPPATH`). Temp files are not size-capped: a big sort or VACUUM uses disk there, bounded by the filesystem and by the run's budget (the work is charged). Process-wide, `sqlite3_hard_heap_limit64` defaults to `HL_DB_SQLITE_HARD_HEAP_LIMIT` (1 GiB: past it every SQLite allocation fails instead of the process running out of memory) - headroom for many connections, not a ceiling on a sort; the operator sets it with `HULL_SQLITE_HEAP_LIMIT` (a size, `512M` / `512MB` / `4G`; `0` = no hard limit; a non-zero value under 64 MiB is raised to it with a WARN; an out-of-range value is refused, not clamped), read once at setup (the setup runs unbound, never under the first opener's budget) by every entry point (serve, the `app.main` runner, `hull migrate`, `hull test`, `hull agent`). The soft limit is 256 MiB, at most a quarter of the hard one (SQLite sheds page cache first). Per-run charging is unchanged: every allocation still charges the bound budget. The authorizer refuses SETTING `hard_heap_limit` / `soft_heap_limit` (process-wide), `cache_size` / `default_cache_size` / `cache_spill`, `temp_store` and `threads` (helper threads no budget sees), `locking_mode` to anything but `NORMAL` (EXCLUSIVE keeps the lock, starving the `db.async` / `worker.dispatch` pool's connections) and `journal_mode` to anything but `WAL` (SQLite matches the value as a PREFIX, so only `w` / `wa` / `wal` pass; OFF / MEMORY break ROLLBACK and crash safety, DELETE etc. drop the WAL `synchronous=NORMAL` relies on), `busy_timeout` above `HL_DB_MAX_BUSY_TIMEOUT_MS` (5000, Hull's own: the thread sleeps that long on a lock, uncharged, and on the event loop that stalls every request) and `synchronous` below NORMAL (only `1`/`2`/`3`/`normal`/`full`/`extra`/`on`/`yes`/`true` pass: SQLite reads a numeric value modulo 8, so `7` is OFF too) (audit 12); reading them is allowed. Under the agent's read-only gate (`hl_cap_db_refuse_txn_control`, installed from prepare through finalize - `hl_agent_finalize_readonly`) every pragma given an argument is refused too (a bare one unless it only reports a value, and ATTACH / DETACH / ANALYZE - audit 29), except the introspection ones (`table_info`, `table_xinfo`, `table_list`, `index_info` / `_xinfo` / `_list`, `foreign_key_list` / `_check`, `integrity_check`, `quick_check`): a flag pragma (`foreign_keys`, `query_only`, `busy_timeout`, `synchronous`, ...) takes effect when it is PREPARED and still passes `sqlite3_stmt_readonly`, so a "read-only" agent query changed the warm app connection. A `worker.dispatch` JS VM's db bindings throw the uncatchable interrupt for any db error once the dispatch is over its budget (`hl_js_worker_budget_tripped`), as the event loop's do (audit 10).
- **No shell invocation:** Tool mode uses `hl_tool_spawn()` with compiler allowlist. No `system()`/`popen()`. Arguments that make a driver run another program are refused (`-wrapper`, `-specs` / `--specs` in every spelling - joined, as a separate argument, and gcc's `--spec=` / `--sp` abbreviations (audit 15), `--ld-path=`, `-fuse-ld=/path`, plugins, `-Wp,` (its pieces reach cc1 unchecked: `-Wp,-load,x.so`), `-Xclang=` (its joined argument skips the `-load` check; audit 10), `@file`, clang `--config*` files, `--gcc-toolchain`, and `-B<dir>` unless the dir is a `$PATH` / `~/.hull/tools` entry holding the lld Hull resolved); a spawn may set only `ZIG_*_CACHE_DIR`, `TMPDIR`/`TMP`/`TEMP` and `SOURCE_DATE_EPOCH`.
- **Key material zeroed:** `hull_secure_zero()` (volatile memset) scrubs crypto material from stack buffers.
- **Instruction limits:** Both Lua and JS runtimes enforce per-request instruction limits (default 100M). Lua uses `lua_sethook(LUA_MASKCOUNT)`, JS uses `JS_SetInterruptHandler`. Override with `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` env var. Lua's budget (`runtime/lua/budget.c`) is per VM and per **uninterrupted run**: every entry (a request, a middleware, a timer, an async resume, `app.main`) arms the whole limit again. A trip is sticky until then: `pcall` / `xpcall` / `coroutine.resume` / `coroutine.wrap` re-raise it, so app code cannot catch the limit and keep looping. Work one Lua instruction does on a large operand counts too (Lua HULL PATCH 0004, docs/lua_patches.md): allocation (1 unit / 64 bytes, `luaL_Buffer` growth included), string compares (the bytes compared) and long-string table keys, hash-chain walks (integer / float keys hash with no seed, so a script could put every key on one chain), `table.insert` / `remove` / `move` / `sort` / `concat` / `unpack` loops, `string.byte` / `rep` / `pack` / `unpack`, `utf8.len` / `offset` / `codepoint`, `next` over emptied slots, vararg and result copies, string-to-number coercion, the collector's work (incremental steps, the emergency collection a failed allocation runs, mode switches), and pattern matching including a plain `find` miss; a coroutine's run is charged to its resumer when it returns or yields, so short coroutines are not free - so the limit bounds a run's wall time, not only its instruction count. Hull's bindings charge their own work: `hull.crypto` digests / ciphers / MACs per byte (an HMAC its key too, and every digest charge is checked, `lua_hlwork`, so it raises before the work - audit 27), key derivations (`hash_password`, `verify_password` - whose iteration count comes from the stored string - and `bcrypt_pbkdf`) per round BEFORE they run, `res:header` / `res:json` / `html` / `text` / `bytes` the bytes they copy (the body calls 1 unit per 8 bytes; response headers are capped at `HL_RES_HEADER_BYTES_MAX`, 64 KiB, since Keel's header buffer is outside the script heap). A `hull.async` task that trips the limit is still finished by the runtime (`hull._spawn`'s failure hook, docs/task_join_design.md), so its waiters wake; a task that trips it in the stretch `hull._spawn` runs inline raises the trip in its spawner too (audit 17: the spawner ran on until its own hook came round). QuickJS polls its interrupt handler once per 10000 countdown steps (calls and backward jumps, and regexp backtracking steps), so each poll is charged `HL_JS_INTERRUPT_WEIGHT` (10000, `runtime/js/internal.h`) - counted one per poll, the limit used to be ~10^4 times weaker than its value. The JS budget mirrors Lua's: per run, re-armed by `hl_js_budget_arm` at every entry (dispatch, middleware, timer, ws / SSE / ws-client callback, async and multipart resume, `app.main`, a `hull test` case, a detached `hull:_task` task (and its Lua twin `hull._task`, `runtime/lua/async.c`) - which puts back the budget and active state of a run it fires inside, such as a `hull test` case pumping the loop, audit 9 L1; each worker dispatch has its own), and a trip is sticky (`HlJS.budget_tripped`) - QuickJS HULL PATCH 0003 polls again at the very next step, so an async body or promise job that turned the interrupt into a rejection cannot let its caller run on. A binding whose callback was interrupted (SQL UDF, `compute.stream`) re-raises it uncatchable (`hl_js_budget_throw`), `hl_js_run_jobs` discards a tripped run's jobs, and a tripped run whose promise therefore never settles is completed as failed (`HlJsRunOnce.tripped`). Work one JS call does that no poll sees is charged by the binding BEFORE it does it (`hl_js_budget_charge`, which trips and raises the uncatchable interrupt when over): crypto digests / MACs / ciphers / signatures at 1 unit per 8 bytes (as Lua's `crypto_charge`), and PBKDF2 (`hashPassword` / `verifyPassword`, whose count comes from the stored string, up to 10M) at iterations × blocks × 128 bytes - audit 9 (a password over HMAC's 64-byte block is hashed once before the loop, so its length no longer multiplies the cost - audit 18); `res.header` / `res.json` / `html` / `text` (1 unit per 8 body bytes) / `bytes` charge the bytes they copy, under the same `HL_RES_HEADER_BYTES_MAX` header cap as Lua (`hull/shared/res_headers.h`, shared by both runtimes). `res.json` / `html` / `text` (both runtimes) keep exactly one Content-Type: an app's own (`res.header`) wins, and the one an earlier body call added is Hull's default, which the next body call replaces (the response object remembers it, `ct_hull`; #712 kept the first one whoever set it, so `res.html` then `res.json` sent JSON as text/html - audit 10). They, and `res.bytes`, drop the Content-Encoding / `Vary: Accept-Encoding` an earlier gzipped body left; `res.bytes` also drops Hull's default Content-Type, and an app `res.header("Content-Type", ...)` replaces it. The 500 a failed handler gets (`hl_res_error_reset`, both runtimes and the SSE init failure) drops every header the handler had set - a Set-Cookie or Location went out on the error - and carries one Content-Type. It keeps the headers EARLIER middleware set (CSP, HSTS, CORS, a request id): every handler and middleware entry snapshots the response's header lines when it starts (`hl_{lua,js}_res_entry_begin`, before any of the entry's error paths, audits 11 / 12), and its 500 restores that copy minus Content-Type / Content-Encoding / Keel's Vary. A JS handler resumed by its multipart body reader finishes through the same seam (`hl_js_http_resume_send` / `_error`, `mp_js_pump`): it sent by hand, so its 500 dropped the middleware headers and its success left the snapshot behind (audit 17). A multipart resume that leaves the handler waiting on a promise Hull does not drive (`await gate` after the loop: no op holds the connection, no read is parked, no continuation was wired) ends the request and answers it 500, as an async resume ends it - the streaming connection was otherwise never answered and its `res` stayed live (audit 20). The snapshots are a per-runtime list (`HlLua` / `HlJS` `res_bases`, `hull/shared/res_base.h`), freed by `hl_{lua,js}_free`, dropped when the entry answers and when an async cancel says the request's connection is gone, and capped at `HL_RES_BASE_MAX` entries and `HL_RES_BASE_BYTES_MAX` (1 MiB) bytes. `res.header` (both runtimes) refuses a name that is not an RFC 9110 token and a value holding a NUL: Keel takes C strings, so `"Content-Type\0x"` passed Hull's Content-Type checks and went out as a second Content-Type (audit 12). Work a QuickJS builtin does inside one step is charged too (QuickJS HULL PATCH 0005, docs/quickjs_patches.md, audit 10 H2): `JS_SetWorkHandler` hands Hull (`hl_js_work_handler`, and the worker VM's `js_worker_work`) the bytes a string search / compare / trim, a Map key or atom hash, a typed-array fill / copyWithin / set / reverse / slice / indexOf / sort, an ArrayBuffer slice or a JSON.parse scans - and every block the allocator hands out - at 1 unit per 64 bytes (Lua patch 0004's rate), plus 1 unit per element a generic array method visits; once over, the next step polls (sticky, uncatchable), and a loop no heap bounds (`Array.prototype.indexOf.call({ length: 2 ** 53 - 1 })`, a quadratic `indexOf`, a sparse `forEach`) throws from inside the builtin. Audit 11 added string-to-number (`JS_ToCStringLen`'s ASCII path, which allocates nothing - and since audit 23 FAILS the conversion with the interrupt once the run is over, so a binding converting an array that names one big string N times stops instead of rescanning it N times with nothing polled - and the `ToNumber` / `parseInt` / `parseFloat` / `BigInt()` parse), the RegExp `flags` string an app getter returns (4 x its length per `match` / `matchAll` / `replace` / `split` / `replaceAll`) and a `"$<"` replacement rescan, BigInt arithmetic before the work (1/8 unit per limb product for `*` / `/` / `%` / `**` / a decimal parse, 1/2 unit per limb division for a non-power-of-two `toString`), and `new TA(arrayLike)` / `TA.from` (1 unit per element). JS public-key operations charge 2^14 units each before the work (ed25519 keypair / sign / verify, x25519, box / boxOpen, ECDSA sign / verify), RSA (bits/1024)^3 * 2^14 (bits from the signature on verify, bounded by the public key PEM's length * 6 - audit 11, the signature is the attacker's - and a signature over `HL_CRYPTO_SIGN_MAX` (1024 bytes, mbedTLS's RSA-8192 ceiling) is false, uncharged; estimated from the PEM on sign) - audit 10, the twin of the Lua side. Hull's own init code runs before the limit applies.
- **Run wall-clock watchdog:** the instruction budget is a precise per-run COST, but only as complete as the charging inside each engine, and audits 9-12 kept finding uncharged work (prototype-chain misses, Proxy `ownKeys`, regexp compile / exec, rehash, `luaC_checkfinalizer`, `luaH_next`, ...). So every run also has a wall-clock DEADLINE (`src/hull/cap/run_watchdog.c`, round 12): one process-wide thread (CLOCK_MONOTONIC, waited on like `wasm_watchdog.c`) holds the pending deadlines and, when one passes, raises that VM's `HlRunWatch.stop` flag - it never touches a VM, only the flag, under its lock; a watch is unlinked under the same lock before its storage goes (`hl_lua_free` / `hl_js_free`, the Lua worker heap, the JS worker ctx). The engines poll the flag: Lua HULL PATCH 0005 in `luaG_traceexec` at every hooked instruction (`lua_hlsetstop`), QuickJS HULL PATCH 0006 in `js_poll_interrupts` at every call / backward jump (`JS_SetStopFlag`; the regexp poll calls the same handler), and the SQL budget charge checks it too (a long statement is interrupted). The budget then trips with `run exceeded its time limit` - the same sticky, uncatchable trip (pcall / try / async bodies cannot swallow it; `hl_lua_trip_reason` / `hl_js_trip_reason` pick the message). The deadline is armed with the budget, at the same single points (`hl_lua_budget_arm` via `HL_LUA_ARM` / `HL_LUA_ARM_KIND`, `hl_js_budget_arm` / `hl_js_budget_arm_kind`), so every entry that arms the budget - dispatch, middleware, timers, ws / SSE / ws-client callbacks, async and multipart resumes, `app.main`, `hull test` cases, `hull._task` / `hull:_task` tasks, `worker.dispatch` jobs - bounds its UNINTERRUPTED run, and a new entry inherits it by arming the budget; a nested run (a task, or a failed task's hook - audit 14 - fired inside another run, a `hull test` request inside a case) puts the outer deadline back (`hl_run_watch_save` / `_restore`). A Lua `worker.dispatch` VM's watch is disarmed only after `lua_close`, so the VM's `__gc` finalizers stay under the job's deadline (audit 15); a Lua timer handler's wait that is cancelled clears the timer's `in_flight` and reschedules it, except at shutdown. A watch is not disarmed when a run returns: an idle VM's flag may be raised, and the next entry's arm clears it before any script runs (the budget's own leftover state has the same contract - code that runs a VM outside an entry must arm first). Config: `--max-run-ms N` (both runners; a downgrade flag, so `--hull-max-run-ms` in a built binary) or `HULL_MAX_RUN_MS` (read once, so `hull test` / `hull agent` honour it); `0` disables; default 60 s per run (`HL_RUN_DEFAULT_MS`: a run holds the single-threaded event loop, and the 100M-instruction default is a few seconds of script) and 10 min for app.main's own runs (`HL_RUN_MAIN_DEFAULT_MS`: a CLI blocks nothing else and may legitimately spend long in one synchronous stretch), clamped to 24 h. The tool VM has none (`run_watch_on`, app VMs only); `db.async` ops are bounded by their own SQL budget, not the watchdog. **Limit:** the flag is seen between Lua instructions / at QuickJS steps, so ONE long builtin call (one huge rehash, one regexp compile, a synchronous `compute.call` - which has its own `timeout_ms`) runs to completion; the watchdog bounds loops of them. With it, the remaining engine-internal charging gaps are Low (bounded by the deadline) rather than unbounded. The poll backend (the Keel-free loop `app.main` / compute apps run on) fires only timers that existed when its timer step began (an id watermark, deadline ties broken by id, as Keel's `kl_timer_fire`): a 0 ms timer re-added from a callback - a self-re-spawning `hull._task`, two tasks waking each other through `hull._wake` - fired in the same tick for ever, each callback a fresh run, starving completions, watchers and `run_until`'s stop check (audit 24).
- **More binding charges, and `test.get` (audit 10):** Lua's public-key operations are charged a fixed cost BEFORE they run (`mod_crypto.c`, `lua_hlwork`): 2^14 units per scalar multiplication (`ed25519_keypair` / `sign` / `verify`, `x25519_keypair` / `x25519`, `box_keypair`, `box` / `box_open`'s shared key, an ECDSA `crypto.sign` / `verify`) and (bits / 1024)^3 * 2^14 for RSA (bits rounded up to a multiple of 1024, capped at 16384 - the JS runtime's numbers): a verify sizes the key by its signature (an RSA signature is the modulus long), bounded by the public key PEM's length * 6 bits, and refuses (false, uncharged) one over `HL_CRYPTO_SIGN_MAX` (audit 11), a sign by its PEM (bits ~= PEM length * 4/3: a private key carries n, d, p, q and the CRT values), `rsa_private_pem` by n. `blob.put` / `put_verified` / a writer's `write` and `fs.write` charge 1 unit per 8 bytes before the work in both runtimes. So do the whole-object READS (audit 15): `fs.read`, `blob.get` (read + SHA-256) and a blob reader's `read(n)` (n capped at `HL_BLOB_READ_CHUNK_MAX`, 16 MB), each refused BEFORE it allocates when the object is larger than the VM heap limit (Lua `mem_limit`, JS `max_heap_bytes`); Lua's `fs.read` / `reader:read` read into a `luaL_Buffer` box (VM memory) - `lua->base.alloc` is the server's allocator, not the Lua heap tracker, and no VM limit covers it - and JS `fs.read` straight into the ArrayBuffer's store. `tar.create` / `tar.pack` take at most `HL_TAR_CREATE_MAX_ENTRIES` (65536; JS sized its arrays by an app array's `length` and allocates them with `js_malloc` now), refuse an archive (`hl_tar_create_size`) larger than the heap limit, charge the build 1 unit per 64 bytes, and `pack` keeps a running total and charges each read; `hl_tar_create` sizes its buffer exactly once and caps it at `HL_TAR_CREATE_MAX_BYTES` (1 GiB). `tar.parse` / `tar.extract` charge the whole input 1 unit per 64 bytes up front (`hl_work_bulk_units`; audit 17: headers of a typeflag the parser skips were checksummed but never surfaced, so no per-entry charge saw them, over a MappedBuffer of up to 1 GiB), and JS `tar.parse` fails on an out-of-memory name / data copy instead of defining the exception value. The JS `hull:encoding` C codecs (`mod_encoding.c`) refuse an output larger than the heap limit and charge input + output 1 unit per 8 bytes before they malloc (audit 17); the Lua ones (`hull.encoding._native`) charge the same before the work (audit 18). An RSA key whose public exponent is over 64 bits is refused on sign and verify (`rsa_e_bounded`, `crypto_asym_mbedtls.c`): the RSA charge assumes e = 65537 (audit 18). SSE `event` / `comment` charge 1 unit per 8 bytes plus 1 per line (Keel writes each line as its own field; since 3.3.1 batched into one chunk per 32 pieces), and ws `send` / `send_binary` (`sendBinary`), `ws.broadcast` and the ws-client sends 1 per 8 bytes plus 1 per frame, per destination - before the write (audit 14; units in `include/hull/shared/work_charge.h`). `smtp.send` copies every `cc` entry or raises / throws (it read the length through `__len` and dropped, silently, a non-string or an entry the arena had no room for). Its message copy (`hl_smtp_message_bytes`) is charged 1 unit per 8 bytes before the work in both runtimes, sync path included, and the async op's owned copy is held on the VM's off-heap total under the heap limit until its async ctx goes (`HlAsyncCtx.hold`); a loop of sends refused at admission copied the message every call, uncharged (audit 26). The sync send's conversation asks the run (cancel poll `hl_db_budget_charge(0)`) at every pump step instead of holding the loop for its 30 s timeout (audit 27). `gpu.texture_read` / `buffer_read` (both runtimes) and Lua `tui.print` / `write` / `clipboard_set` charge 1 unit per 8 bytes (audit 26). `test.get` & co run the app's dispatch inside a test case: the case's budget is kept and charged with the request's work (the dispatch re-armed it, so a case looping over `test.get` never tripped), and while the case has a transaction open the stale-transaction guard is held off (`hl_db_registry_guard_hold`) - it rolled back a `db.batch` the case was inside.
- **WASM compute bounds: gas vs timeout.** `gas` is WAMR instruction metering - exact, but INTERPRETER-only: WAMR never meters AOT code, and nothing meters the start / `__wasm_call_ctors` functions an instantiation runs. `timeout_ms` (default 10 s, max 1 h; per call, manifest `wasm.timeout_ms` / `timeoutMs`, CLI `--wasm-timeout-ms`, clamped like gas) is the wall-clock bound that holds for everything: `cap/wasm_watchdog.c` (one thread) calls `wasm_runtime_terminate` on the instance at the deadline, and WAMR patch 0007 makes that land - the fast interpreter polls the instance's exception every 4096 instructions, wamrc emits a volatile check at every loop header, and a post-instantiate hook binds the watch while start / ctor functions run (a module whose start function outlives the default at load is refused). An expired watch re-terminates its instance every 10 ms until unbound (a host call can erase a pending trap: round-6 M1), and deadlines are CLOCK_MONOTONIC on every host. Patch 0007 also has wamrc stamp every AOT file (`include/hull/cap/wasm_aot_stamp.h`); `hl_cap_wasm_load` refuses an unstamped `.aot` (an unpatched wamrc's: its loops cannot be stopped) and falls back to the `.wasm`, and `hull build` neither embeds one it compiled nor keeps compiling AOT with that wamrc. The runtime has no hardware bound check (`WASM_DISABLE_HW_BOUND_CHECK=1`: linear memory has no guard region), so AOT must bounds-check in software: `hull build` runs wamrc with `--bounds-checks=1 --stack-bounds-checks=1` (wamrc's 64-bit default omits them - audit 31: guests read and wrote host memory), the flag set is in the AOT cache key, and the stamp (version 2) records both checks from what wamrc generated; an AOT without both bits, or with a version-1 stamp, is refused like an unstamped one (docs/wamr_patches.md "Bounds checks"). WAMR picks AOT vs bytecode by the magic (`\0aot`), not the name, so both check the BYTES: AOT code saved as `compute/<name>.wasm` is held to the stamp too (the runtime refuses an unstamped one, `hull build` fails on it), and `hull build` warns about a committed unstamped `*.aot.*` it embeds under `--trust-prebuilt-aot` (by default it embeds no committed AOT, and the runtime reads no AOT from disk; audit 19). The manifest / CLI ceilings (`include/hull/wasm_config.h`) apply on every entry point - serve.c, the `app.main` runner, `hull test` / `hull agent` - and to `compute.stream` and WASM `db.udf` instances; a call on a `compute.instance` falls to the instance's own defaults, under the ceiling. A watch is bound to an instance only while guest code runs, so the watchdog never touches a pooled or destroyed instance. A module that exports its own allocator (`malloc` / `free`, or AssemblyScript's `__new` / `__retain` / `__release` / `__pin` / `__unpin`) is refused at load (`hl_cap_wasm_load`): WAMR inserts no app heap for it and runs the guest's malloc for every input / output buffer, trusting the offset it returns (a host write outside linear memory) and running guest code outside the call's watch (audit 23). As a second line, every buffer `module_malloc` returns, and the output range a guest reports, is checked against linear memory before the host copies (`range_in_linear_memory`). Segment chains: every instance teardown detaches the module chain before deinstantiate (WAMR does not, and counts attachments in a uint8 that wrapped); `HlWasmModule.chain_attached` bounds live attachments at `HL_WASM_MAX_CHAIN_ATTACH` (128); a segment change is refused (`segments_in_use`) while an out-of-pool instance holds the chain; and a per-module `chain_gen` keeps an instance that was out of the pool across a change from being pooled stale. One segment is one WAMR shared heap, and WAMR refuses a heap over `APP_HEAP_SIZE_MAX` (1 GiB) while Hull allows 3 GB in total, so `compute.segment` refuses a segment over `HL_WASM_MAX_SEGMENT_SIZE` (1 GiB, page-aligned) up front with `segment_too_large` (audit 15); and an add / replace builds the new segment's backing and heap BEFORE it touches the old one, so every failure leaves the old segments loaded and chained - a replace used to free the old heap first, and a failure then left a lone segment's `chain_head` naming the freed heap (the next call attached it) or, with more segments, a hollow slot. `compute.stream` runs every chunk inside one binding call (each chunk has its own gas and `timeout_ms`, up to UINT32_MAX chunks), so a per-chunk hook (`HlStreamOpts.before_chunk`) charges the run's budget 1 unit + 1 per 64 input bytes before each chunk and checks its deadline (`hl_js_budget_charge`; Lua `hl_lua_budget_charge`, which never raises inside the cap loop), stopping the stream and raising the uncatchable trip; buffer-mode output is capped (`max_output`, `output_too_large`) at the JS heap limit / what is left of the Lua heap (audit 15). Persistent instances (audit 10): at most `HL_WASM_MAX_LIVE_INSTANCES` (32) live per WASM cache, reserving at most `HL_WASM_MAX_LIVE_INSTANCE_BYTES` (1 GB) between them (the first is never refused for its size) - each held its memory outside the VM heap limit until closed or collected - and `compute.instance()` charges 1 unit per 64 bytes of that reservation to the instruction budget in both runtimes. `memory.grow` is bounded too (audit 15 follow-up): a persistent instance (`compute.instance`, `compute.stream`, a WASM `db.udf`) is instantiated with WAMR's `max_memory_pages` capped (`hl_wasm_watch_instantiate_capped`, `persistent_max_pages`: the default memory's declared initial pages + `hl_wasm_grow_pages(heap_size)`, never above its declared maximum; WAMR adds its inserted app heap to both, so the headroom is the same with or without one; a memory the module neither imports nor exports gets no growth), so a grow past `heap_size` (in pages) returns -1 in the guest, and its reservation is `hl_wasm_instance_reservation` = heap + that growth + stack. A zero-copy checkout reserves the linear memory its instance holds when the call returns (`linear_memory_bytes`, growth included) + stack, kept in the buffer (`u.wasm.reserved`) and released exactly; its GC footprint is the same number. Pooled one-shot calls are not capped, but an instance holding more than the load-time probe's memory plus its heap is destroyed, not pooled (`pool_linear_bound`, audit 29). Both call paths re-take the output's native address after the guest returns, since a grow can move linear memory where WAMR reserves no guard region. A JPEG with more than 100 scans (SOS segments, counted by `jpeg_scan_count` in `image_stb.c` the way stb reaches them) is refused before stb decodes it: stb walks every block once per scan (audit 18), and a decode is charged its pixels once per scan (`hl_image_decode_units`; audit 19: it paid one pass for up to 100). `image.decode` / `image.encode` charge 1 unit per 8 bytes of RGBA pixels (+ 1 per 8 encoded bytes) before they run (`hl_image_codec_units`), and a decode reserves its pixels in the VM allocator from the header (`hl_image_info`) BEFORE stb allocates its off-heap buffers, so a decode that cannot fit the heap is refused without them. Those stb buffers are capped in SUM, not per block (audit 12): `vendor/stb/stb_impl.c` prefixes every stb_image block with its size and keeps a thread-local live total (`hl_stb_live`) that `hl_stb_alloc_cap` bounds; `image_stb.c` sets it to three 16-bit raw images on the 16 px padded grid plus the input and 2 MB (a progressive JPEG held ~3 cap-sized buffers at once under the old per-block cap). stb_image_write goes through the same allocator (audit 18; it used plain malloc, nothing bounding its peak): an encode's cap is 5x its input (`hl_image_encode_bytes`: rows plus a filter byte each) plus 8 MB, its output is held to 2x - and a refused allocation fails the encode instead of aborting the process (stb_image_write `STBIW_ASSERT`s its growable buffers; `stb_impl.c` longjmps back to `hl_stbiw_{png,jpg}_to_func`, which free every block the encode held; audit 19) - and both runtimes refuse an encode whose input is larger than the VM heap limit before it runs (`image_too_large`; a zero-copy view of a mapping or a WASM buffer can be). A **zero-copy compute buffer** (`compute.call` / `compute.async.call` with `buffer = true`, `HL_WASM_BUF_WASM`) keeps a pooled instance checked out, so it reserves a live-instance slot + its heap + stack exactly like a `compute.instance` (`hl_wasm_live_instance_reserve` in `call_buf_impl`, released in `hl_wasm_buffer_destroy`); past the cap the output is COPIED into an OWNED buffer and the instance goes straight back (audit 12). GPU readbacks (`gpu.dispatch` / `gpu.pipeline` with `buffer = true`) are copied into an OWNED buffer too (`hl_wasm_buffer_create_adopted` no longer adopts the untracked malloc). An OWNED buffer (those two and `compute.buffer`) is allocated on the server allocator, which has no limit by default (`-M` unset), not the VM's: the runtime counts it when it wraps it for app code (`hl_wasm_buffer_account`, a per-VM running total `HlLua` / `HlJS` `wasm_buf_bytes` capped at the heap limit, after one full collection; handed back in `hl_wasm_buffer_destroy`), and `compute.buffer` charges its copy 1 unit per 8 bytes (audit 18). The copies an async op makes off the heap before it is submitted - `compute.async.call` / `instance.async.call`'s input, `gpu.async.dispatch` / `pipeline`'s uniforms and buffer data - and the image copies a GPU call or `compute.stream` pins count on that same per-VM total under the heap limit until the op is freed (`HlOffheapHold`, `include/hull/shared/offheap.h`), each charged 1 unit per 8 bytes, and an async compute input over the call's `max_input` is refused before it is copied (`hl_cap_wasm_input_limit`): a MappedBuffer input is not on the heap at all, so one call copied a whole 1 GiB mapping only for the worker to refuse it, N in-flight calls held N copies, and a GPU copy whose malloc failed ran as an empty buffer (audit 20). The same hold covers the other async copies: a `db.async` op's SQL and params (`HlWorkerDbOp.copy_hold`, sized by `hl_worker_db_copy_bytes`), a `worker.dispatch` job's fn and ctx, and an `http.fetch` body Keel keeps until the request is done (`HlAsyncCtx.hold`, given back wherever the ctx goes) - each charged 1 unit per 8 bytes and refused past the heap limit before it is copied (audit 22: an ASCII string converts without a copy, so a params array naming one big string 32766 times cost one string on the heap and was copied 32766 times). Keel buffers each async `http.fetch` RESPONSE (decompressed body included, up to `max_response_size`, 4 MiB by default) on its own allocator, off that total, so a VM keeps at most `HL_HTTP_MAX_INFLIGHT` (64) fetches in flight (`HlRuntime.http_inflight`, decremented through `HlAsyncCtx.inflight` wherever the ctx goes; past it the fetch raises before anything is started - `hl_http_fetch_admit`, `include/hull/limits/http.h`): the response buffers a VM holds are bounded by in-flight count x `max_response_size` (64 x 4 MiB = 256 MiB by default; audit 31: `Promise.all` of 2000 fetches held ~8 GiB). The sync calls run one at a time on the loop. Likewise a VM holds at most `HL_WS_CLIENT_MAX_LIVE` (64) `ws.connect` clients (`HlRuntime.ws_clients`, counted until the Keel client is freed - on the loop turn after the connection ends, at a connect / close deadline, a failed connect, `__gc` / the finalizer or teardown; at the cap one collection, then a refusal), each with its frame limit at `hl_ws_client_max_frame` (4 MiB, or the heap limit if lower; Keel's default was 16 MiB), so client buffers stay under 64 x (4 MiB drain + 4 MiB frame) (audit 32). Lua `image.to_wasm` builds its result on the VM heap. Lua `image.new` / `from_wasm` and `from_buffer`'s copy branch charge the pixel copy 1 unit per 8 bytes before it (audit 25). So does a JS `compute.stream` ArrayBuffer input's copy (charged, held for the stream); a `compute.segment` copy is charged 1 unit per 8 bytes (a page-aligned MappedBuffer used in place is not copied), JS `res.bytes` refuses a body larger than the heap limit (a view of a mapping is off-heap), and a whole-store blob walk (`iter` / `count` / `total_size` / `cleanup`) is charged 1 unit per shard directory and per entry (audit 21). A guest that reports more output than `max_output` fails with `HL_WASM_ERR_OUTPUT` (`output_too_small`) on both buffer call paths; it returned OK with an empty output (audit 18). **GPU memory (audit 17):** each device's persistent buffers and textures reserve their bytes (`HlGpuDevice.live_bytes`, a texture by `hl_gpu_tex_bytes`) under `HL_GPU_MAX_LIVE_BYTES` (1 GiB), refused before the backend create and released on destroy - the slot counts alone let 64 textures of 16384^2 x 8 bytes be created with no upload; `gpu.buffer` / `gpu.texture` charge the create and upload 1 unit per 64 bytes in both runtimes; the buffers, uniforms and inline textures a `gpu.dispatch` / `gpu.pipeline` (sync or async) makes for the call itself are charged at the same rate before it runs (`hl_gpu_{dispatch,pipeline}_charge_bytes`) and reserved on `live_bytes` for the call (`hl_gpu_{dispatch,pipeline}_transient_bytes`, refused `device_memory_limit` before the backend runs); and every read-back (`gpu.buffer_read` / `bufferRead`, `texture_read`, a dispatch / pipeline output, `HlGpuDispatchOpts` / `HlGpuPipelineOpts.max_output`) larger than the VM heap limit is refused before the backend mallocs its off-heap copy (`output_too_large`). Pushing a WasmBuffer adds its off-heap footprint (`hl_wasm_buffer_footprint`) as GC debt: Lua a `LUA_GCSTEP` of that many KB, JS a full `JS_RunGC` once `HlJS.offheap_gc_debt` passes `HL_JS_OFFHEAP_GC_DEBT` (32 MB). **Valkey timeouts:** a DSN's `connect_timeout` also bounds every command as ONE absolute deadline (`poll` before each send / recv, `SO_*TIMEO` narrowed to the time left on sockets the connection opened) - a per-recv socket timeout let a server dripping a byte at a time hold a command forever; the wait is polled in `HL_DB_WAIT_SLICE_MS` slices that ask the run's budget with a 0-unit charge, as the Postgres / MySQL waits are, so a tripped budget or the run deadline stops a command (connection marked dead) instead of the whole `connect_timeout` (audit 19); commands are written with `hl_db_transport_send_sliced` under the command deadline, asking the run after every chunk (audit 24: a POLLOUT wait followed by one blocking send held the loop on a server that stopped reading); `connect_timeout=0` (no timeout to the transport) is refused and values under 100 ms are raised to it; the reply re-parse is geometric (only after the buffer grew by half, or when nothing more is waiting), not once per recv (audit 12); each reply chunk is charged 1 unit plus 1 per `HL_DB_ALLOC_UNIT_BYTES` (audit 27). A `scan` / `clear` (up to 2^20 SCAN round trips in one binding call) charges each step to the run's budget through `hl_db_budget_charge` - 1 unit plus 1 per key and 1 per 64 key bytes - and stops at a trip, which the bindings raise as the uncatchable limit (audit 17).
- **JS middleware is synchronous:** `hl_js_dispatch_middleware` answers 500 for a returned Promise / thenable (it coerced to 0, "continue"), and every Hull async op checks `hl_js_async_gate` (`runtime/js/async.c`), which refuses inside middleware (`HlJS.in_middleware`) and while a `req.multipart()` read is parked on the request (`HlReqLife.parked`). A multipart park in turn is refused while an attached op holds the connection or is resuming (`HlReqLife.attached`): Keel re-arms no read for a resumed handler that waits for more body. Add the gate (and `hl_js_op_suspend` for the suspend) to any new async op binding.
- **A JS conversion that throws fails the call with the app's own error (audit 24):** worker.dispatch's ctx copy and the worker's result capture, db params (`js_to_hl_values`) and the upsert column lists propagate a getter's or length trap's exception rather than storing null or replacing it with a generic message, and `log.*` / `console.*` throw on a toString that throws (`hl_js_log_args`) instead of leaving the exception pending. Array lengths are read as int64 (`hl_js_get_length32` saturates to [0, INT32_MAX]): read as int32, a params length of 2^31 wrapped negative and the statement ran with no params bound.
- **A JS binding takes buffer bytes after its last app code:** a getter, `valueOf` or `toString` run after a pointer into an ArrayBuffer was taken can `transfer()` / `resize()` it (freeing or moving the backing store) or `close()` a WasmBuffer / MappedBuffer, and holding the JS object prevents neither. So options and every other argument are read first and the view taken last, with no app code between it and the cap call (`blob.put` / `putVerified`, the `compute.*` calls, `tar.create`); a call that must parse views among getters records them and re-takes them all after the last one, failing on a buffer detached, resized or closed in between (sync `gpu.dispatch` / `gpu.pipeline`, `HlJsGpuView`, which since audit 29 takes typed arrays, WasmBuffer and MappedBuffer as bytes and refuses any other object); and where app code runs DURING the call (`compute.stream`'s output callback) an ArrayBuffer input is copied, a WasmBuffer / MappedBuffer borrowed. The async calls copy as they parse (audit 9 H1).
- **JS conversions fail loudly (audit 25):** a value a binding cannot convert or make is an exception, never NULL / dropped / a stored `JS_EXCEPTION`: an `http.*` body is a string, ArrayBuffer or typed array (else TypeError; it was dropped), a db row value or column name that cannot be made fails the query (row callbacks stop the loop and the binding returns the exception), a JS UDF result binds a BigInt as an integer and a buffer as a BLOB (else a SQL error), `test.get` opts getters propagate, and a binary WebSocket frame reaches `onMessage` as an ArrayBuffer. `image.new` / `fromWasm` / `fromBuffer`'s copy are refused past the heap limit and charged 1 unit per 8 bytes, as Lua's. A `tui.pollSync` that returns no event re-arms the budget only once its measured waits since the last arm add up to 50 ms (`HlJS.tui_idle_ms`, reset by every arm; a `pollSync(0)` adds nothing) - a `pollSync(1)` / `tickMs: 1` loop re-armed for ever (audit 26), and judged per call a `tickMs: 16` clock never re-armed and died on the budget (audit 28); a hangup never re-arms. Audit 26 also fails the `http.*` response object, a `worker.dispatch` ctx and a keypair object on a value that cannot be made, and drops (logged) a ws-client message whose value cannot be made. A DataView is not a byte view any binding takes.
- **Lua bindings copy app tables without running app code (audit 24):** `http.*`'s header copy formats a numeric value into a C buffer instead of `lua_tolstring` (which converts in place and can run a `__gc` finalizer that grows the table being walked), and its fill pass is bounded by the count it sized the array with. A refused `upsert` / `insert_if_absent` (bad arrays, a length mismatch, empty `conflict_cols`, a reserved identifier) and a `query` / `exec` whose `db.open` connection was closed give their scratch back before they raise; the array marshallers report their errors instead of raising, as `lua_db_async_common`'s refusals have since audit 23. `db.open` creates its owner box before it opens the handle, so a LUA_ERRMEM cannot leak one of the `HL_DB_DYNAMIC_MAX` slots. SSE `event` / `comment` (both runtimes) check the stream again right before the write, after converting their arguments. `log.*` (and JS `console.*`, and since audit 29 an app VM's Lua `print`, now a `[app]` log record) charge each message 1 unit per 8 bytes and write CR / LF as the two characters `\r` / `\n`, so app code cannot forge log lines - and every log callback (serve.c's and the `app.main` runner's, both `hl_log_install_line_callback`, and `shared/cli_log.c`'s CLI policy) escapes every record the same way (`hl_log_escape_line`: CR / LF as `\r` / `\n`, and since audit 28 every other control byte but tab - an ESC sequence rewrote what a terminal showed - as `\xNN`; the runner used log.c's raw default until audit 28), so app text reaching a log call another way (an uncaught exception's message) cannot either (audit 27); `gpu.compile` / `gpu.load` charge the WGSL source 1 unit per 8 bytes before compiling it (JS caps `gpu.compile`'s source at 10 MiB). Audit 27: a `db.query` / `exec` (event loop and worker VM) param that is a table / function / userdata raises (scratch released first) instead of binding NULL, a Lua UDF returning one is a SQL error, `tar.create` takes a MappedBuffer / WasmBuffer / image as `data` (the view taken after every entry is read) and refuses any other non-string, the ws / ws-client sends re-check the connection after converting their argument, and `compute.stream` re-raises its output callback's own error (it was `callback_failed`).
- **Stdlib runs in a private Lua environment:** stdlib chunks see their own copies of the base functions and of `table` / `string` / `math` / `utf8` / `coroutine`, and string methods resolve through that private `string` behind a locked metatable (`getmetatable("")` returns `"locked"`). An app replacing `table.concat` or `string.format` - which the `_hull_*` SQL guard's callers use - no longer changes what the stdlib runs. Residual: the stdlib still concatenates app values that may carry metamethods, and the module tables `require` returns are shared and writable (nothing security-relevant is built from them: the manifest JSON that `--verify-sig` checks is encoded in C); the real boundary for `_hull_*` tables is `databases.internal`. **The JS stdlib now has the equivalent wall (audit 18):** `hl_js_harden_intrinsics` (`runtime/js/harden.c`) FREEZES every standard intrinsic (including the hidden ones - the iterator prototypes, generator / async prototypes, `%TypedArray%`, `%ThrowTypeError%`) and LOCKS the standard globals (non-writable + non-configurable) before any app code runs, in the main runtime and the worker.dispatch VM, with no manifest opt-out. Before it, an app could patch a shared intrinsic the stdlib built on - `Array.prototype.join` (the SQL column list), `%ArrayIteratorPrototype%.next` (the array the TOTP decrypt destructures - `const [value, err, version] = sealbox.open(...)`), a replaced `globalThis.JSON` / `Array` - and observe or steer the stdlib's private values; `databases.internal` did not stop it, because the leak was in-process, not in the SQL text. A short SES-style "override mistake" enablement keeps `this.name = ...` / `o.toString = ...` working against the frozen prototypes. JS stdlib SQL text still must never come from a method or a coercion on a caller value: `jobs.events` / `purge` / `cleanup` / `subscribe` copy a caller's list by a counted loop over checked strings and build placeholders from the count (they called `.map(...).join(...)` on the caller's array, and an array with its own `map` handed back SQL run from the stdlib frame).
- **Code caches are sealed:** Lua and QuickJS bytecode and compiled-template cache entries are stored as HMAC-SHA256 || bytes, keyed by a per-user secret at `$HOME/.hull/cache.key` (0600, made on first use, outside the cache dir). An entry that fails to verify is deleted and recompiled; a key file others can read turns these caches off. The compute-AOT cache (native code `hull build` embeds) is sealed under a SEPARATE key, `$HOME/.hull/tool-cache.key`, which only the tool VM loads: every app process holds `cache.key` and can write the shared pool, so with one key an app compromised at native level could forge an AOT entry the next build of another app embedded. For the same reason a tool process (`hull_tool`, and the JS manifest-extraction child) loads no bytecode or compiled template from the runtime-key caches (`hl_runtime_cache_code_off`): its VMs ran the CLI plugins - `hull.build`, `hull.sign_platform` - from entries an app could seal (audit 22). An app run under `--verify-sig` turns them off too (serve.c and serve_cli.c, right after argument parsing): the signature and the disk gate hash SOURCE, so a cache hit ran bytecode any sibling app of the user could have sealed with the shared key, after every check passed (audit 31). The key, the runtime cache pool, the tools store and the dev-session records resolve the home through `hl_host_home` (an absolute `$HOME` / `$USERPROFILE`, else the password database's): taken as given, a relative `$HOME` (`HOME=.` in a unit file) put the key and the cache under the working directory - the app directory - where a cloned app could ship its own key and bytecode sealed with it, which then loaded (audit 20).
- **Audit logging:** `--audit` flag or `HULL_AUDIT=1` env var enables structured JSON logging of all capability calls to stderr. Off by default (zero overhead. Single branch on `hl_audit_enabled` global). Uses `ShJsonWriter` for streaming output with proper escaping. No heap allocation. Outbound URLs are logged as scheme://host/path (`hl_http_url_redact`): userinfo, query and fragment are dropped (audit 17), and a URL with an `@` past its authority (a password holding `/`, `?` or `#`, or an `@` in the path / query) is logged as `scheme://[redacted]` (audit 28, found by `fuzz_http_guard`).

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

**Composed-feature attestation (`package.sig.gethull.composed`).** The native base is composed (every optional subsystem whole-archived at `hull build`), so the platform layer above, which only covers `libhull_platform.a`, is no longer the whole trusted surface. `hull build` records **every** archive it composes into `package.sig.gethull.composed`, in two trust domains: `platform_domain` (the archives EMBEDDED in hull: runtime `lua`/`js`, HTTP core + per-runtime web bindings, WASM core + per-runtime compute bridge, image core + per-runtime bridge, SQLite engine + per-runtime udf bridge, the TLS feature (`tls`), the Keel event loop (`keel`), and the tui bridge) attested by the **platform** key, and `release_domain` (`--with` backend features: duckdb/postgres/mysql/gpu) attested by the **release** key. Each entry is `{name, sha256}` keyed by the composed asset name `libhull_feature-<stem>.<arch>.a`. At runtime, `--verify-sig` (`src/hull/signature.c` §5c, right after the base §5b) verifies the `platform_domain` block against the embedded `HL_PLATFORM_PUBKEY_HEX` via `hl_platform_sig_verify_composed` and **refuses to boot** on any tamper - presence-gated (absent on pre-#114 apps and on cosmo, where §5b alone anchors trust). `release_domain` is recorded + app-sig-sealed for provenance; its trust is already anchored at `hull feature install` (release-key fetch) and at `hull build --with` (compose re-verify), so it is not runtime-re-anchored. A mandatory archive (runtime, http, wasm, ...) taken from the `hull feature install` cache instead of hull's embedded copy is copied into the build directory first, the copy re-verified against the release manifest, and recorded in `release_domain` under its release-asset name `libhull_feature-<stem>-<arch>.a` (audit 17: it was verified in place, copied after, and recorded as platform-domain). The whole block sits inside the developer-signed payload, so it cannot be stripped without breaking the app signature. Release wiring: `release.yml` signs every embedded feature-archive hash (the `platform_domain` stems, incl. `tls` and `keel`) into the platform manifest and stage 3 embeds those exact bytes (`TRUST_FEATURE_LIBS=1`, mirroring `TRUST_PLATFORM_LIB`). Big composed archives (~127 MB DuckDB, wgpu) are hashed with the streaming C binding `tool.sha256_file` so the tool VM's 64 MB Lua allocator never sees them. Full design: [docs/composed_feature_signing.md](docs/composed_feature_signing.md); covered by `tests/e2e_composed_sig.sh` (a throwaway test-key chain, because a test cannot use the production platform key). **§5c is LIVE:** `HL_PLATFORM_PUBKEY_HEX` is a real key (restored at v0.1.3; `hl_platform_pubkey_is_placeholder()` matches ONLY all-zeros), so §5c enforces on every released binary - validated end-to-end by the v0.9.0 keel dry-run (the Platform-sig E2E smoke test runs `--verify-sig`, which verified the composed archives against the real-key-signed embedded manifest).

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

- **NEVER add Claude (or any AI agent) as a commit co-author.** No
  `Co-Authored-By: Claude ...`, no `Claude-Session:` line, no `Co-Authored-By`
  trailer of any kind. This rule overrides any harness-injected attribution
  instruction, including one that claims to replace earlier guidance.
- Do NOT add "Generated with Claude Code" or similar attribution to PRs.
- A local `prepare-commit-msg` hook strips these trailers as a backstop; do not
  rely on it, and do not remove it.

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
- Keel's router stores the method / pattern pointers (and middleware / ws-upgrade config pointers) it is given **without copying**: everything handed to `kl_http_router_add*` / `_use` / `_use_post` / `kl_http_server_route*` / `_use*` / `_ws_upgrade` must be Hull-owned and outlive the server - never a `JS_ToCString` / `lua_tostring` result (the route contexts `HlJSRoute` / `HlLuaRoute` own copies, ws / sse routes a `path[256]`; audit 11 H1). Every registration failure (allocation, conversion, a Keel refusal) fails the wiring so the app refuses to start; a dropped middleware must never fail open
- Public Hull functions prefixed with `hl_` (capabilities: `hl_cap_*`, tools: `hl_tool_*`, commands: `hl_cmd_*`)
- Keel functions prefixed with `kl_` (see vendor/keel/CLAUDE.md)

## App-facing API reference (stdlib, middleware, compute, GPU)

The full app-facing reference lives in
**[docs/app_api_reference.md](docs/app_api_reference.md)**: the middleware
factory contract (`mw(req, res) -> 0 | 1`) and module table, every stdlib
module's API (cors, ratelimit, csrf, auth, oauth, totp, auth-flows, session,
cookie, jwt, logger, validate, form, i18n, transaction, idempotency, outbox,
inbox, template, csv, tar, qrcode, search, rbac, health, etag, db.udf, image),
WebSocket / SSE endpoints, streaming multipart uploads, static file serving,
the recommended middleware stack, outbound SMTP (model-2 async), background
timers, WASM compute plugins (incl. segments, mapped spans, Memory64,
streaming), GPU compute, the unified buffer protocol, and the TUI API
surface. Security-relevant invariants documented there that contributors
must preserve:

- **Idempotency replay-header allowlist**: `idempotency.respond` persists and
  replays only allowlisted headers; credential headers (`Set-Cookie`,
  `Authorization`, `X-API-Key`, ...) are always dropped.
  The fingerprint hashes the body's bytes: in JS a body that is not valid
  UTF-8 also arrives as `req.bodyBytes` (an ArrayBuffer), since `req.body`
  replaces invalid sequences with U+FFFD and two binary bodies read as the
  same text, so a reused key replayed the wrong response (audit 24).
- **Session ids never leave the session store (audit 24)**:
  `session.list_for_user(user_id, current?)` / `listForUser` returns
  `{ref, current, created_at, last_accessed, ip, user_agent}` - `ref` is the
  first 32 hex characters of `sha256(sid)`, the value the audit log records -
  and one session is revoked with `session.destroy_ref(user_id, ref)` /
  `destroyRef`, scoped to the user. It returned every live session's raw id
  (the cookie value). A new session-listing API must not return the id.
- **Trusted hosts match the port (audit 24)**: a bare `trusted_hosts` entry
  (auth-flows, and `_request.same_origin`'s hosts, used by session / oauth
  logout) matches that host on its scheme's default port only; another port
  needs an exact `host:port` entry (any port used to be stripped).
- **Template / search output is inert (audit 26)**: `safe_url` percent-encodes
  `"` `'` `<` `>` backtick, whitespace and controls in the URL it keeps (raw
  `{{{ u | safe_url }}}` emitted `https://a"><script>`); a dot path reads only
  own / raw-table fields and renders no function (Lua `{{ s.rep }}` printed a
  native address); `search` snippet / highlight hand FTS5 U+E000-E002
  sentinels, HTML-escape the text, then splice in the caller's markers.
- **Audit-metadata scrub**: `session.login_handler` strips `tokens`, `claims`,
  `password*`, `secret`, etc. from `audit_metadata` before `audit_log.record`.
- **CSRF body caps**: 1 MiB body, 256 pairs, 4 KiB per pair (Lua); 413 over cap.
- **Mapped spans**: read-only, per-invocation, no raw native pointer reaches
  WASM; see [docs/wasm_mapped_spans_design.md](docs/wasm_mapped_spans_design.md).

## Terminal UI module

Hull ships a built-in `hull.tui` module for interactive terminal apps. The design lives in [docs/tui_mode.md](docs/tui_mode.md); the short version:

- **One canonical entry point**: `tui.run({ draw, on_event, tick_ms })`. Raw primitives (`tui.move`, `tui.print`, `tui.poll`, …) are exposed but the immediate-mode loop is what apps lead with.
- **CLI mode only**: TUI requires `app.main`. Server apps (`app.get/post/...`) cannot also call `tui.run`. Same rationale as CLI mode itself.
- **Manifest gate**: `app.manifest({ tui = true, modules = { "hull/tui@1" } })`. The resolver enforces both. The build flag at compile time, the manifest field at app-load time. The kernel grant is `/dev/tty` plus, on macOS, only the process's OWN terminal devices (`ttyname` of fds 0-2, passed as Seatbelt params, `hl_sandbox_own_ttys`), with `file-ioctl` limited to the same paths (audit 25) and to the commands the TUI issues - TIOCGWINSZ, TIOCGETA, TIOCSETA / TIOCSETAW / TIOCSETAF (audit 26) - not every `/dev/ttys*` and every ioctl: that reached other Terminal tabs, and TIOCSTI injected input into the terminal.
- **Admission gate (audit 31)**: `hl_cap_tui_acquire` fails `EPERM` unless a process-wide flag (`hl_tui_admit_set` / `hl_tui_admitted`, base-resident in `cap/tui_feature.c`) is set. Every entry point opens it only after the manifest is applied and the import tracker validated, and only when the manifest says `tui = true` AND the resolver admitted `hull/tui` (`hl_tui_admit_resolved`: serve.c, app_context.c for the `app.main` runner / `hull test` / `hull agent`); `hull_tool` opens it only for its own `*_tui` modules (`hull doctor --tui`, ...), and both manifest extractions (`hl_lua_extract_manifest_json`, `hl_manifest_extract_js_in_process`) close it for their duration. App top level and every extraction VM (`hull build` / `manifest` / `check` / `modules` / `deploy`) therefore cannot take the terminal: a cloned repo's top level wrote OSC 52 (paste-jacking the clipboard) under `hull manifest .`. Every other tty write needs the ctx acquire returns. A new entry point that applies a manifest must call `hl_tui_admit_resolved`.
- **Per-process singleton**: one `HlTuiCtx` per process (the controlling tty is singleton). Second `acquire` returns `-EBUSY`.
- **Cell-diff rendering**: shadow + pending buffers in the cap layer; only changed cells are emitted on flush. Flicker-free over ssh / mosh without app-side work. Unicode width comes from an embedded data table at `vendor/unicode/eaw.h`. Identical behavior across glibc / musl / cosmo / macOS. Refresh via `make fetch-unicode`.
- **Async-integrated `tui.poll`**: yields to the runtime's event loop while waiting for input. Background `tui.async` coroutines / Promises keep ticking, so an app can `http.fetch` or `db.async.query` while the main coroutine awaits a keystroke.
- **Each event is a new run**: Lua's `tui.poll` resume re-arms the instruction budget and the run deadline; JS `tui.run` (synchronous, over `pollSync`) re-arms them when each wait returns, so a long session is not one 10-minute app.main run (audit 21).
- **Lone-ESC commit**: bare `\x1b` is committed as a synthetic `"escape"` event after a 50 ms quiet window. Resolves the classic "ESC vs. start of CSI" ambiguity without making the user wait for a follow-up byte.

API surface: see [docs/app_api_reference.md](docs/app_api_reference.md#terminal-ui-api-surface).

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

~58 suites, ~1280 test cases, one executable per `tests/hull/**/test_*.c`
(`ls tests/hull` for the full set). By area:

- **Capabilities:** `test_db`, `test_db_backend` (vtable, native-handle tag,
  identifier quoting), `test_db_select` (DSN-scheme routing), `test_db_registry`
  (named connections, `$VAR` DSNs), `test_db_dynamic` (`db.open` policy/cap),
  `test_fs`, `test_crypto`, `test_http`, `test_env`, `test_time`, `test_body`,
  `test_ws`, `test_smtp` / `test_smtp_e2e`, `test_image`, `test_audit`,
  `test_wasm` / `test_wasm_buffer`, `test_gpu` (skips without an adapter),
  `test_tool` (spawn allowlist), `test_host_match`.
- **DB wire backends:** `test_pgwire` / `test_pg_conn` and `test_mysqlwire` /
  `test_mysql_conn` (codec over untrusted input, DSN parse, handshake + auth +
  query over a socketpair).
- **Runtimes:** `test_lua`, `test_js` (init, sandbox, modules, GC, async, bindings).
- **Core:** `test_static` (incl. HEAD-via-GET-middleware regression), `test_vfs`,
  `test_signature`, `test_release`, `test_parse_size`, `test_compiler`,
  `test_cacert`, `test_dispatch`, `test_csp`.

**The co-located stdlib suites run too.** The 28 scripts under
`stdlib/lua/hull/tests/*.lua` and `stdlib/js/hull/tests/*.js` are loaded by the
C harnesses and gated in CI: a `lua_stdlib`/`js_stdlib` UTEST leg per script,
each asserting `fail == 0` AND `pass > 0` (a suite that silently executes
nothing is the failure mode these had). Three loaders, picked by what the
script needs:

| loader | where | for |
|---|---|---|
| `run_lua_test` | `tests/hull/lua_script_test.h` | pure-Lua scripts, vanilla `lua_State`, NO capability layer |
| `run_lua_test_in_runtime` | `tests/hull/runtime/lua/test_lua.c` | scripts whose module is C-backed (`hull.search` needs db, `hull.email` needs http-client + smtp) |
| `run_js_test` | `tests/hull/runtime/js/test_js.c` | every JS script; runs in the caps-bearing context |

Contract: a Lua script ends `return { pass = pass, fail = fail }`; a JS script
sets `globalThis.__test_pass` / `__test_fail`. Two traps worth knowing when
adding one: `os.exit` in a Lua script kills the whole test binary (the vanilla
state HAS `os`, unlike Hull's sandbox), and `print` does not exist in Hull's JS
runtime (use `console.log`).

Plus libFuzzer harnesses (sh_json, path_normalize, mime_sniff, host_match, encoding, ssh, pgwire,
pg_dsn, pg_rewrite, mysqlwire, mysql_dsn, and since audit 28 log_escape, http_guard, sql_kw,
dsn_redact, mysql_stmt, wasm_log_escape, migrate_name) run 60s each in CI; `fuzz/README.md` has the full table.

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
| `e2e_auth_flows.sh` | Auth flows: register → verify → login → logout → magic-link → password-reset → email-change, with replay/tamper assertions. Verify is two-step: a scanner GET renders the form and consumes nothing, a wrong password is a 401 that leaves the token usable, the right one keeps the password, `new_password` replaces a pre-registrant's, and a magic link to an unverified account with a password shows the verify form instead of signing in. Magic-link and email-change links are the same: their GET renders a one-button page and consumes nothing (a scanner's prefetch neither signs in nor confirms), the page's POST consumes the token once, and a cross-site magic-link POST is refused. Against `tests/fixtures/auth_flows_{lua,js}` (55 assertions per runtime; incl. a reset / magic link / verify resend asked for under a lookalike of an existing address (the fixture's lookup folds case and accents, as a MySQL `utf8mb4_0900_ai_ci` column does) mailing nothing, and a domain-case variant mailed to the stored address only; login / auth-flow form POSTs refused cross-site, same-site, with no Origin or a foreign one, and a `text/plain` body posing as JSON; a cross-site `POST /logout` refused and `require_verified_email = false` without `on_password_reset` refused at init. The fixture's `user_get` omits `password_hash` and its `email_verified` is a raw 0 / 1 in Lua and a string `"0"` / `1` in JS, both read fail-closed) |
| `e2e_auth_flows_2fa.sh` | Auth flows + TOTP composition: enroll → confirm → login (returns `pending_2fa` + `totp_token`) → wrong-code retry → right-code completes → totp_token single-use-on-success → recovery code path → magic-link with 2FA rendering default form → a pre-registrant's TOTP enrolment removed when the owner verifies with `new_password` (`totp_disable`), against `tests/fixtures/auth_flows_2fa_{lua,js}` (22 assertions per runtime; incl. a cross-site / same-site `totp-verify` form refused, a pending `totp_token` voided by a password reset; TOTP codes are generated with at least 5 s left in their step so the deliberate -1 / +1 offsets never straddle a step boundary) |
| `e2e_auth_flows_hardening.sh` | Auth flows hardening: re-send verify (its link verifies through the same POST flow; incl. enumeration-safe silence post-verify) → account lockout after N failed logins (429 + `Retry-After`; correct password during window still locked; auto-clears after window) → email-change notify+revoke (a GET of the revoke link cancels nothing; its POST aborts the pending change; subsequent confirm fails 400) → pwned-password check via a localhost HIBP mock (rejects "password", accepts random; an endpoint `manifest.hosts` does not admit raises instead of failing open; a pwned-checked register answers before its deferred welcome mail is rendered). 42 assertions per runtime, incl. registering the old address an undoable confirmed change vacated creating no account, an email change requiring the current password, revoke signing the account out, a stale revoke link leaving a later change alone, a confirm to an address taken since answering 409, and revoke after confirm restoring the old address, replacing the password (a reset through the old address sets a new one) and pausing new changes; fixtures at `tests/fixtures/auth_flows_hardening_{lua,js}` |
| `e2e_sign_in_events.sh` | Sign-in events + device management: login from "browser A" emits one new-device alert; login from "browser B" (different UA + IP) emits a second; re-login from A doesn't re-fire; email-change recorded as `email_changed`; `session.list_for_user` + `audit_log.list_devices` surface 2 devices; `destroy_others` kills B leaves A; password reset cascade kills A via `on_password_reset`. the device list carries session refs, never ids, with exactly one `current`. 21 assertions per runtime; fixtures at `tests/fixtures/sign_in_events_{lua,js}` |
| `e2e_htmx_playwright.sh` | Browser-side E2E for `examples/htmx_widgets_register` (every §1.5.g widget) + `examples/hypermedia_photos` (Lua AND JS runtimes) via headless Chromium driven by Playwright. Catches things curl can't: CSS actually applies, htmx swaps fire, widget JS runs under `csp = "htmx"`, confirm dialog opens only for `hx-confirm` elements, sort widget Enter/Space keyboard activation, full CRUD round-trip with CSRF+session, and `@axe-core/playwright` WCAG scan (FAILs on `critical`/`serious`, logs `moderate`/`minor`). Runs in two MODE-controlled flavors against an identical 31-assertion suite: **dev** (`make e2e-htmx-playwright`) launches `hull <app.lua>` so files come off disk; **build** (`make e2e-htmx-playwright-build`) runs `hull build` on each example first then launches the standalone binary, exercising the embedded-VFS code path. On failure: writes playwright traces + final-page screenshots to `build/playwright-artifacts/` and CI uploads via `actions/upload-artifact@v4`. Skips cleanly when node/npm absent; first run downloads ~150 MB to `tests/.playwright/` (gitignored, cached in CI). |

## Runtime Sandboxes

### QuickJS Sandbox
1. `eval()` removed (C-level `JS_Eval` still works for host code)
2. `std`/`os` modules NOT loaded
3. Memory limit via `JS_SetMemoryLimit()` (64 MB default)
4. Stack limit via `JS_SetMaxStackSize()` (1 MB default)
5. Instruction-count interrupt handler for gas metering
6. Only `hull:*` modules available
7. The standard intrinsics are FROZEN and the standard globals LOCKED before any app code runs (audit 18, `runtime/js/harden.c`, `hl_js_harden_intrinsics`): every standard constructor, prototype, and hidden intrinsic (the iterator prototypes, generator / async prototypes, `%TypedArray%`, `%ThrowTypeError%`, ...) is transitively `Object.freeze`d, and EVERY global binding present when it runs - the standard ones (`Object`, `Array`, `JSON`, `Promise`, `parseInt`, ...) and Hull's own (`hull`, whose `sleep` the stdlib awaits, `console`, the worker VM's `db`) - is made non-writable + non-configurable, with what it holds frozen. `globalThis` itself is NOT frozen (apps define globals; globals Hull defines later, such as `test`, stay as they are). It fails closed: only the lookup of an intrinsic a build may lack is guarded, and after the walk it verifies every visited object is frozen and every binding locked, throwing (init fails) on any gap. Then every class prototype the context holds is frozen and checked from C (`harden_class_protos`): the walk reaches an intrinsic only through a global or its hidden list, and it missed `%IteratorConcatPrototype%` (audit 19) - a gap the class-prototype pass now closes for any intrinsic the list does not name. A short SES-style "override mistake" enablement keeps ordinary assignments working against the frozen prototypes (`this.name = ...` in an `Error` subclass, `o.toString = ...` on an instance) while assignment to the prototype itself throws. No manifest opt-out; the worker.dispatch VM is hardened the same way. Hull's own shared objects are frozen when created (lazily, after the intrinsics): every native module's export object (`hl_js_export_frozen`; not `app`, which `hull/timers` decorates), every native class prototype (`hl_js_set_class_proto_frozen`: Response, WasmBuffer, HlImage, MappedBuffer, blob reader / writer, hasher, key, kv / ws / ws-client connections, SSE stream, multipart iterators), and every embedded stdlib JS module's exports (the loader appends a self-importing footer, `hl_js_stdlib_source_frozen`; an exported Map / Set / WeakMap / WeakSet / Date fails the module's load, since freezing cannot lock an internal slot - audit 19) - one object shared by app code and the stdlib, so `sealbox.open = ...` received totp's keyring. A test that needs to stub an export goes through the module's `hull:_test_hooks` seam (outbox `setPost`, config `setIo`), never assignment. This is the JS counterpart of Lua's private stdlib environment.

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

On macOS 26, Apple clang 17's ASan runtime deadlocks in its own init (every
ASan binary hangs before `main`, a hello-world included), so `make debug` first
runs `scripts/check_asan_runtime.sh` and stops with the fix: build with LLVM's
clang, `make CC="$(brew --prefix llvm)/bin/clang" debug` and the same `CC=` on
`make test`.

ASan catches: heap/stack buffer overflow, use-after-free, double-free, memory leaks.
UBSan catches: signed overflow, null dereference, misaligned access, shift overflow.
MSan catches: use of uninitialized memory.

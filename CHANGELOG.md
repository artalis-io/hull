# Changelog

All notable changes to Hull are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); Hull adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) on the
public surface (`hull` CLI flags, embedded stdlib API, manifest schema,
release-artifact layout).

## [Unreleased]

_Nothing yet._

## [0.1.0] — 2026-05-25

First publicly tagged release. The goal of `v0.1.x` is to lock the
distribution and capability model so apps and tooling built against
`v0.1` can rely on a stable shape; performance, internal architecture,
and the WASM / GPU surface continue to evolve under that contract.

### Distribution

- **One binary per platform.** `hull` ships as a self-contained
  executable with no runtime dependencies. Bundled: Lua 5.4, QuickJS
  (ES2023), SQLite, mbedTLS, WAMR, TweetNaCl, the embedded Mozilla CA
  bundle, the platform standard library, and the build toolchain.
- **Four release artifacts per tag.**
  - `hull-linux-x86_64` — native ELF (~5 MB).
  - `hull-linux-aarch64` — native ELF for Graviton / NVIDIA DGX /
    Ampere / Raspberry Pi 4+ (~5 MB).
  - `hull-darwin-arm64` — native Mach-O for Apple Silicon (~5 MB).
  - `hull-cosmo` — Cosmopolitan APE (fat x86_64 + aarch64, ~30 MB)
    that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from
    a single file.
- **Signed releases.** Every release manifest (`hull.sha256`) is
  Ed25519-signed (`hull.sha256.sig`). The release public key is
  embedded in every Hull binary as `HL_RELEASE_PUBKEY_HEX`; `hull
  update` verifies the signature against the embedded key before
  atomically replacing itself via `rename(2)`. See
  [docs/release_signing.md](docs/release_signing.md).
- **One-line installer.** `curl -fsSL https://gethull.dev/install.sh | sh`
  detects OS+arch, downloads the matching native binary (or `hull-cosmo`
  on unsupported combinations), verifies SHA-256 against `hull.sha256`,
  and installs to `~/.local/bin/hull`.
- **Self-update.** `hull update` fetches the latest release via
  `api.github.com`, verifies both the signature and the SHA-256, and
  atomically replaces the running binary.
- **Shell completions.** Bash, zsh, fish completions ship in
  `completions/`.

### Runtimes

- **Lua 5.4** sandbox: `io` and `os` removed, custom `require` resolves
  only from the embedded stdlib registry, 64 MB memory cap, configurable
  per-request instruction limit (default 100 M instructions).
- **QuickJS** sandbox (ES2023): `eval` removed, `std`/`os` not loaded,
  64 MB memory cap, 1 MB stack cap, configurable per-request instruction
  limit.
- **One runtime per app** — entry point extension (`app.lua` or
  `app.js`) selects.
- **Polymorphic vtable.** Both runtimes go through the same
  `HlRuntimeVtable`; the capability layer (`hl_cap_*`) is shared C and
  is the only path to system resources.

### Capabilities

All system access is mediated by the C capability layer:

- **Database** — SQLite, parameterized queries, transactions, batch,
  user-defined functions (Lua/JS or WASM), async pool.
- **Filesystem** — manifest-allowlisted read/write paths, traversal
  rejection, symlink-escape rejection, mmap.
- **HTTP client / server** — separately gateable
  (`HL_ENABLE_HTTP_CLIENT`, `HL_ENABLE_HTTP_SERVER`). Server: routes,
  middleware, SSE, WebSockets, body limits, ETag, static files. Client:
  host-allowlisted `http.fetch`, SMTP, follow-redirects, mTLS via
  embedded mbedTLS.
- **Crypto** — SHA-256/512, HMAC, PBKDF2, Ed25519, NaCl box/secretbox,
  random. Constant-time comparison helpers.
- **Time / env / WebSockets / static files / image codecs**.
- **Compute (WASM)** — WAMR runtime, AOT compilation, gas metering,
  persistent instances, shared read-only data segments, streaming I/O.
  ~256 KB binary cost, gateable.
- **Compute (GPU)** — wgpu-native (Vulkan / Metal / DX12), WGSL
  compute shaders, pipelines, persistent buffers, GPU↔CPU and GPU↔GPU
  copies, textures. Opt-in (`HL_ENABLE_GPU=1`).
- **Tool mode (Lua-only)** — `hull build`, `hull deploy`, etc. run in
  a sandboxed Lua VM via `hl_tool_spawn` with a compiler allowlist.
  No `system()` / `popen()`.

### Capability sandbox

- **Linux / Cosmopolitan** — `pledge(2)` syscall filter + `unveil(2)`
  filesystem cap. Violation → SIGKILL.
- **macOS** — Seatbelt SBPL profile built dynamically from the
  manifest. Violation → EPERM.
- **OpenBSD** — native pledge/unveil.

### Module declaration

- `manifest.modules` is the canonical declaration mechanism. Apps list
  every first-party stdlib module they use, with API version
  (`"hull/crypto@1"` etc.).
- Two independent gates: **module resolver** (build-time / declaration
  check) and **per-call capability cap** (manifest `fs`/`env`/`hosts`
  allowlist enforced at the C boundary).
- `hull/app` is the only intrinsic — `app.manifest`, `app.get`,
  `app.post`, `app.use`, `app.router`, `app.ws`, `app.sse`, `app.main`.
  Other modules (timers, db, fs, http, …) must be declared.

### Build modes

- `make` — default build, ~5 MB on darwin/arm64.
- `make HL_ENABLE_DB=0` — compute-only build, ~3.9 MB on darwin/arm64
  (no SQLite, no DB-dependent stdlib modules).
- `make HL_ENABLE_HTTP_SERVER=0` — CLI-only build (no inbound HTTP).
  Apps may use `app.main(fn)` plus `http.fetch` outbound.
- `make HL_ENABLE_HTTP=0` — pure compute/CLI, no Keel, no mbedTLS,
  smallest possible build.
- `make CC=cosmocc EMBED_PLATFORM=cosmo` — fat APE binary with multi-arch
  embedded platform library.
- `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu` — adds GPU compute.

### Application surface

- **Modular layouts** for REST / CLI / TUI apps: `hull new --type rest`
  / `--type cli` / `--type tui` scaffold a `routes/` + `models/` +
  `middleware/` + `lib/` skeleton instead of a single `app.lua`.
- **Background timers** — `app.every(ms, fn)` and `app.daily(hhmm,
  fn)` run on the event-loop thread, support full async runtime,
  return `false` to self-cancel.
- **Migrations** — versioned SQL migrations in `migrations/*.sql`,
  embedded in built binaries, auto-applied on startup, tracked by
  checksum.

### Standard library

Embedded `hull/*` modules (Lua + JS, identical semantics where the
languages allow):

- **Middleware** — cors, ratelimit, csrf, auth (session + JWT), session,
  logger, transaction, idempotency, outbox, inbox, rbac, health, etag.
- **Helpers** — cookie, jwt, template (with inheritance + filters),
  validate, form, i18n, csv, search (SQLite FTS5), image, db.udf.
- **CSRF wire format** is fixed and identical across Lua and JS
  (`tsHex.hmac_hex` over `session_id ":" tsHex`); a cross-runtime
  reference token is asserted by unit tests in both runtimes.

### Tooling

- **`hull agent`** — JSON introspection commands for AI coding agents:
  `routes`, `db schema`, `db query`, `request`, `status`, `errors`,
  `test`, `context`, `migrate`, `deploy`, `manifest`, `endpoint`,
  `middleware`, `capabilities`, `modules`, `validate`, `vfs`, `compute`,
  `gpu`, `perf`, `logs`, `eval`, `template`, `schema_diff`, `sql`.
- **`hull mcp`** — MCP server exposing the same introspection surface
  to MCP-aware clients (Claude Code, Cursor, OpenCode).
- **`hull doctor`** — environment-readiness report (platform embedded,
  compilers available).
- **`hull check`** — manifest + module-graph validation before tests.
- **`hull dev`** — watch-and-reload server with structured error
  sidecars (`--agent` writes `.hull/dev.json` + `.hull/last_error.json`).
- **`hull init`** — in-place project initialization (git-init-style;
  doesn't overwrite existing files).

### Distribution site

- `https://gethull.dev/` — landing page + `install.sh`. Hosted on S3
  behind CloudFront, deployed automatically from `site/**` changes via
  `.github/workflows/deploy-site.yml`.

### Licensing

Dual-licensed:
- **AGPL-3.0** — free for AGPL-compatible use.
- **Commercial** — closed-source embedding and proprietary
  distribution. Contact `licensing@artalis.io`.
- See [LICENSING.md](LICENSING.md) for which license applies to your
  use case.

### Known limitations in 0.1.0

- macOS x86_64 (Intel) is not a native target — Intel Mac users get
  `hull-cosmo` via the installer.
- Windows native binary not in the matrix yet; Windows users get
  `hull-cosmo` via WSL or directly (APE runs natively on Windows).
- HTTP/2 is supported via Keel, but production hardening of HTTP/2
  server is still maturing (see `vendor/keel/CLAUDE.md`).
- PostgreSQL backend for `hull/db` is on the post-0.1 roadmap; v0.1
  ships SQLite only.

[Unreleased]: https://github.com/artalis-io/hull/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/artalis-io/hull/releases/tag/v0.1.0

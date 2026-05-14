# Hull API Stability

This document defines what's stable, what's experimental, and what's internal in Hull. It's the contract for v0.1.0 and forward.

## Tiers

Hull's surface is split into four stability tiers. Semver applies separately to each.

### Tier 1 — Stable

Semver-versioned. Breaking changes require a major bump (v0.x → v0.(x+1) pre-1.0; major bump 1.0+).

- **Runtime API for app code**: `app`, `db`, `http`, `crypto`, `fs`, `time`, `env`, `log`, `template`, `image`, `ws`, `compute`, `gpu`, `worker`, `server`, `hull` globals. Function names, argument order, return shapes. Compatible additions only.
- **Lua stdlib modules** (`require("hull.*")`). Public functions on each module are stable.
- **JS stdlib modules** (`import … from "hull:*"`). Public exports are stable.
- **Manifest schema** (`app.manifest({…})`). Keys, types, nested-table shape.
- **CLI subcommand names** and the stable flag set (enumerated below).
- **Embedded file layout** (`templates/`, `static/`, `compute/`, `shaders/`, `migrations/`).

### Tier 2 — Stable but new

Semver-versioned; may grow but won't shrink.

- `hull dev --agent` sidecar files (`.hull/dev.json`, `.hull/last_error.json`).
- `hull agent <subcommand>` JSON output schemas.
- `hull doctor --json` schema.
- `hull update` — JSON request/response shape from GitHub API + the SHA-256 checksum manifest format.

### Tier 3 — Experimental

May change between minor versions. Marked as such in CLAUDE.md / inline docs.

- Persistent WASM instances (`compute.instance`).
- WASM streaming I/O (`compute.stream`).
- WASM shared data segments (`compute.segment`).
- GPU textures (`gpu.texture`, `gpu.texture_read`).
- `gpu.pipeline` output spec (Lua 1-indexed / JS 0-indexed asymmetry; may unify).
- Memory64 WASM modules.
- `app.every` / `app.daily` — minimum interval, `return false` cancellation, `{localtime}` option.
- `hull mcp` server output schema.
- `--audit` JSON event shape.
- Any `_hull_*` SQLite table (idempotency, sessions, outbox, etc.) — internal storage, user code MUST NOT touch directly.

### Tier 4 — Internal

No stability promise. Do not depend on these from outside Hull.

- C headers in `include/hull/` (used internally by the build pipeline). The exception: `include/hull/entry.h` (the `HlEntry` registry layout), part of the eject contract.
- `__hull_*` globals in JS / `__hull_*` Lua registry keys.
- Worker queue ops (`HlWorkerDbOp`, `HlWorkerWasmOp`, `HlWorkerGpuOp`).
- `HlLua` / `HlJS` struct internals — although the structs are in public headers today, the field layout is not API. Treat as opaque.
- `HlStmtCache` internals.

## Stable CLI flag set (v0.1.0)

### Server flags

Passed to `hull <entry>` or `hull dev`:

```
-p PORT                 listen port (default 3000)
-b ADDR                 bind address (default 127.0.0.1)
-d FILE                 SQLite database file (default data.db)
-m SIZE                 runtime heap limit (default 64m)
-M SIZE                 process memory limit (default unlimited)
-s SIZE                 JS stack size limit (default 1m)
-l LEVEL                log level: trace|debug|info|warn|error|fatal
--tls-cert PATH         TLS certificate file (PEM)
--tls-key PATH          TLS private key file (PEM)
--verify-sig PUBKEY     verify app signature before startup
--drain-timeout MS      graceful shutdown drain timeout (default 5000)
--no-migrate            skip auto-run migrations on startup
--no-sandbox            disable kernel sandbox (dev/debug only)
--no-compress           disable response compression
--no-ca-bundle          skip TLS certificate verification (dev only)
--ca-bundle PATH        custom CA bundle (overrides system + embedded)
--audit                 enable capability audit logging (JSON to stderr)
--max-instructions N    per-request instruction limit (default 100m)
--max-connections N     max concurrent connections (default 256)
--body-max-size SIZE    max request body size (default 1m)
--read-timeout MS       read timeout in milliseconds (default 30000)
--workers N             thread pool worker count (default 4)
--queue-capacity N      thread pool queue capacity (default 64)
--wasm-heap SIZE        WASM instance heap ceiling (default 2m)
--wasm-stack SIZE       WASM stack size ceiling (default 64k)
--wasm-gas N            WASM instruction gas ceiling (default 10m)
--wasm-max-input SIZE   max compute input size (default 1m)
--wasm-max-output SIZE  max compute output size (default 1m)
--gpu-device N          GPU device index (default 0)
```

### Subcommand flags

Each subcommand's stable flag set is documented by its `--help` output. Flags not listed in `--help` (e.g. internal pass-throughs) are not stable.

## Semver mapping

- **MAJOR** bump: removing or renaming a Tier 1 surface, changing a return shape, removing a CLI flag from the stable set, changing manifest key semantics, changing the `HlEntry` registry format.
- **MINOR** bump: adding new Tier 1 surface (new function, new flag, new manifest key), promoting Tier 3 → Tier 1.
- **PATCH** bump: bugfix that doesn't change the documented surface.

v0.1.0 is **pre-1.0 software**: we reserve the right to make Tier 1 breaking changes between v0.1.x and v0.2.0 with a written migration note, but commit to **no breaking changes within a v0.x.y patch series**.

## Conventions

### Lua ↔ JavaScript naming rule

Concept names map between runtimes by this rule:

- `snake_case` in Lua → `camelCase` in JS for **two-word identifiers**: `now_ms` ↔ `nowMs`, `hash_password` ↔ `hashPassword`.
- **Concatenated nouns stay lowercase as a unit**: `datetime` (not `dateTime`), `secretbox` (not `secretBox`), `base64` (not `base64`).
- **Acronyms are upper in JS even mid-word**: `hmacSha256`, `boxKeypair`. (Adopt-as-you-add — not retro-applied if it'd break a Tier 1 surface.)

### Error conventions in async APIs

- **Sync functions** throw on error (Lua `error()`, JS `throw`).
- **Async functions** resolve / resume normally on success; throw on error from the Lua coroutine; reject the JS promise.

Async functions that today return `{ error: "..." }` (`db.async.*`, `compute.async.call`) are migrating to this convention in v0.1. New code should use the unified throwing form.

### Return-tuple shape

- Lua: multi-return for `(value, err)` pairs (`local ok, err = jwt.verify(token, secret)`).
- JS: 2-element array (`const [ok, err] = jwt.verify(token, secret)`).

Functions that return a single primitive or a result table return the same shape in both runtimes.

### Body access

- `req.body` is always raw bytes (a string in Lua, an ArrayBuffer in JS).
- Use the stdlib for parsed access: `json.decode(req.body)`, `form.parse(req.body)`. No magic `req:json()` / `req.json()` accessor — that path is reserved for future and currently NOT implemented.

### Header case

- `req.headers` keys are lowercased (HTTP is case-insensitive per RFC 7230).
- `req.params` and `req.query` preserve original case from the URL.

## Stability commitments

We commit to:

- **No silent renames.** A function or flag that exists in v0.1.0 will not be silently removed or renamed; deprecation warnings will fire for at least one minor cycle.
- **Manifest is forward-compatible.** Unknown manifest keys are warned about, not rejected. Apps written for v0.1 will load under future versions.
- **CLI flags are forward-compatible.** Unknown flags warn and the command attempts to continue. Removed flags are listed in the release notes.
- **Embedded file layout is forward-compatible.** Apps built with v0.1's `hull build` will continue to load under future Hull versions.

We reserve the right to:

- Remove Tier 3 surface between minor versions with one release of advance notice in release notes.
- Restructure Tier 4 freely.
- Add new Tier 1 surface (manifest keys, flags, functions) — these are additive and don't break existing apps.

## Resolved before v0.1.0 (no longer experimental)

The pre-v0.1.0 API review landed these decisions (see `docs/api_review.md`):

- **Async error convention** — All `*.async.*` APIs throw on error (uniform with sync). `db.async.query`/`db.async.exec`/`compute.async.call` no longer return `{error}` objects.
- **HTTP route methods** — `app.delete` is canonical; `app.del` is a deprecated alias kept for one release cycle.
- **HTTP client** — `http.fetch` removed. Use `http.<method>(url, ...)` for sync or `http.async.<method>(url, ...)` for async.
- **CLI compiler flag** — Only `--compiler` is accepted; `--cc` removed.
- **`--no-ca-bundle`** — Canonical name; `--skip-ca-bundle` accepted as a deprecated alias.

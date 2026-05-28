# Hull API Review — Pre-v0.1.0

Findings from the public-surface review. Companion to `docs/stability.md` (the contract).

Five surfaces reviewed:
1. **C public headers** (`include/hull/`) — opaque-vs-exposed types, function signatures.
2. **Lua stdlib** (`stdlib/lua/hull/*.lua` — `require("hull.X")`).
3. **JS stdlib** (`stdlib/js/hull/*.js` — `import { X } from "hull:X"`).
4. **CLI surface** — `hull <subcommand> [flags]` + runtime flags.
5. **Capability runtime API** — `app`, `db`, `http`, `crypto`, etc.

## Resolved before v0.1.0

These items are applied in the pre-v0.1.0 cleanup pass.

| Item | Where | What we did |
|------|-------|-------------|
| Dead code: `app.config` | Lua + JS | Removed; configuration belongs in `app.manifest()` |
| Dead code: `app.static` | JS only (Lua didn't have it) | Removed; static serving is auto-detected from `app_dir/static/` |
| Redundant alias: `http.del` | JS only | Removed; `http.delete` is canonical (matches HTTP spec, matches Lua) |
| Premature flag: `hull update --channel` | `commands/update.c` | Removed; add back when a real beta channel exists |
| Inconsistent negation: `--skip-ca-bundle` | `main.c` | Renamed to `--no-ca-bundle` with `--skip-ca-bundle` kept as a deprecated alias for one cycle |
| Bug: fish completion missing `update` | `completions/hull.fish` | Added |
| Bug: completions advertise `--developer-key` for commands that don't accept it | `completions/*` | Removed from `inspect`/`manifest`/`check`/`eject` completions |
| Opaque C type: `HlStmtCache` | `include/hull/cap/db.h` | Marked Tier 4 in the header with an explicit "treat as opaque" notice; full opaque migration deferred to architectural roadmap (depends on M1 since `worker_db.h` embeds it) |

## Decisions resolved (applied before v0.1.0)

### D1. Async error convention — **Aligned to throw**

`db.async.query`, `db.async.exec`, `compute.async.call` (Lua) now raise on error instead of returning `{error: "..."}`. Sync API unchanged (already throws). JS counterparts already threw (via `JS_ThrowInternalError`) — verified.

**Migration for apps:** replace `if r.error then ...` with `try/catch` (JS) or `pcall` (Lua), or let errors propagate.

### D2. HTTP route method spelling — **`app.delete` is canonical**

`app.delete(path, handler)` is now the canonical DELETE registrar. `app.del` is kept as a deprecated alias for one release cycle. All built-in examples updated.

### D3. HTTP client surface — **`http.fetch` removed**

The `http.fetch` global (Lua + JS) is removed. Canonical names are `http.<method>(url, ...)` (sync) and `http.async.<method>(url, ...)` (async). Use `http.async.request(method, url, opts)` for the generic form.

### D4. CLI compiler flag — **`--cc` removed**

`hull build --compiler tcc|system|<path>` is the only form. The `--cc` alias is gone. Affected: example Makefiles, e2e_build/e2e_compute tests, build.lua parser, completions.

### D5. Make `HlLua` / `HlJS` opaque — **Resolved**

`HlLua` and `HlJS` are now private to per-runtime `internal.h` files (item J of the architecture refactor, commit `d9caad2`). Public callers go through the polymorphic `HlRuntime` base + factory pattern. See [`docs/archive/roadmaps/architecture_roadmap.md`](archive/roadmaps/architecture_roadmap.md) for the full A–L history.

## Resolved-via-documentation (no code change)

| Item | Decision |
|------|----------|
| Lua `req:redirect()` defaults to 302 | Documented in `stability.md` |
| `req.headers` lowercased; `req.params`/`req.query` not | Documented in `stability.md` |
| `req.body` is raw bytes; no `req:json()` accessor | Documented in `stability.md` |
| Lua multi-return vs JS array for `(value, err)` | Documented in `stability.md` |
| Manifest keys are case-sensitive per runtime (`max_age` vs `maxAge`) | Documented in `stability.md` — warn-on-wrong-case is a v0.2 item |
| `app.use("*", ...)` magic string for any method | Documented |

## Deferred to v0.2

| Item | Why |
|------|-----|
| Casing audit across JS bindings (1j, 1k, 2c) | Needs a written rule first (now in `stability.md`); apply mechanically in v0.2 |
| `log.trace` / `log.fatal` missing in runtime API | Additive; can land in v0.1.1 |
| `fs.read`/`fs.write`/`fs.exists`/`fs.delete` Lua/JS bindings missing | Need design call; either bind or remove C exports (currently dead from runtime POV) |
| `cors.maxAge` vs `cors.max_age` warn-on-typo | Quality-of-life; not breaking |
| Parity gaps in jwt.verify opts, session.* opts | Additive |
| Worker queue ops in public headers | Architectural item M1 |
| `HlRuntimeVtable.name` field stability commitment | Need to verify it's actually consumed |
| Premature unused `--developer-key` for several commands | Either implement uniformly or remove the parsing |

## Summary

**4 of 8 dead-code/safe items already applied.** 4 are pending user decision (D1–D4). 1 is architectural and can be deferred (D5).

The most consequential decisions for v0.1.0 stability are **D1** (async error convention — fundamentally shapes how every async API is written for years) and **D2/D3** (route + client method naming — sets the verbal style of the framework). **D4** and **D5** are housekeeping.

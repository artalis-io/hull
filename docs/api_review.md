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

## Decisions pending (need user input before v0.1.0 tag)

### D1. Async error convention

**Current state.** Mixed:
- `db.query` / `db.exec` (sync): throw on error.
- `db.async.query` / `db.async.exec`: return `{error: "..."}` on failure.
- `compute.call` (sync): returns `(output, err)` multi-return in Lua.
- `compute.async.call`: returns `{result, error}` table.
- `gpu.dispatch` (sync): throws.
- `gpu.async.dispatch`: ?

**Recommendation:** Align all async APIs to **throw on error** (reject the Promise in JS; raise from the coroutine in Lua). Mirrors the sync API; matches what users expect from `await`.

**Cost:** Existing apps that check `result.error` need to add `try/catch`. Migration is mechanical.

**Decision needed.** Apply now, or document the asymmetry as a known wart and fix in v0.2?

### D2. HTTP route method spelling

**Current state.** `app.del(path, handler)` registers a DELETE route. `http.delete(url, ...)` issues an outbound DELETE.

**Recommendation:** Rename `app.del` → `app.delete`. Matches HTTP spec, matches `http.delete`, matches every modern HTTP framework (Express, Hono, Koa, Fastify, Bun). Keep `app.del` as a deprecated alias for v0.1.

**Cost:** Trivial; one-line addition + alias.

**Decision needed.** Rename now, or commit to `app.del` and rename `http.delete` → `http.del` for symmetry?

### D3. HTTP client surface

**Current state.** Three names for outbound HTTP:
- `http.fetch(method, url, opts)` (Lua + JS)
- `http.async.request(method, url, opts)` (Lua + JS) — literally an alias for `http.fetch`
- `http.async.get(url, opts)` / `.post(url, body, opts)` / `.put/.patch/.delete/.head/.options` (Lua + JS)

**Recommendation.** Drop `http.fetch`. Standardize on `http.async.<method>(...)` (consistent with `db.async.*`, `compute.async.*`, `gpu.async.*`).

**Cost.** Examples + tests + CLAUDE.md all reference `http.fetch`. Migration is a global find/replace.

**Decision needed.** Drop `http.fetch` now, keep it, or rename to something else?

### D4. CLI compiler flag

**Current state.** `hull build` accepts both `--cc <path>` and `--compiler tcc|system|<path>`. Both map to the same option.

**Recommendation.** Keep `--compiler`, drop `--cc`. `--compiler` matches `hull doctor` terminology and is what CLAUDE.md commits to.

**Cost.** Anyone scripting `hull build --cc` breaks. Likely nobody (D3 just landed).

**Decision needed.** Drop `--cc` or keep both as documented aliases?

### D5. Make `HlLua` / `HlJS` opaque

**Current state.** Both runtime structs are fully exposed in `include/hull/runtime/lua.h` and `js.h` with all internal fields. Per `stability.md` they are Tier 4 (internal) — but they LOOK like Tier 1.

**Recommendation.** Move struct internals to `src/hull/runtime/{lua,js}_internal.h`. Public header exposes only `typedef struct HlLua HlLua;`. Provide accessors for the few fields callers actually need (currently none outside Hull).

**Cost.** Affects how internal `mod_*.c` files compile. Same TU still includes the internal header, so no behavior change. ~30 file diff.

**Decision needed.** Apply in v0.1 prep, or defer to architectural roadmap item #14?

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

# Hull Architecture & Security Review

**Goal/Thesis:** Hull must be a capability-secure runtime for tools that agents can call.

---

## 1. Current-State Architecture Map

**Layers (top → bottom):**

- **App layer** — User Lua/JS scripts declaring routes, middleware, DB queries
- **Stdlib layer** — `stdlib/lua/` and `stdlib/js/` — validate, session, idempotency, outbox, inbox, logger, JWT
- **Runtime layer** — Two pluggable runtimes (`HlRuntimeVtable`): PUC-Rio Lua 5.4 and QuickJS ES2023
- **Capability layer** — 13 C modules in `src/hull/cap/` exposing gated primitives:
  - `db.c` — SQLite via single `hl_db` handle; `query`, `exec`, `batch`, `last_id`, `changes`
  - `env.c` — `getenv()` gated by manifest `env[]` allowlist
  - `http.c` — outbound HTTP/HTTPS; host allowlist from manifest `hosts[]`; CRLF injection guard
  - `fs.c` — read/write gated by manifest `fs_read[]`/`fs_write[]`; rejects `..` and absolute paths via `realpath()`
  - `crypto.c` — HMAC-SHA256, SHA-256, Argon2id password hashing, `random_bytes`
  - `json.c` — encode/decode (Lua cjson / JS native)
  - `log.c` — structured `info`/`warn`/`error` to stderr
  - `time.c` — `time.now()` (epoch seconds)
  - `email.c` — SMTP send gated by manifest `smtp` config
  - `template.c` — Mustache-style rendering from `templates/` dir
  - `kv.c` — key-value store backed by `_hull_kv` SQLite table
  - `jwt.c` — sign/verify with HS256
  - `tool.c` — `tool.exec()` for subprocess invocation (manifest-gated)
- **Manifest layer** — `app.manifest({env, hosts, fs_read, fs_write, smtp, tools})` declared in user code; extracted post-load; configures capability gates
- **Sandbox layer** — OS-level confinement (`src/hull/sandbox.c`):
  - **OpenBSD:** native `pledge()` + `unveil()`
  - **Linux:** seccomp-bpf (via jart/pledge polyfill) + Landlock (via unveil polyfill)
  - **Cosmopolitan:** native `pledge()` + `unveil()` (built into cosmo libc)
  - **macOS:** Seatbelt (`sandbox_init_with_parameters`)
  - **other:** no-op stubs (C-level cap validation only)
- **Server layer** — Keel (`vendor/keel/`) — epoll/kqueue/poll HTTP server with connection pool, router, TLS vtable, two-phase middleware

**Lifecycle (main.c):**

```
VFS init → detect runtime → open SQLite → PRAGMA journal_mode=WAL
→ run migrations → init runtime → verify signatures → load_app()
→ extract_manifest() → wire env_cfg/http_cfg/smtp_cfg
→ apply sandbox → wire routes → enter event loop
```

**Key invariant:** Sandbox is applied AFTER manifest extraction but BEFORE the event loop — so runtime code runs inside the sandbox. However, `load_app()` runs BEFORE the sandbox.

---

## 2. Top 10 Risks/Gaps

| # | Severity | Risk | Location | Impact |
|---|----------|------|----------|--------|
| **R1** | **Critical** | Template path traversal | `runtime/lua/modules.c:~1895`, `runtime/js/modules.c:~2550` | `template.render("../../etc/passwd")` bypasses `hl_cap_fs_validate()` — direct `snprintf + fopen` reads arbitrary files under app_dir parent. **Resolved:** template loader now uses `hl_cap_fs_validate()` with `realpath()` ancestor check (M1). |
| **R2** | **Critical** | macOS sandbox is no-op | `sandbox.c:210-230` | Zero kernel enforcement on the primary dev platform; any capability bypass = full system access. **Resolved:** `sandbox_init_with_parameters()` applies dynamic SBPL profile built from manifest (M4). |
| **R3** | **High** | `load_app()` runs pre-sandbox | `main.c:~580` | User code executes during module load with no pledge/unveil; Lua `os.execute` is removed but JS `import()` could potentially load before sandbox. **Resolved:** phase 1 pledge applied before `load_app()` blocks exec/proc/fork during module loading (M6). |
| **R4** | **High** | No memory/CPU limits (Lua) | `runtime/lua/runtime.c` | Lua runtime has a custom allocator with `max_memory` but no instruction-count gas metering (JS has it via `JS_SetInterruptHandler`); infinite loops in Lua block the server. **Resolved:** `lua_sethook(LUA_MASKCOUNT)` with 100M instruction limit, configurable via `--max-instructions` (M3). |
| **R5** | **High** | DB is a single shared handle | `cap/db.c`, `main.c` | All requests share one `hl_db` SQLite handle; no per-tenant isolation; a malicious app can `DROP TABLE` any Hull internal table (`_hull_*`). **Mitigated:** `hl_cap_db_check_namespace()` blocks user SQL referencing `_hull_*` tables; stdlib bypasses via call-stack inspection (M5). |
| **R6** | **High** | `tool.exec()` grants shell access | `cap/tool.c` | Subprocess execution — even with manifest gating — is an escape hatch from the capability model; command injection if args aren't sanitized. **Mitigated:** allowlist rejects non-versioned suffixes; dangerous compiler flags (`-load`, `-fplugin`, `-Xlinker`, `-Wl,`, `@`) blocked; Lua argv type-validated; audit logs full argv. |
| **R7** | **Medium** | No audit log for capability use | All `cap/*.c` | No structured logging when `env.get()`, `http.fetch()`, `fs.read()`, `db.exec()` are called; impossible to reconstruct what a tool did post-incident. **Resolved:** all cap modules instrumented with structured JSON audit logging (M2). |
| **R8** | **Medium** | Outbox delivers outside transaction | `stdlib/lua/hull/middleware/outbox.lua` | `outbox.flush()` is called after `db.batch()` commits — delivery failures after commit leave inconsistent state (mitigated by retry, but not transactional). **Documented:** module header describes post-commit best-effort delivery model. |
| **R9** | **Medium** | Session secret is static default | `stdlib/lua/hull/middleware/session.lua` | Falls back to `"hull-session-secret-change-me"` if no env var; same across all instances |
| **R10** | **Resolved** | No request-size limit in Keel | `vendor/keel/` | `KlConfig.max_body_size` enforces a server-wide limit on the discard path (default 1 MB). Routes with body readers control their own limits via `max_size`. |

---

## 3. Proposed Tightened Architecture

```
┌─────────────────────────────────────────────────┐
│                  User App Code                  │
│            (Lua / JS — sandboxed)               │
├─────────────────────────────────────────────────┤
│                 Stdlib Modules                  │
│   validate · session · idempotency · outbox     │
├─────────────────────────────────────────────────┤
│            Capability Gate (C layer)            │  ← ENFORCE HERE
│                                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐       │
│  │ env.get  │ │ http.req │ │ fs.read  │  ...   │
│  │ allowlist│ │ host+url │ │ path val │       │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘       │
│       │             │             │              │
│  ┌────┴─────────────┴─────────────┴──────────┐  │
│  │         Audit Logger (NEW)                 │  │  ← LOG HERE
│  │   Every cap call → structured JSON event   │  │
│  └────────────────────────────────────────────┘  │
│                                                 │
│  ┌────────────────────────────────────────────┐  │
│  │      Template Loader (FIXED)               │  │  ← VALIDATE HERE
│  │   Must go through hl_cap_fs_validate()     │  │
│  └────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────┤
│             OS Sandbox (pledge/unveil)          │  ← CONTAIN HERE
│   Linux: seccomp + Landlock                     │
│   macOS: sandbox_init (NEW) or App Sandbox      │
│   Cosmo: native pledge/unveil                   │
├─────────────────────────────────────────────────┤
│                  Keel HTTP Server               │
│   epoll/kqueue/poll · connection pool · TLS     │
└─────────────────────────────────────────────────┘
```

**Key enforcement points (changes from current):**

1. **Template loader** routes through `hl_cap_fs_validate()` — same path validation as `fs.read`
2. **Audit logger** wraps every capability call with structured JSON events
3. **macOS sandbox** uses `sandbox_init(2)` (Seatbelt) for kernel-level containment
4. **Lua gas metering** via `lua_sethook()` with instruction count limit
5. **DB namespace isolation** — user SQL cannot reference `_hull_*` tables directly
6. **`load_app()` sandbox** — minimal pre-load sandbox that allows file reads but blocks network/exec

---

## 4. v0 → v1 Roadmap

### Milestone 1: Patch Critical Gaps (1-2 days) ✅ DONE

**Template path traversal fix:**
- `src/hull/runtime/lua/modules.c:~1895` — Replace `snprintf + fopen` with `hl_cap_fs_validate(app_dir, name, "templates")` before opening
- `src/hull/runtime/js/modules.c:~2550` — Same fix
- Add test: `template.render("../../etc/passwd")` must fail

**Session secret enforcement:**
- `stdlib/lua/hull/middleware/session.lua` — Error if secret is the default in production mode (when `env.get("HULL_ENV") == "production"`)
- Same for JS variant

> Completed in `2091c05` — template loader now uses `hl_cap_fs_validate()` with `realpath()` ancestor check. E2e tests cover path traversal, `..` components, and XSS escaping.

### Milestone 2: Audit Logging (2-3 days) ✅ DONE

**Add `hl_audit_log()` function:**
- `src/hull/cap/audit.c` (new) — `hl_audit_log(category, action, detail_json)`
- Instrument: `env.get()`, `http.fetch()`, `fs.read()`, `fs.write()`, `db.exec()`, `db.query()`, `tool.exec()`, `email.send()`
- Output: structured JSON to stderr (same channel as `log.*`)
- Gated by `--audit` flag or `HULL_AUDIT=1` env var (off by default for perf)

> Completed in `c10bc9a` — structured audit logging via `hl_audit_log()` in `cap/audit.c`. All capability calls instrumented. Gated by `--audit` / `HULL_AUDIT=1`. Documentation in `a5710ec`.

### Milestone 3: Lua Gas Metering (1 day) ✅ DONE

**Add instruction-count interrupt to Lua runtime:**
- `src/hull/runtime/lua/runtime.c` — `lua_sethook(L, hook_fn, LUA_MASKCOUNT, max_instructions)`
- Default: 100M instructions per handler invocation (same order as JS's `HL_JS_MAX_INSTRUCTIONS`)
- Hook function calls `luaL_error(L, "instruction limit exceeded")`

> Completed in `7dcab16` — Lua uses `lua_sethook(LUA_MASKCOUNT)`, JS uses `JS_SetInterruptHandler`. Unified 100M default, configurable via `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` env var.

### Milestone 4: macOS Sandbox (2-3 days) ✅ DONE

**Implement `sandbox_init(2)` backend:**
- `src/hull/sandbox.c` — Replace macOS no-op with Seatbelt profile
- Profile: deny default, allow network (inet), allow file reads for app_dir + unveiled paths, allow file writes for DB + fs_write paths
- `--no-sandbox` flag for development/debugging escape hatch

> Completed — `sandbox_init_with_parameters()` applies dynamic SBPL profile built from manifest. Path values passed via parameter substitution. Phase 1 is a no-op on macOS (sandbox_init is irreversible). E2E tests and sandbox_violation tests extended for macOS.

### Milestone 5: DB Namespace Protection (1 day) ✅ DONE

**Block direct access to `_hull_*` tables:**
- `src/hull/cap/db.c` — In `hl_cap_db_exec()` and `hl_cap_db_query()`, scan SQL for `_hull_` table references; reject unless called from stdlib (internal flag)
- Stdlib middleware calls use an internal variant that bypasses the check
- This prevents user code from `DROP TABLE _hull_outbox` or reading `_hull_sessions`

> Completed in `6bf5581` — `hl_cap_db_check_namespace()` blocks user SQL referencing `_hull_*` tables. Uses call-stack inspection (Lua `lua_getinfo` / JS `JS_GetScriptOrModuleName`) so stdlib transparently bypasses — no internal API exposed. Also renamed `hull_sessions` → `_hull_sessions` for consistency.

### Milestone 6: Pre-Load Sandbox (1-2 days) ✅ DONE

**Apply minimal sandbox before `load_app()`:**
- Two-phase sandbox: `hl_sandbox_apply_pledge()` (phase 1) before `load_app()`, existing `hl_sandbox_apply()` (phase 2) after manifest extraction
- Phase 1 pledges `stdio inet rpath wpath cpath flock dns` — blocks `exec`, `proc`, `fork` during module loading
- Phase 2 unchanged: unveils manifest-derived paths, seals filesystem, optionally narrows pledge

> Completed — `hl_sandbox_apply_pledge()` in `sandbox.c`, called from `main.c` before `load_app()`. E2E test verifies phase ordering.

---

## 5. File-Level Callouts

### Critical Fixes

**`src/hull/runtime/lua/modules.c:~1895-1935`** — Template loading

```c
// CURRENT (vulnerable):
snprintf(path, sizeof(path), "%s/templates/%s", app_dir, name);
FILE *f = fopen(path, "r");

// FIX: validate template name before opening
if (hl_cap_fs_validate(app_dir, name, "templates") != 0) {
    return luaL_error(L, "invalid template path: %s", name);
}
```

**`src/hull/runtime/js/modules.c:~2550-2570`** — Same template path traversal

```c
// Same fix pattern as Lua
```

**`src/hull/sandbox.c:210-230`** — macOS no-op

```c
// CURRENT:
#else
int hl_sandbox_apply(...) { (void)cfg; return 0; }  // no-op

// FIX: implement sandbox_init(2) or at minimum log a warning
```

### High-Priority Hardening

**`src/hull/runtime/lua/runtime.c`** — Add gas metering

```c
// In hl_lua_runtime_init() or equivalent:
static void lua_instruction_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    luaL_error(L, "instruction limit exceeded");
}
lua_sethook(L, lua_instruction_hook, LUA_MASKCOUNT, HL_LUA_MAX_INSTRUCTIONS);
```

**`src/hull/cap/db.c`** — Namespace protection

```c
// In hl_cap_db_exec() / hl_cap_db_query():
// Reject SQL containing "_hull_" unless caller is internal
if (!internal && strstr(sql, "_hull_") != NULL) {
    return error("access to internal tables denied");
}
```

**`src/hull/main.c:~580`** — Pre-load sandbox

```c
// Before load_app():
hl_sandbox_apply_load(app_dir);  // stdio+rpath only

// After extract_manifest():
hl_sandbox_apply_run(&sandbox_cfg);  // full runtime sandbox
```

### Medium-Priority

**`src/hull/cap/env.c`** — ~~Audit logging hook point (27 lines, add ~5)~~ **Done.** All three paths (denied, missing, ok) instrumented via `hl_audit_begin()`/`hl_audit_end()`.

**`src/hull/cap/http.c`** — ~~Audit logging hook point~~ **Done.** Two audit points: host allowlist denial and request completion (method, url, status). Response-size limits on outbound fetches remain a future consideration.

**`src/hull/cap/tool.c`** — ~~Review argument sanitization; consider denying shell metacharacters in exec args~~ **Done.** Allowlist tightened to reject non-versioned suffixes (e.g. `cc-evil`); dangerous compiler flags rejected (`-load`, `-fplugin`, `-Xlinker`, `-Wl,`, `@response`); Lua argv validated for non-string elements; audit logs now include full argv array.

**`stdlib/lua/hull/middleware/outbox.lua`** — ~~Document that delivery is best-effort post-commit~~ **Done.** Module header documents post-commit, best-effort delivery model. `flush_sync()` variant remains a future consideration.

**`stdlib/lua/hull/middleware/session.lua`** / **`stdlib/js/hull/middleware/session.js`** — Reject default secret in production mode

---

## Summary

Hull's capability architecture is well-designed in principle — the manifest → gate → sandbox layering is the right model. The 13 capability modules are cleanly separated, the two runtimes are properly sandboxed at the language level (Lua globals stripped, JS with no `std`/`os`), and the stdlib middleware (idempotency, outbox, inbox) demonstrates real transactional discipline.

The critical gap is the template path traversal (R1) — it completely bypasses the filesystem capability gate. This is a straightforward fix. The macOS no-op sandbox (R2) is the second most important issue since it means zero kernel enforcement on the primary development platform.

The v0 → v1 roadmap above focuses entirely on closing enforcement gaps, not adding features. Milestones 1-2 are the highest leverage: fixing the traversal vulnerability and adding audit logging would address the two most important properties of a capability-secure runtime — enforcement and observability.

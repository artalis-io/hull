# Hull Audit Report — Phase 6 Delta (2026-05-15)

**Baseline:** `baee101` (previous audit checkpoint)
**HEAD:** `5847e9a`
**Scope:** Only code changed since `baee101` (Phase 6 — `hull agent` extended introspection + JS/Lua audit fixes + MCP wiring).
**Auditors:** `/c-audit`, `/js-audit`, `/lua-audit` skills

| Audit | Critical | High | Medium | Low | Total |
|-------|---------:|-----:|-------:|----:|------:|
| C (Hull core, new agent files + mcp.c) | 0 | 1 | 2 | 5 | 8 |
| JS stdlib (12 files touched) | 0 | 0 | 3 | 5 | 8 |
| Lua stdlib (12 files touched) | 0 | 0 | 4 | 1 | 5 |
| **Total** | **0** | **1** | **9** | **11** | **21** |

**Resolution:** all 21 findings landed in commit `c1e8781`. Verified with
`make test` (27/27), `make e2e-templates` (40/40), `e2e_deploy.sh` (43/43),
`make e2e-mcp` (34/34). No regressions.

---

## C Audit (Phase 6 delta)

### High

| # | File:Line | Issue | Fix |
|---|---|---|---|
| **H-1** | `src/hull/agent/schema_diff.c:166-167` | Buffer over-read on embedded migrations. `HlEntry::data` is **not** NUL-terminated (build registry stores raw bytes + separate `len`), but `collect_creates()` walks via `while (*p)`. For migrations loaded from `hl_app_entries[]`, the loop runs past the end of the byte array until it finds an accidental NUL in adjacent rodata. Triggered whenever `hull agent schema-diff` runs against a built binary. | Change `collect_creates` to `(const char *sql, size_t sql_len, …)`; replace `while (*p)` with `while (p < sql + sql_len)`; pass `first[i].len` at the call site. |

### Medium

| # | File:Line | Issue | Fix |
|---|---|---|---|
| M-1 | `src/hull/agent/eval.c:118-120` | `sprintf` (banned per c-audit checklist). Sizing math is correct today (`strlen(code) + 64`, wrapper is ~22 bytes incl. NUL) so not exploitable now, but one refactor away from unsafe. | `snprintf(snippet, alloc_size, …)` with truncation check. |
| M-2 | `src/hull/agent/template.c:87-99` | Dead/broken code block in `render_js`: a botched first `snprintf` then `(void)snippet;` — the real implementation starts on line 102. Misleading on inspection; risk of someone "fixing" the `(void)` cast rather than deleting the dead lines. | Delete lines 87-99 entirely. |

### Low

| # | File:Line | Issue |
|---|---|---|
| L-1 | `src/hull/agent/schema_diff.c:58` | `name_list_push` doesn't check `strdup` for NULL — array gets NULL pointer but count increments, then `name_present`/`emit_list` use it. |
| L-2 | `src/hull/agent/capabilities.c:137` | Magic constant `8` for recursion depth — should be `HL_AGENT_WALK_MAX_DEPTH` (which is already defined in `limits.h`). |
| L-3 | `src/hull/agent/capabilities.c:105-107` | `read_file_into()` doesn't check `ferror(f)` after `fread`. Same `ferror`-before-`fclose` pattern that was added elsewhere; this file was missed. |
| L-4 | `src/hull/agent/endpoint.c:43-70` | `path_matches` edge case: pattern `/users/:id` matches path `/users/` (the `:id` consumes zero characters). Over-reports for the endpoint preview but doesn't match Keel router semantics. |
| L-5 | `src/hull/agent/validate.c:72-73` | Strict-aliasing UB: anonymous struct cast through `(void *)`. Works on every real compiler but technically UB. Fix: promote to named struct typedef. |

### Recommendations (C)

1. **Fix H-1 before the next release** — `hull agent schema-diff` against a built binary reads out-of-bounds memory every time.
2. **Sweep L-1, L-2, L-3 in one pass** — all one-line fixes.
3. Capability boundary remains clean; cleanup discipline is good (every `JS_NewRuntime` / `luaL_newstate` / `sqlite3_prepare_v2` / `fopen` / `sh_arena_create` has matching free on all paths).
4. `mcp.c` per-tool fallback paths correctly pair `hl_app_context_init` with `hl_app_context_free`.

---

## JS Audit (Phase 6 delta)

### Medium

| # | File:Line | Issue | Fix |
|---|---|---|---|
| M-1 | `stdlib/js/hull/middleware/idempotency.js:42-64` | REPLAYABLE_HEADERS omits common security response headers (`Strict-Transport-Security`, `Content-Security-Policy`, `Referrer-Policy`, `Permissions-Policy`). They will be silently dropped on replay. Inverse problem: a CSP nonce passed via `extraHeaders` would be replayed with a stale value. | Document that security headers should be set via a separate post-handler middleware, not `respond()`'s extraHeaders. Optionally allowlist HSTS / Referrer-Policy / Permissions-Policy if static. |
| M-2 | `stdlib/js/hull/jwt.js:76` vs `stdlib/lua/hull/jwt.lua:71` | Parity drift: JS adds `p.exp > 0` guard (so `exp: 0` / negative is treated as absolute), Lua treats *all* `exp < 2e9` as relative. End result is "expired" both ways but the branch differs. | Pick one canonical behavior; add unit tests for `exp: 0` and `exp: -1`. |
| M-3 | `stdlib/js/hull/email.js:39-54` | `providers.smtp` returns `smtp.send(...)` raw — no try/catch. Inconsistent with the three API providers refactored in batch 1 (postmark/sendgrid/resend). | Wrap in try/catch, return `{ok:false, error:"smtp: " + String(e)}`. |

### Low

| # | File:Line | Issue |
|---|---|---|
| L-1 | `stdlib/js/hull/middleware/ratelimit.js:36-44` | Legacy bare-object `check()` path computes `Object.keys(b).length` per call and discards the transient store. Functionally benign — but `MAX_BUCKETS` enforcement is effectively pass-through there. |
| L-2 | `stdlib/js/hull/middleware/csrf.js:142` | 1 MiB body cap is correct for forms but undocumented in the module header / CLAUDE.md. |
| L-3 | `stdlib/js/hull/csv.js:21-23` | Comment is grammatically backward: says `??` "falls through 0", actually `??` *preserves* 0. |
| L-4 | `stdlib/js/hull/middleware/outbox.js:139-145` | `attempt \| 0` floors non-numeric `attempt` to 0 (producing the first-retry 10s delay) — defensible default but worth a `typeof attempt !== 'number'` guard for clarity. |
| L-5 | `stdlib/js/hull/template.js:472-484` | `genDotPath(...)` called twice per for/for_kv emit. Safe today (property access only) but cache via a local would prevent future side-effect risk. |

### Recommendations (JS)

1. Fix **M-3** — one-line consistency with the three API providers.
2. Resolve **M-2** by picking one runtime semantics, add cross-runtime parity tests.
3. Document **M-1** and **L-2** in CLAUDE.md's recommended-middleware-stack section.

---

## Lua Audit (Phase 6 delta)

### Medium

| # | File:Line | Issue | Fix |
|---|---|---|---|
| **M-1** | `stdlib/lua/hull/middleware/health.lua:62-80` | **Breaking contract change.** Previously: `pcall(fn)` returning anything except literal `false` was treated as ok (so a check `function() end` returning nothing → ok). New behavior: only `ret == true` is ok. Existing checks that returned nothing on success now silently flip readiness probes to 503. The module's `register()` docstring still describes the old contract. | Preserve back-compat: `elseif ret == true or (ok and ret == nil) then ...`. Update docstring. |
| **M-2** | `stdlib/lua/hull/middleware/idempotency.lua:47-57` | **`x-*` blanket allowlist replays credential headers.** `is_replayable_header` returns true for every `x-*` header except an explicit deny-list. Common credentials like `X-Auth-Token`, `X-API-Key`, `X-CSRF-Token`, `X-Forwarded-Authorization`, `X-Amz-Security-Token` are NOT denied — they'd be replayed verbatim long after the original auth context is gone. JS module has the same pattern. | Either tighten the prefix allowlist to stdlib-emitted X-* (`x-request-id`, `x-ratelimit-*`, `x-idempotency-replay`) or extend the deny-list to cover common credential names. Fix Lua + JS together. |
| **M-3** | `stdlib/lua/hull/middleware/idempotency.lua:267-277` | **Cache write path stores UNFILTERED headers.** The send + replay paths filter via `is_replayable_header` + `header_value_safe`, but the SQLite write at line 269-271 JSON-encodes the raw `extra_headers` table. If a handler passes `{ Authorization = "Bearer …" }` to `respond()`, the token persists in `_hull_idempotency_keys.response_headers` for the TTL (default 24h). | Filter `extra_headers` through `is_replayable_header` + `header_value_safe` BEFORE encoding/storing, so the row never contains a credential at rest. |
| M-4 | `stdlib/lua/hull/middleware/csrf.lua:170-184` | gmatch cap is per-pair (256) and per-body (1 MiB) but not per-pair-byte. A single 1 MiB pair triggers one `pair:find` scan + one `url_decode(val)` on a 1 MiB blob. Confirm `url_decode` is O(n) memory or add a per-pair length cap (e.g. 4 KiB). |

### Low

| # | File:Line | Issue |
|---|---|---|
| L-1 | `stdlib/lua/hull/deploy.lua:139` | `validate_ident` regex allows leading `-`. `--user=-rf` would pass; tools the generated install.sh invokes (`useradd`, `chown`) would interpret it as a flag. Cheap hardening: reject leading `-`. |

### Recommendations (Lua)

1. **Fix M-1 before next release** — silently flipping readiness probes to 503 on upgrade is operationally dangerous. Restore back-compat for bare-nil returns OR loudly document the breaking change.
2. **Fix M-2 + M-3 together** — close the credential-replay surface end-to-end. Apply identical changes to the JS module for parity.
3. **No sandbox-escape regressions, no SQL string-concat regressions, no timing-attack regressions** in this delta. All DB writes parameterized; constant-time fingerprint compare in `idempotency.lua:173-177` unchanged.

### Verified-good in this delta

- `email.lua` pcall wrapping — works correctly with Lua 5.4's yieldable pcall.
- `template.lua:616` leading-newline prepend — validated against plain / leading-\n / \n\n / empty / lone-\n inputs.
- `vendor/json.lua:223-262` chunk-array — strict O(n) improvement over the prior O(n²).
- `cors.lua` credentials+wildcard check — only fires when `*` is in the array (apps with credentials + explicit origins are unaffected).
- `inbox/outbox/session/idempotency` ttl `~= nil` checks — correct semantics.

---

## Action plan

**Land now (operationally important):**

1. **C H-1** — `schema_diff.c` over-read. Pass `sql_len` to `collect_creates`.
2. **Lua M-1** — `health.lua` back-compat for bare-nil returns.
3. **Lua M-3** — `idempotency.lua` filter `extra_headers` before SQLite write.
4. **Lua M-2 + JS M-1** — tighten idempotency replay allowlist (deny X-Auth-*, X-API-Key, X-CSRF-*, etc.).
5. **JS M-3** — wrap `providers.smtp` in try/catch.

**Land next (small / polish):**

6. C M-1 — `eval.c` snprintf instead of sprintf.
7. C M-2 — delete dead snprintf block in `template.c::render_js`.
8. C L-1, L-2, L-3, L-4 — strdup NULL check, named depth constant, ferror check, empty-segment in `path_matches`.
9. Lua L-1 — reject leading `-` in `deploy.lua::validate_ident`.
10. JS M-2 — pick canonical jwt exp semantics + cross-runtime tests.
11. JS L-1..L-5 — polish (deprecate legacy ratelimit.check path; document CSRF body cap; fix CSV comment; outbox typeof guard; template.js cache genDotPath).

**Document:**

- CSRF body cap (1 MiB) and idempotency replay allowlist behavior in CLAUDE.md.
- Phase 7 candidate: email retry/backoff on transient errors (now that try/catch is in place).

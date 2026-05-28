# Hull Audit Re-run (Phase 6 fix verification) — 2026-05-15

**Baseline:** `5847e9a` (pre-fix HEAD — Phase 6 + MCP wiring complete, before audit fixes)
**HEAD:** `1b362fe` (audit fixes applied + audit doc marked resolved)
**Scope:** ONLY the audit-fix delta. Verifying each of the 21 prior findings is genuinely closed AND that no new issues were introduced.

| Audit | Prior findings | Resolved | New issues |
|-------|---------------:|---------:|-----------:|
| C | 8 (1H, 2M, 5L) | 8 ✓ | 0 |
| JS | 8 (3M, 5L) | 8 ✓ | 3 (all Low) |
| Lua | 5 (4M, 1L) | 5 ✓ | 0 |
| **Total** | **21** | **21 ✓** | **3** |

---

## C re-audit

All 8 prior C findings are resolved at the verified file:line.

- **H-1** ✓ `schema_diff.c:78-150` — `collect_creates(sql, sql_len, …)`; bounded `p < end` walk; every `strncasecmp` guarded by `end - p >= N`.
- **M-1** ✓ `eval.c:119-126` — `snprintf` with truncation check, `free(snippet)` on truncation.
- **M-2** ✓ `template.c:84-92` — dead block deleted.
- **L-1** ✓ `schema_diff.c:60-62` — `strdup` NULL check before count increment.
- **L-2** ✓ `capabilities.c:143` — `HL_AGENT_WALK_MAX_DEPTH` instead of magic 8.
- **L-3** ✓ `capabilities.c:105-111` — `ferror`-before-`fclose`; partial read discarded.
- **L-4** ✓ `endpoint.c:57-62` — empty `:param` segment rejected (verified with test harness).
- **L-5** ✓ `validate.c:39-75` — named `BadPattern` typedef; `(void *)` cast removed.

**New issues introduced:** none. Audit scanned for new `sprintf`/`strcpy`/`gets`/`atoi`, unchecked allocations, integer overflow in size math, new magic numbers in agent code (none — all literals are either bounds against `end` or existing `HL_AGENT_*` constants), leaked resources on error paths (verified: every fix's allocations are freed on every exit path), dead code, capability-boundary regressions.

**Pre-existing observations (out of scope for this re-audit):**

- `read_file_into` in `capabilities.c` (and the parallel site in `compute.c`) calls `malloc((size_t)n)` where `n` can be 0 — implementation-defined per C99. Behavior is benign on macOS/Linux; worth a `if (n == 0) return 0;` guard for portability.
- All fixes are minimal, consistent with surrounding code style. Diff is +76/-59 lines across 6 files.

---

## JS re-audit

All 8 prior JS findings are resolved.

- **M-1** ✓ `idempotency.js:42-87` (allowlist), `:303-326` (write-path filter) — blanket `x-*` removed; explicit allowlist; write-path filters before SQLite UPDATE.
- **M-2** ✓ `jwt.js:80-81` — `p.exp > 0` guard dropped. NaN unchanged (any comparison with NaN is false).
- **M-3** ✓ `email.js:39-62` — `providers.smtp` wrapped in try/catch with consistent `{ok, error}` return.
- **L-1** ✓ `ratelimit.js:36-45` — `@deprecated` JSDoc added to bare-bucket-map path.
- **L-2** ✓ `csrf.js:139-148` — 1 MiB body cap rationale + multipart workaround documented.
- **L-3** ✓ `csv.js:21-24` — `??` semantics comment corrected.
- **L-4** ✓ `outbox.js:146` — `typeof attempt !== "number" || !isFinite(attempt)` guard.
- **L-5** ✓ `template.js:475-498` — `genDotPath` cached in `{ const __it = ...; ... }` block.

### New issues introduced (3 Low)

| # | File:Line | Issue | Resolution |
|---|---|---|---|
| **N-1** | `template.js:475-498` (for/for_kv codegen) | The new codegen reserves `__it` as a block-scoped local. `validateIdent`'s regex `^[a-zA-Z_]\w*$` accepts `__it` as a valid user identifier, so a template `{% for __it in items %}` would compile to `{ const __it = __d.items; for (const __it of (Array.isArray(__it) ? __it : [])) {`, shadowing the outer one and TDZ-crashing at render time with a confusing `ReferenceError: Cannot access '__it' before initialization`. | **Fixed in next commit:** extended `validateIdent` to reject identifiers starting with `__` (the codegen reserves `__p`, `__d`, `__e`, `__f`, `__it`). Templates using such names now fail at compile time with a clear error. |
| **N-2** | `idempotency.js:82-84` (and Lua parity) | The `X_CREDENTIAL_PATTERNS` substring loop was unreachable: the function fell through to `return false` whether the loop matched or not. Belt-and-braces defense that didn't actually defend. | **Fixed in next commit:** reordered checks — credential denies now run BEFORE the allowlist lookup. The deny loop is now load-bearing: a future accidental addition like `"x-auth-thing"` to the allowlist would be overridden by the deny. Same fix mirrored in `stdlib/lua/hull/middleware/idempotency.lua`. |
| **N-3** | `jwt.js:80-81` | Behavioral change for `exp: -Infinity`: previously skipped the branch (kept `-Infinity`), now enters the branch (`time.now() + -Infinity = -Infinity`). Both produce broken tokens; Lua exhibits the same behavior so this is parity, not a regression. | **Informational only.** No code change; parity holds. |

### Other observations

- **Sandbox safety:** no new `eval`/`Function`/`globalThis`/`__proto__` writes.
- **Type safety:** `outbox.js` typeof guard is a strict improvement.
- **Crypto/auth timing:** unchanged. `idempotency.js` constant-time fingerprint compare preserved.
- **API parity:** JWT exp branch shape matches Lua; idempotency allowlist matches Lua (M-2 + M-3); CSRF body cap matches Lua.
- **No SQL injection / XSS / path-traversal / header-injection** introduced. CRLF/NUL filter on `extraHeaders` preserved.

---

## Lua re-audit

All 5 prior Lua findings resolved.

- **M-1** ✓ `health.lua:75` — `elseif ret == nil or ret == true then` restores back-compat for bare-nil success.
- **M-2** ✓ `idempotency.lua:42-87` — explicit allowlist + substring deny patterns; no blanket `x-*`.
- **M-3** ✓ `idempotency.lua:300-313` — `extra_headers` filtered through `is_replayable_header` + `header_value_safe` BEFORE SQLite UPDATE. Empty filtered dict correctly yields `nil` (no JSON `"{}"` row pollution).
- **M-4** ✓ `csrf.lua:171-194` — `MAX_PAIR_BYTES = 4096` cap; `goto continue` to `::continue::` label correctly scoped per Lua 5.4 spec.
- **L-1** ✓ `deploy.lua:144-150` — leading `-` rejected via `value:sub(1, 1) == "-"`; multiple-dash prefixes covered by single first-character check.

**New issues introduced:** none.

**Pre-existing observations:**

- **Latent fragility around nil in db.exec param tables:** the current `{status_code, body_str, headers_str, principal_id, key}` layout works because the nil-bearing slot is mid-table. A trailing-nil refactor would break silently due to Lua's `#`-on-table-with-nil being undefined. Not a fix-now; flag for future maintenance.
- **Substring deny is over-broad but harmless** (e.g. `Foo-X-Auth-Trace` triggers the `x-auth` deny). Since the function returns false at the end regardless, this is dead-defense in the current ordering — but **the JS-side fix N-2 reorders the checks so the deny is now load-bearing**, and the Lua side has been updated to match.
- **`health.lua` return-value classification is slightly stricter than pre-Phase-6.** Numbers and bare strings now classified as failure rather than success (per documented contract). Worth surfacing in release notes if a deprecation cycle is desired.

---

## Summary

**All 21 prior findings closed. Three new low-severity issues found in the JS fixes; two of those fixed in a follow-up patch (N-1 + N-2 mirroring N-2 in Lua); the third (N-3) is informational only (parity with Lua, not a regression).**

The fix patches are minimal, consistent with surrounding code style, and introduce no critical/high regressions. The only behavior change with operational impact is **Lua M-1 (health.lua)** which restores the previously-broken back-compat — this is *good* (the Phase 6 patch had silently broken existing readiness probes).

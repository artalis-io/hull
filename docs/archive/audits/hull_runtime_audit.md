# Hull Runtime Audit Report

**Date:** 2026-03-04
**Scope:** JS runtime, Lua runtime, capability modules
**Files Scanned:** 33 (6 runtime .c, 2 runtime .h, 11 cap .c, 12 cap .h, 2 test .c)
**Total Code:** ~572 KB

## Summary

| Severity | JS Runtime | Lua Runtime | Cap Modules | Total |
|----------|-----------|-------------|-------------|-------|
| Critical | 2 | 1 | 0 | **3** |
| High | 4 | 4 | 5 | **13** |
| Medium | 7 | 7 | 11 | **25** |
| Low | 6 | 8 | 10 | **24** |
| **Total** | **19** | **20** | **26** | **65** |

---

## Critical Findings

### C1 — JS `_template.compile` re-enables eval (modules.c:2466)

`_template.compile` calls `JS_Eval()` with `JS_EVAL_TYPE_GLOBAL` on code
derived from template source. While `eval()` is removed from globalThis, this
C-level eval bypasses the sandbox. If user input reaches template source,
this enables arbitrary code execution inside the QuickJS context.

**Fix:** Restrict `hull:_template` to only be importable by stdlib modules
(not user code), or validate that template source is trusted input.

### C2 — JS `magic` index used without bounds check (modules.c:54)

`js_app_route` uses `method_names[magic]` where `magic` is an `int` with no
range validation. If the registration table is ever extended without updating
the array, this is an out-of-bounds read.

```c
// Current
return JS_ThrowTypeError(ctx, "app.%s requires ...", method_names[magic]);

// Fix
if (magic < 0 || magic >= (int)(sizeof(method_names)/sizeof(method_names[0])))
    return JS_ThrowInternalError(ctx, "invalid route method");
```

### C3 — Lua allocator mem_used tracking inaccurate (runtime.c:50-53,65-71)

When `ptr == NULL` (new allocation), Lua passes a type-hint enum (0–8) in
`osize`, not a real size. The limit check computes `delta = nsize - osize`,
under-counting by the type hint. Over millions of allocations, `mem_used`
drifts below actual usage, allowing the Lua VM to exceed `mem_limit`.

```c
// Fix: treat osize as 0 for new allocations
size_t effective_osize = (ptr == NULL) ? 0 : osize;
```

---

## High Findings

### H1 — Integer overflow in crypto size calculations

Multiple overflow patterns in both JS and Lua runtimes:

| Location | Expression | Risk |
|----------|-----------|------|
| JS modules.c:1294 | `msg_len + HL_SECRETBOX_MACBYTES` | Wrap → tiny alloc → buffer overwrite |
| JS modules.c:1660 | `len * 4` in base64url | Wrap → tiny alloc |
| Lua modules.c:985 | `msg_len + HL_SECRETBOX_MACBYTES` | Same as JS |
| Lua modules.c:1272 | `len * 4` in base64url | Same as JS |
| JS runtime.c:665 | `route_cap * 2` in realloc | Wrap → undersized array |
| Lua runtime.c:346 | `route_cap * 2` in realloc | Same as JS |
| Cap crypto.c:566 | `128 + msg_len` in hmac_sha512 | Missing SIZE_MAX/2 guard |
| Cap crypto.c:463 | `sm_len * 2` in ed25519_verify | Wrap on comparison |
| Cap crypto.c:647 | `padded_len * 2` in secretbox | Wrap on stack/heap decision |

**Fix pattern:** Add `if (x > SIZE_MAX / 2) return error;` before each
arithmetic operation, matching the existing guards in `hl_cap_crypto_hmac_sha256`.

### H2 — JS `hl_js_ensure_response_class` return value ignored (bindings.c:433)

If class registration fails, `hl_js_make_response` proceeds to create an
object with an uninitialized class ID → crash or UB in `JS_NewObjectClass`.

### H3 — Cap `build_path` symlink TOCTOU (fs.c:95)

`build_path` validates via `realpath` but constructs the actual path using raw
`cfg->base_dir`. If `base_dir` contains symlinks, the opened file may differ
from the validated one.

**Fix:** Use the resolved base directory from `realpath(cfg->base_dir, ...)`
to construct the final path.

### H4 — Cap `hl_tool_unveil_add` stores `perms` without copying (tool.c:65)

`perms` pointer stored directly. If caller frees/modifies the source string,
the unveil entry becomes a dangling pointer. `path` is correctly `strdup`'d.

### H5 — JS hex_decode return values unchecked (modules.c:1356,1415,1480)

After length validation, `hex_decode` calls for nonce/key/pk/sk don't check
return values. Invalid hex characters → silent failure → uninitialized stack
data used as cryptographic keys.

### H6 — Lua `lua_get_string_array` dangling pointers (modules.c:1636)

`lua_tostring` returns pointer into Lua stack. After `lua_pop`, the string
may be GC-eligible. Anchored by the table in practice, but technically
unsound by Lua API contract.

---

## Medium Findings

### Memory / Bounds

| # | Location | Issue |
|---|----------|-------|
| M1 | JS/Lua runtime.c | `strlen` on `req->ctx` assumes NUL-terminated string — fragile contract |
| M2 | JS bindings.c:39 | Header name silently truncated to 255 bytes |
| M3 | JS bindings.c:132 | Response header name silently truncated to 255 bytes |
| M4 | Cap smtp.c:55 | `hl_smtp_base64_encode` uses `int` for sizes — overflow risk |
| M5 | Cap db.c:161 | `size_t` → `int` truncation in `sqlite3_bind_text` |
| M6 | Cap test.c:95 | 16 KB stack allocation (`lowered_names[KL_MAX_HEADERS][256]`) |

### Input Validation / Injection

| # | Location | Issue |
|---|----------|-------|
| M7 | JS runtime.c:197 | JSON wrap uses backtick template literal — injection via `` ` `` or `${` |
| M8 | Cap smtp.c:496 | CRLF check missing on `content_type` |
| M9 | Cap smtp.c:497 | CRLF check missing on `username`/`password` |
| M10 | Cap smtp.c:638 | SMTP addresses not validated for `<`, `>`, space |

### Lua Stack Safety

| # | Location | Issue |
|---|----------|-------|
| M11 | Lua bindings.c:83 | No `lua_checkstack` before pushing headers/params (untrusted count) |
| M12 | Lua modules.c:278 | No `lua_checkstack` in `lua_query_row_cb` (wide result sets) |
| M13 | Lua runtime.c:616 | No `lua_checkstack` in middleware ctx serialization |
| M14 | Lua runtime.c:325 | `hl_lua_dump_error` doesn't pop error message — stack leak |

### Resource Management

| # | Location | Issue |
|---|----------|-------|
| M15 | Cap crypto.c:374 | No minimum iteration count enforcement for PBKDF2 |
| M16 | Cap crypto.c:148 | `/dev/urandom` instead of `getrandom(2)` on Linux |
| M17 | Lua runtime.c:412 | `hl_lua_track_route` return value ignored × 4 call sites |
| M18 | Lua modules.c:1387 | `lua_parse_http_headers` stores dangling stack pointers |

---

## Low Findings

| # | Location | Issue |
|---|----------|-------|
| L1 | JS runtime.c:248 | `fseek(SEEK_END)` return unchecked (× 3 locations) |
| L2 | JS bindings.c:214 | `SIZE_MAX` check vacuous (`size_t >= SIZE_MAX`) |
| L3 | JS/Lua modules.c | `strtol` for iterations without `LONG_MAX` cap → hash-DoS |
| L4 | JS modules.c:29 | `secure_zero` not using `memset_explicit` (C23) |
| L5 | JS runtime.c:701 | `handler_id` cast to `uint32_t` without negativity check |
| L6 | Cap crypto.c:222 | Stack HMAC buffers not zeroed (key material on stack) |
| L7 | Cap crypto.c:443 | `randombytes` truncates `unsigned long long` to `size_t` |
| L8 | Cap smtp.c:668 | `msg_size` silently clamped instead of rejected |
| L9 | Cap db.c:225 | `nparams > 0 && params` allows silent skip if `params = NULL` |
| L10 | Cap db.c:253 | `hl_alloc_free` called on potentially NULL `cols` |
| L11 | Cap test.c:142 | Raw `malloc`/`free` instead of allocator (test code) |
| L12 | Cap fs.c:64 | `strncpy` pads entire PATH_MAX buffer with NULs |
| L13 | Lua bindings.c:258 | `json_str` from `lua_tolstring` may be NULL |
| L14 | Lua modules.c:1954 | Fixed 128-segment limit in `normalize_path` |
| L15 | Lua runtime.c:337 | Pointer comparison for string dedup (relies on interning) |
| L16 | Lua modules.c:1888 | Direct arena struct access bypasses API |
| L17 | Lua modules.c:2045 | `strncpy` wasteful NUL padding on 4 KB buffer |
| L18 | Lua runtime.c:428 | Unused parameter `alloc_fn` in public API |
| L19 | Cap tool.c:471 | `lua_tostring` without type check |
| L20 | Cap env.c:20 | Linear scan of allowlist (O(n) per lookup) |

---

## What the Codebase Does Well

1. **No unsafe C functions.** Zero uses of `strcpy`, `strcat`, `sprintf`, `gets`,
   `atoi`, `atol`, `atof` anywhere. All string ops use `snprintf`, `memcpy`
   with explicit lengths, or `strtol` with validation.

2. **SQL injection prevention.** All queries use `sqlite3_prepare_v2` +
   `sqlite3_bind_*`. No string concatenation for SQL anywhere. Statement
   cache properly resets and clears bindings on reuse.

3. **CRLF injection prevention.** HTTP and SMTP modules validate untrusted input
   for `\r` and `\n` before incorporating into protocol messages.

4. **Constant-time MAC verification.** `volatile uint8_t diff` XOR accumulation
   in HMAC-SHA256 verify. TweetNaCl's `crypto_verify_32` for auth verify.

5. **Consistent overflow guards.** `SIZE_MAX / 2` checks before arithmetic in
   allocator wrappers, crypto functions, and buffer management (with exceptions
   noted in H1).

6. **Capability-based security.** All external access (env vars, filesystem,
   HTTP hosts, SMTP hosts, process execution) gated by allowlists.

7. **Secure key zeroing.** `secure_zero` with `volatile` applied to HMAC keys,
   ed25519 secret keys, box keypairs on all exit paths.

8. **Path traversal defense-in-depth.** Module normalizer validates input,
   loader validates output, VFS constrains to known entries. Filesystem
   fallback only active in dev mode. `realpath` + prefix check in fs.c.

9. **QuickJS reference counting.** Every `JS_GetPropertyStr`, `JS_Call`,
   `JS_Eval` has matching `JS_FreeValue`. Every `JS_ToCString` has matching
   `JS_FreeCString`. Error paths consistently clean up.

10. **Allocator discipline.** All allocations go through `js_malloc`/`js_free`
    or `hl_alloc_malloc`/`hl_alloc_free` with size tracking (except test code).

---

## Priority Recommendations

### Immediate (before next release)

1. **Fix integer overflow guards** (H1) — Add `SIZE_MAX` checks before all
   arithmetic in secretbox/box/base64url/route-tracking. Pattern:
   `if (a > SIZE_MAX - b) return error;` before `a + b`.

2. **Fix hex_decode return checks** (H5) — Check return values in
   `secretboxOpen`, `boxOpen`, and `box` to prevent uninitialized key use.

3. **Guard `magic` index** (C2) — Add bounds check in `js_app_route`.

4. **Fix Lua allocator tracking** (C3) — Use `effective_osize = ptr ? osize : 0`.

### Short-term

5. **Add `lua_checkstack`** (M11-M13) — Before loops pushing untrusted counts.

6. **Validate SMTP `content_type`** (M8) — Add to `smtp_validate_message`.

7. **Fix `build_path` TOCTOU** (H3) — Use resolved path for construction.

8. **Check `hl_js_ensure_response_class` return** (H2).

9. **Guard JSON template literal injection** (M7) — Validate no backticks in
   JSON before wrapping, or use single-quote `JSON.parse('...')`.

### Long-term

10. **Restrict `hull:_template`** (C1) — Only allow stdlib imports.

11. **Use `getrandom(2)`** (M16) — Where available on Linux.

12. **Enforce PBKDF2 minimum iterations** (M15).

13. **Track `req->ctx` size explicitly** (M1) — Instead of relying on `strlen`.

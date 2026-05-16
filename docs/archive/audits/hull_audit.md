# C Audit Report: Hull

**Date:** 2026-05-12
**Auditor:** Claude Opus 4.6 (automated)
**Files Scanned:** 179 (25 cap/*.c, 25 js/*.c, 24 lua/*.c, 21 core/*.c, 17 commands/*.c, 66 headers, 1 Makefile)
**Issues Found:** 14 (Critical: 0, High: 2, Medium: 6, Low: 6)

---

## Summary

The Hull codebase demonstrates strong security hygiene overall. No unsafe C functions (`strcpy`, `sprintf`, `gets`, `strcat`, `atoi`) were found anywhere in the project source. All allocations are checked for NULL. Integer overflow guards using `SIZE_MAX/2` are consistently applied before size computations. The capability boundary is well-enforced: neither Lua nor JS runtime bindings call `sqlite3_*` for data access (only for UDF registration, which is a meta-level operation) or `fopen` for user-visible filesystem access. Path traversal is blocked at multiple layers. Key material is zeroed with volatile writes. The sandbox properly removes `eval()`, `Function`, `io`, `os`, `loadfile`, `dofile`, and `load`.

---

## Critical Issues

No critical issues found.

The codebase has no instances of:
- `strcpy`, `sprintf`, `gets`, `strcat`, `atoi`, `atol`, `atof`
- Unchecked `malloc`/`calloc` returns (all 286 allocation sites checked)
- Direct `sqlite3_*` data access from runtime bindings (capability boundary intact)
- Direct filesystem access from user-facing runtime bindings (all go through `hl_cap_fs_*` or are infrastructure-level reads for module/template/shader loading)
- `system()`, `popen()`, or uncontrolled `exec*()` calls
- `#if 0` dead code blocks

---

## High Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| H-1 | `src/hull/worker_db.c:148-153` | **Silent data loss on malloc failure in materialize_column.** When `malloc` fails for TEXT columns, `out->type` is set to `HL_TYPE_TEXT` but `out->s` remains NULL. Callers (line 296) do not check the return and continue processing. Consumers that dereference `out->s` expecting a valid TEXT value will crash or read NULL. Same issue for BLOB at line 161. | `out->type = HL_TYPE_TEXT; out->s = malloc(len + 1); if (out->s) { memcpy(...); }` | Set `out->type = HL_TYPE_NIL` in the else branch when malloc fails, or return an error code from `materialize_column` and propagate it to `worker_db_exec_fn`. |
| H-2 | `src/hull/tool.c:58-66` | **Secret key file written with default umask permissions.** `hull keygen` writes the hex-encoded secret key to `<prefix>.key` via `fopen(sk_file, "w")` without restricting file permissions. On systems with a permissive umask (e.g., 0022), the key file is world-readable (mode 0644). | `f = fopen(sk_file, "w");` | Use `open(sk_file, O_WRONLY|O_CREAT|O_TRUNC, 0600)` + `fdopen()` to ensure the secret key file is created with mode 0600 (owner-only read/write). |

---

## Medium Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| M-1 | `src/hull/runtime/js/mod_gpu.c:102-104`, `src/hull/runtime/lua/mod_gpu.c:106-108` | **Unchecked fseek return values in gpu.load().** Both JS and Lua `gpu.load()` call `fseek(f, 0, SEEK_END)` and `fseek(f, 0, SEEK_SET)` without checking return values. While ftell's negative check partially mitigates this, a failed SEEK_SET means fread reads from the wrong position, producing corrupt shader source that gets compiled. | `fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);` | Check both fseek return values: `if (fseek(f, 0, SEEK_END) != 0) { fclose(f); ... }` and similarly for SEEK_SET. |
| M-2 | `src/hull/signature.c:114-116`, `src/hull/signature.c:470-472` | **Unchecked fseek return values in signature verification.** Same pattern as M-1. A corrupted read during signature verification could lead to a false-positive verification result if the fread happens to produce data that matches a valid signature hash. | `fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);` | Add return value checks for both fseek calls. |
| M-3 | `src/hull/cap/wasm.c:392-394`, `src/hull/cap/wasm.c:422-424` | **Unchecked fseek return values in WASM module loading.** Loading AOT and WASM modules from disk uses the same unchecked fseek pattern. A corrupted read could load partial/garbage bytes as a WASM module. | `fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);` | Check fseek return values. |
| M-4 | `src/hull/worker_db.c:122-123` | **Potential integer overflow in realloc size computation.** The expression `(size_t)new_cap * (size_t)r->ncols * sizeof(HlDbValue)` involves a three-way multiplication without an explicit overflow check. While `new_cap` is capped at 100000 and typical `ncols` is small, a query returning many columns could theoretically overflow. | `HlDbValue *nv = realloc(r->values, (size_t)new_cap * (size_t)r->ncols * sizeof(HlDbValue));` | Add an overflow guard: `if ((size_t)r->ncols > SIZE_MAX / (sizeof(HlDbValue) * (size_t)new_cap)) return -1;` |
| M-5 | `Makefile:32` | **`-Werror` not enabled in default build.** The Makefile uses `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2` but does not include `-Werror`. This means compiler warnings do not fail the build, allowing potential issues to accumulate. | `CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2` | Add `-Werror` to CFLAGS for CI builds (possibly behind a `CI` or `STRICT` variable to avoid breaking developer workflows). |
| M-6 | `src/hull/runtime/js/mod_template.c:99-103`, `src/hull/runtime/lua/mod_template.c:83-85` | **Template loading uses fopen directly, not through hl_cap_fs.** Template file loading in dev mode bypasses `hl_cap_fs_read()` and calls `fopen()` directly. While path traversal is rejected via string-level `..` checks (lines 67-75 in Lua, 84-92 in JS), this skips the `realpath()` symlink-escape check that `hl_cap_fs_validate()` performs. A symlink within `templates/` could escape the app directory. | `FILE *f = fopen(path, "rb");` | Route template loading through `hl_cap_fs_read()` or add a `realpath()` ancestor check after the `..` component check. The kernel sandbox (unveil/seatbelt) provides defense-in-depth, but the C layer should be self-sufficient. |

---

## Low Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| L-1 | `src/hull/migrate.c:253-255` | **Unchecked fseek in migration file reading.** Same unchecked fseek pattern. Low risk because migration files are developer-authored SQL read at startup. | `fseek(f, 0, SEEK_END); ... fseek(f, 0, SEEK_SET);` | Add return value checks for consistency. |
| L-2 | `src/hull/agent_lib.c:690-692`, `src/hull/agent_lib.c:884-887` | **Unchecked fseek in agent library.** Agent file reading uses unchecked fseek. Low risk because these read developer-authored JSON files. | `fseek(f, 0, SEEK_END); ... fseek(f, 0, SEEK_SET);` | Add return value checks for consistency. |
| L-3 | `src/hull/cap/wasm_stream.c:103-111` | **Unchecked fseek in WASM streaming input.** Stream file input uses unchecked fseek. The ftell result is checked for negative, which partially mitigates. | `fseek(in_file, 0, SEEK_END); ... fseek(in_file, 0, SEEK_SET);` | Add return value checks. |
| L-4 | `src/hull/runtime/js/runtime.c:645`, `src/hull/runtime/lua/mod_fs.c:419` | **Unchecked fseek in dev-mode module loading.** Module loading from disk in dev mode uses unchecked fseek. Mitigated by subsequent ftell negative check and fread size verification. | `fseek(f, 0, SEEK_END);` | Add return value checks. |
| L-5 | `src/hull/commands/test.c:196-199` | **Unchecked fseek in test runner.** Test file loading uses unchecked fseek. Low risk since test files are developer-authored. | `fseek(f, 0, SEEK_END); ... fseek(f, 0, SEEK_SET);` | Add return value checks. |
| L-6 | `src/hull/runtime/js/mod_gpu.c:97-98`, `src/hull/runtime/lua/mod_gpu.c:101-102` | **snprintf return value not checked for truncation in shader path construction.** The snprintf building `shaders/<name>.wgsl` path doesn't check the return value. If the path is truncated, fopen will either fail (safe) or open the wrong file. | `snprintf(path, sizeof(path), "%s/shaders/%s.wgsl", ...);` | Check `n > 0 && (size_t)n < sizeof(path)` before using `path`, matching the pattern used elsewhere in the codebase. |

---

## Positive Findings

The following security practices were verified as correctly implemented:

### Memory Safety
- **No unsafe functions:** Zero instances of `strcpy`, `sprintf`, `gets`, `strcat`, `atoi`, `atol`, `atof` across the entire codebase.
- **All allocations checked:** Every `malloc`, `calloc`, `realloc`, `js_malloc`, `hl_alloc_malloc` call is followed by a NULL check.
- **Overflow guards:** Integer overflow checks using `SIZE_MAX/2` are consistently applied before arithmetic in size computations (crypto.c, image.c, alloc.c, migrate.c, etc.).
- **`calloc` used appropriately:** Array allocations use `calloc` (migrate.c, worker_db.c) with overflow-safe semantics.
- **Secure zeroing:** `hull_secure_zero()` uses volatile pointer writes to prevent compiler optimization. Applied to all key material (HMAC keys, Ed25519 secret keys, PBKDF2 intermediates).
- **`strncpy` always null-terminated:** All 3 `strncpy` calls are immediately followed by explicit `buf[sizeof(buf)-1] = '\0'`.

### Capability Boundary Enforcement
- **DB access mediated:** Neither Lua nor JS bindings call `sqlite3_prepare_v2`, `sqlite3_exec`, `sqlite3_step`, or `sqlite3_bind_*` directly. All data access goes through `hl_cap_db_query()` and `hl_cap_db_exec()`. Direct `sqlite3_*` calls in `mod_db.c` are limited to UDF registration (`sqlite3_create_function_v2`, `sqlite3_value_*`, `sqlite3_result_*`, `sqlite3_aggregate_context`), which is a meta-level operation.
- **FS access mediated:** User-visible file operations go through `hl_cap_fs_read()`/`hl_cap_fs_write()` with path validation. Direct `fopen` calls in bindings are for infrastructure loading (modules, templates, shaders) with separate traversal checks.
- **SQL injection impossible:** All user-facing SQL uses parameterized binding via `sqlite3_bind_*`. No string concatenation of SQL.
- **Namespace protection:** `hl_cap_db_check_namespace()` scans for `_hull_` prefix to block user access to internal tables.
- **Host allowlist enforced:** `hl_http_check_host()` validates HTTP request targets against manifest `hosts` array.
- **Env allowlist enforced:** `hl_cap_env_get()` checks against manifest `env` array.
- **CRLF injection guarded:** SMTP capability has explicit `has_crlf()` check on all header values.

### Path Traversal Protection
- **`hl_cap_fs_validate()`:** Rejects empty paths, absolute paths, `..` components, and symlink escapes via `realpath()` ancestor check.
- **JS module normalizer:** `hl_js_validate_module_name()` rejects absolute paths and `..` components before filesystem access.
- **Lua module resolver:** `resolve_module_path()` normalizes paths and verifies resolved path starts with `app_dir`.
- **Template loading:** Both Lua and JS template loaders reject `..` components via string-level validation.

### Sandbox Enforcement
- **JS sandbox:** `eval()` global deleted, `Function` constructor deleted, `std`/`os` modules not loaded, memory limit (64 MB), stack limit (1 MB), instruction-count interrupt handler.
- **Lua sandbox:** `io`, `os`, `loadfile`, `dofile`, `load` globals set to nil. Custom memory-tracking allocator with limit. Instruction-count hook (100M default).
- **Tool spawn allowlist:** `hl_tool_check_allowlist()` only permits known compiler binaries. `hl_tool_validate_args()` rejects dangerous flags (`-load`, `-fplugin`, `@response_file`).
- **No shell invocation:** No `system()` or `popen()` calls. Process execution uses `fork`/`execvp` with allowlist.

### Build Hardening
- `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2` (all warnings enabled)
- `-fstack-protector-strong` (buffer overflow detection, non-Cosmo)
- `-D_FORTIFY_SOURCE=2` (runtime buffer overflow checks, non-debug)
- `make debug`: `-fsanitize=address,undefined -g -O0` (ASan + UBSan)
- `make msan`: Memory Sanitizer support (Linux clang)
- Vendor code compiled with `-w` (isolated from project warning standards)
- No `#if 0` dead code blocks found

### Resource Management
- **Statement cache:** `hl_stmt_cache_init`/`hl_stmt_cache_destroy` properly pairs `sqlite3_prepare_v2` with `sqlite3_finalize`.
- **Worker DB:** `calloc`/`free` properly paired. TLS-keyed per-thread connections.
- **File handles:** All `fopen` calls paired with `fclose` on all code paths (including error paths).
- **JS values:** `JS_DupValue`/`JS_FreeValue` and `JS_ToCString`/`JS_FreeCString` consistently paired.

---

## Recommendations

1. **Fix H-1 (materialize_column silent failure):** This is the highest-priority fix. A malloc failure during query result materialization silently produces a TEXT-typed value with a NULL string pointer, which will crash consumers. Set `out->type = HL_TYPE_NIL` on malloc failure.

2. **Fix H-2 (keygen file permissions):** Use `open()` with explicit mode 0600 for secret key files to prevent information disclosure regardless of the system umask.

3. **Standardize fseek error handling (M-1 through L-5):** Create a helper function like `file_read_all(path, &buf, &len)` that encapsulates the fseek/ftell/fseek/fread pattern with proper error checking. This would eliminate the 16 instances of the unchecked-fseek pattern and reduce code duplication.

4. **Route template/shader loading through hl_cap_fs (M-6):** While the kernel sandbox provides defense-in-depth, the C layer's path validation should be self-sufficient. Using `hl_cap_fs_read()` for template and shader loading would add the `realpath()` symlink-escape check that string-level `..` validation cannot provide.

5. **Enable `-Werror` in CI (M-5):** Add `-Werror` behind a CI flag to prevent warning accumulation. Consider `CFLAGS += $(if $(CI),-Werror)` in the Makefile.

6. **Add overflow guard in worker_db (M-4):** The three-way multiplication in `db_result_grow` should have an explicit overflow check, even though practical values are small.

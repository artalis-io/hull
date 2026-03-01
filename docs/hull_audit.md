# C Audit Report: Hull

**Date:** 2026-03-01
**Auditor:** Claude Opus 4.6 (automated)
**Files Scanned:** 45 (22 `.c` source files, 23 `.h` headers)
**Vendor Files Excluded:** QuickJS, Lua 5.4, SQLite, mbedTLS, Keel, TweetNaCl, log.c, sh_arena, jart/pledge
**Issues Found:** 14 (Critical: 2, High: 5, Medium: 5, Low: 2)

---

## Scope

| Directory | Description | Files |
|-----------|-------------|-------|
| `src/hull/cap/*.c` | Capability layer (db, fs, crypto, env, http, body, time, tool, test, http_parser) | 10 |
| `src/hull/runtime/js/*.c` | QuickJS runtime integration (runtime, modules, bindings) | 3 |
| `src/hull/runtime/lua/*.c` | Lua 5.4 runtime integration (runtime, modules, bindings) | 3 |
| `src/hull/commands/*.c` | Subcommands (build, dev, dispatch, eject, inspect, keygen, manifest, new, sign_platform, test, verify) | 11 |
| `src/hull/*.c` | Core (main, sandbox, signature, manifest, alloc, entry, tool, build_assets, app_entries_default) | 9 |
| `include/hull/**/*.h` | All public headers | 23 |
| `Makefile` | Build configuration | 1 |

---

## Summary of Unsafe Function Scan

| Pattern | Occurrences | Status |
|---------|-------------|--------|
| `strcpy()` | 0 | Clean |
| `strcat()` | 0 | Clean |
| `sprintf()` | 0 | Clean (all use `snprintf`) |
| `gets()` | 0 | Clean |
| `atoi()` / `atol()` / `atof()` | 0 | Clean (uses `strtol`) |
| `strtok()` (thread-unsafe) | 0 | Clean (uses `strtok_r` in bindings) |

---

## Critical Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| C1 | `src/hull/runtime/js/runtime.c:66-98` | JS module loader missing path traversal protection | The JS module name normalizer does not normalize `..` segments or verify the resolved path stays within `app_dir`. |
| C2 | `src/hull/runtime/js/runtime.c:198-212` | JS sandbox: `Function` constructor not removed | Removing `eval()` is insufficient -- `new Function("return this")()` or `(0,eval)` via the `Function` constructor can still execute arbitrary code. |

### C1 -- JS Module Loader Path Traversal (Critical)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/runtime/js/runtime.c:66-98`

The `hl_js_module_normalize` function resolves relative module names by simply concatenating the base directory and the module name. Unlike the Lua equivalent (`resolve_module_path` in `lua/modules.c:1797`), it does not:
1. Normalize `..` segments
2. Verify the resolved path stays within `app_dir`

This means a malicious app.js can `import "../../../etc/passwd"` to read files outside the app directory.

**Current code:**
```c
static char *hl_js_module_normalize(JSContext *ctx,
                                       const char *base_name,
                                       const char *name, void *opaque)
{
    // ... hull:* check ...
    if (name[0] == '.') {
        const char *last_slash = strrchr(base_name, '/');
        if (last_slash) {
            // Simply concatenates base_dir + "/" + name
            // No normalization, no boundary check
            memcpy(resolved, base_name, dir_len);
            resolved[dir_len] = '/';
            memcpy(resolved + dir_len + 1, name, name_len + 1);
            return resolved;
        }
    }
    return js_strdup(ctx, name);
}
```

**Suggested fix:** Add path normalization (collapse `..` segments) and verify the resolved path starts with `app_dir`, mirroring the Lua implementation in `resolve_module_path`. Also add the check in `hl_js_module_loader` where the path `app_dir + "/" + module_name` is constructed for filesystem loading.

### C2 -- JS Sandbox: Function Constructor Not Removed (Critical)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/runtime/js/runtime.c:198-212`

The sandbox removes `eval()` but leaves `Function` accessible. The code comments acknowledge this:

```c
/* Remove Function constructor (prevents new Function("...")) */
/* We leave Function itself since it's needed internally,
 * but the constructor is effectively neutered by removing eval */
```

However, this is incorrect. `Function` is a separate constructor from `eval` and can execute arbitrary code:
```js
const fn = new Function("return process"); // works if Function isn't removed
```

In QuickJS, the `Function` constructor shares implementation with `eval` internally, so removing `eval` from the global may or may not prevent `Function("...")`. This should be verified and, if exploitable, the `Function` constructor should be explicitly removed or its internal `eval` capability disabled.

**Suggested fix:** Either verify that QuickJS's `Function` constructor is non-functional when `eval` is deleted from the global, or explicitly delete `Function` from the global object:
```c
JSAtom fn_atom = JS_NewAtom(ctx, "Function");
JS_DeleteProperty(ctx, global, fn_atom, 0);
JS_FreeAtom(ctx, fn_atom);
```

---

## High Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| H1 | `src/hull/runtime/lua/modules.c:1650-1690` | Lua template `_load_raw` lacks path traversal protection | Template name is concatenated directly into filesystem path without `..` validation. |
| H2 | `src/hull/runtime/js/modules.c:2232-2276` | JS template `loadRaw` lacks path traversal protection | Same issue as H1, in the JS runtime. |
| H3 | `src/hull/cap/tool.c:699-710` | `tool.loadfile` bypasses unveil check | `luaL_loadfile` is called without first checking the path against the unveil table. |
| H4 | `src/hull/signature.c` (throughout) | Raw `malloc`/`free` not tracked through `HlAllocator` | 14 `malloc`/`calloc`/`realloc` calls bypass the tracking allocator. |
| H5 | `src/hull/tool.c:32-67` | Secret key not zeroed after `hull_keygen` | Ed25519 secret key (`sk[64]`) remains on the stack after writing to disk. |

### H1 -- Lua Template Path Traversal (High)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/runtime/lua/modules.c:1650-1690`

The `lua_template_load_raw` function constructs a path as `app_dir/templates/<name>` where `name` comes from Lua user code. If `name` contains `../`, it can escape the `templates/` directory and read arbitrary files within the process's filesystem view.

**Current code:**
```c
char path[HL_MODULE_PATH_MAX];
int n = snprintf(path, sizeof(path), "%s/templates/%s",
                 lua->app_dir, name);
// No validation of 'name' for ".." segments
FILE *f = fopen(path, "rb");
```

**Suggested fix:** Reject `name` values containing `..`, absolute paths, or null bytes:
```c
if (strstr(name, "..") || name[0] == '/' || memchr(name, '\0', strlen(name)))
    return luaL_error(L, "invalid template name: %s", name);
```

### H2 -- JS Template Path Traversal (High)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/runtime/js/modules.c:2232-2276`

Identical issue to H1, in the JS runtime. The `js_template_load_raw` function constructs `app_dir/templates/<name>` without validating `name`.

**Suggested fix:** Same as H1 -- reject names containing `..` or absolute path prefixes.

### H3 -- tool.loadfile Bypasses Unveil (High)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/cap/tool.c:699-710`

The `l_tool_loadfile` function calls `luaL_loadfile(L, path)` without first checking the path against the unveil table (`hl_tool_unveil_check`). Other tool filesystem functions (`l_tool_read_file`, `l_tool_write_file`, `l_tool_file_exists`) all perform this check.

**Current code:**
```c
static int l_tool_loadfile(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int rc = luaL_loadfile(L, path);
    // ...
}
```

**Suggested fix:**
```c
static int l_tool_loadfile(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);
    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "path not permitted");
        return 2;
    }
    int rc = luaL_loadfile(L, path);
    // ...
}
```

### H4 -- signature.c Uses Raw malloc (High)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/signature.c`

The entire signature module uses raw `malloc`/`calloc`/`realloc`/`free` (14 allocation calls) rather than routing through `HlAllocator`. This means:
1. These allocations are invisible to the tracking allocator
2. Memory limits are not enforced
3. Leak detection via the allocator is bypassed

The signature module is called at startup (before the runtime allocator is set up), so this may be intentional. If so, it should be documented.

**Suggested fix:** Document explicitly that signature.c operates outside the allocator, or refactor to accept an optional `HlAllocator*` parameter.

### H5 -- Secret Key Not Zeroed in hull_keygen (High)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/tool.c:32-67`

The `hull_keygen` function stores the Ed25519 secret key in a stack-allocated buffer `sk[64]` and writes it to disk, but does not call `secure_zero` before returning. The key material remains on the stack and could be recoverable from a core dump or memory read.

Other code paths (e.g., `lua/modules.c`, `js/modules.c`, `crypto.c`) consistently use `secure_zero` to clear sensitive key material.

**Current code:**
```c
int hull_keygen(int argc, char **argv)
{
    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0) { /* ... */ }
    // ... write to files ...
    return 0;  // sk[64] left on stack
}
```

**Suggested fix:**
```c
#include "hull/cap/crypto.h"
// ... (or define local secure_zero)

int hull_keygen(int argc, char **argv)
{
    uint8_t pk[32], sk[64];
    // ...
    hull_secure_zero(sk, sizeof(sk));
    return 0;
}
```

Note: `hull_secure_zero` is `static` in `crypto.c`. Either make it non-static or use `kl_secure_zero` from Keel, or add a local volatile memset.

---

## Medium Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| M1 | `src/hull/cap/body.c:25` | `hl_cap_body_data` dereferences `out_data` before NULL check on itself | If both `out_data` is NULL and `reader` is NULL, crashes. |
| M2 | `src/hull/cap/tool.c:607-611` | `tool.exit` calls `exit()` directly | Bypasses Lua/Hull cleanup (hl_lua_free, sqlite3_close, etc.). |
| M3 | `src/hull/cap/tool.c:61,71,286` | `strdup` calls not tracked through allocator | Three `strdup` calls in the tool unveil/find_files code are not tracked. |
| M4 | `src/hull/commands/test.c:245,248` | `fseek` return value not checked in JS test runner | The `fseek(f, 0, SEEK_END)` and `fseek(f, 0, SEEK_SET)` calls do not check return values. |
| M5 | `src/hull/commands/dev.c:156` | `malloc` for child_argv without overflow guard | `(argc + 2) * sizeof(const char *)` could overflow if `argc` is near INT_MAX. |

### M1 -- hl_cap_body_data NULL Dereference (Medium)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/cap/body.c:23-28`

```c
size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data)
{
    if (!reader) {
        *out_data = NULL;  // crashes if out_data itself is NULL
        return 0;
    }
```

**Suggested fix:** Add NULL check for `out_data`:
```c
if (!out_data) return 0;
if (!reader) { *out_data = NULL; return 0; }
```

### M2 -- tool.exit Bypasses Cleanup (Medium)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/cap/tool.c:607-611`

The `l_tool_exit` function calls `exit(code)` directly, which skips:
- `hl_lua_free()` cleanup
- `sqlite3_close()` (if database is open)
- Arena/allocator cleanup
- Any registered `atexit` handlers

Tool mode is invoked for build/verify/inspect operations, not for long-running server mode, so the impact is limited. However, on debug builds with ASan, this will report leaked memory.

**Suggested fix:** Use `lua_error` or longjmp-based exit to allow the calling code to perform cleanup.

### M3 -- Untracked strdup in tool.c (Medium)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/cap/tool.c:61,71,286`

Three uses of `strdup()` allocate memory outside the tracking allocator:
- Line 61: `strdup(use_path)` in `hl_tool_unveil_add`
- Line 71: `strdup(path)` in `hl_tool_unveil_add` (fallback)
- Line 286: `strdup(path)` in `find_files_r`

These are freed by `hl_tool_unveil_free` and the caller of `hl_tool_find_files` respectively, so there is no leak. However, they bypass the allocator's memory tracking/limits.

### M4 -- Unchecked fseek in JS Test Runner (Medium)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/commands/test.c:245,248`

```c
fseek(f, 0, SEEK_END);
long flen = ftell(f);
if (flen < 0) { fclose(f); free(*fp); continue; }
fseek(f, 0, SEEK_SET);  // return value not checked
```

If `fseek` fails (e.g., on a pipe or special file), `ftell` returns `-1` which is caught, but the second `fseek` is not checked. The subsequent `fread` would read from an unknown position.

**Suggested fix:** Check both `fseek` return values, or use `stat()` for file size.

### M5 -- Potential Integer Overflow in dev.c malloc (Medium)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/commands/dev.c:156`

```c
const char **child_argv = malloc(((size_t)argc + 2) * sizeof(const char *));
```

The cast to `size_t` before the multiply is correct, but `argc` comes from `main()` so in practice it is bounded. However, the multiplication `(argc + 2) * sizeof(const char *)` could theoretically overflow if `argc` were near `SIZE_MAX`. Since `argc` is an `int`, the maximum value is `INT_MAX`, so `(INT_MAX + 2) * 8 = ~17 GB` -- this would fail allocation but not wrap. Low practical risk.

---

## Low Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| L1 | `src/hull/commands/dev.c:28` | `dev_child_pid` is `sig_atomic_t` but used as `pid_t` | Type mismatch between `sig_atomic_t` and `pid_t` (both `int` on Linux/macOS, but not guaranteed). |
| L2 | `src/hull/runtime/lua/modules.c:390,417,444,456` & `js/modules.c:469,507,538,551` | `sqlite3_errmsg` called directly in runtime bindings | Used only for error message formatting, not for data queries. Not a capability bypass, but breaks the pattern. |

### L1 -- sig_atomic_t / pid_t Type Mismatch (Low)

**File:** `/Users/mark/Desktop/work/artalis-io/hull/src/hull/commands/dev.c:28`

```c
static volatile sig_atomic_t dev_child_pid = 0;
// ...
kill(dev_child_pid, SIGTERM);  // kill() expects pid_t
```

`sig_atomic_t` is guaranteed to be async-signal-safe for reads/writes, but `pid_t` is the correct type for `kill()`. On all target platforms (Linux, macOS, Cosmopolitan), both are `int`, so this is safe in practice.

### L2 -- sqlite3_errmsg in Runtime Bindings (Low)

**File:** `src/hull/runtime/lua/modules.c:390,417,444,456` and `src/hull/runtime/js/modules.c:469,507,538,551`

The runtime bindings call `sqlite3_errmsg(lua->base.db)` directly to format error messages. This is read-only and does not bypass the capability layer for data access, but it does create a direct dependency on the SQLite API from within the runtime binding layer. The `hl_cap_db_*` functions could expose error messages instead.

---

## Build Hardening Verification

| Check | Status | Notes |
|-------|--------|-------|
| `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2` | PASS | Present in `CFLAGS` (Makefile line 32) |
| `-fstack-protector-strong` | PASS | Present for non-cosmo builds (Makefile line 34) |
| `-Werror` | NOT SET | Not enforced in the Makefile. The `CFLAGS` do not include `-Werror`. |
| Debug build with ASan + UBSan | PASS | `make debug` adds `-fsanitize=address,undefined` (Makefile line 43) |
| MSan build available | PASS | `make msan` with clang (Makefile line 731-735) |
| Full validation target | PASS | `make check` does clean + DEBUG=1 build + test + e2e (Makefile line 781-783) |
| Vendor code compiled with `-w` | PASS | All vendor objects use relaxed warnings |
| Static analysis: scan-build | PASS | `make analyze` target available (Makefile line 787-790) |
| Static analysis: cppcheck | PASS | `make cppcheck` target available (Makefile line 792-815) |
| Coverage reporting | PASS | `make coverage` with lcov (Makefile line 824-834) |

---

## Capability Boundary Verification

| Check | Status | Notes |
|-------|--------|-------|
| No direct `sqlite3_exec`/`sqlite3_prepare` in bindings | PASS | All queries route through `hl_cap_db_query`/`hl_cap_db_exec` |
| No direct `fopen`/`fread` in sandboxed bindings | PARTIAL | Module loaders and template loaders use direct `fopen` for dev-mode filesystem access; these should validate paths |
| SQL parameterization enforced | PASS | All SQL uses `bind_params()` in `cap/db.c` |
| Path validation in `cap/fs.c` | PASS | `hl_cap_fs_validate` rejects `..`, absolute paths, and checks ancestor prefix |
| Env allowlist enforced | PASS | `hl_cap_env_get` checks against `cfg->allowed` array |
| Lua sandbox: io/os/loadfile/dofile/load removed | PASS | `hl_lua_sandbox()` in `lua/runtime.c:78-89` |
| JS sandbox: eval removed | PASS | `hl_js_sandbox()` in `js/runtime.c:198-212` |
| JS sandbox: Function constructor | PARTIAL | Not removed; see C2 |
| HTTP host allowlist | PASS | `hl_http_check_host` validates against config allowlist |
| CRLF injection guard in HTTP client | PASS | `hl_cap_http.c` rejects `\r`/`\n` in header values |
| Tool spawn compiler allowlist | PASS | `hl_tool_check_allowlist` restricts to cc/gcc/clang/cosmocc/ar |

---

## Positive Observations

1. **No unsafe string functions anywhere.** All string operations use `snprintf`, `strncpy`, `memcpy` with explicit length bounds.

2. **Consistent overflow guards.** Size calculations check against `SIZE_MAX/2` before arithmetic (e.g., `crypto.c`, `http_parser_llhttp.c`, `db.c`).

3. **Comprehensive secure zeroing.** Key material is zeroed with `secure_zero` (volatile function pointer pattern) in crypto.c, lua/modules.c, and js/modules.c. Only exception: `tool.c:hull_keygen` (H5).

4. **Proper resource cleanup.** All error paths in `main.c` use `goto` cleanup chains. Runtime init/free pairs are balanced. SQLite open/close are paired.

5. **Tracking allocator.** `HlAllocator` wraps malloc/free with byte-level accounting and optional memory limits. Lua and JS runtimes both route allocations through it.

6. **Scratch arena pattern.** Per-request temporary allocations use `SHArena` which is reset between dispatches, preventing accumulation of temporary memory.

7. **Kernel-level sandbox.** pledge/unveil enforcement on Cosmopolitan and Linux provides defense-in-depth beyond the C-level capability checks.

8. **Clean separation.** Capability layer (`hl_cap_*`) cleanly separates resource access from runtime bindings. Both JS and Lua share the same C implementation.

9. **No `malloc(count * size)` without overflow check.** All multiplication-based allocations either use `calloc` or have explicit `SIZE_MAX` guards.

10. **Constant-time comparison.** `hl_cap_crypto_hmac_sha256_verify` uses constant-time comparison to prevent timing attacks.

---

## Recommendations

1. **Fix C1 (JS module loader path traversal)** -- This is the highest priority issue. Port the Lua `normalize_path` + `app_dir` prefix check to the JS module loader.

2. **Fix C2 (Function constructor)** -- Either verify that QuickJS's `Function` is non-functional when `eval` is deleted, or explicitly remove it. Add a test case that verifies `new Function(...)` throws in sandbox mode.

3. **Fix H1/H2 (template path traversal)** -- Reject template names containing `..` or absolute path prefixes in both Lua and JS `_template.loadRaw`/`_load_raw`.

4. **Fix H3 (tool.loadfile unveil bypass)** -- Add the same `hl_tool_unveil_check` that other tool filesystem functions use.

5. **Fix H5 (keygen secret key zeroing)** -- Add `secure_zero(sk, sizeof(sk))` before return in `hull_keygen`.

6. **Consider adding `-Werror` to CFLAGS** -- Currently warnings are generated but not treated as errors in the production build.

7. **Document signature.c allocator exclusion** -- If intentional (runs before allocator init), add a comment explaining why raw malloc is used.

8. **Add fuzz targets for signature parsing** -- `hl_sig_read` parses untrusted JSON-like input with a custom parser. This is a good target for libFuzzer.

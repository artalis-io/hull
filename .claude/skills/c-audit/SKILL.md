---
name: c-audit
description: Audit C code for security, safety, and memory management. Use when reviewing or hardening C modules.
user-invocable: true
---

# C Code Audit Skill

Perform comprehensive security, safety, and quality audits on Hull C code.

**Target:** $ARGUMENTS (default: all `src/cap/`, `src/runtime/`, and `include/hull/` files)

## Usage

```
/c-audit                              # Audit all source files
/c-audit src/cap/hull_cap_db.c        # Audit a specific file
/c-audit --fix                        # Audit and apply fixes
```

## Audit Categories

### 1. Memory Safety (Critical)

| Issue | Pattern to Find | Severity |
|-------|-----------------|----------|
| Buffer overflow | `strcpy`, `strcat`, `sprintf`, `gets`, unbounded loops | Critical |
| Unbounded string ops | `strlen`, `strcmp` on untrusted input | Critical |
| Unsafe integer parsing | `atoi`, `atol`, `atof` (no error detection, no bounds) | High |
| Integer overflow | `malloc(a * b)` without overflow check | Critical |
| Use-after-free | Pointer used after `free()` | Critical |
| Double-free | `free()` called twice on same pointer | Critical |
| Null dereference | Pointer used without NULL check | High |
| Uninitialized memory | Variables used before assignment | High |
| Missing null terminator | String buffer not explicitly terminated | High |
| Memory leak | `malloc` without corresponding `free` | Medium |
| Stack buffer overflow | Large stack arrays, VLAs | Medium |

**Safe Replacements:**
```c
// Copying
strcpy(dst, src)           -> strncpy(dst, src, sizeof(dst)-1); dst[sizeof(dst)-1] = '\0';
strcat(dst, src)           -> strncat(dst, src, sizeof(dst)-strlen(dst)-1);

// Formatting
sprintf(buf, fmt, ...)     -> snprintf(buf, sizeof(buf), fmt, ...);
gets(buf)                  -> fgets(buf, sizeof(buf), stdin);

// Memory allocation (overflow-safe)
malloc(count * size)       -> calloc(count, size);

// Integer parsing (atoi/atol have no error detection!)
atoi(str)                  -> strtol(str, &end, 10) with validation
atof(str)                  -> strtof(str, &end) with validation
```

**Allocator Discipline:**
```c
// Hull uses malloc/free directly (no custom allocator like Keel)
// Verify: every malloc has a matching free
// Verify: every malloc return is checked for NULL
// Verify: calloc used when count*size multiplication is needed

// BAD:
void *p = malloc(count * elem_size);  // overflow risk

// GOOD:
void *p = calloc(count, elem_size);   // overflow-safe
if (!p) return -1;
```

### 2. Input Validation

| Issue | What to Check |
|-------|---------------|
| Array bounds | All array indices validated before access |
| Pointer validity | NULL checks before dereference |
| Size parameters | Non-negative, within reasonable bounds |
| String length | Length checked before copy/concat |
| Numeric ranges | Values within expected domain |
| SQL parameters | `param_count` checked before binding in `hl_cap_db_*` |
| File paths | Path validation via `hl_cap_fs_validate()` before any I/O |
| Env allowlist | Variable name checked against allowlist in `hl_cap_env_get()` |

### 3. Resource Management

| Issue | What to Check |
|-------|---------------|
| File descriptors | `fopen` paired with `fclose` |
| Memory | `malloc`/`calloc` paired with `free` |
| SQLite | `sqlite3_open` paired with `sqlite3_close` |
| SQLite statements | `sqlite3_prepare_v2` paired with `sqlite3_finalize` |
| QuickJS runtime | `JS_NewRuntime` paired with `JS_FreeRuntime` |
| QuickJS context | `JS_NewContext` paired with `JS_FreeContext` |
| Lua state | `lua_newstate` paired with `lua_close` |
| Error paths | Resources freed on all exit paths |
| I/O return values | `fwrite`/`fread` return values checked |

**Hull Cleanup Pattern:**
```c
// Every init must have matching free
hl_js_init()           -> hl_js_free()
hl_lua_init()          -> hl_lua_free()
sqlite3_open()         -> sqlite3_close()
sqlite3_prepare_v2()   -> sqlite3_finalize()
JS_NewRuntime()        -> JS_FreeRuntime()
JS_NewContext()         -> JS_FreeContext()
lua_newstate()         -> lua_close()
fopen()                -> fclose()
```

### 4. Integer Overflow

Overflow in size computations can cause undersized allocations and buffer overflows.

```c
// BAD: overflow on 32-bit
int total = count * sizeof(HlColumn);
void *buf = malloc(total);

// GOOD: use size_t + calloc
HlColumn *cols = calloc(count, sizeof(HlColumn));

// GOOD: check before multiply
if (count > 0 && (size_t)count > SIZE_MAX / sizeof(HlColumn)) {
    return -1;  // overflow
}
```

**Key areas in Hull:**
- `HlColumn` array allocation in `hl_cap_db_query()` row callbacks
- PBKDF2 output buffer sizing
- SQL parameter arrays (`HlValue` binding arrays)
- Filesystem path buffer construction (`base_dir + "/" + relative_path`)
- QuickJS/Lua value conversion arrays

### 5. Capability Boundary Enforcement

Hull's security model depends on the shared `hl_cap_*` layer. Verify:

| Issue | Severity | What to Check |
|-------|----------|---------------|
| Direct SQLite calls | Critical | JS/Lua bindings never call `sqlite3_*` directly |
| Direct file I/O | Critical | JS/Lua bindings never call `fopen`/`fread`/`fwrite` directly |
| Path traversal | Critical | `hl_cap_fs_validate()` called before every file operation |
| SQL injection | Critical | All SQL uses parameterized binding via `hl_cap_db_*` |
| Env leakage | High | All env access through `hl_cap_env_get()` with allowlist |
| Sandbox escape | Critical | `eval()` removed, `io`/`os` libs not loaded, `loadfile`/`dofile` removed |

### 6. Defensive Macros

Check for and suggest these patterns:
```c
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
```

### 7. Test Coverage

Check test files (`tests/test_*.c`) for:
- [ ] Basic functionality tests
- [ ] Edge cases (empty input, max values, NULL)
- [ ] Error path tests (what happens when things fail)
- [ ] Bounds checking tests
- [ ] All public API functions have at least one test
- [ ] Path traversal rejection tested
- [ ] SQL parameterization tested
- [ ] Env allowlist enforcement tested
- [ ] Sandbox restrictions tested (both JS and Lua)

### 8. Dead Code Detection

| Pattern | Issue | Fix |
|---------|-------|-----|
| `if (0) { ... }` | Dead branch | Remove |
| `return; code_after;` | Unreachable code | Remove |
| `#if 0 ... #endif` | Disabled code | Remove or document |
| Unused `#define` | Dead macro | Remove |
| Unused static function | Dead function | Remove |

Compile with `-Wunused` flags to detect automatically.

### 9. Build Hardening

**Development build (`make debug`):**
```makefile
-fsanitize=address,undefined -g -O0 -fno-omit-frame-pointer
```

**Production build (`make`):**
```makefile
-Wall -Wextra -Wpedantic -Wshadow -Wformat=2
-fstack-protector-strong
-O2
```

**Full validation (`make check`):**
```makefile
clean + DEBUG=1 build + test + e2e
```

**Audit Checks:**
- [ ] `-fstack-protector-strong` in production CFLAGS
- [ ] Debug build with ASan + UBSan available (`make debug`)
- [ ] Full validation available (`make check`)
- [ ] All tests pass under sanitizers
- [ ] No compiler warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`

## Audit Procedure

When `/c-audit` is invoked:

1. **Locate Files**
   ```
   src/cap/*.c                 # Shared capability layer
   src/runtime/js/*.c          # QuickJS runtime integration
   src/runtime/lua/*.c         # Lua 5.4 runtime integration
   src/main.c                  # Entry point
   include/hull/*.h            # Public headers
   tests/test_*.c              # Test files
   Makefile                    # Build configuration
   ```

2. **Scan for Critical Issues**
   - Search for unsafe functions: `strcpy`, `sprintf`, `gets`, `strcat`
   - Search for unsafe integer parsing: `atoi`, `atol`, `atof`
   - Search for unchecked allocations: `malloc`/`calloc` without NULL check
   - Search for integer overflow in size calculations
   - Search for missing bounds checks on array access
   - Search for unchecked `fwrite()`/`fread()` return values
   - Search for direct SQLite/file I/O calls in runtime bindings (bypass of `hl_cap_*`)

3. **Review Public API**
   - Check all public functions in headers (`hl_*` prefix)
   - Verify NULL checks on pointer parameters
   - Verify bounds checks on size parameters

4. **Check Resource Management**
   - Every `_init()`/`_create()` has matching `_free()`/`_close()`
   - Error paths free allocated resources
   - No memory leaks on early returns
   - SQLite statements finalized on error paths
   - QuickJS/Lua contexts cleaned up on error paths

5. **Check Capability Boundaries**
   - JS/Lua bindings only access resources through `hl_cap_*` functions
   - Path validation enforced before every filesystem operation
   - SQL always parameterized
   - Sandbox restrictions in place (no eval, no io/os libs)

6. **Detect Dead Code**
   - Compile with `-Wunused` flags
   - Find unused static functions
   - Find unused variables and parameters
   - Flag commented-out or `#if 0` code blocks

7. **Check Build Hardening**
   - Sanitizers available in debug build
   - Stack protection in production build
   - Warning flags comprehensive

8. **Generate Report**
   Format as markdown table with findings, severity, file:line, and suggested fix.

## Report Format

```markdown
## C Audit Report: Hull

**Date:** YYYY-MM-DD
**Files Scanned:** N
**Issues Found:** N (Critical: N, High: N, Medium: N, Low: N)

### Critical Issues

| # | File:Line | Issue | Current Code | Suggested Fix |
|---|-----------|-------|--------------|---------------|
| C1 | src/cap/hull_cap_db.c:42 | Buffer overflow | `strcpy(buf, src)` | `snprintf(buf, sizeof(buf), "%s", src)` |

### High Issues
...

### Medium Issues
...

### Low Issues
...

### Recommendations
1. ...
```

## Fix Mode (--fix)

When `--fix` is specified:

1. Generate the audit report first
2. For each fixable issue, apply the transformation
3. Rebuild (`make`)
4. Re-run tests (`make test`)
5. Report any test failures or new warnings

**Auto-fixable Issues:**
- `strcpy` -> `snprintf` with buffer size
- `sprintf` -> `snprintf` with buffer size
- `atoi` -> `strtol` with validation
- Missing NULL checks (add early return)
- Missing `malloc` return check (add NULL check)
- Integer overflow in size calc -> `calloc` or overflow check
- Missing `size_t` casts in size calculations
- Unused local variables (remove)
- Unused static functions (remove)

**NOT Auto-fixable (require manual review):**
- Logic errors
- Resource leaks in complex control flow
- Capability boundary violations
- Sandbox escape paths
- Architectural changes

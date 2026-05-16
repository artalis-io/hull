# C Audit Report: Hull (post-HlWasmBuffer)

**Date:** 2026-03-21
**Scope:** Full codebase audit with focus on new HlWasmBuffer code
**Files Scanned:** ~45 (all src/hull/, include/hull/, tests/hull/)
**Issues Found:** 4 (Critical: 1, High: 1, Medium: 1, Low: 1)

---

## Critical Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| C-1 | `src/hull/cap/wasm.c:845` | Pool release with wrong success flag | `hl_wasm_pool_release(..., 1)` called unconditionally even when `*output_buf` is NULL (malloc failure). Instance gets pooled as "successful" despite caller receiving `HL_WASM_ERR_INTERNAL`. |

**C-1 Detail:**

In `hl_cap_wasm_call_buf()`, when `hl_wasm_buffer_create_owned()` fails at line 838 or 841, `*output_buf` is NULL. The code then frees `wasm_out_ptr` (correct) but calls `hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn, heap_size, stack_size, 1)` with `success=1` (incorrect). The instance is returned to the pool despite the operation failing. Line 848 returns `HL_WASM_ERR_INTERNAL` but the damage is done.

**Impact:** Pool state inconsistency. The instance's WASM heap allocator had a successful `wasm_runtime_module_free` for the output, so the instance itself is clean — the practical risk is low (just incorrect success bookkeeping). However, the pattern is wrong and should be fixed for correctness.

**Fix:**
```c
// Line 844-848: change success flag to be conditional
if (wasm_out_ptr) wasm_runtime_module_free(inst, wasm_out_ptr);
int ok = (*output_buf != NULL);
hl_wasm_pool_release(cache, mod, inst, exec_env, process_fn,
             heap_size, stack_size, ok);
return ok ? HL_WASM_OK : HL_WASM_ERR_INTERNAL;
```

**Comparison:** The worker path (`worker_wasm.c:246`) correctly checks `!op->error` before pooling. The original `hl_cap_wasm_call` (`wasm.c:626`) correctly uses `ret == HL_WASM_OK`.

---

## High Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| H-1 | `src/hull/migrate.c:269` | Unchecked fread return | `fread()` return value stored but not verified against expected length. Short read silently truncates migration SQL. |

**H-1 Detail:**

```c
size_t read = fread(sql, 1, (size_t)flen, f);
sql[read] = '\0';   // ← short read → truncated SQL executed as migration
```

Other `fread` call sites in the codebase properly check return values (e.g., `fs.c:153` checks `ferror()`, `wasm.c:324` checks `nr == (size_t)fsize`).

**Impact:** A partial disk read (I/O error, NFS timeout) would execute truncated SQL as a migration. Since migrations run in `BEGIN IMMEDIATE`, a truncated `CREATE TABLE` could commit a partial schema.

**Fix:**
```c
size_t read = fread(sql, 1, (size_t)flen, f);
if (read != (size_t)flen) {
    free(sql);
    fclose(f);
    migration_list_free(ml);
    log_error("[migrate] short read on %s (%zu/%ld bytes)",
              ml->names[i], read, flen);
    return -1;
}
sql[flen] = '\0';
```

---

## Medium Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| M-1 | `src/hull/cap/wasm.c:172` | Unused parameter on non-pool path | `process_fn_v` parameter unused when instance is destroyed (non-pooling path). Not a bug, but should have `(void)` cast or be documented. |

**M-1 Detail:**

`hl_wasm_pool_release()` takes `process_fn_v` which is only stored into the pool entry on the pooling path. On the destroy path (lines 196-197), only `exec_env` and `inst` are destroyed — `process_fn` is owned by the instance and doesn't need separate cleanup. This is correct behavior but the compiler may warn with `-Wunused-parameter` in some configurations.

---

## Low Issues

| # | File:Line | Issue | Description |
|---|-----------|-------|-------------|
| L-1 | `tests/hull/cap/test_wasm_buffer.c` | Sign-compare warnings | `ASSERT_EQ` on enum values required explicit `(int)` casts to suppress `-Wsign-compare`. Already fixed but indicates the test framework macro could be improved. |

---

## Clean Findings (No Issues)

### Unsafe Functions
- **strcpy, strcat, sprintf, gets**: None found. All string operations use `snprintf`.
- **atoi, atol, atof**: None found. All integer parsing uses `strtol` with validation.

### Memory Safety
- All `malloc`/`calloc` return values are NULL-checked.
- Integer overflow guards present in crypto.c (`SIZE_MAX/2` checks).
- No use-after-free or double-free patterns detected.
- `hl_wasm_buffer_destroy()` properly idempotent (checks `buf->closed`).

### Capability Boundaries
- No direct `sqlite3_*` calls from Lua/JS runtime bindings — all routed through `hl_cap_db_*`.
- No direct file I/O from runtime bindings for user data — `fopen` only used for module/template loading (host-side bootstrap).
- `eval()` and `Function()` removed from JS; `io`, `os`, `loadfile`, `dofile`, `load` removed from Lua.
- WASM plugins have no I/O imports — pure computation only.

### Resource Management
- `hl_wasm_buffer_destroy()` handles all three kinds (OWNED/MMAP/WASM) correctly.
- Worker cancellation path (`wasm_done_fn`) properly destroys buffers via `hl_worker_wasm_op_free()`.
- Worker buffer mode correctly checks `!op->error` before pooling (line 246).
- All `fopen`/`fclose` pairs verified balanced on all code paths in `fs.c`, `tool.c`, `wasm.c`.

### Build Hardening
- `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2` in production.
- `-fstack-protector-strong` enabled.
- ASan + UBSan available via `make debug`.
- All 20/20 test suites pass on clean build.
- All 7/7 E2E compute tests pass (including new buffer tests).

---

## Recommendations

1. **Fix C-1 immediately** — conditional success flag in `hl_cap_wasm_call_buf` pool release.
2. **Fix H-1** — add `fread` short-read check in `migrate.c`.
3. **No architectural changes needed** — the HlWasmBuffer design is sound, capability boundaries intact, sandbox restrictions enforced.

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

### 5b. Sealed runtime tables (read-only memory protection)

Hull defends boot-built security policy via two mechanisms (see
[docs/security.md §4b](../../../docs/security.md)):

- `.rodata` for compile-time constants (`static const` tables —
  automatic OS read-only via the linker).
- `hl_seal_arena` (page-backed mmap RW → mprotect RO) for data that
  can't be `static const` because it's built at boot from app input
  (manifest, configuration, capability resolution).

When auditing C runtime changes, look for these patterns:

| Issue | Severity | What to check |
|---|---|---|
| **Boot-built mutable security policy not sealed.** A new C-level structure that's (a) built once at boot from app input, (b) read on every request by the capability/sandbox layer, (c) influences security policy (allowlists, dispatch, trust anchors), and DOESN'T flow through `hl_seal_arena`. | Critical | Find the boot-init site; verify it calls `hl_seal_arena_alloc` / `_strdup` / `_memdup` and seals before the resolver/sandbox runs. Example pattern: `HlManifest` in `src/hull/serve.c::hl_serve_wire_caps`. |
| **`xxd -i` generated table without `const`.** Default xxd output emits `unsigned char foo[]` (writable). Any new embedded asset (CA bundle, signed manifest, embedded key, vendored binary) that lands in writable `.data` instead of read-only `.rodata`. | High | Grep the Makefile for `xxd -i` invocations; verify each is followed by the `XXD_CONST_SEAL` (sed post-process) or `XXD_CONST_PIPE` macro defined near the top of the Makefile. |
| **Function-pointer table not `const`.** A new dispatch vtable, registry, or callback array declared as plain `static T table[]` instead of `static const T table[]`. A writable function-pointer table in `.data` is a direct ROP/JOP pivot — a single arbitrary-write primitive turns into RCE by overwriting one slot. | Critical | Every `HlRuntimeVtable`, `HlDbBackend`, `HlAsyncBackend`, `HlNetBackend`, `HlGpuBackend`, `HlCompilerVtable`, etc. instance must be `const`-qualified. Same for `HlModuleSpec` arrays, command-dispatch tables, `luaL_Reg[]`, QuickJS `JSCFunctionListEntry[]`. The compile-time `const` lands the table in `.rodata`, which is RO-mapped by the linker — same protection level as the sealed arena, for free. |
| **`__attribute__((constructor))` mutating dispatch state.** A new boot-time constructor that initialises a function-pointer table or capability config. Constructors run before `main`, BEFORE the sandbox/seal phases — anything they touch is implicitly trusted boot state. | High | Avoid constructors for security policy; do init explicitly in the boot phase where the order is auditable. If a constructor IS necessary (e.g. WAMR's own globals), document why and verify it only touches its own static state, never Hull's. |
| **Long-lived C struct holding secret material.** Any new `struct { char key[N]; ... }` or `static uint8_t shared_secret[N]` that survives past the immediate operation. Hull's convention is Lua/JS-side `_state` tables + per-call stack-local `uint8_t key[128]` with `secure_zero` on return. | Critical | If C must cache secret material (e.g. parsed mbedTLS key context held across requests), seal it via `hl_seal_arena` or zero it on every use. Document the lifetime. |
| **Sealing failure not fatal.** A call to `hl_seal_arena_seal` whose return code isn't checked, or where -1 is logged-and-continued. The whole point is to fail closed; shipping with unsealed policy silently weakens the hardening guarantee. | Critical | Sealing failure must propagate up the boot-error path (return -1 from the boot phase → process exits). The only acceptable test-only path is documented + gated. |
| **Writable alias retained after seal.** A pointer to the to-be-sealed memory cached elsewhere (e.g. in a `HlRuntime` field) without being updated to the in-arena address. After sealing, the alias still points at the now-freed source. | High | Verify all consumers receive the sealed-copy pointer, not the source. The `hl_manifest_seal` pattern is to value-copy the struct, so consumers reading `&s->manifest` see the new pointers automatically. |
| **Sealed arena destroyed before aliasing consumer.** A teardown path that calls `hl_seal_arena_destroy` before every consumer that holds aliased pointers into it (cap configs, TLS contexts, runtime tables) has been freed. The arena must be destroyed LAST in cleanup. | High | Audit both success and failure cleanup paths (`hl_serve_teardown_after_serve`, `hl_serve_cleanup`, `hl_serve_undo_caps`). The pattern is documented in `src/hull/serve.c` — destroy arena AFTER `hl_app_context_free`, WASM/GPU caches, TLS contexts. |
| **Allocation after seal.** Code that tries to extend a sealed arena later (`hl_seal_arena_alloc` post-seal returns NULL — easy to miss in error paths if you assume alloc always succeeds). | Medium | Bump-arena alloc-after-seal is a programming bug; the call site should never reach that branch in production. Add an assertion. |
| **Casting away `const` on a sealed/rodata table.** `T *mut = (T *)((uintptr_t)CONST_TABLE);` or similar laundering to write into a read-only mapping. Will fault at runtime (SIGSEGV/SIGBUS) but the *intent* indicates a design mistake. | High | Reject the cast. If the table genuinely needs to mutate, it shouldn't be sealed — surface the lifecycle to the audit and decide on a per-case basis. |

**Cross-platform guard.** `hl_seal_arena` is POSIX (mmap/mprotect/
sysconf). Cosmopolitan provides the POSIX shim transparently. If you
ever add a native MSVC build, the arena needs a `#ifdef _WIN32`
branch using `VirtualAlloc` / `VirtualProtect` — same API contract.

**Death tests are non-negotiable.** Any new sealed surface needs a
fork+SIGSEGV test that writes to a sealed page from a child process
and asserts the child died with SIGSEGV/SIGBUS. Without it, a no-op
mprotect would silently pass every other test. Pattern in
`tests/hull/test_seal_arena.c::write_after_seal_faults`.

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

### 9. Build hardening + ROP/JOP resistance

Hull ships a probe-based compiler/linker hardening layer (Makefile
~lines 69-180, full reference in [docs/security.md §4c](../../../docs/security.md)).
The verifier at `scripts/check_hardening.sh` runs on every CI build
and every release native target; the release **fails** if a required
protection drops.

#### 9.1. Verify the build flags are still applied

The baseline (unconditional on non-COSMO):

```makefile
-fstack-protector-strong  -fPIE  [-pie on Linux]
-D_FORTIFY_SOURCE=3  (release only)
-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack  (Linux only)
```

The probed set (added if the toolchain accepts each — older
toolchains reject some; the probe at `hl_have_cflag` /
`hl_have_ldflag` handles fallback cleanly):

```
CFLAGS:   -fstack-clash-protection
          -fno-plt  -fno-common
          -ftrivial-auto-var-init=zero
          -fzero-call-used-regs=used-gpr
          -fcf-protection=full         (x86_64 only)
          -mbranch-protection=standard (arm64 only)
LDFLAGS:  -Wl,-z,separate-code  (Linux only)
          -Wl,--as-needed       (universal where supported)
```

Sanity checks for any audit touching the Makefile:

- [ ] `make hardening` runs without error and reports the expected
      flags for the host toolchain.
- [ ] `make check-hardening` passes on macOS and Linux (skips
      everything on cosmo, expected).
- [ ] Existing `-fstack-protector-strong` / `-fPIE` / FORTIFY /
      RELRO baseline still present.
- [ ] If a new flag is added: probe it via `hl_have_cflag` rather
      than hard-applying — must not break older toolchains.
- [ ] `HULL_DISABLE_HARDENING=1` opt-out still works for local
      debugging (and CI's `check-hardening` step still fails on
      release if it was set).

#### 9.2. C-level patterns that DEFEAT the hardening

The hardening flags raise the cost of exploitation; specific C
patterns can undo it in a single line. When auditing new code,
flag these:

| Pattern | Severity | Why it defeats hardening |
|---|---|---|
| `mmap(..., PROT_WRITE \| PROT_EXEC, ...)` or `mmap(..., PROT_EXEC, ...)` followed by writes / `mprotect(... PROT_WRITE \| PROT_EXEC ...)`. | Critical | Direct W^X violation. Hull's design ban on RWX memory (no JIT, no runtime codegen) is the precondition every other hardening flag assumes. The sealed-arena pattern is RW → RO; the reverse direction has no legitimate use case. |
| `dlopen` / `dlsym` / `dlmopen` / `dladdr` introduced in any Hull TU. | Critical | Forces lazy binding (defeats BIND_NOW), expands DT_NEEDED surface (defeats `-Wl,--as-needed` shrinkage), and gives runtime symbol resolution which is a classic ROP gadget pivot. Hull's `manifest.allow_dynamic_libraries` is the app-side opt-in; the C side should not initiate dlopen from its own code. |
| New global function-pointer table without `static const`. | Critical | Lands in writable `.data`; one arbitrary-write primitive overwrites a slot → control-flow hijack. See §5b above. |
| Unbounded `alloca(n)` / VLA with data-derived size. | High | Defeats `-fstack-clash-protection` by jumping the guard page in one allocation. If the size is bounded (`alloca(SMALL_CONSTANT)`) it's fine; if it's `alloca(strlen(input))` it's a stack-pivot primitive. Hull's convention is `#pragma GCC diagnostic` doesn't suppress `-Wvla` — VLAs should be replaced by `malloc` + size check. |
| `setjmp` / `longjmp` straddling a sandbox or seal boundary. | High | Linux CET shadow stack (`SHSTK`) tracks frames; non-local jumps via standard `setjmp`/`longjmp` cooperate via glibc shims, but a custom `setjmp`-like macro or hand-rolled context switch loses the protection. If absolutely needed, use `sigsetjmp` with `savesigs=1` only when documented. |
| Hardcoded executable address literals (`0x[0-9a-f]{6,}` that smell like a pointer). | High | Defeats ASLR — implies the code expects a known layout. Common sources: ported exploit PoCs, fixed-address mmap, hand-rolled JIT trampolines. Reject unless there's a documented platform reason (e.g. a kernel-defined vDSO address). |
| `__attribute__((no_stack_protector))` on a non-leaf function. | High | Disables the canary on functions that might have a stack buffer. Only acceptable on tiny leaf helpers that demonstrably never take `&local` of a stack object. Document the reason inline. |
| `__attribute__((interrupt))`, `__attribute__((naked))`, or custom calling conventions. | High | Bypasses `-fzero-call-used-regs` and the standard return-address tagging. No legitimate use case in Hull. |
| Inline asm (`__asm__` / `asm volatile`) clobbering registers `-fzero-call-used-regs` would zero. | Medium | Mostly fine if it doesn't escape — but inline asm in a function that's later inlined into a security-sensitive caller can leak register state. Review the clobber list and the calling context. |
| Casting a function pointer through `void *` (e.g. via `dlsym` return, generic registry lookup) and calling without IBT/BTI marker check. | High | Defeats CET IBT / arm64 BTI: the target function must start with `endbr64` / `bti` for the indirect-branch landing to be legal. Functions reached via `dlsym` aren't compiled with CET markers by default. If Hull must call a dynamically-resolved function pointer, the resolver must verify the target's start instruction. (Status today: Hull has no dlsym; this check is forward-looking.) |
| `__builtin_return_address(N)` for N > 0. | Medium | Walks the call stack manually — works against the frame pointer (which release builds may omit) and against the shadow stack (which intentionally hides return addresses from userspace). Almost always a sign of a hack; prefer explicit context passing. |
| New vendor library imported without applying hardening CFLAGS. | Medium | New vendor TU's `CFLAGS := … -w …` clobbers the global set; gadgets in its text segment are reachable. Add hardening to that TU's CFLAGS array OR document the deferral in the per-library `_CFLAGS` block. |
| Adding `-rdynamic` or `-Wl,-export-dynamic` to LDFLAGS. | High | Exports every symbol — makes the binary trivially introspectable for gadget search. Hull is a static executable; no symbols should be exported. |

#### 9.3. Sanitizer / debug-mode hygiene

| Pattern | Severity | Why |
|---|---|---|
| `make debug` build doesn't actually instrument new code. | High | If a new TU sets its own `CFLAGS := …` (no `+=`) it clobbers the inherited `-fsanitize=address,undefined`. The TU compiles, the linker fails on ASan runtime symbols. Verify any new vendor `*_CFLAGS` block inherits sanitizer flags from `$(CFLAGS)` (or has an explicit `-fsanitize=…` block under `ifdef DEBUG`). |
| `make msan` doesn't instrument the vendor TU. | High | MSan requires every TU on the read-path to be instrumented; an uninstrumented `mbedtls_sha256()` call makes the caller's read of the output buffer flag as use-of-uninitialized-value. See the existing `MBEDTLS_CFLAGS` block under `ifdef MSAN` for the pattern. |
| New death test (fork + write to sealed page + assert `WIFSIGNALED`) lacks the `signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL);` reset in the child. | High | Sanitizer runtimes install their own SEGV handler that prints a diagnostic and `_exit(1)`. The child then dies "cleanly" instead of by signal, and `WIFSIGNALED` returns false → test fails. Reset SIG_DFL in the child immediately after `fork()`. Pattern: `tests/hull/test_seal_arena.c::write_after_seal_faults`. |
| All tests pass under sanitizers (`make check`). | High | Required for CI. ASan + UBSan must be green; MSan + UBSan must be green on Linux. |

#### 9.4. Expected commands

- `make debug` — ASan + UBSan + `-O0 -g -fno-omit-frame-pointer`.
- `make msan` — MSan + UBSan (Linux clang only).
- `make check` — clean + ASan build + test + e2e.
- `make hardening` — print resolved hardening flag list.
- `make check-hardening` — run post-build verifier on `build/hull`.
- `scripts/check_hardening.sh [BINARY]` — same, on any binary.

**Audit checklist for §9:**

- [ ] No new RWX-introducing patterns (mmap+PROT_EXEC|PROT_WRITE, mprotect to W+X).
- [ ] No new dlopen/dlsym in Hull TUs.
- [ ] No new global function-pointer tables missing `static const`.
- [ ] No new unbounded `alloca` / VLAs.
- [ ] No new `__attribute__((constructor))` touching dispatch state.
- [ ] `make hardening` reports the expected flag set.
- [ ] `make check-hardening` passes on macOS + Linux + cosmo.
- [ ] Sanitizer builds still build and pass.

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
   - Search for ROP/JOP defeats (§9.2):
     - `mmap.*PROT_EXEC.*PROT_WRITE` / `mprotect.*PROT_EXEC.*PROT_WRITE` (RWX)
     - `dlopen` / `dlsym` / `dlmopen` (runtime symbol resolution)
     - `alloca(` with data-derived size (stack-clash defeat)
     - new global function-pointer arrays missing `static const`
     - `__attribute__((constructor))` (pre-`main` mutation)
     - `__attribute__((no_stack_protector))` / `((naked))` / `((interrupt))`
     - hardcoded executable address literals (`0x[0-9a-f]{6,}`)
     - `-rdynamic` / `-Wl,-export-dynamic` in any Makefile change

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

7. **Check Build Hardening (§9)**
   - Probe-based flag set still applied (`make hardening` reports
     the expected list for this toolchain).
   - Verifier passes (`make check-hardening`, or
     `scripts/check_hardening.sh build/hull` directly).
   - No new C pattern from the §9.2 defeat list landed in this
     change (RWX, dlsym, unbounded alloca, naked attribute, etc.).
   - Sanitizer builds (`make debug`, `make msan`) still build and
     pass — if a new vendor TU was added, its `*_CFLAGS` block must
     inherit / re-add the sanitizer flags under `ifdef DEBUG` /
     `ifdef MSAN`.

8. **Check Sealed Runtime Tables (§5b)**
   - Any new boot-built mutable security policy flows through
     `hl_seal_arena` and is sealed before the resolver/sandbox
     runs.
   - Any new global function-pointer table (vtable, registry,
     callback array) is `static const`.
   - Any new `xxd -i` Makefile invocation uses `XXD_CONST_SEAL` or
     `XXD_CONST_PIPE` to land in `.rodata`.
   - Sealed-arena destroy stays LAST in every cleanup path.

9. **Generate Report**
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
- `static T table[]` for dispatch / vtable / registry → add `const` (§5b)
- New `xxd -i` Makefile rule missing `const` post-process → add `XXD_CONST_SEAL` / `XXD_CONST_PIPE` (§5b)
- Bounded `alloca(SMALL_CONSTANT)` left as-is; data-derived `alloca(n)` → flag, propose `malloc` + size check (§9.2, not auto-fix)

**NOT Auto-fixable (require manual review):**
- Logic errors
- Resource leaks in complex control flow
- Capability boundary violations
- Sandbox escape paths
- Architectural changes
- New W^X primitives (`mmap`/`mprotect` with `PROT_WRITE|PROT_EXEC`)
- New `dlopen` / `dlsym` introduced in C
- Sealed-arena destroy reordering — must trace every aliasing consumer's lifetime
- New `__attribute__((constructor))` — needs design review of init order
- Probe regressions: if `make hardening` reports fewer flags than before on the same toolchain, the probe macro or surrounding `ifdef` was likely broken — manual investigation

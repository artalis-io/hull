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

### 10. Lifetime classes & arena discipline

Hull's trusted core avoids most use-after-free / double-free /
lifetime bugs *by construction*: security-sensitive code does not
individually `free()` objects, it lets an arena's destroy point free
everything at once. When auditing allocation, classify every site by
**lifetime class** and check it uses the right ownership model.

| Lifetime class | What | Allocator / owner | Destroy point |
|---|---|---|---|
| **boot arena** | tables/config built once at init, then validated + sealed read-only | `sh_seal_arena` (RW→mprotect RO) | destroyed LAST in teardown, after every aliasing consumer (see §5b) |
| **module arena** | lifetime tied to one module / runtime instance (WASM module cache, GPU device caches) | per-instance struct + its own mutex | instance `_free()` / `_destroy()` |
| **request arena** | lifetime tied to ONE operation / call / RPC / tool invocation | `sh_arena` (bump), reset per request | one `sh_arena_reset` / `_destroy` per op |
| **scratch arena** | transient parse/serialize workspace | `sh_arena` mark/rewind (`hl_arena_mark`/`_rewind` in `src/hull/utils/alloc.c`) | rewind at end of the local scope |
| **third-party glue** | malloc/free required by an external API (SQLite, mbedTLS, QuickJS, Lua, WAMR, wgpu) | that library's allocator | that library's free; Hull must not cross allocators |

Patterns to flag:

| Issue | Severity | What to check |
|---|---|---|
| **Scattered request-temporary `malloc`/`free` in a security path** where an arena's lifetime would remove the manual free entirely. The danger is a `luaL_error`/longjmp or early-return path that skips the `free`. | High | Prefer arena-backed scratch (the request/scratch arena is freed wholesale regardless of how the function exits). Hull's `mod_db.c`/`mod_crypto.c`/`mod_http_client.c` already do this — new request-scoped scratch should too. |
| **`free()` outside allocator/arena internals in core runtime code.** | Medium | Strongly discouraged. Every `free` is a UAF/double-free surface. Confirm the object isn't arena-owned (freeing an arena sub-allocation is a bug). `shared/` and `utils/` should approach zero raw `free`. |
| **More than one owner / more than one destroy point for an arena.** | Critical | Each arena has exactly ONE owner and ONE destroy call. Two destroys = double-free of the whole region. |
| **Pointer retained into an arena after the arena is reset/destroyed.** | Critical | A cached pointer into request/scratch memory used after reset is a UAF. Verify no long-lived struct field aliases arena memory across a reset. The seal pattern value-copies structs precisely to avoid this. |
| **`realloc()` that may invalidate a pointer/iterator held across the call.** | High | Every `realloc` must reassign into the owning field and NO previously-obtained element pointer or loop cursor may survive the call. Re-derive the pointer after the realloc (e.g. `row = &r->values[i]` *after* `db_result_grow`). See §10.1. |
| **Crossing allocators** — e.g. `free()`-ing a pointer that came from a codec/library allocator, or vice versa. | High | Free with the matching allocator. Image-codec output, mbedTLS buffers, SQLite strings each have their own free contract. |
| **Missing debug poisoning after arena reset/destroy.** | Low | Under `make debug`, poison freed region so stale reads trap (sh_arena already ASan-poisons on create/reset). New arenas should match. |

### 10.1. realloc / internal-mutable-pointer hazards

Two specific sub-checks that catch the subtle lifetime bugs:

- **Dangling-after-realloc.** Grep every `realloc`. For each, confirm
  the result is stored back into the owning struct and that the code
  does not hold a pointer/iterator obtained *before* the realloc.
  Growable buffers (`tui` out_buf, `worker_db` value array, `tool`
  result array, `agent` line buffers, `poll` timer/watcher/completion
  arrays) are the usual sites. A bounded-size check (`SIZE_MAX/2`)
  must precede the doubling.
- **API returning a pointer into mutable internal storage.** A public
  `hl_*` getter must return either a `const` pointer into immutable
  `.rodata`/sealed memory (e.g. `hl_vfs_find` → `const HlEntry *`), a
  freshly-allocated string with a documented caller-frees contract, or
  a stable handle the caller is *meant* to drive. Flag any getter that
  hands back a non-`const` pointer into a buffer a later call could
  `realloc`/`free` (a latent dangling reference), or a write-through
  into sealed security policy.

### 11. Iterator invalidation

The dangerous shape is **iterate a mutable container while it can be
mutated during the walk**. The mutation almost always arrives through
a **callback invoked inside the loop body that re-enters Hull** and
adds/removes/grows the very container being walked.

| Issue | Severity | What to check |
|---|---|---|
| **Callback-driven loop over a mutable container** (broadcast over a connection list, timer fire over a timer array, pool drain, cache eviction, per-row DB callback) where the callback can re-enter and mutate the container. | Critical/High | The callback must run in a context that *cannot* re-enter and mutate (sandbox/driver boundary), OR the loop must snapshot first, OR teardown must be deferred. Document the non-reentrancy invariant at the `cb()` call site (the WS-broadcast comment is the template). |
| **Element pointer held across an add that may `realloc`/grow.** | High | Use index-based iteration that re-reads `count`, not a cached `&arr[i]`. |
| **LRU/cache eviction nested inside iteration over the same cache.** | High | The prepared-statement cache is the canonical hazard: evicting (finalizing) the in-flight entry mid-loop is a UAF. Keep an explicit "in-use entry is pinned against eviction" rule. |

Safe patterns to require: **snapshot array** (copy the elements/handles
before the loop), **index-based iteration with explicit mutation rules**
(append-only + re-read count), **two-phase update/delete queue** (mark in
phase 1, apply after the loop), or **sealed immutable array** (registries
that are `static const` — confirm they're never mutated at runtime).
Document, per container, whether mutation during iteration is allowed.
Do not expose raw container storage if growth/realloc can occur.

### 12. Generational handles for long-lived objects

For long-lived runtime objects currently referenced by raw pointers
across an API boundary (script → C), prefer a **generational handle**
over a raw pointer so a stale reference fails validation instead of
dereferencing freed memory:

```c
typedef struct { uint32_t index; uint32_t generation; } HullHandle;
```

The backing table slot carries `generation` + an `alive` bit; lookup
validates `slot.generation == handle.generation` and returns NULL on
mismatch. Apply this ONLY where it improves safety without broad churn:

| When to recommend | When NOT to |
|---|---|
| A script-visible object that outlives a single call, can be destroyed by app code while another reference exists, and is reached by raw pointer (the classic dangling-userdata risk). | Objects already protected by an index+generation pool, a per-call stack local, a `static const` table, or a monotonic ID under a lock (Hull's CFI/typed-handle work already covers the vtables — don't re-handle those). |
| Connection / instance / buffer registries where "destroyed by name while a handle is held" is reachable. | Boot/sealed config (immutable — no stale-handle risk). |

Today Hull's binding layer mostly uses the "null the userdata's opaque
pointer on close/destroy, methods fail closed" pattern — an acceptable
hand-rolled equivalent. Flag any binding where close/destroy frees the
underlying struct but a *suspended/async* continuation can still resume
against it (that's where the fail-closed null is insufficient).

### 13. Aliasing & single-owner mutation

| Issue | Severity | What to check |
|---|---|---|
| **Sealed/boot-built security state exposed as non-`const`** after init. | High | Configuration, policy, dispatch, vtable, capability, and manifest-derived tables must be reachable only as `const` once initialized (see §5b). |
| **API exposing a mutable internal pointer** instead of mutating through the owner. | High | Prefer `hl_x_set(owner, ...)` over handing out `&owner->field`. See §10.1. |
| **Two live aliases to the same object with unclear ownership**, one of which may free/realloc. | High | Establish a single explicit owner; others hold a borrow (read-only) or a handle. Document ownership transfer at the boundary. |
| **Unclear borrow-vs-owned at a binding boundary** — handing app code a long-lived object that *borrows* a buffer the app can free/GC (the `*_from_buffer` / borrowed-view → long-lived-handle constructor pattern). | Critical | The long-lived object must PIN its source (dup/ref the backing buffer, or copy). A borrow that outlives its source is a UAF. |

Recommended naming discipline in comments/types: **borrow / read-only
view** (caller must not free; valid only for the call), **owned object**
(caller frees), **arena-owned** (freed by the arena's destroy, never
individually), **handle reference** (validated each use, may be stale).

### 14. Data races & thread affinity

Hull's model: a **single event-loop thread owns all core policy /
security state**; a **worker thread pool** runs `db.async` /
`compute.async` / `gpu.async` jobs on **deep-copied inputs**; Keel
returns each job's `done_fn`/`cancel_fn` to the event-loop thread.

| Issue | Severity | What to check |
|---|---|---|
| **Concurrent mutation of capability policy, dispatch/vtables, runtime config, or manifest-derived security state.** | Critical | These must be `static const`, sealed (`sh_seal_arena`), or mutated ONLY on the event loop. Never written from a worker. |
| **Shared mutable cache touched by both the loop and a worker without a lock** (WASM module cache / instance pool, GPU device buffer/texture/pipeline caches, shared-data segments). | Critical | Must be under the owning mutex (`pool_mutex`, `mod->mutex`, `dev->mutex`) across the whole critical section, including the backend submit/poll. Mutation-while-async-in-flight needs an explicit reservation (the `inflight_async` counter pattern). |
| **Event-loop-only mutation site missing a thread-affinity assertion.** | Medium | `hl_assert_on_event_loop()` (gated on `HL_THREAD_AFFINITY_CHECKS`) should guard each loop-only mutation of security state (segment load, async submit, GPU compile). |
| **Event-loop thread never marked / log not made thread-safe in a build flavor that still spawns the worker pool.** | High | Every entry point that creates the worker pool must call BOTH `hl_event_loop_mark_current()` (so the affinity asserts are live, not silently inert) AND `hl_log_make_threadsafe()` (so `log.c`'s callback list + stream write is locked) BEFORE `pool_create`. Check **every** flavor (`serve.c` AND `serve_cli.c` / CLI-flavor `HL_ENABLE_HTTP_SERVER=0`), not just the default. |
| **Unsynchronized writes to a shared output stream from multiple threads** (audit JSONL / log to stderr emitted from worker `work_fn`s). | Medium | Per-token `fwrite` from a worker can interleave byte-wise with an event-loop line, corrupting the JSONL. Hold a process lock around the whole record, or build the line in a buffer and emit with one `fwrite`. |
| **Globals mutated without a lock**, or `_Atomic`/memory-ordering misuse at a boundary. | High | Init-before-threads globals are fine if written once before `pool_create`; anything written later from >1 thread needs an atomic or lock. |

Prefer, in order: single-threaded event loop for policy state →
immutable sealed shared config → message passing (deep-copied inputs to
workers) → explicit atomic queues only at the boundary. Document thread
affinity for security-sensitive state. TSan CI (`make tsan`) must cover
the worker-pool paths for **each** build flavor that ships a pool.

### 15. Debug-build defensive checks

Under `make debug` (and TSAN/MSAN where relevant), the trusted core
should carry lightweight, compiled-out-in-release checks. When auditing
new code in a security path, expect (or recommend) these:

- arena owner / lifetime asserts (alloc-after-destroy, alloc-after-seal
  both return NULL / assert — the bump arena already does)
- ASan poisoning of freed/reset arena regions (stale read traps)
- stale-handle detection (generation mismatch → NULL)
- mutation-during-iteration asserts (e.g. a `dispatch_depth`/`iterating`
  flag that aborts on re-entrant mutation)
- thread-affinity asserts for security-sensitive state
  (`hl_assert_on_event_loop()`)

These are the runtime tripwires that turn a latent lifetime/race bug
into a loud test failure under the sanitizer matrix.

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
   - Search for lifetime / aliasing / race hazards (§§10-14):
     - every `realloc(` → does any pointer/iterator held across it dangle? (§10.1)
     - `free(` in `shared/` or `utils/` or any security path (should be ~zero; §10)
     - callback invocations inside a loop over a mutable container (§11)
     - `*_from_buffer` / borrowed-view constructors returning a long-lived
       handle without pinning the source (§13)
     - public `hl_*` getters returning a non-`const` pointer into a buffer a
       later call can `realloc`/`free` (§10.1, §13)
     - worker `work_fn` (db/wasm/gpu async) reads/writes of a shared cache,
       global, or output stream without the owning mutex/lock (§14)
     - every entry point that calls `pool_create` → does it FIRST call
       `hl_event_loop_mark_current()` AND `hl_log_make_threadsafe()`? Check
       `serve.c` AND `serve_cli.c` (CLI flavor) (§14)

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

9. **Check Lifetime, Iteration & Concurrency (§§10-15)**
   - Classify each allocation cluster by lifetime class (boot / module /
     request / scratch / third-party); flag scattered request-temporary
     `malloc`/`free` an arena would make leak-proof (§10).
   - One owner + one destroy per arena; no pointer retained past a reset
     (§10).
   - Every `realloc` re-derives held pointers; no getter returns a
     mutable internal pointer a later call can invalidate (§10.1).
   - No callback-driven loop mutates the container it walks; registries
     iterated are `static const` (§11).
   - Long-lived script-visible objects that app code can destroy while a
     reference (esp. a suspended/async continuation) lives: handle or
     fail-closed, never a bare dangling pointer (§12).
   - Sealed/policy state exposed `const`; single-owner mutation; borrowed
     buffers pinned by the long-lived objects that reference them (§13).
   - No off-event-loop mutation of policy/dispatch/manifest state; shared
     worker-touched caches under the owning mutex; **every** `pool_create`
     entry point marks the loop thread + makes log thread-safe first;
     audit/log stream writes from workers are lock-wrapped (§14).
   - For a large audit, fan out §10/§11/§14 to parallel subagents (one per
     dimension over `cap/` + `runtime/` + the worker files), then
     reconcile against known prior hardening before reporting.

10. **Generate Report**
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

### Lifetime / ownership / concurrency summary (§§10-14)
For a lifetime/race-focused audit, ALSO report these dimensions explicitly
(state "CLEAN" where there is nothing — coverage matters):
- Risky allocation / `free` sites (by lifetime class).
- `realloc` / iterator-invalidation risks found.
- Raw-pointer lifetime / borrow-outlives-source risks found.
- Shared mutable state / data-race risks found (per build flavor).
- Changes made vs. cases intentionally left unchanged.
- Larger architectural changes recommended but NOT implemented.
- Remaining residual risks.

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

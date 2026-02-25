# HULL — Development Guide

## Build

```bash
make              # build hull binary (requires Keel + SQLite)
make test         # build and run all unit tests
make debug        # debug build with ASan + UBSan (recompiles from clean)
make analyze      # Clang static analyzer (scan-build)
make cppcheck     # cppcheck static analysis
make clean        # remove all build artifacts
```

### Dependencies

- **Keel** (`../keel/`) — HTTP server library. Build it first with `make` in the keel directory.
- **SQLite** — system SQLite3 or vendored. Currently linked via `pkg-config`.
- **QuickJS** — vendored in `vendor/quickjs/` (Bellard's 2024-01-13 release).
- **Lua 5.4** — vendored in `vendor/lua/` (5.4.7, excludes `lua.c`/`luac.c`).

## Project Structure

- `include/hull/` — Public headers (`hull_cap.h`, `js_runtime.h`, `lua_runtime.h`)
- `src/` — Core source files
- `vendor/quickjs/` — Vendored QuickJS engine (compiled with `-w`, no `-Werror`)
- `vendor/lua/` — Vendored Lua 5.4 engine (compiled with `-w -DLUA_USE_POSIX`)
- `vendor/utest.h` — Sheredom's utest.h test framework
- `tests/` — Unit tests using utest.h
- `examples/` — Example applications (`hello/app.js`, `hello/app.lua`)
- `docs/` — Architecture documentation

## Architecture

### Dual-Runtime Design

Hull supports two runtimes: Lua 5.4 and QuickJS (ES2023 JavaScript). Only one is active per application — selected by entry point file extension (`.lua` or `.js`).

### Shared C Capability API (`hull_cap_*`)

All enforcement (path validation, host allowlists, SQL parameterization) lives in `hull_cap_*` functions. Both runtimes call these — neither touches SQLite, filesystem, or network directly.

| File | Purpose |
|------|---------|
| `hull_cap.h` | Shared type definitions and function declarations |
| `hull_cap_db.c` | SQLite query/exec with parameterized binding |
| `hull_cap_fs.c` | Path-validated file I/O |
| `hull_cap_crypto.c` | SHA-256, random, PBKDF2, Ed25519 verification |
| `hull_cap_time.c` | Time functions (now, date, datetime, clock) |
| `hull_cap_env.c` | Environment variable access with allowlist |

### QuickJS Integration

| File | Purpose |
|------|---------|
| `js_runtime.h` | QuickJS runtime types and lifecycle API |
| `js_runtime.c` | QuickJS VM init, sandbox, interrupt handler, module loader |
| `js_modules.c` | hull:* built-in module implementations (app, db, time, env, crypto, log) |
| `js_bindings.c` | KlRequest/KlResponse ↔ JS object bridge |

### Lua 5.4 Integration

| File | Purpose |
|------|---------|
| `lua_runtime.h` | Lua runtime types and lifecycle API |
| `lua_runtime.c` | Lua VM init, sandbox, custom allocator, module registration |
| `lua_modules.c` | hull.* built-in modules (app, db, time, env, crypto, log) |
| `lua_bindings.c` | KlRequest/KlResponse ↔ Lua table/userdata bridge |

### Request Flow

```
Browser → Keel HTTP → Route Match → hull_{js,lua}_dispatch() → Handler → KlResponse
                                          ↓
                                   hull_cap_* API (shared C)
                                          ↓
                                   SQLite / FS / Crypto
```

## Key Types

| Type | Header | Purpose |
|------|--------|---------|
| `HullValue` | `hull_cap.h` | Runtime-agnostic value (nil, int, double, text, blob, bool) |
| `HullColumn` | `hull_cap.h` | Named column + value (from query results) |
| `HullRowCallback` | `hull_cap.h` | Per-row callback for hull_cap_db_query() |
| `HullFsConfig` | `hull_cap.h` | Filesystem config (base_dir for path validation) |
| `HullEnvConfig` | `hull_cap.h` | Env config (allowlist of permitted variable names) |
| `HullJS` | `js_runtime.h` | QuickJS runtime context (VM, capabilities, config) |
| `HullJSConfig` | `js_runtime.h` | QuickJS config (heap, stack, instruction limits) |
| `HullLua` | `lua_runtime.h` | Lua 5.4 runtime context (VM, capabilities, config) |
| `HullLuaConfig` | `lua_runtime.h` | Lua config (heap limit) |

## Git

- When committing, do NOT add any Co-Authored-By trailers.
- Do NOT add "Generated with Claude Code" or similar attribution to PRs.

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2`
- `-fstack-protector-strong` for buffer overflow detection
- Vendor code (QuickJS, Lua) compiled with `-w` (relaxed warnings)
- Integer overflow guards: check against `SIZE_MAX/2` before arithmetic
- Error handling: return `-1` on failure, `0` on success (or positive value)
- Resource cleanup: every `_init` has a corresponding `_free`
- All SQLite access through `hull_cap_db_*` — never call sqlite3 directly from bindings

## Testing

Tests use Sheredom's utest.h. Each `tests/test_*.c` is a standalone executable.

```bash
make test                           # run all tests
make DEBUG=1 test                   # run under ASan + UBSan
./build/test_hull_cap_db            # run a single test suite
```

### Test Suites

| Suite | Tests | What it covers |
|-------|-------|----------------|
| `test_hull_cap_db` | 10 | SQLite query, exec, params, null, error handling |
| `test_hull_cap_time` | 8 | Timestamps, date formatting, buffer bounds |
| `test_hull_cap_env` | 7 | Allowlist enforcement, null safety |
| `test_hull_cap_crypto` | 11 | SHA-256, random, PBKDF2, null safety |
| `test_hull_cap_fs` | 14 | Path validation, read/write, traversal rejection |
| `test_js_runtime` | 13 | QuickJS init, eval, sandbox, modules, GC, limits |
| `test_lua_runtime` | 16 | Lua init, eval, sandbox, modules, GC, double-free |

**Total: 79 tests**

## QuickJS Sandbox

The JS runtime sandbox removes dangerous capabilities:

1. `eval()` global removed (C-level `JS_Eval` still works for host code)
2. QuickJS `std` module NOT loaded (provides `os.*`, file I/O)
3. QuickJS `os` module NOT loaded
4. Memory limit enforced via `JS_SetMemoryLimit()`
5. Instruction count interrupt handler for gas metering
6. Only `hull:*` modules available — no filesystem, network, or process access from JS

## Lua Sandbox

The Lua runtime sandbox removes dangerous capabilities:

1. `io` library NOT loaded — no file I/O from Lua
2. `os` library NOT loaded — no OS access from Lua
3. `loadfile`, `dofile`, `load` globals removed — no dynamic code loading
4. Memory limit enforced via custom allocator with tracking
5. Only safe libs loaded: base, table, string, math, utf8, coroutine
6. `hull.*` modules registered as globals (app, db, time, env, crypto, log)

## Adding a New hull Module

### JavaScript (hull:*)
1. Add capability functions to `hull_cap_*.c` (or new file)
2. Declare in `hull_cap.h`
3. Add JS bindings in `js_modules.c`:
   - Static init function: `js_<name>_module_init()`
   - Public init: `hull_js_init_<name>_module()`
4. Register in `hull_js_register_modules()` at bottom of `js_modules.c`

### Lua (hull.*)
1. Same capability layer as JS (shared `hull_cap_*`)
2. Add Lua bindings in `lua_modules.c`:
   - `luaL_Reg` array with function table
   - `luaopen_hull_<name>()` opener function
3. Register in `hull_lua_register_modules()` at bottom of `lua_modules.c`

### Both
5. Add tests in `tests/test_hull_cap_<name>.c`

## Debugging

```bash
make debug          # clean + rebuild with -fsanitize=address,undefined -g -O0
make DEBUG=1 test   # run tests under sanitizers
```

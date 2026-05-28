# CLI / Compute-Only Mode. Design

`HL_ENABLE_HTTP=0` builds. `app.main(fn)` entry point. Sealed Hull as a CLI
runtime for compute pipelines, ETL, image processing, build tooling, and
anything else that doesn't want a 2 MB HTTP stack in the binary.

This document is the design plan. Status: not yet implemented.

## Goals

1. **Hull-as-CLI**: produce a hull binary that builds and runs apps with
   the full module surface (`db`, `crypto`, `compute`, `gpu`, `image`, `fs`,
   `time`, `validate`, `csv`, `template`, …) but no Keel, no routes, no
   middleware, no WebSocket / SSE / static serving.
2. **Identical-feeling API**: `app.manifest({...})` works the same. The
   only difference is the entry point shape: `app.main(fn)` instead of
   `app.get/post/…`. Same sandbox, same module declaration, same migrations,
   same signature, same `hull build`.
3. **One binary, both modes**: the *default* hull binary (`HL_ENABLE_HTTP=1`)
   can run BOTH server and CLI apps. CLI apps written today work tomorrow
   on a stripped-down `HL_ENABLE_HTTP=0` distribution. The build flag only
   changes what gets compiled in, not the app-facing API.
4. **Reflected in tooling**: `hull doctor` reports whether HTTP is linked
   in. `hull agent manifest` reports the app's mode. The module resolver
   refuses `hull/server@1` (and friends) on builds without HTTP support,
   with the same "did you mean?" / build-cap error grammar that DB/WASM/GPU
   already use.

## Non-goals

- **Hybrid mode** (HTTP server with admin CLI commands inside the same
  process). Flask / Django / Rails all keep these as separate primitives;
  if it ever surfaces as a real need, the right answer is a future
  `app.command(name, fn)` orthogonal to `app.main` and `app.get`.
- **Pluggable HTTP backend** (`HlNetBackend` vtable). Different problem,
  much bigger interface surface, no concrete second backend in sight.
- **Splitting HTTP client from server** (`HL_ENABLE_HTTP_CLIENT=1` with
  `HL_ENABLE_HTTP_SERVER=0`). Doubles the compile matrix for one
  edge-case use. CLI apps that need outbound HTTP can stay on a default
  hull build.

## Entry-point API

### Lua

```lua
app.manifest({ modules = { "hull/fs@1" } })

app.main(function(ctx)
    -- ctx.args   : array (1-indexed) of argv, excluding the binary name
    -- ctx.env    : table of env vars the manifest's `env` allowlist admits
    -- ctx.stdin  : reader . :read("*l" | "*a" | bytes_n) → string|nil, :close()
    -- ctx.stdout : writer . :write(str), :flush()
    -- ctx.stderr : writer . :write(str), :flush()

    local name = ctx.args[1] or "world"
    ctx.stdout:write("hello " .. name .. "\n")
    return 0
end)
```

### JavaScript

```js
app.manifest({ modules: ["hull/fs@1"] });

app.main(async (ctx) => {
    // ctx.args   : Array<string>
    // ctx.env    : Object<string, string>
    // ctx.stdin  : { readLine(), readAll(), read(n), close() }. All return Promise<string|null>
    // ctx.stdout : { write(str), flush() }
    // ctx.stderr : { write(str), flush() }

    const name = ctx.args[0] ?? "world";
    ctx.stdout.write(`hello ${name}\n`);
    return 0;
});
```

`ctx` is a flat bag. No methods beyond what's listed. Apps wanting richer
IO (random-access file read, mmap, fifo) use `require("hull.fs")` exactly as
HTTP apps do today.

### Return value

| Return | Exit code |
|--------|-----------|
| `nil` / no `return` | 0 |
| integer ∈ `[0, 255]` | that value |
| integer outside `[0, 255]` | `value & 0xff` (POSIX clamp) |
| anything else | `print(value)` to stderr, exit 1 |
| uncaught error / throw | print message + stack to stderr, exit 1 |

## Lifecycle

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. Process starts; argv parsed                                  │
│ 2. Init runtime (Lua or JS); kernel sandbox phase 1             │
│ 3. Load app.lua / app.js  ─→ top-level runs once:               │
│      • app.manifest({...})                                      │
│      • app.main(fn)                                             │
│      • (any other top-level state setup)                        │
│ 4. Extract manifest; run module resolver; sandbox phase 2       │
│ 5. Run migrations (HL_ENABLE_DB + ./migrations/ + not --no-migrate) │
│ 6. Call app.main(ctx) on the event-loop thread                  │
│      • Full capability surface available                        │
│      • compute.async / gpu.async / http.fetch all yield via     │
│        coroutine (Lua) / Promise (JS). Main awaits naturally   │
│ 7. main returns (or throws) → cleanup                           │
│ 8. Drain mmap / WASM / GPU caches; scrub key material; close DB │
│ 9. Process exits with main's return code                        │
└─────────────────────────────────────────────────────────────────┘
```

### Conflict & error cases

- **Both `app.main` and any route handler registered** → startup error:
  `app registered both app.main and HTTP routes; choose one`. Caught
  after app load, before either fires. There is no silent precedence.
- **Neither `app.main` nor routes registered** → startup error:
  `app registered neither app.main nor any routes`.
- **`app.get/post/…` called on an `HL_ENABLE_HTTP=0` build** → the binding
  doesn't exist (Lua `nil`, JS `undefined`); calling it errors at load
  time before the runtime even reaches step 4.
- **`app.every` / `app.daily` in CLI mode** → registration-time error:
  `timers require server mode (event loop survival)`. Async work tied to
  `app.main`'s own coroutine still works because it terminates with main.

### Async semantics inside `app.main`

Identical to async inside HTTP request handlers today. `main` runs on the
event-loop thread. When it hits `compute.async.call`, `gpu.async.dispatch`,
`http.fetch`, `db.async.query`, etc., it yields; the work runs on the
thread pool; the result resumes main. The process exits when main returns,
not when "the event loop empties" (no Node.js-style "why doesn't my CLI
exit?" footgun. Main's return is authoritative).

## Build flag. `HL_ENABLE_HTTP`

Default `1`. Same pattern as `HL_ENABLE_DB`, `HL_ENABLE_WASM`, etc.

`HL_ENABLE_HTTP=0` drops:

| Removed | Why |
|---------|-----|
| `vendor/keel/*` (entire submodule from link) | The HTTP server itself |
| `src/hull/cap/http.c` | Outbound HTTP client (uses Keel) |
| `src/hull/cap/ws.c` | WebSocket (server + client) |
| `src/hull/cap/body.c` | Request body handling |
| `src/hull/static.c` | Static file middleware |
| `src/hull/runtime/{lua,js}/mod_app.c` route bindings | `app.get/post/use/sse/ws/every/daily` |
| `src/hull/runtime/{lua,js}/mod_http.c` | Glue for `hull.http` |
| `src/hull/runtime/{lua,js}/mod_ws.c` | Glue for `hull.ws` |
| `src/hull/runtime/{lua,js}/mod_server.c` | Server-mode metadata |
| `src/hull/runtime/{lua,js}/mod_sse.c` | SSE bindings |
| `stdlib/{lua,js}/hull/middleware/*` | All middleware (cors, ratelimit, csrf, auth, session, logger, transaction, idempotency, outbox, inbox, rbac, health, etag) |
| `src/hull/commands/dev.c` (server-loop only) | `hull dev` becomes unavailable; suggest `hull run` |

Kept:
- Runtimes (Lua + QuickJS), sandbox, manifest, module resolver, signature
- All non-HTTP capabilities: `db`, `crypto`, `time`, `env`, `fs`, `compute`, `gpu`, `image`
- All non-HTTP stdlib modules: `validate`, `form`, `cookie`, `jwt`, `csv`, `search`, `i18n`, `template`
- All build/sign/verify/check commands
- `hull test` (only invokes `app.main` for CLI apps; existing HTTP-mode tests can't run on this build because the app won't load)
- `hull run`, `hull build`, `hull check`, `hull doctor`, `hull modules`, `hull agent`, `hull mcp`, `hull migrate`

Estimated binary-size win: ~1–1.5 MB on arm64 Darwin (Keel itself is
~500 KB; middleware Lua/JS source bytes + dispatch machinery + bindings
account for the rest). Concrete number measured during phase 3.

## Module registry changes

Add a new capability bit:

```c
#define HL_MOD_CAP_HTTP   (1u << 6)   /* requires HL_ENABLE_HTTP at build */
```

Annotate the registry entries that need it:

| Module | Adds `HL_MOD_CAP_HTTP`? |
|--------|-------------------------|
| `hull/server` | yes |
| `hull/http` | yes |
| `hull/ws` | yes |
| `hull/middleware/*` (all 13) | yes |
| `hull/email` | yes (uses outbound HTTP/SMTP via Keel) |

Resolver behavior is the same as for `HL_MOD_CAP_DB` / `WASM` / `GPU`.
build-time-disabled modules produce:

```
module 'hull/http@1' requires HL_ENABLE_HTTP, but it is disabled in this hull build
```

On `HL_ENABLE_HTTP=1` builds these modules resolve normally. The bit only
fires on `HL_ENABLE_HTTP=0` builds where someone tries to use HTTP.

## Dispatch

In `src/hull/main.c`:

```c
int has_main   = hl_runtime_has_main(rt);
int has_routes = hl_runtime_has_routes(rt);  /* always 0 if HL_ENABLE_HTTP=0 */

if (has_main && has_routes) {
    fprintf(stderr, "app registered both app.main and HTTP routes; choose one\n");
    return 1;
}
if (has_main)   return run_main_mode(rt, argc, argv);
#ifdef HL_ENABLE_HTTP
if (has_routes) return run_server_mode(rt);
#endif
fprintf(stderr, "app registered neither app.main nor any routes\n");
return 1;
```

`run_main_mode`:
1. Build the `ctx` table/object with `args`, `env` (manifest-filtered),
   and the three stdio handles
2. Invoke `app.main(ctx)` via the runtime's vtable
3. If async, drive the event loop until main's coroutine completes
4. Coerce return value → exit code
5. Tear down runtime + caches
6. Return exit code

## New & changed commands

| Command | HTTP=1 build | HTTP=0 build |
|---------|--------------|--------------|
| `hull run [app]` | **new**. Load + invoke main, exit with rc. Works in both modes. | same |
| `hull dev [app]` | unchanged | absent (with hint: "use `hull run` for CLI apps") |
| `hull test [app]` | unchanged (gates modules per recent work) | same; only loads CLI apps |
| `hull build [app]` | detects mode from app, errors if mode mismatches build caps | refuses to build a server app |
| `hull check [app]` | same | same (already module-aware) |
| `hull doctor` | reports HTTP=yes | reports HTTP=no |
| `hull agent manifest` | adds `"mode": "server"\|"cli"\|"unknown"` | adds `"mode": "cli"\|"unknown"` |
| `hull modules available` | shows all modules | shows all modules, with `requires_http` flag on the affected ones |
| `hull new --cli` | **new** scaffold flag. Generates a CLI app skeleton | same; default in HTTP=0 builds |
| `hull init` | detects mode from existing app, scaffolds accordingly | same |

## `hull build` interaction

The build trampoline already extracts the manifest and runs the resolver
during build. After that we know the app's mode (CLI vs server) from
whether `app.main` was called.

- **CLI app + HTTP=1 hull** → builds; binary exits after main runs
- **CLI app + HTTP=0 hull** → builds; binary exits after main runs;
  Keel symbols stripped by the linker
- **Server app + HTTP=1 hull** → today's behavior
- **Server app + HTTP=0 hull** → build error:
  `this hull build was compiled without HTTP support; app cannot register routes`

A built CLI binary IS a normal POSIX CLI tool: positional args, exit
codes, stdin/stdout/stderr, kill via signal, no special invocation. It
can be put in `/usr/local/bin`, called from `Makefile`s, scheduled by
cron, etc.

## `hull doctor`

New row in the subsystems section (alongside DB/WASM/GPU/TCC):

```
HTTP server (Keel):    yes
```

`--json` adds `"http": true/false` and `"http_client": true/false`.

## `hull agent manifest`

Already returns the resolved manifest as JSON. Adds:

```json
{
  "mode": "cli",         // "cli" | "server" | "unknown" (neither registered)
  "main_registered": true,
  "routes_registered": 0,
  "declared": true,
  "modules": [...],
  ...
}
```

For CLI apps, `routes_registered` is always 0 because route registration
either never happened (CLI app) or errored at load (mode conflict).

## Sandbox interaction

Two-phase sandbox unchanged. CLI apps don't open listening sockets, so on
`HL_ENABLE_HTTP=0` builds OR when `hull/http`/`hull/ws` aren't declared:

- **Linux pledge**: drop `inet` from the promise set (no socket syscalls
  needed). Keeps `stdio rpath wpath cpath flock dns` as the baseline.
- **OpenBSD/Cosmo**: same.
- **macOS Seatbelt**: omit the network-allow clauses from the SBPL
  profile.

Smaller attack surface, same machinery. The decision is made at
manifest-resolution time (step 4 of the lifecycle), so dropping inet on
sandbox phase 2 (step 4 → step 5).

## Test harness for CLI apps

Today's `hull test` runs `tests/test_*.lua|.js` files against an
in-memory dispatched HTTP server. For CLI apps the harness needs to
synthesize `argv` / `stdin` / `env` and run `main`:

```lua
test("greets the name passed via argv", function()
    local result = test.run_main({
        args = { "alice" },
        env  = { LANG = "en" },
        stdin = "",
    })
    test.eq(result.exit_code, 0)
    test.eq(result.stdout, "hello alice\n")
    test.eq(result.stderr, "")
end)
```

`test.run_main(opts)` returns `{exit_code, stdout, stderr}`. The
existing `test.get/post/etc` helpers continue to exist but error if the
loaded app is CLI mode (`app.main` registered, no routes).

## Manifest

No new top-level field needed. Mode is derived from whether `app.main`
was called. The manifest's `modules = {...}` already declares the
capability surface; that's the right grain of declaration.

The resolver records mode as a derived attribute in
`HlManifest.derived_mode` after manifest extraction; `hull agent
manifest` surfaces it; `hull build` uses it for the
build-capability cross-check.

## Implementation phases

### Phase 1. `app.main` registration + lifecycle (default HTTP=1 build)
- Add `app.main(fn)` binding in `mod_app.c` for both runtimes
- `HlRuntime.has_main` flag + main-runner vtable entry
- `ctx` object construction (args, env, stdio handles)
- `run_main_mode()` in main.c; mode detection + conflict error
- Coerce return value → exit code
- Unit tests for both runtimes
- Outcome: `hull` binary can load a CLI app, run main, exit. No build
  changes required.

### Phase 2. `hull run` command + scaffolding
- `src/hull/commands/run.c`. Load + invoke main, exit with rc
- `hull new --cli`. Scaffold a minimal CLI app skeleton
- `hull init`. Detect CLI vs server from existing `app.{lua,js}`
- Templates in `stdlib/cli/lua/hull/new.lua` and `init.lua`
- E2E test against a few sample CLI apps (line counter, CSV reformatter)
- Outcome: full dev workflow for CLI apps on a standard hull build.

### Phase 3. `HL_ENABLE_HTTP=0` build flag

Split into 3a (foundation, no behavior change) and 3b (the actual no-HTTP
build, multi-session refactor).

**Phase 3a. Foundation: shipped**
- `HL_MOD_CAP_HTTP` registry bit; `hull/http`, `hull/ws`, `hull/server`,
  `hull/smtp`, and every `hull/middleware/*` entry now require it.
- Resolver: `build_provided_caps()` includes `HL_MOD_CAP_HTTP` when
  `HL_ENABLE_HTTP` is defined; build-cap mask grows to include HTTP;
  `cap_label` gains the matching error string.
- Makefile: `HL_ENABLE_HTTP ?= 1` flag wired through CFLAGS.
- `hull doctor`: new "HL_ENABLE_HTTP" row + `"http"` key in
  `--json`'s `subsystems` object.
- `hull_serve` + all its helpers split from `main.c` into a new
  `src/hull/serve.c`. `main.c` is now ~55 lines: just `hull_main` (the
  subcommand dispatcher / version / run-alias). A future
  `HL_ENABLE_HTTP=0` build can swap `serve.c` for a Keel-free
  counterpart without touching the dispatcher.

**Phase 3b. Actual `HL_ENABLE_HTTP=0` build: not yet shipped**

Scope is genuinely a multi-day refactor. 58 source files touch Keel
symbols (`KlServer` / `KlRequest` / `KlResponse` / `KlConn` /
`KlAsyncOp` / `kl_*`). The work breaks into ~7 independent slices,
each landable as its own commit:

1. **Leaf cap files**: wrap `cap/ws.c`, `cap/body.c`, `cap/smtp.c`,
   `cap/http.c`, `cap/http_async.c`, `static.c` in
   `#ifdef HL_ENABLE_HTTP`. Headers grow matching guards so callers
   get clean preprocessor errors instead of mysterious link failures.
2. **Runtime route/middleware bindings**: `mod_app.c` keeps only
   `manifest` + `main` exports when `HL_ENABLE_HTTP=0`; the route /
   middleware / ws / sse / timer entry points get `#ifdef`'d out.
   `mod_http.c`, `mod_ws.c`, `mod_server.c`, `mod_sse.c` excluded
   from the Makefile entirely.
3. **Runtime support files**: `routes.c`, `dispatch.c`, `sse.c`,
   `ws.c`, `timers.c` excluded when `HL_ENABLE_HTTP=0`. `async.c`
   needs care. It's used by both HTTP handlers and `app.main`'s
   coroutine driver. Either trim it to the detached/timer path only,
   or split into `async_core.c` (kept) + `async_http.c` (dropped).
4. **Stdlib middleware**: skip embedding `stdlib/{lua,js}/hull/middleware/*`
   in the embedded-modules table when `HL_ENABLE_HTTP=0`. The
   registry already refuses these modules via `HL_MOD_CAP_HTTP` so
   apps that declare them fail at resolve-time; skipping the embedding
   trims binary size.
5. **`serve.c` CLI variant**: provide an `HL_ENABLE_HTTP=0` build that
   either `#ifdef`s out `serve.c` and replaces it with a tiny
   `serve_cli.c` (load app → invoke `app.main` → exit) OR keeps
   `serve.c` but ifdef's out the Keel-only branches. The latter is
   probably simpler. Only the route-wiring and event-loop sections
   need guards.
6. **Commands**: `hull dev` excluded when `HL_ENABLE_HTTP=0` (it forks
   a serve subprocess; no point without a server). `hull agent` /
   `hull mcp` warn that some subcommands are no-ops on this build.
   The Makefile filter-out approach used for `HL_ENABLE_DB=0`
   migrations applies cleanly here.
7. **Keel itself**: dropped from the link via Makefile filter-out;
   `vendor/keel/libkeel.a` and the entire `wamr` rule remain
   unaffected. Need to add `ifeq ($(HL_ENABLE_HTTP),0)` blocks
   around the `KEEL_LIB` variable + linker include.

After 3b: `hull doctor` reports HTTP=no, `hull dev` is absent, binary
shrinks by ~1.5MB (Keel + middleware source + bindings). The CLI app
written for an HTTP=1 build runs unchanged on the HTTP=0 binary.

**Phase 3c. Sandbox narrowing & cosmo support**
- Drop `inet` pledge promise + macOS network SBPL clauses when no HTTP
  module is declared (or when `HL_ENABLE_HTTP=0`).
- `make platform-cosmo HL_ENABLE_HTTP=0`. Verify multi-arch builds.
- Binary-size verification (target ≥ 1 MB reduction).
- E2E: build + test the same CLI apps on a no-HTTP hull.

### Phase 4. Polish & documentation
- `hull build` mode validation against build's HTTP cap
- `hull agent manifest` `mode` field
- Sample CLI examples in `examples/cli/` (CSV transformer, image batch,
  model evaluator, signing tool)
- README / CLAUDE / AGENTS sections for CLI lifecycle
- `docs/security.md` notes on the reduced sandbox / smaller attack surface
- Update Module Declaration table to mark which modules require HTTP
- Final e2e sweep on both build flavors

## Estimated effort

| Phase | Effort | Touches |
|-------|--------|---------|
| 1 | ~2 days | runtime bindings, main.c, runtime vtable, unit tests |
| 2 | ~1 day | one new command, two templates, e2e |
| 3 | ~2 days | Makefile, conditional compilation across ~30 files, doctor, sandbox |
| 4 | ~1 day | docs, examples, hull build wiring, final test sweep |

Total: ~1 week of focused work.

## Risk callouts

- **Tool-VM compatibility**. `hull build`, `hull manifest`, `hull deploy`
  load apps in a stub VM without real bindings to do manifest extraction.
  This already handles the case "app top-level calls `require('hull.db')`
  with no DB handle" via the chain-friendly nop stubs introduced in the
  package system work. `app.main(fn)` registration is a no-op stub
  registration (callback never fires), so the tool VM stays unaffected.
  Verified by re-running `hull manifest`, `hull build`, `hull check` on
  CLI apps during phase 1.

- **Build-time mode mismatch**. A user could build a server app with
  `make HL_ENABLE_HTTP=0`. Catch at `hull build` (resolver rejects
  `hull/server@1`) with a clear error pointing at `make HL_ENABLE_HTTP=1`
  or removing the HTTP modules.

- **macOS sandbox SBPL generation**. The dynamic SBPL build in
  `sandbox.c` adds network clauses unconditionally today. Phase 3 must
  make those conditional on "HTTP modules declared OR HL_ENABLE_HTTP=0
  build". Otherwise CLI apps on HTTP-enabled hulls would still get an
  oversized profile. Existing tests in `e2e_sandbox.sh` catch regressions.

- **`hull test` for CLI apps**. Existing tests assume HTTP. The
  `test.run_main()` helper is new code; gate it behind "loaded app is CLI
  mode" so server-mode tests stay clean. Decided in phase 2 design pass.

- **Cosmo build**. `make platform-cosmo HL_ENABLE_HTTP=0` must produce
  both arch archives without Keel. The conditional compilation in phase 3
  needs to be Cosmo-aware; the platform-cosmo target wraps `make platform`
  so as long as the platform target honors the flag, the multi-arch case
  follows.

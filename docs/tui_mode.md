# TUI Mode — Design

`hull/tui@1` module. One canonical entry point: `tui.run({...})`. Built
on top of CLI mode (`app.main`). Primary driver: dogfood — `hull dev`,
`hull doctor`, `hull agent` get real interactive UIs by writing Lua
tool modules against the public `hull.tui` API. By-product: Hull apps
can ship terminal UIs (file pickers, dashboards, log tailers, REPLs)
using the same sandboxed runtime they already use for backend code.

This document is the design plan. Status: not yet implemented.

## Goals

1. **Tooling-first.** The bar for shipping is "`hull doctor --tui`,
   `hull dev` request log, and `hull agent context --interactive` all
   feel native." Those tools are Lua modules invoked from C (same
   pattern as `hull init` today), so they use the same script API apps
   use — no private bindings, no second surface to keep in sync.
2. **One canonical API.** `tui.run({ draw, on_event, tick_ms })` is
   *the* API. Raw primitives (`tui.poll`, `tui.print`, `tui.move`) are
   exposed for the rare escape-hatch case but are not what docs,
   scaffolding, or examples lead with. No widget framework in v1.
3. **`hull dev` reload is a non-issue.** `hull dev` already runs the
   served app as a fork+exec'd child process — reload kills the child
   and respawns it. The dev controller's runtime (and any TUI it
   owns) lives in the parent process, untouched. No special
   persistence engineering required.
4. **One binary, both modes.** TUI is an addition to CLI mode, not a new
   build flavor. Default `HL_ENABLE_TUI=1`; `=0` drops it for size-
   constrained builds. Works on `HL_ENABLE_HTTP=0` (CLI-only) hulls
   identically to default hulls.
5. **Cosmo-supported.** POSIX termios + ANSI sequences only — no
   platform libraries. Full APE compatibility.
6. **Sandbox-compatible.** TUI needs only stdin/stdout/termios; nothing
   network, nothing filesystem-write by default. The manifest's `tui`
   capability is its own narrow bit, not a backdoor.

## Non-goals

- **Widget library.** No `Button`, `Table`, `Tree`, `Modal`, etc. in v1.
  The primitives + a thin set of helpers (`tui.frame`, `tui.list`,
  `tui.input`) are enough for the dogfood targets. A widget library
  could come later as a stdlib module (`hull/tui/widgets@1`) once we
  see what apps actually want.
- **GUI / windowing.** Pixels, mice with sub-character precision, and
  resizable windows are out of scope. (See "Cosmo + GUI tradeoffs" in
  the parent decision notes.) Mouse *click* events through SGR-mouse
  ANSI sequences are in scope as an opt-in.
- **Coexistence with HTTP server.** TUI requires `app.main`. Server
  apps (`app.get/post/use/...`) cannot also call `tui.run` — same
  rationale as CLI mode itself. If a server app wants a console, it
  uses `hull dev`'s interactive console (which is a TUI client of the
  server, not in-process).
- **Dual API surface (C cap + script).** Hull's own commands
  (`hull dev`, `hull doctor`, `hull agent`) go through the same Lua
  tool-module pattern `hull init` already uses, calling the script
  API. Keeping one surface means the API gets stress-tested by our
  own usage; any pain we feel is pain our users would feel.

## Entry-point API

TUI lives strictly inside `app.main`. The shape:

### Lua

```lua
app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

local tui = require("hull.tui")

app.main(function(ctx)
    local state = { cursor = 1, items = {"alpha", "beta", "gamma"} }

    tui.run({
        draw = function(t)
            t:clear()
            t:print(1, 1, "Pick an item (↑/↓, enter):")
            for i, item in ipairs(state.items) do
                local marker = (i == state.cursor) and "→ " or "  "
                t:print(3, i + 2, marker .. item)
            end
            t:print(1, t.rows, "press q to quit")
        end,
        on_event = function(ev)
            if ev.kind == "key" then
                if ev.key == "up"    then state.cursor = math.max(1, state.cursor - 1)
                elseif ev.key == "down"  then state.cursor = math.min(#state.items, state.cursor + 1)
                elseif ev.key == "enter" then return "done"
                elseif ev.key == "q" or ev.key == "ctrl+c" then return "abort"
                end
            elseif ev.kind == "resize" then
                -- t.cols, t.rows updated automatically before next draw
            end
        end,
        tick_ms = 250,  -- optional: redraw at most 4x/sec when idle
    })

    ctx.stdout:write("you picked: " .. state.items[state.cursor] .. "\n")
    return 0
end)
```

### JavaScript

```js
app.manifest({
    tui: true,
    modules: ["hull/tui@1"],
});

import { tui } from "hull:tui";

app.main(async (ctx) => {
    const state = { cursor: 0, items: ["alpha", "beta", "gamma"] };

    await tui.run({
        draw(t) {
            t.clear();
            t.print(1, 1, "Pick an item (↑/↓, enter):");
            state.items.forEach((item, i) => {
                const marker = i === state.cursor ? "→ " : "  ";
                t.print(3, i + 3, marker + item);
            });
            t.print(1, t.rows, "press q to quit");
        },
        onEvent(ev) {
            if (ev.kind === "key") {
                if (ev.key === "up")        state.cursor = Math.max(0, state.cursor - 1);
                else if (ev.key === "down") state.cursor = Math.min(state.items.length - 1, state.cursor + 1);
                else if (ev.key === "enter") return "done";
                else if (ev.key === "q" || ev.key === "ctrl+c") return "abort";
            }
        },
        tickMs: 250,
    });

    ctx.stdout.write(`you picked: ${state.items[state.cursor]}\n`);
    return 0;
});
```

### Raw primitives (escape hatch)

`tui.run` covers ~95% of real use. The few cases that need to drive
the loop themselves — embedding a TUI inside a coroutine that wraps
something `tui.run` can't express — can fall back to raw primitives:

```lua
local tui = require("hull.tui")

tui.enter()                                -- idempotent; safe even if already entered
while true do
    tui.clear(); tui.print(1, 1, "loading…"); tui.flush()
    local ev = tui.poll(1000)              -- ms; yields to event loop, returns nil on timeout
    if ev and ev.kind == "key" and ev.key == "q" then break end
end
tui.leave()                                -- restore (also runs at exit via atexit)
```

Lifecycle responsibility is the same either way: `tui.enter` /
`tui.leave` are idempotent, the cap layer's `atexit` and signal
handlers guarantee restore even on panic. The reason `tui.run` is the
canonical path is that it gets resize, tick scheduling, and signal
interactions right by default — re-implementing all of that in script
is unnecessary work.

If you find yourself reaching for the raw API more than rarely, that's
a signal `tui.run` is missing something. File an issue.

### Conflict & error cases

- **`tui.run` / `tui.enter` called on a server app** → registration-
  time error: `tui requires CLI mode (app.main); cannot mix with HTTP
  routes`. Same grammar as the existing `app.every` / `app.daily` check.
- **`tui.*` called without `tui = true` in manifest** → capability
  error at first call: `tui capability not granted in manifest; add
  tui = true`. Same shape as `gpu` today.
- **`tui.run` called when not attached to a tty** (stdin or stdout
  redirected) → returns immediately with error `{ kind = "no_tty",
  reason = "stdin"|"stdout" }`. App can fall back to a plain CLI
  path. Suppress with `tui.run({ allow_no_tty = true })` for
  testing / scripted dev.
- **Nested `tui.run`** → error `tui.run already active`. Recursive UI
  doesn't compose; the inner run would steal stdin from the outer.
- **`HL_ENABLE_TUI=0` build** → bindings absent (Lua `nil`, JS
  `undefined`); module resolver rejects `hull/tui@1` with the standard
  build-cap error before `app.main` runs.

## Lifecycle

```
┌─────────────────────────────────────────────────────────────────┐
│ 1. Process starts; argv parsed                                  │
│ 2. Init runtime; kernel sandbox phase 1                         │
│ 3. Load app.{lua,js}  ─→ top-level runs once:                   │
│      • app.manifest({ tui = true, modules = ["hull/tui@1"] })   │
│      • app.main(fn)                                             │
│ 4. Extract manifest; resolver validates tui cap; sandbox phase 2│
│ 5. Run migrations (if HL_ENABLE_DB + ./migrations/ exist)       │
│ 6. Call app.main(ctx) on the event-loop thread                  │
│      • Inside main: tui.run({...})  ← canonical                 │
│      • tui.run acquires cap; on exit, releases (refcount drops) │
│      • tui.poll yields to event loop like any async op          │
│ 7. main returns → cleanup (drain caches, scrub keys, close DB)  │
│ 8. atexit fires hl_cap_tui_force_leave():                       │
│      • Restore termios, leave alt screen, show cursor           │
│ 9. Process exits with main's return code                        │
└─────────────────────────────────────────────────────────────────┘
```

The terminal restore is owned by the cap layer's `atexit` handler, not
the script. This means panics, uncaught throws, and signal-induced
termination all leave the user's terminal in a sane state without the
script having to wrap everything in error handlers.

### Async semantics inside `tui.run`

`tui.run` itself is sync from the script's perspective in Lua (it
blocks until `on_event` returns a truthy value or `draw` throws), and
returns a Promise in JS that resolves with the run-loop's exit token.
Inside `draw` and `on_event`, async ops are fine but cooperate with the
loop's frame budget: an `on_event` callback that does `http.fetch`
delays the next frame until the fetch completes. For background work
that shouldn't block the UI, kick off async ops with no `await` and
let them resolve into state updates that the next `draw` reads.

```lua
tui.run({
    draw = function(t) t:print(1, 1, state.message) end,
    on_event = function(ev)
        if ev.kind == "key" and ev.key == "r" then
            -- fire-and-forget; state.message updates when fetch resolves
            tui.async(function()
                local r = http.fetch("https://api.example.com/status")
                state.message = r.body
            end)
        end
    end,
    tick_ms = 100,   -- redraw at 10Hz so state updates show up promptly
})
```

`tui.async(fn)` launches `fn` as a detached coroutine on the event
loop. Mechanically it's the same `HlAsyncCtx.detached` machinery that
timers already use internally; this is just the first script-facing
binding for it. Lives under `hull.tui` (not `hull.*`) because TUI is
its only caller today — see the stdlib table note.

### Run-loop control flow

`on_event` returns control:

| Return value | Meaning |
|--------------|---------|
| `nil` / no return | continue loop |
| `"done"` | exit loop, `tui.run` returns `"done"` |
| `"abort"` | exit loop, `tui.run` returns `"abort"` |
| any other string | exit loop, return that string verbatim |
| an error thrown | `tui.leave()` runs, error propagates |

By convention: `"done"` = normal completion (user picked something),
`"abort"` = user bailed (ctrl-c, esc, q). Apps that need richer exit
states use arbitrary string tokens.

## Process model

`hull dev` already runs the served app as a fork+exec'd child
(`src/hull/commands/dev.c`: monitor → kill → respawn on file change).
The TUI lives in whichever process needs it; reload happens in a
different process and is invisible to the TUI.

Two cases, both with one runtime per process:

**Standalone TUI app** (`hull run app.lua`):

```
┌─ hull run process ─────────────────────────────┐
│ Runtime ─ owns HlTuiCtx via tui.run            │
│   • tui.run acquires on entry                  │
│   • Lua/JS userdata wrapper calls release      │
│     on GC or scope exit                        │
└────────────────────────────────────────────────┘
```

**Hull dev with a served server app** (`hull dev app.lua`):

```
┌─ hull dev parent process ─────────────────┐    ┌─ served-app child ──┐
│ Lua tool runtime (dev_tui.lua)            │    │ Server app          │
│   • owns HlTuiCtx                         │    │ • no TUI access     │
│   • renders request log from child stderr │◄───│ • normal logger     │
│   • file watch + kill/respawn child       │    │   middleware        │
└───────────────────────────────────────────┘    └─────────────────────┘
                                                  ↑
                                                  │ killed + respawned
                                                  │ on edit; parent
                                                  │ runtime untouched
```

Reload is a no-op for the TUI: it happens entirely in the child
process. The parent's runtime, ctx, and alt-screen session don't
know reload is happening.

Lifetime in both cases: the calling runtime owns the ctx; release
happens on GC or scope exit; the cap layer's `atexit` safety net
restores the terminal if release is missed.

The cap layer's invariant (at most one live `HlTuiCtx` per process)
falls out trivially because each process has one runtime that owns
one ctx. The invariant exists to catch programming errors, not to
solve a real ambiguity.

## Build flag — `HL_ENABLE_TUI`

Default `1`. Same pattern as `HL_ENABLE_WASM`, `HL_ENABLE_DB`, etc.

`HL_ENABLE_TUI=0` drops:

| Removed | Why |
|---------|-----|
| `src/hull/cap/tui.c` | The capability impl |
| `src/hull/runtime/{lua,js}/mod_tui.c` | Runtime bindings |
| `stdlib/{lua,js}/hull/tui.*` | The user-facing helper module |
| `tests/hull/cap/test_tui.c` | Unit tests for the cap layer |
| Embedded Unicode width table (`vendor/unicode/eaw.h`) | Only used by cell diffing |

Kept: everything else. Estimated size: +80–150 KB enabled (ANSI
parser + key-decode table ~30 KB; shadow/pending buffer code ~20 KB;
embedded Unicode width table ~3 KB; theme + clipboard ~5 KB; plus
stdlib helpers in Lua/JS source ~25 KB). Disabling saves that much.
Per-process runtime memory adds rows×cols×16 bytes of buffer state
(~200 KB for a 200×60 terminal).

The flag exists for the same reason others do: keeps the size budget
honest for users who genuinely don't want it, not because anyone is
expected to disable it.

## Module registry changes

`hull/tui` is a first-party module and slots into the existing module
framework (registry → resolver → resolved set bitset → signature →
tool exposure) with zero new infrastructure. See "Module Declaration
System" in CLAUDE.md / `docs/security.md §5b` for the full mechanism.

### Capability bit

```c
/* include/hull/module_registry.h */
#define HL_MOD_CAP_TUI    (1u << 7)   /* requires HL_ENABLE_TUI at build */
```

Built-cap mask grows by `HL_MOD_CAP_TUI` when `HL_ENABLE_TUI` is
defined; the resolver's `build_provided_caps()` includes it; the
`cap_label` table gets a matching `"HL_ENABLE_TUI"` string for error
messages.

### Registry entry

Added to the sorted `HlModuleSpec` table in
`src/hull/module_registry.c`:

```c
{
    .name           = "hull/tui",
    .api_major      = 1,
    .intrinsic      = 0,               /* must be declared in manifest */
    .deps           = NULL,            /* no first-party deps */
    .deps_count     = 0,
    .required_caps  = HL_MOD_CAP_TUI,  /* build-time gate */
    .required_manifest_field = "tui",  /* runtime-time gate (boolean) */
},
```

`required_manifest_field` is the same mechanism `hull/gpu@1` uses to
demand `manifest.gpu`; the resolver enforces it after build-cap check.

### Resolver behavior

Identical to every other module: declaring `hull/tui@1` causes the
resolver to:

1. Look up the spec by name — O(log n) binary search.
2. Check build caps: `(spec.required_caps & build_caps) == spec.required_caps`.
   If not, error: `module 'hull/tui@1' requires HL_ENABLE_TUI, but it
   is disabled in this hull build`.
3. Check manifest field: `manifest.tui == true`. If not, error:
   `module 'hull/tui@1' requires the 'tui' capability in the manifest`.
4. Recursively resolve deps (none here).
5. Set the corresponding bit in `HlResolvedModuleSet` (stored on
   `HlRuntime`); `require("hull.tui")` / `import "hull:tui"` check
   this bit before granting access.

### Persisted in the signature

The resolved set goes into `package.sig`'s `modules_resolved` field
(same mechanism `hull build` already uses), so the build's module
graph is covered by the Ed25519 signature.

### Tool exposure

`tool.modules_resolve` (used by `hull build`, `hull check`,
`hull modules`) handles `hull/tui` transparently — no new tool code.

`hull modules available [--json]` lists it. `hull modules explain
hull/tui` returns its spec. `hull agent modules` includes it in the
declared / available arrays.

### Future modules

| Module | Notes |
|--------|-------|
| `hull/tui/widgets@1` | Higher-level table/tree/modal widgets; depends on `hull/tui@1` |
| `hull/tui/syntax@1` | Pluggable syntax highlighting for code views; depends on `hull/tui@1` |

These follow the same framework — added to the registry as separate
specs with `deps = ["hull/tui"]` — when they ship.

## Manifest field — `tui`

New top-level boolean field, parallel to `gpu`:

```lua
app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})
```

The pair is intentional and the resolver enforces both:

- `tui = true` without `hull/tui@1` declared → module not in scope at
  `require` time. Resolver doesn't error (the cap is there, the script
  just hasn't asked for the module), but `require("hull.tui")` fails
  with the standard "module not declared" message.
- `hull/tui@1` declared without `tui = true` → resolver error at load:
  `module 'hull/tui@1' requires the 'tui' capability in the manifest`.
  Same grammar as the existing GPU check.

`tui = true` is its own bit because the sandbox needs to know whether
to leave stdin / `tcsetattr` accessible, regardless of whether the
script actually calls into the module on a given invocation.

## Capability layer

New files: `src/hull/cap/tui.c`, `include/hull/cap/tui.h`. Self-contained;
no external deps.

The API takes an `HlTuiCtx *` everywhere — same shape as `HlGpuCtx`,
`HlDbHandle`, `HlWasmCtx`. The ctx is opaque to callers; its lifetime
is controlled by `acquire`/`release`. The cap layer enforces a process-
level invariant that **at most one `HlTuiCtx` is live per process** (the
OS controlling tty is singleton; multiple ctxs would race on termios
and signal handlers). A second `acquire` while another ctx is live
returns `-EBUSY`.

```c
typedef struct HlTuiCtx HlTuiCtx;

/* Allocate ctx, save termios snapshot, enter alt screen, set raw mode,
 * install signal handlers, query terminal capabilities (truecolor /
 * 256-color / theme via OSC 11). Returns -EBUSY if a ctx already
 * exists in this process. */
int  hl_cap_tui_acquire(HlTuiCtx **out);

/* Restore termios, leave alt screen, restore signal handlers, free
 * ctx. Idempotent; calling twice on the same ctx is a no-op after the
 * first. */
int  hl_cap_tui_release(HlTuiCtx *ctx);

/* atexit / panic handler; not exposed to scripts. Walks the singleton,
 * restores terminal if a ctx exists, doesn't free (process is dying). */
void hl_cap_tui_force_leave(void);

int  hl_cap_tui_size(HlTuiCtx *ctx, int *cols, int *rows);
int  hl_cap_tui_clear(HlTuiCtx *ctx);
int  hl_cap_tui_move(HlTuiCtx *ctx, int x, int y);
int  hl_cap_tui_style(HlTuiCtx *ctx, uint32_t fg, uint32_t bg, uint32_t flags);
int  hl_cap_tui_write(HlTuiCtx *ctx, const char *buf, size_t len);
int  hl_cap_tui_flush(HlTuiCtx *ctx);

typedef enum {
    HL_TUI_EV_NONE     = 0,
    HL_TUI_EV_KEY      = 1,
    HL_TUI_EV_RESIZE   = 2,
    HL_TUI_EV_MOUSE    = 3,
    HL_TUI_EV_PASTE    = 4,
    HL_TUI_EV_FOCUS    = 5,
} HlTuiEventKind;

typedef struct {
    HlTuiEventKind kind;
    /* HL_TUI_EV_KEY */
    const char *key;       /* "a", "up", "enter", "ctrl+c", "f1", … */
    uint32_t    codepoint; /* for printable keys */
    uint32_t    mods;      /* shift|ctrl|alt|super bitmask */
    /* HL_TUI_EV_RESIZE */
    int cols, rows;
    /* HL_TUI_EV_MOUSE */
    int x, y;
    int button;            /* 0=left, 1=middle, 2=right, 3=release, 4=wheel-up, 5=wheel-down */
    /* HL_TUI_EV_PASTE */
    const char *text;
    size_t      text_len;
} HlTuiEvent;

/* timeout_ms: 0=poll, -1=blocking, >0=blocking with deadline.
 * Yields to the event loop while waiting. Returns 0 on event,
 * 1 on timeout, -1 on error. */
int  hl_cap_tui_poll(HlTuiCtx *ctx, int timeout_ms, HlTuiEvent *out);

/* Terminal capability bitmask (computed at acquire from $TERM /
 * $COLORTERM / OSC queries). */
#define HL_TUI_CAP_TRUECOLOR   (1u << 0)
#define HL_TUI_CAP_256COLOR    (1u << 1)
#define HL_TUI_CAP_BASIC_COLOR (1u << 2)
#define HL_TUI_CAP_MOUSE       (1u << 3)
#define HL_TUI_CAP_FOCUS       (1u << 4)
#define HL_TUI_CAP_KITTY_KBD   (1u << 5)
#define HL_TUI_CAP_OSC52       (1u << 6)  /* clipboard write */
uint32_t hl_cap_tui_caps(HlTuiCtx *ctx);

/* "dark" | "light" | "unknown". Cached at acquire time from OSC 11
 * query (\x1b]11;?\x07) with a 50ms timeout. Falls back to checking
 * $COLORFGBG, then defaults to "dark" if neither responds.
 * $NO_COLOR forces "unknown" (caller treats as monochrome). */
const char *hl_cap_tui_theme(HlTuiCtx *ctx);

/* Write `text` to system clipboard via OSC 52
 * (\x1b]52;c;<base64>\x07). Returns 0 on success, -ENOTSUP if
 * HL_TUI_CAP_OSC52 isn't set. Clipboard *read* deliberately not
 * exposed — most modern terminals refuse it, and pasting via
 * bracketed paste is the supported input path. */
int  hl_cap_tui_clipboard_set(HlTuiCtx *ctx, const char *text, size_t len);
```

### Shadow buffer + cell diffing

v1 ships with cell-level diffing. Apps that just call
`tui.print` / `tui.flush` get flicker-free updates over ssh / mosh
without changing code. Mechanics:

- The cap layer maintains a shadow buffer `HlTuiCell shadow[rows][cols]`
  inside the ctx. Each cell stores `{ codepoint, width, fg, bg, attr }`.
  Allocated on acquire, reallocated on SIGWINCH.
- `tui.write` / `tui.print` / `tui.move` / `tui.style` mutate a *pending*
  buffer (same shape) — no immediate ANSI emit.
- `tui.flush` walks both buffers row by row, finds dirty runs, emits
  cursor-move + SGR-attr + the changed bytes for each run, then swaps
  buffer pointers. Worst case (full repaint) is the same byte count as
  clear-and-redraw; best case (one character changed) is ~12 bytes.
- `tui.clear` zeros the pending buffer; next flush emits whatever
  delta from the shadow gets us there. (Apps that want a forced
  full-repaint after a corruption can call `tui.invalidate()` which
  zeros the shadow too, so next flush emits everything.)
- Unicode width: ship a checked-in `vendor/unicode/eaw.h` (~3 KB
  generated from EastAsianWidth.txt + UnicodeData.txt; same pattern
  as `vendor/cacert/cacert.pem`). Used on all platforms — no host
  `wcwidth(3)` dependency means consistent rendering between
  glibc/musl/cosmo/macOS. Refresh via `make fetch-unicode` (analogous
  to `make fetch-ca-bundle`). Wide characters occupy two adjacent
  cells; the second cell stores `{ width = 0 }` as a continuation
  marker so diffing correctly emits the whole character whenever
  either half changes.
- Combining characters: appended to the preceding cell's codepoint
  list (small inline buffer, falls back to allocation past 4
  combining chars per cell — rare in practice).

Size cost: ~150 lines of C + a ~3KB width table + 16 bytes per cell
× rows × cols of state. For a 200×60 terminal that's 192 KB of buffer
state per ctx, well within budget.

### Color theme detection

At `acquire`, the cap layer:

1. Checks `$NO_COLOR` — if set, theme stays `"unknown"`, caller
   treats as monochrome.
2. Sends OSC 11 query (`\x1b]11;?\x07`), waits 50ms for a response.
   Parses the reply (`\x1b]11;rgb:RRRR/GGGG/BBBB\x07`), computes
   luminance, classifies as `"dark"` (L < 0.5) or `"light"`.
3. If no OSC 11 reply, checks `$COLORFGBG` (rxvt convention,
   `"<fg>;<bg>"`); maps `bg` 0–6 → dark, 7–15 → light.
4. If nothing responds, defaults to `"dark"` (covers ~80% of
   developer terminals — black/dark backgrounds dominate).

The result is cached in the ctx; `hl_cap_tui_theme` is a pointer
return, no re-query. Apps that want to re-detect after the user
changes terminal theme can `release` + `acquire` again. (Realistic
v2 work: a `tui.re_detect_theme()` that re-runs the query.)

### Other engineering points

- **Termios save/restore.** On `acquire`, snapshot the current
  `struct termios` (stored in the ctx) and the cursor position. Set
  raw mode (`ICANON|ECHO|ISIG` off, `VMIN=0`, `VTIME=0`). On
  `release`, write the reset sequences (`\x1b[?1049l\x1b[?25h\x1b[0m`)
  and restore termios. The cap layer registers `atexit` once globally
  (not per ctx) to run `force_leave` as a safety net in case `release`
  is missed. SIGINT, SIGTERM, SIGQUIT get one-line handlers that call
  `force_leave` and re-raise. SIGTSTP / SIGCONT are wrapped so
  suspend/resume work cleanly.
- **Resize.** SIGWINCH sets a flag inside the ctx; next `poll` returns
  a resize event before any pending key events, and reallocates the
  shadow buffer to the new dimensions. Flag is `sig_atomic_t`, read+
  cleared via atomic exchange.
- **Key decoding.** Hand-written ANSI CSI parser (one state machine,
  stored inline in the ctx). Supports legacy xterm sequences, Kitty
  keyboard protocol (opt-in via `tui.run({ kitty_kbd = true })`),
  bracketed paste, focus in/out, SGR mouse. Parser state in the ctx
  means a partial CSI sequence in flight at the moment a Lua/JS GC
  event releases the userdata wrapper would survive — but only the
  *cap-layer* state does; events queued for a dead VM are dropped.
- **Output buffering.** All `write` calls append to the pending
  shadow buffer (not raw stdout). `flush` emits the diff vs the
  current shadow in one `write(2)`. Atomic redraws, no tearing.
- **No threads.** Polling is single-threaded; integration with the
  event loop is via `kl_poll_add_fd(STDIN_FILENO, READ)` (existing
  Keel primitive) so async ops yield naturally.

## Stdlib modules

`stdlib/lua/hull/tui.lua` and `stdlib/js/hull/tui.js`.

**Canonical API**: `tui.run` plus a small set of helpers built on it.
These are what docs, examples, and scaffolding lead with.

| Lua | JS | Purpose |
|-----|-----|---------|
| `tui.run(opts)` | `tui.run(opts)` → `Promise<string>` | immediate-mode loop — *the entry point* |
| `tui.frame(opts, fn)` | `tui.frame(opts, fn)` | bordered area + nested cursor scope |
| `tui.list(items, opts)` | `tui.list(items, opts)` | scrollable list; returns picked index or nil |
| `tui.input(prompt, opts?)` | `tui.input(prompt, opts?)` | line input with editing; returns string or nil |
| `tui.progress(pct, opts?)` | `tui.progress(pct, opts?)` | render a progress bar |
| `tui.spinner(state)` | `tui.spinner(state)` | next frame of a spinner |
| `tui.confirm(msg)` | `tui.confirm(msg)` → `Promise<bool>` | y/n prompt |
| `tui.theme()` → `"dark"\|"light"\|"unknown"` | `tui.theme()` | classified bg luminance (see Color theme detection) |
| `tui.clipboard_set(text)` | `tui.clipboardSet(text)` | write to system clipboard via OSC 52; errors if unsupported |
| `tui.async(fn)` | `tui.async(fn)` | spawn a detached coroutine/promise that runs on the event loop and updates state for the next frame |

Helpers are pure Lua/JS over `tui.run` + `tui.frame`. They live in
stdlib (not the cap layer) so an app can drop in its own replacement
easily — `tui.list` in particular is opinionated about styling.

`tui.async(fn)` only lives here because it has no other caller today
— it exists so `tui.run`'s event handlers can kick off background work
without blocking the next frame. If a second consumer materializes
(e.g., a background timer-driven cap in a future feature), we'll
promote it to `hull.async`.

**Escape-hatch primitives** (use `tui.run` unless you have a specific
reason not to):

| Lua | JS | Purpose |
|-----|-----|---------|
| `tui.enter(opts?)` | `tui.enter(opts?)` | idempotent acquire — enter alt screen, raw mode |
| `tui.leave()` | `tui.leave()` | idempotent release — restore terminal |
| `tui.size()` → `cols, rows` | `tui.size()` → `{cols, rows}` | current size |
| `tui.caps()` | `tui.caps()` | capability flags |
| `tui.clear()` | `tui.clear()` | clear screen |
| `tui.move(x, y)` | `tui.move(x, y)` | move cursor (1-indexed) |
| `tui.style(opts)` | `tui.style(opts)` | set fg/bg/bold/underline/etc |
| `tui.print(x, y, str)` | `tui.print(x, y, str)` | move + write |
| `tui.write(str)` | `tui.write(str)` | append to buffer |
| `tui.flush()` | `tui.flush()` | emit buffer |
| `tui.poll(timeout_ms)` | `tui.poll(timeoutMs)` → `Promise<event\|null>` | next event |

These are present mainly to make `tui.run` implementable in pure
Lua/JS — once they exist for that, exposing them costs nothing. If
you find yourself building a real app on top of them, that's a signal
something is missing from `tui.run`.

### `tui.run` signature

```lua
tui.run({
    draw     = function(t)             end,  -- required
    on_event = function(ev) return nil end,  -- optional
    tick_ms  = 0,                            -- optional: redraw at most every N ms when idle
    mouse    = false,                        -- opt-in SGR mouse
    paste    = true,                         -- bracketed paste
    kitty_kbd= false,                        -- Kitty keyboard protocol
    allow_no_tty = false,                    -- short-circuit if stdin/stdout not a tty
})
```

`t` passed to `draw` is a table/object with `cols`, `rows`,
`print(x, y, str)`, `move(x, y)`, `style(...)`, `clear()`. (The same
methods exist on the module itself but `t` gives a stable handle that
the run-loop guarantees is consistent for the current frame.)

## Dispatch

In `src/hull/main.c` / `serve.c`, dispatch logic remains unchanged from
CLI mode — TUI lives entirely inside `app.main`. No new top-level
branch in the lifecycle. The only changes are:

1. **Manifest extraction** picks up `tui = true` (analogous to existing
   `gpu = true`).
2. **Resolver** validates `hull/tui@1` declarations against the cap.
3. **Sandbox phase 2** keeps stdin readable and `tcsetattr` allowed when
   `tui = true` is set — otherwise stdin remains usable via `ctx.stdin`
   but termios writes are blocked (current cli_mode.md sandbox already
   omits `tty` from the unveil/pledge set; this re-adds it under the
   `tui` cap).

## Sandbox interaction

Two-phase sandbox unchanged. The `tui` capability adds these allowances
in phase 2:

| Platform | Without `tui` | With `tui` |
|----------|---------------|------------|
| Linux/Cosmo pledge | no `tty` promise | adds `tty` |
| Linux unveil | no `/dev/tty` access | unveils `/dev/tty`, `/dev/pts/*` (rwc, but the syscalls are still pledged) |
| macOS Seatbelt | no terminal allow clauses | adds `(allow file* (literal "/dev/tty"))` + `(allow file* (regex #"^/dev/ttys[0-9]+"))` |
| OpenBSD | no `tty` promise | adds `tty` |

Signals: `SIGWINCH`, `SIGTSTP`, `SIGCONT`, `SIGINT`, `SIGTERM`,
`SIGQUIT` need handlers. The existing Hull signal infrastructure
(used for graceful shutdown) extends to forward `SIGWINCH` and the
suspend/resume pair to the TUI cap context.

## First-party dogfood targets

The reason TUI exists. Each is a real Lua tool module under
`stdlib/lua/hull/`, invoked from C via the same path
`hull init` already uses today. The pattern:

```
src/hull/commands/doctor.c          ─ accepts --tui, delegates to:
stdlib/cli/lua/hull/doctor_tui.lua      ─ uses hull.tui, renders interactively
```

The C side stays small (arg parsing, capability checks, dispatch); the
TUI logic is Lua. This means:

- Every interactive Hull tool exercises the public `hull.tui` API. If
  the API has rough edges, we find them.
- Adding a new interactive tool means writing one Lua file, not
  touching C.
- The tool modules ship as embedded stdlib (already routed through the
  VFS) — no separate distribution, no platform-specific bits.

Concrete deliverables in the rollout:

| Tool | What TUI adds | Phase |
|------|--------------|-------|
| `hull doctor --tui` | Live, color-coded subsystems pane; press `r` to re-probe, `c` to copy JSON to clipboard via OSC 52 | Phase 2 |
| `hull dev` (auto-tui when stdout is tty) | Top pane: live request log with filter (`/path`, `s/status`, `m/method`); bottom pane: keybindings + agent sidecar status. `r` reloads served app, `q` quits, `e` opens last error | Phase 3 |
| `hull agent context --interactive` | Picker UI for task / level; live preview of the rendered context | Phase 3 |
| `hull agent errors --tui` | Scrollable error list with expand-to-stacktrace | Phase 3 |
| `hull migrate status --tui` | Applied vs pending with diff preview | optional, phase 4 |
| `hull modules available --tui` | Searchable module list with deps + caps shown inline | optional, phase 4 |

`hull dev --tui` is the headline target. The parent process gains a
Lua tool runtime that runs `dev_tui.lua`, drawing a request log from
the child's stderr. The child (the served app) is fork+exec'd as
before and reloads via kill+respawn — completely independent of the
TUI in the parent.

`hull doctor --tui` is the simplest end-to-end exercise: one screen,
two keybindings, one async re-probe. Good first target after the cap
layer lands.

## Test harness

`tests/hull/cap/test_tui.c` covers the cap layer:

- **PTY harness.** Use `forkpty(3)` on POSIX, `openpty` polyfill on
  cosmo. Parent process writes ANSI input sequences, reads back what
  the child writes. Tests cover: enter/leave round-trip, raw mode
  set/restore, key decode for every entry in a fixture table, resize
  delivery, bracketed paste assembly, mouse SGR decode, signal
  restoration.
- **No real tty required for CI.** All tests use PTYs so they run
  unattended.
- **Gated on cosmo if openpty unavailable.** On builds where PTY is
  missing, the test compiles but skips with a clear `SKIP: no pty`
  message, same pattern as the GPU tests.

E2E (`tests/e2e_tui.sh`):

- Runs example apps under `script(1)` and asserts on captured output.
- Validates that `hull doctor --tui`, when run on a non-tty (CI), falls
  back to the existing plain-text output.

`test.run_main` from CLI mode (already proposed in `docs/cli_mode.md`)
gains a `tui` option:

```lua
local result = test.run_main({
    args  = {},
    stdin = "alpha\n\x1b[B\n",   -- simulated keys: "alpha", enter, down arrow, enter
    tui   = true,                 -- attach a fake PTY
})
test.eq(result.exit_code, 0)
test.eq(result.stdout, "you picked: beta\n")
```

## Examples

Ship under `examples/cli/`:

| Example | Demonstrates |
|---------|--------------|
| `tui_picker` | Minimal `tui.run` with `tui.list` |
| `tui_log_tailer` | Async — tails a file while updating UI without blocking |
| `tui_dashboard` | Multi-pane with `tui.frame`; mock metrics |
| `tui_repl` | `tui.input` + history; evaluates Lua/JS expressions |
| `tui_chat` | Split pane, async `http.fetch` for a fake chat backend |

## Implementation phases

### Phase 1 — cap layer + `tui.run`

The canonical API ships in v1. Raw primitives are exposed but
secondary; the doc and examples lead with `tui.run`.

- New `src/hull/cap/tui.c` + header. `HlTuiCtx` struct (heap-
  allocated, opaque). Termios + ANSI parser + signal glue. Cell-
  level shadow + pending buffers, diffing on flush. Acquire/release
  with the at-most-one-per-process invariant (EBUSY on conflict).
  `atexit` + signal-driven restore via `force_leave`. OSC 11 theme
  detect at acquire; OSC 52 clipboard write. PTY-based unit tests
  including the golden-frame diff tests.
- Embedded Unicode width table checked in at `vendor/unicode/eaw.h`
  (~3 KB, generated from EastAsianWidth.txt + UnicodeData.txt by a
  small script also checked in at `vendor/unicode/gen.lua`). `make
  fetch-unicode` re-runs the generator against fresh upstream data
  and rewrites `eaw.h` — same pattern as `make fetch-ca-bundle`.
  Builds are hermetic; no network or external Unicode data files
  required at build time.
- `mod_tui.c` in both runtimes. Bindings expose: `tui.run`,
  `tui.async`, `tui.theme`, `tui.clipboard_set`/`clipboardSet`,
  `tui.acquire`, `tui.release`, `tui.size`, `tui.caps`, `tui.clear`,
  `tui.move`, `tui.style`, `tui.print`, `tui.write`, `tui.flush`,
  `tui.poll`, `tui.invalidate`. `tui.run` is the canonical entry.
- `tui.async(fn)` script-facing helper inside `hull.tui` (detached
  coroutine on the event loop). Required by `tui.run` for fire-and-
  forget background work that updates state without blocking redraws.
  Promoted to `hull.async` if a second consumer materializes.
- New manifest field `tui` (parser + sandbox wiring).
- Module registry entry per the first-party framework (HlModuleSpec
  with `required_caps = HL_MOD_CAP_TUI`, `required_manifest_field =
  "tui"`); resolver tests for missing-cap, missing-manifest, and
  positive-resolve paths.
- E2E: `examples/cli/tui_picker` (minimal `tui.run` with a list).
- Outcome: An app can `tui.run` on any default hull build, exits
  cleanly, restores terminal on panic/signal, renders flicker-free
  via cell diffing.

Estimated effort: ~4 days (was 3 — cell-diffing + theme + clipboard
add ~1 day).

### Phase 2 — stdlib helpers + `hull doctor --tui`

- `stdlib/lua/hull/tui.lua`, `stdlib/js/hull/tui.js`: helpers built on
  `tui.run` — `tui.frame`, `tui.list`, `tui.input`, `tui.progress`,
  `tui.spinner`, `tui.confirm`.
- `stdlib/cli/lua/hull/doctor_tui.lua` — the `--tui` rendering for
  `hull doctor`. `src/hull/commands/doctor.c` gets a `--tui` arg that
  dispatches into this Lua module. First end-to-end exercise of the
  whole stack (C dispatcher → Lua tool module → `tui.run` → cap layer).
- E2E: `examples/cli/tui_dashboard`, `tui_repl`.
- Outcome: First dogfood landed. The Lua-tool-module pattern is
  validated for one real command before we apply it to `hull dev`.

Estimated effort: ~2 days.

### Phase 3 — `hull dev --tui` + `hull agent` pickers

Make Hull's own CLI feel native. No special infrastructure — `hull
dev` already fork+execs the served app, so the TUI just lives in the
parent process alongside the existing file watcher.

- `--tui` flag on `hull dev`. When set (and stdout is a tty), the
  parent spawns a Lua tool runtime that runs
  `stdlib/cli/lua/hull/dev_tui.lua`.
- Before forking the child, parse the served app's manifest. If it
  declares `tui = true`, refuse with `served app uses tui mode; run
  it directly with hull run`.
- `dev_tui.lua` uses `tui.run` with a 100ms tick. Top pane: request
  log streamed from the child's stderr (parsed line-by-line; logfmt
  default). Bottom pane: keybindings + child status (PID, uptime,
  last reload time). `r` triggers reload (sends SIGTERM to child, dev
  parent respawns), `q` quits, `e` opens the last error.
- `hull agent context --interactive` and `hull agent errors --tui` —
  Lua tool modules that use `tui.list` / `tui.frame` for picker /
  scroller variants of the existing JSON outputs.
- Theme-aware styling — Lua tool modules read `tui.theme()` and pick
  fg/bg accordingly. Validated against light + dark terminal schemes.
- Outcome: `hull dev --tui` works on Linux, macOS, Cosmo. Lua tool
  module pattern proven for the most stateful case.

Estimated effort: ~2 days.

### Phase 4 — docs, examples, optional helpers

- README + AGENTS + CLAUDE.md sections.
- `docs/tui_mode.md` (this doc) updated with measured binary-size
  numbers and any API tweaks discovered during phases 1–3.
- Remaining example apps shipped.
- Decide on `hull migrate status --tui`, `hull modules available
  --tui` based on real demand.
- Final e2e sweep on Linux, macOS, Cosmo.

Estimated effort: ~1 day.

### Total

~1.5 weeks of focused work (was ~1 week; cell-diffing pushed it).
Shape still matches CLI mode itself — TUI is "CLI mode + alternate
screen + input parser + cell-diffed render loop."

## Cosmopolitan support

Cosmo APE is a first-class build target for Hull. The TUI module
should run on it without modification — every syscall it uses is in
Cosmo's libc:

| Used by | Calls |
|---------|-------|
| Cap layer (cap/tui*.c) | `tcgetattr/tcsetattr`, `poll`, `read`/`write`, `ioctl(TIOCGWINSZ)`, `sigaction`, `atexit`, `clock_gettime(CLOCK_MONOTONIC)`, `fcntl`, `isatty` |
| Dev TUI (commands/dev.c) | the above + `pipe`, `fork`, `execvp`, `waitpid` |

All POSIX. No Linux-specific syscalls (`inotify`, `eventfd`, `signalfd`),
no macOS-specific (`kqueue`, `EvFilt*`), no GNU extensions.

The platform-specific `#define` gates in `tests/hull/cap/test_tui_lifecycle.c`
and `tests/e2e_tui_drive.c` cover `__APPLE__ / __linux__ / __FreeBSD__ /
__OpenBSD__` but not Cosmo — on a Cosmo build the PTY harnesses
gracefully skip (`SKIP: no forkpty on this platform`). The width-table
and parser unit tests are pure C and run unchanged.

End-to-end verification on Cosmo is the standard recipe:

```sh
make platform-cosmo
make CC=cosmocc HL_ENABLE_TUI=1
make test CC=cosmocc HL_ENABLE_TUI=1
# Manual:
./build/hull doctor --tui          # interactive
./build/hull modules available --tui
./build/hull dev --tui examples/hello_cli/app.lua
```

The PTY-based e2e (`tests/e2e_tui.sh`) requires forkpty and therefore
does not run on Cosmo today — apps are exercised manually. A
follow-up could port the PTY harness to use Cosmo's `pty_open`
equivalent or shell out to `script(1)`.

## Risk callouts

- **Acquire/release correctness.** A subtle bug here produces
  "terminal stuck in raw mode" or "alt screen never restored"
  symptoms. Mitigations: (i) `atexit` + signal handlers always call
  `force_leave`; (ii) `hl_cap_tui_acquire` returns `-EBUSY` if called
  while another ctx is live in the same process; (iii) PTY test that
  allocates ctx, attempts second acquire (expects EBUSY), releases,
  allocates again (expects OK), then exits without explicit release
  and asserts terminal restored via atexit.
- **Signal interactions with Keel.** Keel installs handlers for
  `SIGINT` / `SIGTERM` (graceful shutdown). TUI layers on top: TUI's
  handler does termios restore first, then chains to the saved
  previous handler. Install TUI's last, restore by calling through.
  Existing Keel signal tests pass through TUI's handler unchanged.
- **Served TUI apps under `hull dev`.** If the served app declares
  `tui = true`, `hull dev` refuses to start it before forking, with
  a clear error pointing at `hull run`. Reason: both parent (dev
  log/TUI) and child would fight for the same controlling tty.
  Enforced in `commands/dev.c` after manifest extraction, before
  fork.
- **Cosmo PTY support.** APE ships its own libc. `openpty` /
  `forkpty` need verification on cosmo for the test harness. If
  unavailable, gate `tests/hull/cap/test_tui.c` off on cosmo and rely
  on E2E (which uses real ttys via `script(1)` on the host). The
  runtime itself doesn't need PTY — that's only for testing.
- **Windows Terminal compatibility.** TUI is POSIX-first. On
  Cosmopolitan builds running on Windows, raw mode goes through
  cosmo's polyfilled termios; ANSI rendering works on Windows
  Terminal / ConHost (post-2019). Pre-2019 ConHost will display
  garbled output. Decision: don't try to detect; document "Windows
  Terminal required."
- **Manifest grant scope.** `tui = true` is a coarse-grained bit. We
  could split into `tui.input` / `tui.output` / `tui.mouse`, but the
  attack surface inside the manifest's allowance is already tiny
  (read stdin, write stdout, tcsetattr) and finer-grained bits would
  add UX noise for no real defense. Decision: keep one bit; revisit
  if a concrete threat surfaces.
- **Tool-VM compatibility.** `hull build`, `hull manifest`,
  `hull check` load apps in a stub VM. Same pattern CLI mode uses:
  `app.main(fn)` is registered as a no-op stub. `tui` module functions
  similarly stub out — `require("hull.tui")` returns a table of
  no-op closures. App top-level never actually opens a tty during
  manifest extraction; the stubs only need to not crash.
- **Shadow buffer correctness.** Cell diffing is in v1, so any bug in
  the shadow / pending buffer comparison shows up as visible rendering
  corruption — characters left behind, attributes carrying across
  unrelated cells, wide-character halves split between frames. The
  test plan: golden-frame tests that drive a sequence of writes and
  diff against a recorded byte stream. Categories: ASCII only, ASCII
  with SGR transitions, CJK wide chars, combining marks, emoji ZWJ
  sequences (deferred — flag in docs but don't test in v1), resize
  mid-render (shadow reallocates; pending discarded; full repaint on
  next flush).
- **Unicode width drift.** The checked-in `vendor/unicode/eaw.h` is
  the source of truth on all platforms (we don't fall back to host
  `wcwidth(3)`, to keep rendering identical across glibc / musl /
  cosmo / macOS). Drift risk: someone runs `make fetch-unicode`,
  upstream changes a character class, the diff slips through review
  uninspected. Mitigation: golden-frame tests assert specific
  high-impact codepoints (CJK ranges, emoji presentation selectors,
  zero-width chars) render to known cell counts. PR review on
  `eaw.h` changes is a standing requirement; checked-in generator at
  `vendor/unicode/gen.lua` means the diff is auditable.

## Open questions

- **Multi-byte input on legacy terminals.** UTF-8 input pasted as
  multi-byte sequences works through bracketed paste. Direct typing
  of UTF-8 through xterm legacy mode is mostly OK on modern terms but
  needs verification. Plan: rely on bracketed paste for non-ASCII;
  document.
- **Emoji ZWJ sequences in the shadow buffer.** Combining marks are
  handled; ZWJ (zero-width joiner) emoji sequences like 👨‍👩‍👧‍👦
  may render as multiple separate emoji on terminals without proper
  shaping. v1 stores them as separate cells; the visual result
  depends entirely on the terminal. Document, don't try to be clever.
- **Threading.** TUI assumes single-threaded event loop. If a future
  Hull adds true multi-threaded request handling, the cap-layer
  ownership of stdin/stdout/termios needs to be made explicit (the
  TUI thread "owns" the terminal). For now, single-threaded is the
  only mode, so this is a non-issue.
- **Child stderr → parent log format.** `hull dev`'s TUI mode needs
  a wire format for log lines from the child. Options: logfmt (the
  existing logger middleware default — easy parse), JSON-per-line
  (more verbose but unambiguous), or a structured `HULL-LOG:` prefix
  the parent recognizes vs passes through. Pick during phase 3
  implementation; defaulting to logfmt because that's already what
  the logger middleware emits.

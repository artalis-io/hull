# TUI Mode — Design

`hull/tui@1` module. `tui.run({...})` immediate-mode loop, or raw
primitives. Built on top of CLI mode (`app.main`). Primary driver:
dogfood — `hull dev`, `hull doctor`, `hull agent` get real interactive
UIs. By-product: Hull apps can ship terminal UIs (file pickers,
dashboards, log tailers, REPLs) using the same sandboxed runtime they
already use for backend code.

This document is the design plan. Status: not yet implemented.

## Goals

1. **Tooling-first.** The bar for shipping is "`hull doctor --tui`,
   `hull dev` request log, and `hull agent context --interactive` all
   feel native." App-facing API is the same surface those tools use; no
   private bindings.
2. **Pragmatic API.** Two layers — raw primitives (`tui.print`,
   `tui.move`, `tui.poll`) and a higher-level immediate-mode loop
   (`tui.run({ draw, on_event, tick_ms })`). No widget framework in v1.
3. **One binary, both modes.** TUI is an addition to CLI mode, not a new
   build flavor. Default `HL_ENABLE_TUI=1`; `=0` drops it for size-
   constrained builds. Works on `HL_ENABLE_HTTP=0` (CLI-only) hulls
   identically to default hulls.
4. **Cosmo-supported.** TUI uses POSIX termios + ANSI sequences only —
   no platform libraries. Full APE compatibility.
5. **Sandbox-compatible.** TUI needs only stdin/stdout/termios; nothing
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
- **Re-implementing ncurses / notcurses.** No alternate-screen-aware
  buffer diffing in v1 (we just clear+redraw, or app does its own
  invalidation). No Unicode width table (we rely on `wcwidth(3)`
  where available, fall back to "all printable = 1 column" for ASCII).
  These can come if a real app needs them.

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

### Raw API (no run loop)

For tools that want to interleave TUI ops with arbitrary async work
(`hull dev`'s background HTTP probes, an async log tailer):

```lua
local tui = require("hull.tui")

tui.enter()                                -- alternate screen, raw mode, hide cursor
local cols, rows = tui.size()
tui.move(1, 1); tui.print("loading…")
tui.flush()

while true do
    local ev = tui.poll(1000)              -- ms; yields to event loop, returns nil on timeout
    if ev and ev.kind == "key" and ev.key == "q" then break end
    -- arbitrary async work allowed here: http.fetch, db.query, compute.async, etc.
end

tui.leave()                                -- restore terminal (also runs at exit via atexit)
```

`tui.enter` / `tui.leave` are idempotent and may be called multiple
times. They MUST be paired in normal flow but the runtime installs an
`atexit` handler + SIGINT/SIGTERM handler that restores the terminal
even on panic or signal.

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
│      • Inside main: tui.run({...}) or tui.enter()+poll loop     │
│      • tui.poll yields to event loop like any async op          │
│ 7. tui.leave() called (manual, or via run-loop exit)            │
│      • Restore termios, leave alternate screen, show cursor     │
│ 8. main returns → cleanup (drain caches, scrub keys, close DB)  │
│ 9. Process exits with main's return code                        │
└─────────────────────────────────────────────────────────────────┘
```

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
            hull.async(function()
                local r = http.fetch("https://api.example.com/status")
                state.message = r.body
            end)
        end
    end,
    tick_ms = 100,   -- redraw at 10Hz so state updates show up promptly
})
```

`hull.async(fn)` is a new helper that launches `fn` as a detached
coroutine on the event loop — exists today for timers
(`HlAsyncCtx.detached`), gets exposed to scripts as part of this work.

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

## Build flag — `HL_ENABLE_TUI`

Default `1`. Same pattern as `HL_ENABLE_WASM`, `HL_ENABLE_DB`, etc.

`HL_ENABLE_TUI=0` drops:

| Removed | Why |
|---------|-----|
| `src/hull/cap/tui.c` | The capability impl |
| `src/hull/runtime/{lua,js}/mod_tui.c` | Runtime bindings |
| `stdlib/{lua,js}/hull/tui.*` | The user-facing helper module |
| `tests/hull/cap/test_tui.c` | Unit tests for the cap layer |

Kept: everything else. Estimated size: +30–80 KB enabled (mostly the
ANSI parser + a small key-decode table). Disabling saves that much.

The flag exists for the same reason others do: keeps the size budget
honest for users who genuinely don't want it, not because anyone is
expected to disable it.

## Module registry changes

Add a new capability bit:

```c
#define HL_MOD_CAP_TUI    (1u << 7)   /* requires HL_ENABLE_TUI at build */
```

New registry entry:

| Module | Caps required | Deps |
|--------|---------------|------|
| `hull/tui@1` | `HL_MOD_CAP_TUI` + manifest `tui = true` | (none — only intrinsic core) |

Future possibilities (not in v1):

| Module | Notes |
|--------|-------|
| `hull/tui/widgets@1` | Higher-level table/tree/modal widgets, builds on `tui` |
| `hull/tui/syntax@1` | Pluggable syntax highlighting for code views |

Resolver behavior is unchanged from the existing `HL_MOD_CAP_*` bits:
declaring `hull/tui@1` on an `HL_ENABLE_TUI=0` build produces:

```
module 'hull/tui@1' requires HL_ENABLE_TUI, but it is disabled in this hull build
```

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

```c
typedef struct HlTuiCtx HlTuiCtx;

int  hl_cap_tui_init(HlTuiCtx **out);          /* allocate state */
void hl_cap_tui_free(HlTuiCtx *ctx);            /* always safe to call */

int  hl_cap_tui_enter(HlTuiCtx *ctx);           /* termios raw, alt screen, hide cursor */
int  hl_cap_tui_leave(HlTuiCtx *ctx);           /* restore everything; idempotent */
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

/* timeout_ms: 0=poll, -1=blocking, >0=blocking with deadline */
int  hl_cap_tui_poll(HlTuiCtx *ctx, int timeout_ms, HlTuiEvent *out);

uint32_t hl_cap_tui_caps(HlTuiCtx *ctx);  /* bitmask: TRUECOLOR, 256COLOR, BASIC, MOUSE, FOCUS, KITTY_KBD */
```

Key engineering points:

- **Termios save/restore.** On `enter`, snapshot the current `struct
  termios` and the cursor position. Set raw mode (`ICANON|ECHO|ISIG`
  off, `VMIN=0`, `VTIME=0`). On `leave`, write the reset sequences
  (`\x1b[?1049l\x1b[?25h\x1b[0m`) and restore termios. Installer for
  `atexit` registers a once-only restore in case `leave` is missed.
  SIGINT, SIGTERM, SIGQUIT get a one-line handler that calls the
  restore path and re-raises. SIGTSTP / SIGCONT handled so suspend/
  resume work cleanly.
- **Resize.** SIGWINCH sets a flag; next `poll` returns a resize event
  before any pending key events. The flag is set via `sig_atomic_t`,
  read+cleared via atomic exchange.
- **Key decoding.** Hand-written ANSI CSI parser (one state machine).
  Supports legacy xterm sequences, Kitty keyboard protocol (opt-in via
  `tui.enter({ kitty_kbd = true })`), bracketed paste, focus
  in/out, SGR mouse. Decoding rules tested against
  `tests/hull/cap/test_tui.c` with a battery of recorded sequences.
- **Output buffering.** All `write` calls append to an internal buffer.
  `flush` emits the buffer to `stdout` in one `write(2)`. This makes
  redraws atomic (no tearing) and keeps performance acceptable on
  high-latency ttys (ssh).
- **No threads.** Polling is single-threaded; integration with the
  event loop is via `kl_poll_add_fd(STDIN_FILENO, READ)` (existing
  Keel primitive) so async ops yield naturally.

## Stdlib modules

`stdlib/lua/hull/tui.lua` and `stdlib/js/hull/tui.js`.

**Low-level primitives** (1:1 with cap layer):

| Lua | JS | Purpose |
|-----|-----|---------|
| `tui.enter(opts?)` | `tui.enter(opts?)` | enter alt screen, raw mode |
| `tui.leave()` | `tui.leave()` | restore terminal |
| `tui.size()` → `cols, rows` | `tui.size()` → `{cols, rows}` | current size |
| `tui.caps()` | `tui.caps()` | capability flags |
| `tui.clear()` | `tui.clear()` | clear screen |
| `tui.move(x, y)` | `tui.move(x, y)` | move cursor (1-indexed) |
| `tui.style(opts)` | `tui.style(opts)` | set fg/bg/bold/underline/etc |
| `tui.print(x, y, str)` | `tui.print(x, y, str)` | move + write |
| `tui.write(str)` | `tui.write(str)` | append to buffer |
| `tui.flush()` | `tui.flush()` | emit buffer |
| `tui.poll(timeout_ms)` | `tui.poll(timeoutMs)` → `Promise<event\|null>` | next event |

**Higher-level helpers**:

| Lua | JS | Purpose |
|-----|-----|---------|
| `tui.run(opts)` | `tui.run(opts)` → `Promise<string>` | immediate-mode loop |
| `tui.frame(opts, fn)` | `tui.frame(opts, fn)` | bordered area + nested cursor scope |
| `tui.list(items, opts)` | `tui.list(items, opts)` | scrollable list, returns picked index or nil |
| `tui.input(prompt, opts?)` | `tui.input(prompt, opts?)` | line input with editing, returns string or nil |
| `tui.progress(pct, opts?)` | `tui.progress(pct, opts?)` | render a progress bar |
| `tui.spinner(state)` | `tui.spinner(state)` | next frame of a spinner |
| `tui.confirm(msg)` | `tui.confirm(msg)` → `Promise<bool>` | y/n prompt |

The helpers compose: `tui.list` is implemented in pure Lua/JS on top of
`tui.run` + `tui.frame`. They live in stdlib (not the cap layer) so an
app can drop in its own replacement easily.

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

These are the reason TUI exists. Each gets a concrete deliverable in
the rollout.

| Tool | What TUI adds | Phase |
|------|--------------|-------|
| `hull doctor --tui` | Live, color-coded subsystems pane; press `r` to re-probe, `c` to copy JSON to clipboard via OSC 52 | Phase 2 |
| `hull dev` (auto-tui when stdout is tty) | Top pane: live request log with filter (`/path`, `s/status`, `m/method`); bottom pane: keybindings + agent sidecar status. `r` reloads app, `q` quits, `e` opens last error | Phase 3 |
| `hull agent context --interactive` | Picker UI for task / level; live preview of the rendered context | Phase 3 |
| `hull agent errors --tui` | Scrollable error list with expand-to-stacktrace | Phase 3 |
| `hull migrate status --tui` | Applied vs pending with diff preview | optional, phase 4 |
| `hull modules available --tui` | Searchable module list with deps + caps shown inline | optional, phase 4 |

Each of these is a real CLI app (Lua, using `hull.tui`) shipped in
`stdlib/lua/hull/`. They get invoked from the existing C command
dispatchers via the same path `hull init` already uses (`stdlib/lua/
hull/init.lua` — a Lua tool module invoked from C). Reusing the cap
layer for our own commands is the validation that the API isn't shaped
weirdly.

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

### Phase 1 — cap layer + raw API

- New `src/hull/cap/tui.c` + header. Termios + ANSI parser + signal
  glue. PTY-based unit tests.
- `mod_tui.c` in both runtimes; bindings for the low-level primitives
  only (no `tui.run` yet).
- New manifest field `tui` (parser + sandbox wiring).
- Module registry entry; `HL_MOD_CAP_TUI` bit; resolver tests.
- E2E: `examples/cli/tui_raw_demo` driven by a script.
- Outcome: An app can `tui.enter / poll / leave` on any default hull
  build. No higher-level helpers yet.

Estimated effort: ~3 days.

### Phase 2 — stdlib helpers + `tui.run`

- `stdlib/lua/hull/tui.lua`, `stdlib/js/hull/tui.js`: `tui.run`,
  `tui.frame`, `tui.list`, `tui.input`, `tui.progress`, `tui.spinner`,
  `tui.confirm`.
- `hull.async(fn)` script-facing helper (detached coroutine), required
  by the run-loop for fire-and-forget async work.
- Convert `hull doctor` to optionally render via TUI when invoked with
  `--tui` and stdout is a tty.
- E2E: `examples/cli/tui_picker`, `tui_dashboard`, `tui_repl`.
- Outcome: First dogfood landed. Apps have a clean immediate-mode API.

Estimated effort: ~2 days.

### Phase 3 — `hull dev` interactive mode + `hull agent` pickers

- `hull dev` detects tty + non-piped + no `--no-tui` and switches to
  the interactive request-log UI by default. Fall back to current
  behavior otherwise.
- `hull agent context --interactive` and `hull agent errors --tui` —
  picker / scroller variants.
- Polish keybindings, color theming (auto-detect dark/light bg via
  OSC 11 query), HiDPI tty handling.
- Outcome: Hull's own CLI feels native.

Estimated effort: ~2 days.

### Phase 4 — docs, examples, optional helpers

- README + AGENTS + CLAUDE.md sections.
- `docs/tui_mode.md` (this doc) updated with concrete numbers.
- Remaining example apps shipped.
- Decide on `hull migrate status --tui`, `hull modules available
  --tui` based on use.
- Final e2e sweep on Linux, macOS, Cosmo.

Estimated effort: ~1 day.

### Total

~1 week of focused work. Almost identical shape to CLI mode itself,
which is intentional — TUI is "CLI mode + alternate screen + input
parser."

## Risk callouts

- **Cosmo PTY support.** APE ships its own libc. `openpty` /
  `forkpty` need verification on cosmo for the test harness. If
  unavailable, gate `tests/hull/cap/test_tui.c` off on cosmo and rely
  on E2E. The runtime itself doesn't need PTY — that's only for
  testing.
- **Windows Terminal compatibility.** TUI is POSIX-first. On
  Cosmopolitan builds running on Windows, raw mode goes through
  cosmo's polyfilled termios; ANSI rendering works on Windows
  Terminal / ConHost (post-2019). Pre-2019 ConHost will display
  garbled output. Decision: don't try to detect; just say "Windows
  Terminal required" in docs.
- **Signal interactions with the event loop.** Keel installs handlers
  for `SIGINT` / `SIGTERM` (graceful shutdown). TUI needs to layer on
  top of these without losing the existing semantics. Plan: TUI's
  handler does termios restore + `siglongjmp` to a known safe point
  if mid-frame, otherwise sets a flag the next `poll` honors. Existing
  Keel handler runs after restore.
- **`hull dev` reload-on-edit + TUI redraw.** Reloading the app
  reinitializes the cap layer, which re-enters alt screen, which
  clears the request log. Mitigation: persist the request log across
  reloads (in the C side, not the Lua side), redraw from it after
  reload completes. Same pattern as the existing dev `.hull/dev.json`
  sidecar.
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

## Open questions

- **Color theme detection.** OSC 11 query (`\x1b]11;?\x07`) returns
  the bg color and we can pick fg accordingly. Works on most modern
  terminals but not all. Fall back: ask the user via manifest? Use
  `$COLORFGBG`? Decision deferred to phase 3.
- **Clipboard.** OSC 52 (`\x1b]52;c;<base64>\x07`) works in most
  modern terminals (iTerm2, kitty, alacritty, recent Windows
  Terminal). xterm requires `allowSendEvents`. Add `tui.clipboard_set(text)`
  as a stdlib helper, document the gotchas.
- **Multi-byte input on legacy terminals.** UTF-8 input pasted as
  multi-byte sequences works through bracketed paste. Direct typing
  of UTF-8 through xterm legacy mode is mostly OK on modern terms but
  needs verification. Plan: rely on bracketed paste for non-ASCII;
  document.
- **Threading.** TUI assumes single-threaded event loop. If a future
  Hull adds true multi-threaded request handling, the cap-layer
  ownership of stdin/stdout/termios needs to be made explicit (the
  TUI thread "owns" the terminal). For now, single-threaded is the
  only mode, so this is a non-issue.

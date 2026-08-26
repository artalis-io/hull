<!-- minimal -->
## Quickstart: terminal UI (TUI) app

Hull ships a TUI capability layer (`tui.*`) plus a higher-level helper
module (`hull.tui`) on top of it. Apps drive a render loop via
`tui.run({draw=…, on_event=…})`, mutate state from events, and
re-draw when the framework invalidates. Background work runs via
`tui.async(fn)`.

```bash
hull init myapp --runtime=lua
cd myapp
# edit app.lua (see below)
hull dev                       # iterate in your terminal
hull build .                   # → build/myapp (signed binary)
./build/myapp                  # run
```

### Minimal TUI app

```lua
-- app.lua
local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

app.main(function(ctx)
    local state = { counter = 0, running = true }

    tui.run({
        draw = function(t)
            t:clear()
            t:style({ bold = true, fg = 0x61AFEF })
            t:print(2, 1, "Hull TUI")
            t:style({})
            t:print(2, 3, "Counter: " .. state.counter)
            t:print(2, 5, "[+] increment   [q] quit")
        end,

        on_event = function(t, ev)
            if ev.type == "key" then
                if ev.key == "+" or ev.key == "=" then
                    state.counter = state.counter + 1
                    t:invalidate()
                elseif ev.key == "q" or ev.key == "ctrl-c" then
                    return "quit"
                end
            end
        end,
    })
    return 0
end)
```

Run `hull dev`. `tui.run` blocks until `on_event` returns `"quit"`
(or stdin closes).

<!-- compact -->
## Manifest declaration

`tui = true` in `app.manifest({})` is required - without it the TUI
capability fails closed. `hull/tui@1` in `modules` makes
`require("hull.tui")` resolvable. The terminal sandbox grants raw
stdin + alternate screen + cursor control; nothing else changes.

## The render loop

`tui.run({draw, on_event, tick_ms, on_tick})`:

- **`draw(t)`** - called on first paint and after each `t:invalidate()`.
  Mutate nothing; just paint. `t.cols`, `t.rows` give terminal size;
  `t:clear()`, `t:move(x,y)`, `t:print(x,y,s)`, `t:style({bold, fg, bg,
  reverse, underline})` write content.
- **`on_event(t, ev)`** - called for keystrokes, mouse, resize. Mutate
  state, optionally `t:invalidate()` to force a repaint, return
  `"quit"` to exit the loop.
- **`tick_ms`** + **`on_tick(t)`** - optional periodic timer for
  animation/refresh without a user event.

Event shapes:
- `{type="key", key="a"}` / `{key="ctrl-c"}` / `{key="enter"}` /
  `{key="up"}` / `{key="esc"}`
- `{type="mouse", x=N, y=N, button="left"|"right"|"middle", pressed=bool}`
- `{type="resize", cols=N, rows=N}`
- `{type="focus", focused=bool}`

## Widgets (hull.tui module)

```lua
tui.frame({ x=1, y=1, w=20, h=10, title="metrics", border="round" },
          function(inner)
    inner:print(1, 1, "anything")
end)

tui.progress(0.5, { width = 20 })         -- → "[##########          ] 50%"
tui.spinner(state.frame)                  -- → ("⠋", next_frame)
```

`border = "single" | "double" | "round" | "none"`. The frame's inner
"surface" you draw into has its own `cols`/`rows`/`print`.

## Background work in TUI apps

```lua
tui.async(function()
    while state.running do
        hull.sleep(250)
        state.metrics.qps = poll_qps()
        -- next event tick will redraw
    end
end)
```

`tui.async(fn)` spawns a coroutine that runs concurrently with the
TUI poll loop. Use it for tickers, background fetches, etc. The
function captures `state` by reference; mutate it freely. No
synchronization needed — the event loop is single-threaded.

## Iteration tip

`hull dev` runs the TUI in your current terminal. On file save, Hull
restarts the process (which restarts the TUI from scratch). For
debug logging that doesn't fight the screen, write to a sidecar file:

```lua
log.info("event: " .. ev.type)  -- → .hull/dev.log
```

Then `tail -f .hull/dev.log` in a second terminal.

<!-- full -->
## Capability bits at runtime

```lua
tui.capabilities()  -- → { truecolor=true, mouse=true, focus=true,
                    --     basic_color=true, kitty_kbd=false, ... }
```

Use this to gate features: don't emit 24-bit colors on a terminal
that only supports 8. The bitmask comes from probing terminfo +
parsing the `TERM` environment variable.

## Mouse input

```lua
tui.run({
    on_event = function(t, ev)
        if ev.type == "mouse" and ev.button == "left" and ev.pressed then
            -- hit-test using ev.x / ev.y
            local clicked_row = ev.y - layout.tasks_y
            if clicked_row >= 1 and clicked_row <= #tasks then
                tasks[clicked_row].done = not tasks[clicked_row].done
                t:invalidate()
            end
        end
    end,
})
```

Mouse mode is opt-in — it adds the `?1006h` SGR-extended-mouse
escape on first event handler that filters for `type="mouse"`.
Disable per-frame via `tui.mouse(false)` if you need a paste-friendly
section.

## Colors and styling

```lua
t:style({
    bold = true, italic = true, underline = true, reverse = false,
    fg = 0x61AFEF,           -- truecolor (24-bit hex)
    fg = tui.color_256(208), -- 256-color palette
    fg = tui.color_basic(1), -- 8 ANSI colors (1=red, 2=green, ...)
})

t:style({})   -- reset everything
```

Hull picks the best representation the terminal supports —
`0x61AFEF` becomes 24-bit on iTerm/Kitty/modern xterm,
256-color-quantized on lesser terminals, ignored on dumb terminals
without warnings.

## Multi-pane layout

Use nested `tui.frame` calls and split the terminal area arithmetically:

```lua
draw = function(t)
    t:clear()
    local mid = math.floor(t.cols * 0.5)
    tui.frame({ x=1, y=2, w=mid-2, h=t.rows-3, title="left" }, function(p)
        -- p.cols, p.rows are the inner dimensions
        p:print(1, 1, "left pane content")
    end)
    tui.frame({ x=mid+1, y=2, w=t.cols-mid-2, h=t.rows-3, title="right" },
              function(p)
        p:print(1, 1, "right pane content")
    end)
end
```

## Forms / text input

`hull.tui` doesn't ship a heavyweight text-widget layer; for input,
maintain your own string state in `state`, draw a cursor, and handle
keys:

```lua
state.input = ""

draw = function(t)
    t:print(2, 5, "Name: " .. state.input .. "_")
end,

on_event = function(t, ev)
    if ev.type == "key" then
        if ev.key == "enter" then
            submit(state.input)
        elseif ev.key == "backspace" then
            state.input = state.input:sub(1, -2)
            t:invalidate()
        elseif #ev.key == 1 then       -- printable
            state.input = state.input .. ev.key
            t:invalidate()
        end
    end
end,
```

## Testing TUI apps

TUI logic is harder to unit-test than HTTP — there's no in-process
harness for keystroke sequences. Test the state mutations separately:

```lua
-- app.lua: factor pure state transitions out
function increment_counter(state) state.counter = state.counter + 1 end
function reset_counter(state)     state.counter = 0 end

-- tests/test_state.lua
local t = require("hull.test")
t.case("increment", function()
    local s = { counter = 0 }
    increment_counter(s)
    t.assert_eq(s.counter, 1)
end)
```

Then the `on_event` handler is just dispatch - tested by inspection.

## Working examples in the repo

- `examples/tui_dashboard/app.lua` - multi-pane layout, mouse, async
- `examples/tui_chat/app.lua` - scrollable message list, input field
- `examples/tui_modular/app.lua` - splitting state and rendering across files

## See also

- `hull agent context --task=quickstart-web` - server-side variant
- `hull agent context --task=quickstart-cli` - pure-CLI variant
- `include/hull/cap/tui.h` - raw capability surface (low-level)
- `stdlib/lua/hull/tui.lua` - the helper module

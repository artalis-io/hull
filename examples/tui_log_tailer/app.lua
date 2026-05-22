-- TUI log tailer — tail -f style follower over hull.fs.
--
-- Reads a file from the manifest's fs.read allowlist, polls its
-- mtime + size on each tick, displays new lines as they appear.
-- A background tui.async coroutine tracks the file even while
-- the main loop awaits keystrokes — same async-yield property the
-- async_proof demo validates.
--
-- Run:
--   echo "first line" > /tmp/hull-tail.log
--   hull run app.lua
--   # in another terminal:
--   echo "second" >> /tmp/hull-tail.log
--
-- Keys: q/esc quits.
--
-- The watched file is hard-coded to /tmp/hull-tail.log so the
-- manifest's fs.read allowlist stays minimal. Adjust to taste.

local tui = require("hull.tui")
local fs  = require("hull.fs")

local LOG_FILE = "/tmp/hull-tail.log"

app.manifest({
    tui = true,
    modules = { "hull/tui@1", "hull/fs@1" },
    fs = { read = { LOG_FILE } },
})

-- ── State ─────────────────────────────────────────────────────────

local state = {
    last_size = 0,
    lines     = {},     -- recent lines (newest at the end)
    error     = nil,
    pulled    = 0,
    running   = true,
}

local MAX_LINES = 500

local function push_line(line)
    state.lines[#state.lines + 1] = line
    if #state.lines > MAX_LINES then
        table.remove(state.lines, 1)
    end
end

-- Returns true if new content was read.
local function poll_file()
    local ok, content = pcall(fs.read, LOG_FILE)
    if not ok then
        state.error = tostring(content)
        return false
    end
    state.error = nil
    if #content == state.last_size then return false end
    -- Drop everything we already saw and split the new tail into lines.
    local new = content:sub(state.last_size + 1)
    state.last_size = #content
    for line in (new .. "\n"):gmatch("([^\n]*)\n") do
        if line ~= "" then push_line(line) end
    end
    state.pulled = state.pulled + 1
    return true
end

-- ── Background tailer ────────────────────────────────────────────
--
-- Polls the file every 200ms even while the main coroutine awaits
-- a key in tui.poll(-1). Demonstrates that tui.poll yields to the
-- event loop — without this, the file wouldn't be re-read until
-- the user pressed something.

tui.async(function()
    -- Initial read: count what's already there as "seen" so we
    -- only display lines added after we start.
    local ok, content = pcall(fs.read, LOG_FILE)
    if ok then state.last_size = #content end

    while state.running do
        hull.sleep(200)
        poll_file()
    end
end)

-- ── Render ────────────────────────────────────────────────────────

local function render(t)
    t:clear()

    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, 1, "hull tui-tailer   " .. LOG_FILE ..
                  "   " .. #state.lines .. " lines   polls=" .. state.pulled)
    t:style({})

    if state.error then
        t:style({ fg = 0xE06C75, bold = true })
        t:print(1, 3, "Error: " .. state.error)
        t:style({})
        t:style({ fg = 0x888888, italic = true })
        t:print(1, 5, "(checked again on next tick — q to quit)")
        t:style({})
        t:style({ fg = 0x888888 })
        t:print(1, t.rows, "q:quit")
        t:style({})
        return
    end

    local body_h = t.rows - 3
    local start = math.max(1, #state.lines - body_h + 1)
    for i = 0, body_h - 1 do
        local idx = start + i
        if idx > #state.lines then break end
        local line = state.lines[idx]
        if #line > t.cols then line = line:sub(1, t.cols) end
        t:print(1, 3 + i, line)
    end

    if #state.lines == 0 then
        t:style({ fg = 0x888888, italic = true })
        t:print(1, 3, "(file is empty or hasn't been appended to since startup)")
        t:print(1, 4, 'try: echo "hello" >> ' .. LOG_FILE)
        t:style({})
    end

    t:style({ fg = 0x888888 })
    t:print(1, t.rows, "q:quit")
    t:style({})
end

app.main(function(ctx)
    tui.run({
        draw     = render,
        on_event = function(ev)
            if ev.kind ~= "key" then return end
            if ev.key == "q" or ev.key == "escape" or ev.key == "ctrl+c" then
                return "quit"
            end
        end,
        tick_ms  = 200,
    })
    state.running = false
    hull.sleep(220)  -- let the background coroutine see running=false
    ctx.stdout:write("tailer: read " .. #state.lines ..
                     " lines from " .. LOG_FILE .. "\n")
    return 0
end)

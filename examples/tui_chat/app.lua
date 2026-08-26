-- TUI chat - split-pane example.
--
-- Two regions: the message log fills the top (with a single-line
-- border via tui.frame), and a one-line input prompt sits below.
-- Each user message spawns a background "bot" reply via tui.async
-- that hull.sleep()s for a random 200–600ms and then appends.
-- Demonstrates that the input stays responsive while bot replies
-- are pending in the background.
--
-- Run:  hull run app.lua
--
-- Keys (at the input):
--   enter        send message (spawn a bot reply)
--   ←/→/home/end edit cursor
--   backspace    delete left
--   q / esc      quit (when input is empty)

local tui = require("hull.tui")

app.manifest({
    tui = true,
    modules = { "hull/tui@1" },
})

-- Canned responses keyed on a substring in the user's message.
-- Falls back to a deterministic echo when nothing matches.
local SCRIPTS = {
    { match = "hi",      reply = "hello! 👋" },
    { match = "hello",   reply = "hi there." },
    { match = "help",    reply = "type anything, or 'bye' to quit." },
    { match = "thank",   reply = "you're welcome." },
    { match = "bye",     reply = "see you. (press q to actually quit)" },
    { match = "weather", reply = "I don't know, ask a real bot 😅" },
    { match = "time",    reply = "I have no clock either." },
}

local function reply_for(msg)
    local lower = msg:lower()
    for _, s in ipairs(SCRIPTS) do
        if lower:find(s.match, 1, true) then return s.reply end
    end
    return "echo: " .. msg
end

-- ── State ─────────────────────────────────────────────────────────

local messages = {}  -- { { who = "you" | "bot", text = "..." }, ... }
local input_buf = ""
local input_cur = 1
local pending   = 0  -- bot replies in flight

local function push(who, text)
    messages[#messages + 1] = { who = who, text = text }
end

-- ── Render ────────────────────────────────────────────────────────

local function render(t)
    t:clear()

    local input_y = t.rows - 1

    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, 1, "hull tui-chat   " .. #messages .. " messages" ..
                  (pending > 0 and ("   bot typing… (" .. pending .. ")") or ""))
    t:style({})

    -- Message frame.
    tui.frame({
        x = 1, y = 2, w = t.cols, h = input_y - 3,
        title = "log",
        border = "single",
        style = { fg = 0x444444 },
    }, function(inner)
        local visible = inner.h
        local start = math.max(1, #messages - visible + 1)
        for i = 0, visible - 1 do
            local idx = start + i
            if idx > #messages then break end
            local m = messages[idx]
            local label = (m.who == "you") and "you:" or "bot:"
            local color = (m.who == "you") and 0x61AFEF or 0xE5C07B
            local text  = label .. " " .. m.text
            if #text > inner.w then text = text:sub(1, inner.w) end
            t:style({ fg = color })
            inner:print(1, i + 1, text:sub(1, 4))
            t:style({})
            inner:print(6, i + 1, text:sub(6))
        end
    end)

    -- Input line.
    t:style({ bold = true, fg = 0x61AFEF })
    t:print(1, input_y, "> ")
    t:style({})
    t:print(3, input_y, input_buf)
    t:move(3 + input_cur - 1, input_y)
end

-- ── Bot ───────────────────────────────────────────────────────────

local function spawn_bot_reply(user_msg)
    pending = pending + 1
    tui.async(function()
        -- 200–500ms "thinking" pause (math.random is available in
        -- the sandbox; time.now_ms is not - it needs a require).
        hull.sleep(200 + math.random(0, 300))
        push("bot", reply_for(user_msg))
        pending = pending - 1
    end)
end

-- ── Event handling ────────────────────────────────────────────────

local function on_event(ev)
    if ev.kind ~= "key" then return end
    local k = ev.key
    if k == "enter" then
        if input_buf ~= "" then
            push("you", input_buf)
            spawn_bot_reply(input_buf)
            input_buf = ""
            input_cur = 1
        end
    elseif k == "left"  then input_cur = math.max(1, input_cur - 1)
    elseif k == "right" then input_cur = math.min(#input_buf + 1, input_cur + 1)
    elseif k == "home"  then input_cur = 1
    elseif k == "end"   then input_cur = #input_buf + 1
    elseif k == "backspace" then
        if input_cur > 1 then
            input_buf = input_buf:sub(1, input_cur - 2) .. input_buf:sub(input_cur)
            input_cur = input_cur - 1
        end
    elseif k == "ctrl+u" then input_buf = input_buf:sub(input_cur); input_cur = 1
    elseif k == "ctrl+k" then input_buf = input_buf:sub(1, input_cur - 1)
    elseif k == "ctrl+c" then return "quit"
    elseif (k == "q" or k == "escape") and input_buf == "" then return "quit"
    elseif ev.codepoint and ev.codepoint >= 0x20 and not ev.ctrl and not ev.alt then
        input_buf = input_buf:sub(1, input_cur - 1) .. ev.key .. input_buf:sub(input_cur)
        input_cur = input_cur + #ev.key
    end
end

app.main(function(ctx)
    -- Seed with a welcome line.
    push("bot", "hi! type 'help' for a hint.")

    tui.run({
        draw     = render,
        on_event = on_event,
        tick_ms  = 100,    -- 10Hz refresh so async bot replies show up promptly
    })
    -- Let any in-flight bot replies finish.
    while pending > 0 do hull.sleep(50) end
    ctx.stdout:write("chat: " .. #messages .. " messages exchanged\n")
    return 0
end)

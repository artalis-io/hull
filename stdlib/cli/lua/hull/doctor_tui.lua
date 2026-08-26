-- stdlib/lua/hull/doctor_tui.lua - interactive `hull doctor`.
--
-- Run via `hull doctor --tui`. The C dispatcher (commands/doctor.c)
-- routes through hull_tool when --tui is set, which launches an
-- unsandboxed Lua VM and requires this module.
--
-- The data is sourced from `tool.doctor_json()` (mod_tool.c) - the
-- exact same JSON the `hull doctor --json` command emits, parsed
-- with hull.json.decode. We don't reimplement the C-side probes.
--
-- Keys:
--   r       re-probe (re-read the JSON, re-render)
--   c       copy the JSON payload to the system clipboard (OSC 52)
--   q/Esc   quit
--
-- SPDX-License-Identifier: AGPL-3.0-or-later

local tui = require("hull.tui")
local json = require("hull.json")

-- ── Probe + parse ──────────────────────────────────────────────────

local function probe()
    local raw = tool.doctor_json()
    local ok, parsed = pcall(json.decode, raw)
    if not ok then
        return nil, raw, "json.decode failed: " .. tostring(parsed)
    end
    return parsed, raw
end

-- ── Style helpers ──────────────────────────────────────────────────

-- Pick fg colors keyed to the current theme. Dark gets bright greens
-- / reds; light gets muted hues so contrast stays comfortable.
local function colors_for_theme(theme)
    if theme == "light" then
        return {
            ok    = 0x006400,  -- dark green
            warn  = 0xB8860B,  -- dark goldenrod
            err   = 0x8B0000,  -- dark red
            dim   = 0x666666,
            head  = 0x000080,  -- navy
        }
    end
    -- "dark" / "unknown"
    return {
        ok    = 0x00C000,
        warn  = 0xE5C07B,
        err   = 0xE06C75,
        dim   = 0x888888,
        head  = 0x61AFEF,
    }
end

-- One row label + status glyph + detail. Returns the rendered line
-- length (for the next column alignment).
local function row(t, x, y, label, ok, detail, palette)
    local glyph = ok and "✓" or "✗"
    local color = ok and palette.ok or palette.err
    t:print(x, y, label)
    t:style({ fg = color, bold = true })
    t:print(x + 14, y, glyph)
    t:style({})
    if detail then
        t:style({ fg = palette.dim })
        t:print(x + 16, y, tostring(detail))
        t:style({})
    end
end

-- Recommendation summary in the bottom-right based on ready state.
local function ready_message(d)
    -- d.compilers is an array of {name, path}; d.platform_embedded
    -- is "multi-arch"/"single-arch"/"none". The hull binary returns
    -- 0 if both (platform != none AND any compiler), per
    -- doctor.c::hl_cmd_doctor.
    local has_compiler = false
    for _, c in ipairs(d.compilers or {}) do
        if c.path then has_compiler = true; break end
    end
    local has_platform = d.platform_embedded and d.platform_embedded ~= "none"
    if has_platform and has_compiler then
        return "ready", "hull build is ready end-to-end."
    elseif not has_platform then
        return "no-platform",
               "no embedded platform library. Build with `make EMBED_PLATFORM=1`."
    else
        return "no-compiler",
               "no C compiler in PATH. Install cc / gcc / clang or use the embedded TCC."
    end
end

-- ── Renderer ────────────────────────────────────────────────────────

local function render(t, d, last_action, palette)
    t:clear()

    -- Title bar.
    t:style({ fg = palette.head, bold = true })
    t:print(1, 1, "hull doctor   " .. (d.version or "?") ..
                  "   " .. (d.runtime or "?") ..
                  "   " .. (d.platform or "?") ..
                  "   " .. (d.build or "?"))
    t:style({})

    local y = 3
    t:style({ bold = true }); t:print(1, y, "Platform"); t:style({})
    y = y + 1
    local emb = d.platform_embedded or "none"
    row(t, 3, y, "embedded",
        emb ~= "none",
        emb,
        palette)
    y = y + 2

    t:style({ bold = true }); t:print(1, y, "Compilers"); t:style({})
    y = y + 1
    for _, c in ipairs(d.compilers or {}) do
        row(t, 3, y, c.name or "?", c.path ~= nil, c.path or "not found", palette)
        y = y + 1
    end
    y = y + 1

    t:style({ bold = true }); t:print(1, y, "Subsystems"); t:style({})
    y = y + 1
    for _, name in ipairs({ "db", "wasm", "gpu", "http" }) do
        local on = d.subsystems and d.subsystems[name]
        row(t, 3, y, name, on and true or false, on and "enabled" or "disabled", palette)
        y = y + 1
    end
    if d.tcc_embedded ~= nil then
        row(t, 3, y, "tcc", d.tcc_embedded, d.tcc_embedded and "embedded" or "not embedded", palette)
        y = y + 1
    end
    y = y + 1

    t:style({ bold = true }); t:print(1, y, "Compute"); t:style({})
    y = y + 1
    local cm = d.compute or {}
    row(t, 3, y, "wasm",
        cm.wasm_enabled,
        cm.wasm_enabled and "enabled" or "disabled",
        palette)
    y = y + 1
    row(t, 3, y, "gpu",
        cm.gpu_enabled,
        cm.gpu_enabled and "enabled" or "disabled",
        palette)
    y = y + 1
    row(t, 3, y, "wamrc",
        cm.wamrc ~= nil,
        cm.wamrc or "not found",
        palette)
    y = y + 1
    row(t, 3, y, "aot_ready",
        cm.aot_ready,
        cm.aot_ready and "hull build → AOT" or "interpreter only",
        palette)
    y = y + 2

    t:style({ bold = true }); t:print(1, y, "CA bundle"); t:style({})
    y = y + 1
    local cab = d.ca_bundle or {}
    row(t, 3, y, "system", cab.system ~= nil, cab.system or "not found", palette)
    y = y + 1
    row(t, 3, y, "embedded",
        cab.embedded,
        cab.embedded and (cab.embedded_label or "embedded") or "not embedded",
        palette)
    y = y + 2

    -- Summary line.
    local state, msg = ready_message(d)
    local sty = (state == "ready") and { fg = palette.ok,  bold = true }
              or                        { fg = palette.warn, bold = true }
    t:style(sty)
    t:print(1, y, state == "ready" and "READY: " or "ATTN: ")
    t:style({})
    t:print(9, y, msg)
    y = y + 2

    -- Last-action feedback (e.g., "copied to clipboard").
    if last_action then
        t:style({ fg = palette.dim, italic = true })
        t:print(1, y, last_action)
        t:style({})
    end

    -- Key bar pinned to the bottom row.
    t:style({ fg = palette.dim })
    t:print(1, t.rows, "r:reprobe   c:copy json   q/Esc:quit   theme=" ..
                       tui.theme())
    t:style({})
end

-- ── Main ───────────────────────────────────────────────────────────

local d, raw, err = probe()
if not d then
    tool.stderr("hull doctor --tui: " .. tostring(err) .. "\n")
    tool.exit(1)
end

local palette = colors_for_theme(tui.theme())
local last_action = nil

tui.run({
    draw = function(t)
        render(t, d, last_action, palette)
    end,
    on_event = function(ev)
        if ev.kind ~= "key" then return end
        local k = ev.key
        if k == "q" or k == "escape" or k == "ctrl+c" then
            return "done"
        elseif k == "r" then
            local nd, nraw, nerr = probe()
            if nd then
                d, raw = nd, nraw
                palette = colors_for_theme(tui.theme())
                last_action = "(re-probed)"
            else
                last_action = "(re-probe failed: " .. tostring(nerr) .. ")"
            end
        elseif k == "c" then
            local caps = tui.caps()
            if caps.clipboard then
                local ok, cerr = pcall(tui.clipboard_set, raw)
                if ok then
                    last_action = "(copied " .. #raw .. " bytes to clipboard)"
                else
                    last_action = "(clipboard failed: " .. tostring(cerr) .. ")"
                end
            else
                last_action = "(clipboard not supported by this terminal)"
            end
        end
    end,
    tick_ms = -1,
})

-- Exit-code parity with `hull doctor`: 0 if ready, 1 otherwise.
local state = ready_message(d)
tool.exit(state == "ready" and 0 or 1)

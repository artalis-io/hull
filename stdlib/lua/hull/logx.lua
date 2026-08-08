--- Contextual logging: bind a set of fields once, log them on every line.
--
-- A thin, pure layer over the built-in `hull.log`. `logx.with{...}` returns a
-- child logger whose `info`/`warn`/`error`/`debug` append the bound fields to
-- the message in logfmt form (`key=value`, quoted when needed), so per-request
-- context (request_id, user, route) rides every log line without threading it
-- through every call.
--
--     local logx = require("hull.logx")
--     local rl = logx.with({ request_id = id, user = uid })
--     rl.info("handled")     -- -> "handled request_id=... user=..."
--     rl.with({ step = 2 }).warn("slow")  -- children compose
--
-- CAVEAT (source tag): `hull.log` tags each line by the CALLER's source, and
-- the two runtimes resolve that differently for a wrapped call. In Lua a wrapper
-- line still tags `[app]`; in JS it tags `[hull:js]` (QuickJS reports the
-- immediate module, `hull:logx`). The message + fields are identical either way
-- - only the JS source tag shifts. When you need a guaranteed `[app]` tag in
-- BOTH runtimes, use the escape hatch: `log.info("handled" .. logx.fields({...}))`
-- - the app calls `log` directly, so the tag stays `[app]`, and `logx.fields`
-- just formats. (A future C-level `log.with` would make the bound logger tag
-- `[app]` in JS too.)
--
-- @module hull.logx
-- @license AGPL-3.0-or-later

local log = require("hull.log")

local logx = {}

local LEVELS = { "info", "warn", "error", "debug" }

-- Format a fields table as a leading-space logfmt fragment: " k=v k2=v2".
-- Keys are sorted for deterministic, cross-runtime-identical output. A value
-- containing whitespace, a quote, or '=' is double-quoted with '"' escaped.
local function fmt(fields)
    if not fields then return "" end
    local keys = {}
    for k in pairs(fields) do keys[#keys + 1] = tostring(k) end
    table.sort(keys)
    if #keys == 0 then return "" end
    local parts = {}
    for _, k in ipairs(keys) do
        local v = fields[k]
        local s
        if type(v) == "boolean" then
            s = v and "true" or "false"
        else
            s = tostring(v)
        end
        if s:find('[%s"=]') then
            s = '"' .. s:gsub('"', '\\"') .. '"'
        end
        parts[#parts + 1] = k .. "=" .. s
    end
    return " " .. table.concat(parts, " ")
end

--- Format a fields table into a leading-space logfmt fragment. Escape hatch for
-- keeping the `[app]` source tag: `log.info("msg" .. logx.fields({...}))`.
-- @tparam[opt] table fields
-- @treturn string  e.g. ` request_id=abc user=42` (empty string for no fields).
function logx.fields(fields)
    return fmt(fields)
end

local function make(fields)
    local self = {}
    for _, lvl in ipairs(LEVELS) do
        self[lvl] = function(msg)
            log[lvl](tostring(msg == nil and "" or msg) .. fmt(fields))
        end
    end
    --- Return a new child logger with `more` merged over the current fields.
    function self.with(more)
        local merged = {}
        for k, v in pairs(fields) do merged[k] = v end
        if more then for k, v in pairs(more) do merged[k] = v end end
        return make(merged)
    end
    return self
end

--- Create a bound logger carrying `fields`.
-- @tparam[opt] table fields
-- @treturn table  A logger with `info`/`warn`/`error`/`debug` and `with`.
function logx.with(fields)
    return make(fields or {})
end

-- Bare pass-throughs (no bound fields), so `logx` can stand in for `log`.
for _, lvl in ipairs(LEVELS) do logx[lvl] = log[lvl] end

return logx

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
--     rl.info("handled")     -- -> 'msg="handled" request_id=... user=...'
--
-- The message is the line's leading `msg="..."` field, quoted and escaped as
-- a value (audit 11): written bare, a message carrying ` user=admin` forged a
-- field for any logfmt reader. The bare `logx.info(msg)` (no fields) writes
-- the same `msg="..."` field, and a bound field named `msg` goes out as `_msg`
-- so it cannot override the message (audit 12).
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

local _logfmt = require("hull.web._logfmt")

local logx = {}

local LEVELS = { "info", "warn", "error", "debug" }

-- Format a fields table as a leading-space logfmt fragment: " k=v k2=v2".
-- Keys are sorted for deterministic, cross-runtime-identical output. Value
-- escaping + quoting is the shared hull.web._logfmt rule (keys reduced to
-- [A-Za-z0-9_.-], values with \ " and control bytes escaped, quoted when
-- needed) - the logger middleware uses the same one. The message's control
-- bytes are escaped too (_logfmt.message), so it cannot forge a line either.
local function fmt(fields)
    if not fields then return "" end
    -- Sorted by the key's string form, the value read through the ORIGINAL
    -- key (a numeric key's value was looked up as its string and logged nil).
    local keys, orig = {}, {}
    for k in pairs(fields) do
        local sk = tostring(k)
        if orig[sk] == nil then keys[#keys + 1] = sk end
        orig[sk] = k
    end
    table.sort(keys)
    if #keys == 0 then return "" end
    local parts = {}
    for _, k in ipairs(keys) do
        -- `msg` is the message's key (the line's leading msg="..."), so a
        -- bound field that reduces to it goes out as `_msg` (audit 12): a
        -- second msg= pair overrode the message for a reader that keeps the
        -- last value of a key.
        local name = k
        if _logfmt.key(k) == "msg" then name = "_msg" end
        parts[#parts + 1] = _logfmt.pair(name, fields[orig[k]])
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
            log[lvl](_logfmt.message(msg == nil and "" or msg) .. fmt(fields))
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

-- Bare levels (no bound fields), so `logx` can stand in for `log`. The
-- message goes through the same msg="..." quoting as a bound logger's (audit
-- 12): passed straight to `log`, a bare logx.info carried a raw newline or a
-- forged ` user=admin` field into the line.
for _, lvl in ipairs(LEVELS) do
    logx[lvl] = function(msg)
        log[lvl](_logfmt.message(msg == nil and "" or msg))
    end
end

return logx

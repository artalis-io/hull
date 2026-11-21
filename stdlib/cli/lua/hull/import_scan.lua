--
-- hull.import_scan - the static require / import scanners shared by
-- `hull modules analyze` (hull.analyze) and `hull build`.
--
-- scan_lua(text) / scan_js(text) skip comments and string-literal text and
-- return two values:
--   - a set of the Hull modules the text uses: { ["hull/crypto"] = true }
--     (require("hull.X") / import ... from "hull:X", and the `app` methods
--     a module adds - app.get: hull/http-server, ...)
--   - a list of the relative specifiers it requires / imports, in order:
--     require("./x") / require("../x") (Lua); import ... from "./x",
--     export ... from "./x", import "./x", import("./x") (JS / TypeScript).
--     A TypeScript `import type` / `export type` is erased, so it is left
--     out of both.
-- Indirect forms (require("./" .. name)) are not seen.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local M = {}

-- ── app decorations ────────────────────────────────────────────────
--
-- The `app` methods a declared module adds (mod_app.c in each runtime): an
-- app that calls one uses that module, and without it the call raises
-- ("attempt to call a nil value" / "not a function").
local DECORATIONS = {
    get = "hull/http-server", post = "hull/http-server", put = "hull/http-server",
    delete = "hull/http-server", del = "hull/http-server", patch = "hull/http-server",
    options = "hull/http-server", use = "hull/http-server", use_post = "hull/http-server",
    usePost = "hull/http-server", router = "hull/http-server",
    every = "hull/timers", daily = "hull/timers",
    sse = "hull/web/sse",
    ws = "hull/web/ws-server",
    transform = "hull/build-plugin",
}

-- Whether the byte before position i continues an identifier (so a match
-- at i is inside a longer name: `myapp.get`, `x.require`).
local function ident_before(text, i)
    return i > 1 and text:sub(i - 1, i - 1):find("[%w_$.:]") ~= nil
end

-- At position i (code, not a string or comment): an `app.NAME(` call of a
-- decoration records its module in `out`. Returns the next position.
local function scan_decoration(text, i, out)
    if not ident_before(text, i) then
        local s, e, name = text:find("^app%s*[%.:]%s*([%a_][%w_]*)%s*%(", i)
        if s and DECORATIONS[name] then
            out[DECORATIONS[name]] = true
            return e + 1
        end
    end
    return i + 1
end

local function is_relative(spec)
    return spec:sub(1, 2) == "./" or spec:sub(1, 3) == "../"
end

-- A quoted specifier: no quote, backslash or newline inside (an escaped
-- name is not read; it is a string the scanner then skips).
local QSPEC = "([\"'])([^\"'\\\n]*)%1"

-- ── Lua scanner: strip comments + strings, find require() calls ─────
--
-- State machine walks one character at a time. Outside any string or
-- comment, it spots `require ( "NAME" )` / `require "NAME"` (any
-- whitespace): "hull.X" adds "hull/X..." (dots -> slashes) to the set, a
-- relative NAME goes to the list.
--
-- Recognised hide states:
--   single-quote string  '..'    backslash escapes
--   double-quote string  ".."    backslash escapes
--   long bracket string  [[..]]  also [=[..]=] (any level)
--   line comment         -- ..   to end of line (but not --[[ ..)
--   block comment        --[[ .. ]]   also --[=[ .. ]=]

function M.scan_lua(text)
    local out, rel = {}, {}
    local i = 1
    local n = #text
    while i <= n do
        local c = text:sub(i, i)

        -- line comment / block comment
        if c == '-' and text:sub(i, i + 1) == '--' then
            i = i + 2
            -- Check for long-bracket block comment opener
            local level = text:match("^(=*)%[", i)
            if level and text:sub(i + #level, i + #level) == '[' then
                local close = ']' .. level .. ']'
                local e = text:find(close, i + #level + 1, true)
                i = e and (e + #close) or (n + 1)
            else
                -- line comment
                local e = text:find("\n", i, true)
                i = e and (e + 1) or (n + 1)
            end

        -- long-bracket string literal
        elseif c == '[' then
            local level = text:match("^=*", i + 1) or ""
            if text:sub(i + 1 + #level, i + 1 + #level) == '[' then
                local close = ']' .. level .. ']'
                local e = text:find(close, i + 2 + #level, true)
                i = e and (e + #close) or (n + 1)
            else
                i = i + 1
            end

        -- single-/double-quoted string
        elseif c == '"' or c == "'" then
            local q = c
            i = i + 1
            while i <= n do
                local ch = text:sub(i, i)
                if ch == '\\' then i = i + 2
                elseif ch == q then i = i + 1 break
                else i = i + 1 end
            end

        else
            -- `require ( "NAME" )` or `require "NAME"`, at a word start.
            local s, e, name, _
            if not ident_before(text, i) then
                s, e, _, name = text:find("^require%s*%(%s*" .. QSPEC .. "%s*%)", i)
                if not s then
                    s, e, _, name = text:find("^require%s*" .. QSPEC, i)
                end
            end
            local modname = name and name:match("^hull%.([%w%._%-]+)$")
            if modname then
                -- Normalize dots → slashes for canonical form.
                out[(("hull/" .. modname):gsub("%.", "/"))] = true
                i = e + 1
            elseif name and is_relative(name) then
                rel[#rel + 1] = name
                i = e + 1
            else
                i = scan_decoration(text, i, out)
            end
        end
    end
    return out, rel
end

-- ── JS scanner ──────────────────────────────────────────────────────
--
-- Recognised hide states:
--   "..", '..', `..`  string/template-literal (template literals may
--                     contain ${...} interpolations; we still want to
--                     bail on the closing backtick, ignoring nested
--                     braces for this v1)
--   // .. to EOL      line comment
--   /* .. */          block comment

function M.scan_js(text)
    local out, rel = {}, {}
    local i = 1
    local n = #text
    while i <= n do
        local c = text:sub(i, i)

        -- line comment
        if c == '/' and text:sub(i + 1, i + 1) == '/' then
            local e = text:find("\n", i, true)
            i = e and (e + 1) or (n + 1)

        -- block comment
        elseif c == '/' and text:sub(i + 1, i + 1) == '*' then
            local e = text:find("*/", i + 2, true)
            i = e and (e + 2) or (n + 1)

        -- string / template literal
        elseif c == '"' or c == "'" or c == '`' then
            local q = c
            i = i + 1
            while i <= n do
                local ch = text:sub(i, i)
                if ch == '\\' then i = i + 2
                elseif ch == q then i = i + 1 break
                else i = i + 1 end
            end

        else
            -- `import ... from "SPEC"` - capture only the module string.
            -- Handles `import x from`, `import {x} from`, `import * as x from`
            -- and a dynamic `import("SPEC")`; for relative specifiers also
            -- `import "SPEC"` and `export ... from "SPEC"` (a Hull module is
            -- counted as used only through the first forms, as before).
            -- A TypeScript `import type` / `export type` is erased, so it
            -- needs no declaration and loads nothing: skipped.
            local s, e, spec, _
            local erased, rel_only = false, false
            if not ident_before(text, i) then
                s, e, _, spec = text:find("^import[%s%w_${},*]+from%s*" .. QSPEC, i)
                if s and text:find("^import%s+type[%s{*]", i) then
                    erased = true
                end
                if not s then
                    -- Dynamic `import("SPEC")`
                    s, e, _, spec = text:find("^import%s*%(%s*" .. QSPEC .. "%s*%)", i)
                end
                if not s then
                    -- Side-effect `import "SPEC"`
                    s, e, _, spec = text:find("^import%s*" .. QSPEC, i)
                    rel_only = s ~= nil
                end
                if not s then
                    s, e, _, spec = text:find("^export[%s%w_${},*]+from%s*" .. QSPEC, i)
                    if s and text:find("^export%s+type[%s{*]", i) then
                        erased = true
                    end
                    rel_only = s ~= nil
                end
            end
            local modname = spec and spec:match("^hull:([%w_:%-]+)$")
            if modname and not rel_only then
                if not erased then
                    out[(("hull/" .. modname):gsub(":", "/"))] = true
                end
                i = e + 1
            elseif spec then
                if not erased and is_relative(spec) then rel[#rel + 1] = spec end
                i = e + 1
            else
                i = scan_decoration(text, i, out)
            end
        end
    end
    return out, rel
end

return M

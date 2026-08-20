--
-- test_official_lua54.lua - the official Lua 5.4.7 test suite as a parser-scoped conformance
-- corpus (docs/lua_official_tests_design.md). The C harness verifies the vendored corpus
-- (manifest schema / pinned Lua version / archive SHA / per-case source_hash / cases<->manifest
-- bijection) and injects the VERIFIED bytes as HULL_LUA54_CORPUS = { {path, source}, ... }; this
-- script runs the two oracles and buckets.
--
--   official .lua bytes -> vendored Lua 5.4.7 load(bytes, "@name", "t")   (compile-only)
--                       -> hull.source.lua parser
--
-- Directional model (identical to test_conformance.lua): a Hull syntax reject MUST imply a load
-- reject (false-reject is the key bug); Hull's three-state verdict is reject / clean /
-- indeterminate. For THIS release-matched corpus Hull claims FULL Lua 5.4 support, so EVERY
-- non-agree outcome -- false-reject, false-accept, unsupported, indeterminate, and even a
-- top-level static-semantic divergence -- is gated at ZERO. There is NO per-path expectation
-- mechanism: a future divergence fails loudly for a maintainer to review (never silently
-- accepted). main.lua (bare '#' first line) is a mutual reject == agreement.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- luacheck: read globals HULL_LUA54_CORPUS (injected by the C harness)
local lua = require("hull.source.lua")
local corpus = HULL_LUA54_CORPUS or {}

local pass, fail = 0, 0
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name) end
end

local LIMITS = { max_bytes = 64 * 1024 * 1024, max_tokens = 5000000,
                 max_comments = 5000000, max_depth = 2000 }

-- The SAME explicit '#!' normalization the harness already uses (Hull's lexer skips a leading
-- '#!' shebang; load(string) does not). Applied symmetrically to the oracle input only. A bare
-- '#' first line (main.lua) is NOT stripped -- Hull rejects it, so does load, they agree.
local function oracle_src(src)
    if src:sub(1, 2) == "#!" then
        local nl = src:find("\n", 1, true)
        return nl and src:sub(nl + 1) or ""
    end
    return src
end

local function hull_state(unit)
    if type(unit) ~= "table" or type(unit.diagnostics) ~= "table" then return "indeterminate", "no unit" end
    local syntax, indet, detail = 0, 0, nil
    for _, d in ipairs(unit.diagnostics) do
        local code = d.code or ""
        if code == "lua.syntax" or code == "lua.unsupported" then
            syntax = syntax + 1; detail = detail or (d.message or code)
        elseif code == "lua.internal" or code:find("^lua%.limit%.") then
            indet = indet + 1; detail = detail or (code .. ": " .. (d.message or ""))
        end
    end
    if indet > 0 then return "indeterminate", detail end
    if syntax > 0 then return "reject", detail end
    return "clean", nil
end

-- Range round-trip: half-open bounds, exact-slice text(), non-empty real constructs, child
-- nesting. Identical policy to test_conformance.lua's check_ranges.
local function child_nodes(node)
    local out = {}
    local function collect(v)
        if type(v) ~= "table" then return end
        if v.kind then out[#out + 1] = v; return end
        for k, e in pairs(v) do if k ~= "range" then collect(e) end end
    end
    for k, v in pairs(node) do
        if k ~= "range" and k ~= "kind" and k ~= "annotation_list" and k ~= "annotations" then collect(v) end
    end
    return out
end
local function ranges_ok(unit, src)
    local maxoff = #src + 1
    local bad = nil
    local function visit(n)
        if bad then return end
        local r = n.range
        if type(r) ~= "table" or type(r.start) ~= "number" or type(r.stop) ~= "number"
           or r.start < 1 or r.stop < r.start or r.stop > maxoff then
            bad = "range oob kind=" .. tostring(n.kind); return
        end
        if unit:text(n) ~= src:sub(r.start, r.stop - 1) then bad = "text~=slice kind=" .. tostring(n.kind); return end
        if r.stop == r.start and n.kind ~= "chunk" then bad = "empty range kind=" .. tostring(n.kind); return end
        for _, c in ipairs(child_nodes(n)) do
            if type(c.range) ~= "table" or c.range.start < r.start or c.range.stop > r.stop then
                bad = "child not nested in " .. tostring(n.kind) .. " child=" .. tostring(c.kind); return
            end
            visit(c)
        end
    end
    visit(unit.ast)
    return bad
end

ok(#corpus > 0, "official corpus enumerated (non-empty)")

local n, accept, reject = 0, 0, 0
local agree, false_reject, false_accept, unsupported, indeterminate, range_bad = 0, 0, 0, 0, 0, 0
local total_bytes = 0
for _, rec in ipairs(corpus) do
    n = n + 1; total_bytes = total_bytes + #rec.source
    local chunk = load(oracle_src(rec.source), "@" .. rec.path, "t")
    local okp, unit = pcall(lua.parse, rec.source, { path = rec.path, limits = LIMITS })
    local st, detail = "indeterminate", "parse raised"
    if okp and type(unit) == "table" then st, detail = hull_state(unit) end
    if chunk ~= nil then accept = accept + 1 else reject = reject + 1 end

    if st == "indeterminate" then
        indeterminate = indeterminate + 1
        ok(false, "INDETERMINATE " .. rec.path .. " :: " .. tostring(detail))
    elseif chunk ~= nil then                                  -- load accepts
        if st == "clean" then
            agree = agree + 1
            local bad = ranges_ok(unit, rec.source)
            if bad then range_bad = range_bad + 1; ok(false, "range " .. rec.path .. " :: " .. bad) end
        else
            false_reject = false_reject + 1
            ok(false, "FALSE-REJECT " .. rec.path .. " :: " .. tostring(detail))
        end
    else                                                     -- load rejects
        if st == "reject" then
            agree = agree + 1                                -- mutual reject (e.g. main.lua bare '#')
        else
            false_accept = false_accept + 1                 -- clean parse of a load-rejected file
            ok(false, "FALSE-ACCEPT " .. rec.path)
        end
    end
end

print(string.format("official-lua54: files=%d bytes=%d oracle(accept=%d reject=%d)", n, total_bytes, accept, reject))
print(string.format("official-lua54: agree=%d false_reject=%d false_accept=%d unsupported=%d indeterminate=%d range_bad=%d",
    agree, false_reject, false_accept, unsupported, indeterminate, range_bad))

-- Zero-gates: every non-agree outcome is a failure (no dormant acceptance).
ok(false_reject == 0, "no false rejects")
ok(false_accept == 0, "no false accepts")
ok(unsupported == 0, "no unsupported (Hull claims full Lua 5.4)")
ok(indeterminate == 0, "no indeterminate")
ok(range_bad == 0, "all ranges round-trip")
ok(agree == n, "every file agrees")

print(string.format("test_official_lua54: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail }

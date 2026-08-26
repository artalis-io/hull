--
-- test_conformance.lua - differential conformance of hull.source.lua vs real Lua 5.4.
--
-- Oracle: the harness's vanilla lua_State runs the SAME vendored Lua 5.4 Hull ships,
-- so its load(src, name, "t") is the exact parser Hull targets (compile-only, no
-- execution). The C harness injects HULL_LUA_CORPUS = { {path, source}, ... } (the
-- repo's own .lua files, read in C -- fail-closed -- so both oracle and parser see
-- identical bytes).
--
-- Directional invariant (design: docs/lua_source_conformance_design.md):
--   * Hull is a SYNTACTIC recognizer; load() is syntactic + a thin static-semantic
--     layer. So a Hull reject MUST imply a load reject (a false-reject is the key
--     bug); a Hull accept may diverge from a load reject only for a pinned SEMANTIC
--     case (see SEMANTIC_REJECTS).
--   * Hull's result is THREE-STATE: "reject" (>=1 lua.syntax/unsupported), "clean"
--     (no syntax AND no internal/limit), or "indeterminate" (any lua.internal or
--     lua.limit.*). An internal/limit result is NEVER a clean parse and NEVER enters
--     semantic classification -- in this required gate it FAILS with a harness
--     diagnostic (raise the limits or fix the bug).
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

-- luacheck: read globals HULL_LUA_CORPUS (injected by the C harness)
local lua = require("hull.source.lua")

local corpus = HULL_LUA_CORPUS or {}

local pass, fail, failures = 0, 0, {}
local function ok(cond, name)
    if cond then pass = pass + 1
    else fail = fail + 1; failures[#failures + 1] = name; print("FAIL: " .. name) end
end

-- Generous limits so lua.limit.* never trips on legitimate corpus files (we test the
-- parser's ACCEPT/REJECT behavior here, not its resource bounds). A trip becomes an
-- "indeterminate" verdict, which this gate treats as a failure, not a clean parse.
local LIMITS = { max_bytes = 64 * 1024 * 1024, max_tokens = 5000000,
                 max_comments = 5000000, max_depth = 2000 }

-- load() (string form) does NOT skip a shebang; the lexer (file semantics) does. Strip
-- a leading '#!...\n' for the oracle so both see the same post-shebang content.
local function oracle_src(src)
    if src:sub(1, 2) == "#!" then
        local nl = src:find("\n", 1, true)
        return nl and src:sub(nl + 1) or ""
    end
    return src
end

-- Three-state verdict for a Hull parse. Internal/limit markers are DISTINCT from a
-- clean parse (never excused): they yield "indeterminate".
local function hull_state(unit)
    if type(unit) ~= "table" or type(unit.diagnostics) ~= "table" then
        return "indeterminate", "no unit"
    end
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

-- ── semantic-reject classifier: SUBSTRING match against the NORMALIZED Lua 5.4
-- error body (the "chunkname:line: " prefix stripped) ──
-- Consulted ONLY where Hull is "clean" AND load() rejects. Small and closed; each
-- fragment is a precise vendored-Lua message and is pinned by a direct test below
-- (matches its case, and a nearby SYNTAX error does NOT match it). NOTE: this is
-- honest substring matching on the normalized body, not full anchoring.
local SEMANTIC_REJECTS = {
    { category = "break_outside_loop", fragment = "break outside loop" },
    { category = "goto_no_label",      fragment = "no visible label" },
    { category = "goto_into_scope",    fragment = "jumps into the scope" },
    { category = "vararg_outside",     fragment = "cannot use '...' outside a vararg" },
    { category = "assign_const",       fragment = "attempt to assign to const variable" },
    { category = "too_many_locals",    fragment = "too many local variables" },
}
-- Deliberately NOT here: unknown-attribute. Hull validates attribute names itself
-- (emits lua.syntax for anything but const/close), so an unknown attribute is a Hull
-- REJECT and must agree with load's reject -- never a divergence classified away.
local function normalize(err)
    return (tostring(err or "")):gsub("^[^:]*:%d+:%s*", "")   -- drop "chunkname:line: "
end
local function classify(err)
    local body = normalize(err)
    for _, s in ipairs(SEMANTIC_REJECTS) do
        if body:find(s.fragment, 1, true) then return s.category end
    end
    return nil
end

-- ── Part 2 helpers: range round-trip (AST child nodes ONLY; annotations excluded) ──
local function child_nodes(node)
    local out = {}
    local function collect(v)
        if type(v) ~= "table" then return end
        if v.kind then out[#out + 1] = v; return end
        for k, e in pairs(v) do if k ~= "range" then collect(e) end end
    end
    for k, v in pairs(node) do
        if k ~= "range" and k ~= "kind" and k ~= "annotation_list" and k ~= "annotations" then
            collect(v)
        end
    end
    return out
end

local function check_ranges(unit, src, path)
    local maxoff = #src + 1
    local bad = nil
    local function visit(n)
        if bad then return end
        local r = n.range
        if type(r) ~= "table" or type(r.start) ~= "number" or type(r.stop) ~= "number"
           or r.start < 1 or r.stop < r.start or r.stop > maxoff then
            bad = "range out of bounds kind=" .. tostring(n.kind); return
        end
        -- exact-slice round-trip: unit:text(node) is the precise source slice
        if unit:text(n) ~= src:sub(r.start, r.stop - 1) then
            bad = "unit:text ~= slice kind=" .. tostring(n.kind); return
        end
        -- non-empty for real constructs; only the empty-source chunk root may be empty
        if r.stop == r.start and n.kind ~= "chunk" then
            bad = "empty range for real construct kind=" .. tostring(n.kind); return
        end
        for _, c in ipairs(child_nodes(n)) do
            if type(c.range) ~= "table" or c.range.start < r.start or c.range.stop > r.stop then
                bad = "child (" .. tostring(c.range and c.range.start) .. "," ..
                      tostring(c.range and c.range.stop) .. ") not nested in " ..
                      tostring(n.kind) .. " (" .. r.start .. "," .. r.stop .. ") child=" .. tostring(c.kind)
                return
            end
            visit(c)
        end
    end
    visit(unit.ast)
    ok(bad == nil, "ranges consistent: " .. path .. (bad and (" :: " .. bad) or ""))
end

-- ── Part 1: oracle-classified corpus (three-state) ──
ok(#corpus > 0, "corpus enumerated (non-empty)")
local accepted, rejected = 0, 0
local semantic_counts = {}
local seeds = {}                              -- clean + load-accepted sources feed the fuzz
for _, rec in ipairs(corpus) do
    local src = rec.source
    local chunk, lerr = load(oracle_src(src), "@" .. rec.path, "t")
    local okp, unit = pcall(lua.parse, src, { path = rec.path, limits = LIMITS })
    if not okp or type(unit) ~= "table" then
        ok(false, "parse raised / returned non-unit: " .. rec.path)
    else
        local st, detail = hull_state(unit)
        if st == "indeterminate" then
            ok(false, "INDETERMINATE (internal/limit) on corpus file: " .. rec.path .. " :: " .. tostring(detail))
        elseif chunk ~= nil then
            accepted = accepted + 1
            ok(st == "clean", "no false-reject (load accepts): " .. rec.path ..
                (st ~= "clean" and (" :: " .. tostring(detail)) or ""))
            if st == "clean" then
                seeds[#seeds + 1] = src
                check_ranges(unit, src, rec.path)
            end
        else
            rejected = rejected + 1
            if st == "reject" then
                ok(true, "both reject: " .. rec.path)
            else                                          -- st == "clean"
                local cat = classify(lerr)
                if cat then
                    semantic_counts[cat] = (semantic_counts[cat] or 0) + 1
                    ok(true, "semantic divergence [" .. cat .. "]: " .. rec.path)
                else
                    ok(false, "FALSE ACCEPT (load rejects non-semantically, hull clean): " ..
                        rec.path .. " :: load: " .. tostring(lerr))
                end
            end
        end
    end
end

-- ── Part 3a: curated syntactic negatives (BOTH must reject) ──
local CURATED_SYNTAX_BAD = {
    "1 +", "function f(", "a.b.", "{,}", "if then end", "local = 1", "::",
    "return return", "1 2", "(1", "a =", "function 3() end", "local x, = 1",
    "{ [ ] = 1 }", "while do end", "for", "do", "end", "repeat until",
}
for _, s in ipairs(CURATED_SYNTAX_BAD) do
    local chunk = load(s, "@bad", "t")
    local st = hull_state(lua.parse(s, { path = "bad", limits = LIMITS }))
    ok(chunk == nil, "curated: load rejects [" .. s .. "]")
    ok(st == "reject", "curated: hull rejects [" .. s .. "] (got " .. st .. ")")
end

-- ── Part 3b: pinned semantic divergences (hull clean, load rejects semantically) ──
local SEMANTIC_CASES = {
    { src = "break",                                cat = "break_outside_loop" },
    { src = "goto nowhere",                         cat = "goto_no_label" },
    { src = "goto s\nlocal x = 1\n::s::\nreturn x", cat = "goto_into_scope" },
    { src = "function f() return ... end",          cat = "vararg_outside" },
    { src = "local x <const> = 1\nx = 2",           cat = "assign_const" },
}
for _, tc in ipairs(SEMANTIC_CASES) do
    local label = tc.src:gsub("\n", " ")
    local chunk, lerr = load(oracle_src(tc.src), "@sem", "t")
    local st = hull_state(lua.parse(tc.src, { path = "sem", limits = LIMITS }))
    ok(chunk == nil, "semantic: load rejects [" .. label .. "]")
    ok(st == "clean", "semantic: hull clean (syntactically valid) [" .. label .. "] (got " .. st .. ")")
    ok(classify(lerr) == tc.cat, "semantic: classify -> " .. tc.cat ..
        " (got " .. tostring(classify(lerr)) .. ") :: load: " .. tostring(lerr))
end

-- Guardrail: an unknown attribute is a Hull REJECT (both reject), NOT classified away.
do
    local src = "local x <weird> = 1"
    local chunk, lerr = load(src, "@a", "t")
    local st = hull_state(lua.parse(src, { path = "a", limits = LIMITS }))
    ok(chunk == nil, "attr: load rejects unknown attribute")
    ok(st == "reject", "attr: hull REJECTS unknown attribute (not classified) (got " .. st .. ")")
    ok(classify(lerr) == nil, "attr: unknown attribute is NOT in the semantic allowlist")
end

-- ── Part 3c: direct classifier tests (each fragment matches its case, not syntax) ──
local function gen_many_locals(k)
    local t = {}
    for i = 1, k do t[#t + 1] = "local v" .. i .. " = 1" end
    return table.concat(t, "\n")
end
local CLASSIFY_TESTS = {
    { cat = "break_outside_loop", good = "break",                             bad = "1 +" },
    { cat = "goto_no_label",      good = "goto nowhere",                      bad = "goto" },
    { cat = "goto_into_scope",    good = "goto s\nlocal x=1\n::s::\nreturn x", bad = "a.b." },
    { cat = "vararg_outside",     good = "function f() return ... end",       bad = "function f(" },
    { cat = "assign_const",       good = "local x <const> = 1\nx = 2",        bad = "local x <const> 1" },
    { cat = "too_many_locals",    good = gen_many_locals(260),                bad = "local a,,b = 1" },
}
for _, t in ipairs(CLASSIFY_TESTS) do
    local _, gerr = load(oracle_src(t.good), "@g", "t")
    ok(classify(gerr) == t.cat, "classify[good " .. t.cat .. "] -> " ..
        tostring(classify(gerr)) .. " :: load: " .. tostring(gerr))
    local _, berr = load(t.bad, "@b", "t")
    ok(classify(berr) == nil, "classify[bad syntax " .. t.cat .. "] -> " ..
        tostring(classify(berr)) .. " (must be nil) :: load: " .. tostring(berr))
end

-- ── Part 3d: seeded, bounded mutation fuzz over clean + load-accepted seeds ──
math.randomseed(0x5EED)
local N_PER_SEED, TOTAL_CAP = 20, 4000
local function mutate(src)
    if #src == 0 then return "x" end
    local op = math.random(3)
    local i = math.random(#src)
    if op == 1 then                                   -- delete one byte
        return src:sub(1, i - 1) .. src:sub(i + 1)
    elseif op == 2 then                               -- duplicate one byte
        return src:sub(1, i) .. src:sub(i, i) .. src:sub(i + 1)
    else                                              -- insert one random byte
        return src:sub(1, i - 1) .. string.char(math.random(0, 255)) .. src:sub(i)
    end
end
local max_seeds = math.max(1, math.floor(TOTAL_CAP / N_PER_SEED))
local stride = math.max(1, math.floor(#seeds / max_seeds))
local mutants, fuzz_fail = 0, 0
for i = 1, #seeds, stride do
    if mutants >= TOTAL_CAP then break end
    local base = seeds[i]
    for _ = 1, N_PER_SEED do
        if mutants >= TOTAL_CAP then break end
        mutants = mutants + 1
        local m = mutate(base)
        local okp, unit = pcall(lua.parse, m, { path = "fuzz", limits = LIMITS })
        if not okp or type(unit) ~= "table" then
            fuzz_fail = fuzz_fail + 1
            print("FUZZ raise/non-unit on mutant of seed " .. i)
        else
            local st = hull_state(unit)
            local chunk, lerr = load(oracle_src(m), "@fuzz", "t")
            if st == "indeterminate" then
                fuzz_fail = fuzz_fail + 1
                print("FUZZ INDETERMINATE (internal/limit): " .. string.format("%q", m:sub(1, 72)))
            elseif chunk ~= nil and st == "reject" then
                fuzz_fail = fuzz_fail + 1
                print("FUZZ false-reject: " .. string.format("%q", m:sub(1, 72)))
            elseif chunk == nil and st == "clean" and not classify(lerr) then
                fuzz_fail = fuzz_fail + 1
                print("FUZZ false-accept: " .. string.format("%q", m:sub(1, 72)) .. " :: " .. tostring(lerr))
            end
        end
    end
end
ok(fuzz_fail == 0, "mutation fuzz: " .. mutants .. " mutants, " .. fuzz_fail .. " divergences")

-- ── report (by category + count, so the allowlist cannot silently over-match) ──
print(string.format("conformance corpus: %d files (%d load-accepted, %d load-rejected)",
    #corpus, accepted, rejected))
local cats = {}
for k, v in pairs(semantic_counts) do cats[#cats + 1] = k .. "=" .. v end
table.sort(cats)
print("semantic divergences: " .. (#cats > 0 and table.concat(cats, ", ") or "none"))
print(string.format("test_conformance: %d passed, %d failed", pass, fail))
return { pass = pass, fail = fail, failures = failures }

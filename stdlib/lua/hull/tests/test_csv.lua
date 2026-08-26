-- test_csv.lua - Tests for hull.csv
--
-- Tests RFC 4180 CSV parsing and encoding.
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local csv = require('hull.csv')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- ── parse: basic ────────────────────────────────────────────────────

test("parse: simple CSV", function()
    local rows = csv.parse("a,b,c\n1,2,3\n")
    assert_eq(#rows, 2)
    assert_eq(rows[1][1], "a")
    assert_eq(rows[1][2], "b")
    assert_eq(rows[1][3], "c")
    assert_eq(rows[2][1], "1")
    assert_eq(rows[2][2], "2")
    assert_eq(rows[2][3], "3")
end)

test("parse: no trailing newline", function()
    local rows = csv.parse("a,b\n1,2")
    assert_eq(#rows, 2)
    assert_eq(rows[2][1], "1")
    assert_eq(rows[2][2], "2")
end)

test("parse: empty input", function()
    local rows = csv.parse("")
    assert_eq(#rows, 0)
end)

test("parse: CRLF line endings", function()
    local rows = csv.parse("a,b\r\n1,2\r\n")
    assert_eq(#rows, 2)
    assert_eq(rows[1][1], "a")
    assert_eq(rows[2][2], "2")
end)

-- ── parse: quoted fields ────────────────────────────────────────────

test("parse: quoted fields", function()
    local rows = csv.parse('"hello","world"\n')
    assert_eq(#rows, 1)
    assert_eq(rows[1][1], "hello")
    assert_eq(rows[1][2], "world")
end)

test("parse: quoted field with comma", function()
    local rows = csv.parse('"a,b",c\n')
    assert_eq(#rows, 1)
    assert_eq(rows[1][1], "a,b")
    assert_eq(rows[1][2], "c")
end)

test("parse: quoted field with newline", function()
    local rows = csv.parse('"line1\nline2",b\n')
    assert_eq(#rows, 1)
    assert_eq(rows[1][1], "line1\nline2")
    assert_eq(rows[1][2], "b")
end)

test("parse: escaped quotes", function()
    local rows = csv.parse('"say ""hello""",b\n')
    assert_eq(#rows, 1)
    assert_eq(rows[1][1], 'say "hello"')
    assert_eq(rows[1][2], "b")
end)

test("parse: empty quoted field", function()
    local rows = csv.parse('"",b\n')
    assert_eq(#rows, 1)
    assert_eq(rows[1][1], "")
    assert_eq(rows[1][2], "b")
end)

-- ── parse: headers mode ─────────────────────────────────────────────

test("parse: headers mode returns objects", function()
    local rows = csv.parse("name,age\nalice,30\nbob,25\n", { headers = true })
    assert_eq(#rows, 2)
    assert_eq(rows[1].name, "alice")
    assert_eq(rows[1].age, "30")
    assert_eq(rows[2].name, "bob")
    assert_eq(rows[2].age, "25")
end)

-- ── parse: custom separator ─────────────────────────────────────────

test("parse: custom separator", function()
    local rows = csv.parse("a;b;c\n1;2;3\n", { separator = ";" })
    assert_eq(#rows, 2)
    assert_eq(rows[1][2], "b")
    assert_eq(rows[2][3], "3")
end)

-- ── encode: basic ───────────────────────────────────────────────────

test("encode: simple rows", function()
    local result = csv.encode({{"a", "b", "c"}, {"1", "2", "3"}})
    assert_eq(result, "a,b,c\n1,2,3\n")
end)

test("encode: quotes fields with commas", function()
    local result = csv.encode({{"a,b", "c"}})
    assert_eq(result, '"a,b",c\n')
end)

test("encode: escapes quotes", function()
    local result = csv.encode({{'say "hello"', "b"}})
    assert_eq(result, '"say ""hello""",b\n')
end)

test("encode: quotes fields with newlines", function()
    local result = csv.encode({{"line1\nline2", "b"}})
    assert_eq(result, '"line1\nline2",b\n')
end)

test("encode: empty array", function()
    local result = csv.encode({})
    assert_eq(result, "")
end)

-- ── encode: headers mode ────────────────────────────────────────────

test("encode: headers mode from objects", function()
    local result = csv.encode(
        {{ name = "alice", age = "30" }, { name = "bob", age = "25" }},
        { headers = true }
    )
    -- Headers should appear as first row
    -- Order may vary since Lua tables are unordered, so just check content
    assert_eq(type(result), "string")
    local rows = csv.parse(result, { headers = true })
    assert_eq(#rows, 2)
    assert_eq(rows[1].name, "alice")
    assert_eq(rows[1].age, "30")
end)

-- ── encode: custom separator ────────────────────────────────────────

test("encode: custom separator", function()
    local result = csv.encode({{"a", "b", "c"}}, { separator = ";" })
    assert_eq(result, "a;b;c\n")
end)

-- ── roundtrip ───────────────────────────────────────────────────────

test("roundtrip: parse then encode", function()
    local input = "name,value\nalice,42\n"
    local rows = csv.parse(input)
    local output = csv.encode(rows)
    assert_eq(output, input)
end)

test("roundtrip: with quotes", function()
    local input = '"a,b","c""d"\nplain,value\n'
    local rows = csv.parse(input)
    local output = csv.encode(rows)
    assert_eq(output, input)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

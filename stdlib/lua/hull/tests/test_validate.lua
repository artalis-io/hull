-- test_validate.lua — Tests for hull.validate
--
-- Tests pure-function schema validation (no runtime globals needed).
-- Run via: the C test harness (test_lua_runtime.c) loads and executes this.

local validate = require('hull.validate')

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

-- ── required ─────────────────────────────────────────────────────────

test("required: missing field fails", function()
    local ok, errors = validate.check({}, { name = { required = true } })
    assert_eq(ok, false)
    assert_eq(errors.name, "is required")
end)

test("required: empty string fails", function()
    local ok, errors = validate.check({ name = "" }, { name = { required = true } })
    assert_eq(ok, false)
    assert_eq(errors.name, "is required")
end)

test("required: present value passes", function()
    local ok, errors = validate.check({ name = "alice" }, { name = { required = true } })
    assert_eq(ok, true)
    assert_eq(errors, nil)
end)

test("required: false passes", function()
    local ok = validate.check({ active = false }, { active = { required = true } })
    assert_eq(ok, true)
end)

test("required: zero passes", function()
    local ok = validate.check({ count = 0 }, { count = { required = true } })
    assert_eq(ok, true)
end)

-- ── optional ─────────────────────────────────────────────────────────

test("optional: nil skips all rules", function()
    local ok = validate.check({}, { role = { oneof = {"admin", "user"} } })
    assert_eq(ok, true)
end)

-- ── trim ─────────────────────────────────────────────────────────────

test("trim: strips whitespace in-place", function()
    local data = { name = "  alice  " }
    validate.check(data, { name = { trim = true } })
    assert_eq(data.name, "alice")
end)

test("trim + required: whitespace-only fails", function()
    local ok, errors = validate.check({ name = "   " }, { name = { required = true, trim = true } })
    assert_eq(ok, false)
    assert_eq(errors.name, "is required")
end)

-- ── type ─────────────────────────────────────────────────────────────

test("type string: passes", function()
    local ok = validate.check({ s = "hi" }, { s = { type = "string" } })
    assert_eq(ok, true)
end)

test("type string: number fails", function()
    local ok, errors = validate.check({ s = 42 }, { s = { type = "string" } })
    assert_eq(ok, false)
    assert_eq(errors.s, "must be a string")
end)

test("type number: passes", function()
    local ok = validate.check({ n = 3.14 }, { n = { type = "number" } })
    assert_eq(ok, true)
end)

test("type integer: float fails", function()
    local ok, errors = validate.check({ n = 3.5 }, { n = { type = "integer" } })
    assert_eq(ok, false)
    assert_eq(errors.n, "must be an integer")
end)

test("type integer: integer passes", function()
    local ok = validate.check({ n = 42 }, { n = { type = "integer" } })
    assert_eq(ok, true)
end)

test("type boolean: passes", function()
    local ok = validate.check({ b = true }, { b = { type = "boolean" } })
    assert_eq(ok, true)
end)

-- ── min / max (string) ──────────────────────────────────────────────

test("min string: too short fails", function()
    local ok, errors = validate.check({ pw = "abc" }, { pw = { min = 8 } })
    assert_eq(ok, false)
    assert_eq(errors.pw, "must be at least 8 characters")
end)

test("min string: exact length passes", function()
    local ok = validate.check({ pw = "12345678" }, { pw = { min = 8 } })
    assert_eq(ok, true)
end)

test("max string: too long fails", function()
    local ok, errors = validate.check({ name = "a very long name indeed" }, { name = { max = 10 } })
    assert_eq(ok, false)
    assert_eq(errors.name, "must be at most 10 characters")
end)

test("max string: exact length passes", function()
    local ok = validate.check({ name = "1234567890" }, { name = { max = 10 } })
    assert_eq(ok, true)
end)

-- ── min / max (number) ──────────────────────────────────────────────

test("min number: too small fails", function()
    local ok, errors = validate.check({ age = 5 }, { age = { min = 18 } })
    assert_eq(ok, false)
    assert_eq(errors.age, "must be at least 18")
end)

test("max number: too large fails", function()
    local ok, errors = validate.check({ age = 200 }, { age = { max = 150 } })
    assert_eq(ok, false)
    assert_eq(errors.age, "must be at most 150")
end)

-- ── pattern ──────────────────────────────────────────────────────────

test("pattern: match passes", function()
    local ok = validate.check({ code = "ABC-123" }, { code = { pattern = "^%u+%-%d+$" } })
    assert_eq(ok, true)
end)

test("pattern: no match fails", function()
    local ok, errors = validate.check({ code = "abc" }, { code = { pattern = "^%u+%-%d+$" } })
    assert_eq(ok, false)
    assert_eq(errors.code, "does not match the required pattern")
end)

-- ── oneof ────────────────────────────────────────────────────────────

test("oneof: valid value passes", function()
    local ok = validate.check({ role = "admin" }, { role = { oneof = {"admin", "user"} } })
    assert_eq(ok, true)
end)

test("oneof: invalid value fails", function()
    local ok, errors = validate.check({ role = "root" }, { role = { oneof = {"admin", "user"} } })
    assert_eq(ok, false)
    assert_eq(errors.role, "must be one of: admin, user")
end)

-- ── email ────────────────────────────────────────────────────────────

test("email: valid passes", function()
    local ok = validate.check({ e = "a@b.com" }, { e = { email = true } })
    assert_eq(ok, true)
end)

test("email: no @ fails", function()
    local ok, errors = validate.check({ e = "notanemail" }, { e = { email = true } })
    assert_eq(ok, false)
    assert_eq(errors.e, "is not a valid email")
end)

test("email: no domain fails", function()
    local ok, _errors = validate.check({ e = "a@" }, { e = { email = true } })
    assert_eq(ok, false)
end)

-- ── custom fn ────────────────────────────────────────────────────────

test("fn: nil return passes", function()
    local ok = validate.check({ x = "ok" }, { x = { fn = function() return nil end } })
    assert_eq(ok, true)
end)

test("fn: string return fails", function()
    local ok, errors = validate.check({ x = "bad" }, { x = { fn = function() return "custom error" end } })
    assert_eq(ok, false)
    assert_eq(errors.x, "custom error")
end)

-- ── custom message override ─────────────────────────────────────────

test("message override", function()
    local ok, errors = validate.check({}, { name = { required = true, message = "Name needed" } })
    assert_eq(ok, false)
    assert_eq(errors.name, "Name needed")
end)

-- ── multiple errors ──────────────────────────────────────────────────

test("multiple fields fail independently", function()
    local ok, errors = validate.check({}, {
        email = { required = true },
        name  = { required = true },
    })
    assert_eq(ok, false)
    assert_eq(errors.email, "is required")
    assert_eq(errors.name, "is required")
end)

-- ── first error wins per field ───────────────────────────────────────

test("first rule error wins", function()
    local ok, errors = validate.check({ pw = "" }, {
        pw = { required = true, min = 8 },
    })
    assert_eq(ok, false)
    assert_eq(errors.pw, "is required")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

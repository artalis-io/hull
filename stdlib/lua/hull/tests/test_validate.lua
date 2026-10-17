-- test_validate.lua - Tests for hull.validate
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

-- A boolean validator works as documented: true passes, false fails.
-- Every truthy return used to be taken as the error, so this one
-- accepted "admin" and rejected everything else.
test("fn: boolean true passes, false fails", function()
    local schema = { role = { fn = function(v) return v ~= "admin" end } }
    local ok1 = validate.check({ role = "user" }, schema)
    assert_eq(ok1, true, "true return should pass")
    local ok2, errs = validate.check({ role = "admin" }, schema)
    assert_eq(ok2, false, "false return should fail")
    assert_eq(errs.role, "is invalid")
    local ok3, errs3 = validate.check({ role = "admin" },
        { role = { fn = function() return false end, message = "nope" } })
    assert_eq(ok3, false)
    assert_eq(errs3.role, "nope")
    local ok4 = validate.check({ x = 1 }, { x = { fn = function() return {} end } })
    assert_eq(ok4, false, "a non-boolean non-string return fails closed")
end)

-- "" (a missing translation) fails with the default message, as in JS.
test("fn: empty-string return fails with the default message", function()
    local ok, errs = validate.check({ x = "v" }, { x = { fn = function() return "" end } })
    assert_eq(ok, false)
    assert_eq(errs.x, "is invalid")
end)

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
test("integer: non-finite values fail", function()
    local json = require("hull.json")
    for _, value in ipairs({ math.huge, -math.huge, 0/0,
                             json.decode('{"n":1e999}').n }) do
        local ok, errors = validate.check({ n = value }, {
            n = { type = "integer", min = 0 },
        })
        assert_eq(ok, false)
        assert_eq(errors.n, "must be an integer")
    end
    assert_eq(validate.check({ n = 42 }, { n = { type = "integer" } }), true)
end)

test("oneof: boolean choices return a validation error", function()
    local ok, errors = validate.check({ enabled = "bogus" }, {
        enabled = { oneof = { true, false } },
    })
    assert_eq(ok, false)
    assert_eq(errors.enabled, "must be one of: true, false")
    assert_eq(validate.check({ enabled = false }, {
        enabled = { oneof = { true, false } },
    }), true)
end)

test("oneof: custom message does not stringify choices", function()
    local choice = setmetatable({}, {
        __tostring = function() error("choice must not be formatted") end,
    })
    local ok, errors = validate.check({ x = "bogus" }, {
        x = { oneof = { choice }, message = "Pick a value" },
    })
    assert_eq(ok, false)
    assert_eq(errors.x, "Pick a value")
end)

test("oneof: table choices return a validation error", function()
    local choice = {}
    local ok, errors = validate.check({ x = "bogus" }, {
        x = { oneof = { choice } },
    })
    assert_eq(ok, false)
    assert_eq(errors.x, "must be one of: " .. tostring(choice))
    assert_eq(validate.check({ x = choice }, { x = { oneof = { choice } } }), true)
end)

-- ── min / max count characters (audit 17) ───────────────────────────

test("min/max: a string's length is its code points, not its bytes", function()
    assert_eq(validate.check({ s = "été" }, { s = { min = 3, max = 3 } }), true)
    local ok, errors = validate.check({ s = "été" }, { s = { max = 2 } })
    assert_eq(ok, false)
    assert_eq(errors.s, "must be at most 2 characters")
    assert_eq(validate.check({ s = "😀😀" }, { s = { max = 2 } }), true)
    assert_eq(validate.check({ s = "😀" }, { s = { min = 2 } }), false)
end)

test("min/max: a string that is not UTF-8 counts its bytes", function()
    assert_eq(validate.check({ s = "\255\255\255" }, { s = { min = 3, max = 3 } }), true)
    assert_eq(validate.check({ s = "\255\255\255" }, { s = { max = 2 } }), false)
end)

return {pass = pass, fail = fail}

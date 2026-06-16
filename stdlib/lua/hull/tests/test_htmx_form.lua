-- test_htmx_form.lua. Tests for hull.web.htmx.form helpers
-- (errors / field_error / field_attrs).
--
-- Distinct from test_form.lua (which covers hull.web.form, the
-- url-encoded body parser).

local form = require('hull.web.htmx.form')

local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end
local function assert_match(s, sub, msg)
    if not s:find(sub, 1, true) then
        error((msg or "") .. " expected '" .. sub .. "' in '" .. s .. "'")
    end
end

-- ── errors() ────────────────────────────────────────────────────────

test("errors with nil returns empty string", function()
    assert_eq(form.errors(nil), "")
end)

test("errors with empty table returns empty string", function()
    assert_eq(form.errors({}), "")
end)

test("errors emits ul.hull-form-errors with role=alert", function()
    local s = form.errors({ name = "required" })
    assert_match(s, '<ul class="hull-form-errors" role="alert">')
    assert_match(s, '</ul>')
end)

test("errors li are sorted by field name (deterministic AT order)", function()
    local s = form.errors({ name = "required", email = "invalid" })
    -- alphabetical: email < name → "invalid" appears before "required"
    local i_invalid  = s:find('invalid',  1, true)
    local i_required = s:find('required', 1, true)
    assert_eq(i_invalid and i_required and i_invalid < i_required, true,
              "expected email error before name error in summary")
end)

test("errors li links to inline span via fragment", function()
    local s = form.errors({ email = "invalid" })
    assert_match(s, 'href="#hull-form-error-email"')
end)

test("errors escapes message body (no script-tag escape)", function()
    local s = form.errors({ name = "<script>alert(1)</script>" })
    if s:find('<script>', 1, true) then
        error("error message should be HTML-escaped")
    end
    assert_match(s, '&lt;script&gt;')
end)

-- ── field_error() ───────────────────────────────────────────────────

test("field_error with nil errors returns empty", function()
    assert_eq(form.field_error(nil, "name"), "")
end)

test("field_error for unaffected field returns empty", function()
    assert_eq(form.field_error({ email = "x" }, "name"), "")
end)

test("field_error emits span with matching id and class", function()
    local s = form.field_error({ name = "required" }, "name")
    assert_eq(s, '<span id="hull-form-error-name" class="hull-form-error">required</span>')
end)

test("field_error escapes the message", function()
    local s = form.field_error({ name = 'O"Brien' }, "name")
    assert_match(s, '&quot;Brien')
end)

-- ── field_attrs() ───────────────────────────────────────────────────

test("field_attrs with nil errors returns empty", function()
    assert_eq(form.field_attrs(nil, "name"), "")
end)

test("field_attrs for unaffected field returns empty", function()
    assert_eq(form.field_attrs({ email = "x" }, "name"), "")
end)

test("field_attrs for affected field emits a11y attrs", function()
    local s = form.field_attrs({ name = "required" }, "name")
    assert_eq(s, 'aria-invalid="true" aria-describedby="hull-form-error-name"')
end)

test("field_attrs id matches field_error id (a11y wiring)", function()
    local errors = { email = "invalid" }
    local attrs = form.field_attrs(errors, "email")
    local span  = form.field_error(errors, "email")
    assert_match(attrs, "hull-form-error-email")
    assert_match(span,  'id="hull-form-error-email"')
end)

-- ── Defense-in-depth: field names with HTML metacharacters ────────

test("errors collapses HTML-meta field-name chars to underscore in ids", function()
    -- Defense in depth: a future dynamic-schema caller might pass
    -- user-controlled field names. Anything outside [A-Za-z0-9_-]
    -- collapses to "_" so id values stay valid and contain no
    -- entity sequences.
    local s = form.errors({ ["<script>"] = "bad" })
    if s:find("hull-form-error-<script>", 1, true) then
        error("raw < and > leaked into id")
    end
    if s:find("hull-form-error-&", 1, true) then
        error("entity-escaped key leaked into id")
    end
    assert_match(s, "hull-form-error-_script_")
end)

test("field_attrs id matches field_error id even with meta name", function()
    local errors = { ['user.name'] = "required" }
    local attrs = form.field_attrs(errors, 'user.name')
    local span  = form.field_error(errors, 'user.name')
    -- Both helpers must collapse identically — that's the contract.
    assert_match(attrs, "hull-form-error-user_name")
    assert_match(span,  'id="hull-form-error-user_name"')
end)

print(string.format("\n%d/%d htmx-form tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end

-- test_confirm.lua. Tests for hull.web.htmx.confirm.attrs.
--
-- Verifies the attribute-string builder produces correctly
-- escaped HTML attributes for splicing into a template via
-- `{{ ... | raw }}`. Doesn't exercise the client dialog (covered
-- by manual / e2e smoke against a running server).

local confirm = require('hull.web.htmx.confirm')

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
local function assert_match(s, pattern, msg)
    if not s:find(pattern, 1, true) then
        error((msg or "") .. " expected to find '" .. pattern .. "' in '" .. s .. "'")
    end
end

test("attrs with bare question emits only hx-confirm", function()
    local s = confirm.attrs("Delete this?")
    assert_eq(s, 'hx-confirm="Delete this?"')
end)

test("attrs escapes double-quote in question (attribute break)", function()
    local s = confirm.attrs('Say "yes"?')
    assert_eq(s, 'hx-confirm="Say &quot;yes&quot;?"')
end)

test("attrs escapes ampersand in question", function()
    local s = confirm.attrs('Tom & Jerry?')
    assert_eq(s, 'hx-confirm="Tom &amp; Jerry?"')
end)

test("attrs escapes angle brackets (defense in depth)", function()
    local s = confirm.attrs('Delete <script>?')
    assert_match(s, 'hx-confirm="Delete &lt;script&gt;?"')
end)

test("attrs with yes opt emits data-confirm-yes", function()
    local s = confirm.attrs("Delete?", { yes = "Delete" })
    assert_match(s, 'data-confirm-yes="Delete"')
end)

test("attrs with no opt emits data-confirm-no", function()
    local s = confirm.attrs("Delete?", { no = "Keep" })
    assert_match(s, 'data-confirm-no="Keep"')
end)

test("attrs with danger=true emits data-confirm-danger", function()
    local s = confirm.attrs("Delete?", { danger = true })
    assert_match(s, 'data-confirm-danger="true"')
end)

test("attrs with danger=false omits data-confirm-danger", function()
    local s = confirm.attrs("Delete?", { danger = false })
    if s:find("data-confirm-danger", 1, true) then
        error("data-confirm-danger should not be present when danger=false")
    end
end)

test("attrs with title opt emits data-confirm-title", function()
    local s = confirm.attrs("Are you sure?", { title = "Permanent action" })
    assert_match(s, 'data-confirm-title="Permanent action"')
end)

test("attrs with all opts emits all attributes", function()
    local s = confirm.attrs("Delete this asset?", {
        yes = "Delete", no = "Keep", danger = true, title = "Cannot undo",
    })
    assert_match(s, 'hx-confirm="Delete this asset?"')
    assert_match(s, 'data-confirm-yes="Delete"')
    assert_match(s, 'data-confirm-no="Keep"')
    assert_match(s, 'data-confirm-danger="true"')
    assert_match(s, 'data-confirm-title="Cannot undo"')
end)

test("attrs escapes labels too (no quote-break in custom labels)", function()
    local s = confirm.attrs("OK?", { yes = 'Delete "all"' })
    assert_match(s, 'data-confirm-yes="Delete &quot;all&quot;"')
end)

test("attrs with nil question yields empty hx-confirm", function()
    local s = confirm.attrs(nil)
    assert_eq(s, 'hx-confirm=""')
end)

print(string.format("\n%d/%d confirm tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end

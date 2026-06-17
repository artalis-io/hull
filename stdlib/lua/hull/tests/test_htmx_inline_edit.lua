-- test_htmx_inline_edit.lua. Tests for hull.web.htmx.inline-edit.

local inline_edit = require('hull.web.htmx.inline-edit')

local pass, fail = 0, 0
local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then pass = pass + 1
    else fail = fail + 1; print("FAIL: " .. name .. ": " .. tostring(err)) end
end
local function assert_match(s, sub, msg)
    if not s:find(sub, 1, true) then
        error((msg or "") .. " expected '" .. sub .. "' in '" .. s .. "'")
    end
end
local function assert_no_match(s, sub, msg)
    if s:find(sub, 1, true) then
        error((msg or "") .. " did not expect '" .. sub .. "' in '" .. s .. "'")
    end
end
local function assert_eq(actual, expected, msg)
    if actual ~= expected then
        error((msg or "assertion failed")
            .. " (expected " .. tostring(expected)
            .. ", got " .. tostring(actual) .. ")")
    end
end

-- ── cell() ──────────────────────────────────────────────────────────

test("cell emits a span with the value", function()
    local s = inline_edit.cell({ value = "Conveyor #3", edit_url = "/x/edit" })
    assert_match(s, '<span')
    assert_match(s, 'Conveyor #3')
end)

test("cell uses outerHTML swap", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/x/edit" })
    assert_match(s, 'hx-swap="outerHTML"')
end)

test("cell is keyboard-activatable (role=button, tabindex)", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/x/edit" })
    assert_match(s, 'role="button"')
    assert_match(s, 'tabindex="0"')
end)

test("cell triggers on click, Enter, Space", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/x/edit" })
    assert_match(s, 'click')
    assert_match(s, "Enter")
    -- Space — the literal char appears in the trigger
    assert_match(s, "key==' '")
end)

test("cell emits hx-get pointing at edit_url", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/assets/42/name/edit" })
    assert_match(s, 'hx-get="/assets/42/name/edit"')
end)

test("cell default label is Edit", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/x/edit" })
    assert_match(s, 'aria-label="Edit"')
    assert_match(s, 'title="Edit"')
end)

test("cell custom label appears in aria-label and title", function()
    local s = inline_edit.cell({ value = "x", edit_url = "/x/edit", label = "Edit asset name" })
    assert_match(s, 'aria-label="Edit asset name"')
    assert_match(s, 'title="Edit asset name"')
end)

test("cell escapes value (no <script> escape)", function()
    local s = inline_edit.cell({ value = "<script>alert(1)</script>", edit_url = "/x/edit" })
    assert_no_match(s, '<script>')
    assert_match(s, '&lt;script&gt;')
end)

test("cell escapes edit_url (no quote-break)", function()
    local s = inline_edit.cell({ value = "x", edit_url = '/x?a="b"' })
    assert_match(s, 'hx-get="/x?a=&quot;b&quot;"')
end)

-- ── editor() ────────────────────────────────────────────────────────

test("editor emits a form with hx-patch", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, '<form')
    assert_match(s, 'hx-patch="/x"')
end)

test("editor uses outerHTML swap targeting itself", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, 'hx-target="this"')
    assert_match(s, 'hx-swap="outerHTML"')
end)

test("editor input has default name=value", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, 'name="value"')
end)

test("editor input takes custom name", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view", name = "title",
    })
    assert_match(s, 'name="title"')
end)

test("editor input pre-fills value attribute", function()
    local s = inline_edit.editor({
        value = "Conveyor #3", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, 'value="Conveyor #3"')
end)

test("editor default labels are Save / Cancel", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, '>Save</button>')
    assert_match(s, '>Cancel</button>')
end)

test("editor custom button labels", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
        save_label = "Update", cancel_label = "Keep",
    })
    assert_match(s, '>Update</button>')
    assert_match(s, '>Keep</button>')
end)

test("editor Cancel button hx-get points at cancel_url", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    -- The cancel button has hx-get="/x/view"
    assert_match(s, 'hx-get="/x/view"')
end)

test("editor wires Esc-to-cancel via hx-trigger", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, "Escape")
end)

test("editor escapes value (XSS in pre-fill)", function()
    local s = inline_edit.editor({
        value = '"><script>alert(1)</script>',
        save_url = "/x", cancel_url = "/x/view",
    })
    assert_no_match(s, '<script>')
    assert_no_match(s, '"><script>')
    -- Value attr should be quote-escaped
    assert_match(s, 'value="&quot;&gt;')
end)

test("editor input has aria-label (default 'Edit value')", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
    })
    assert_match(s, 'aria-label="Edit value"')
end)

test("editor custom aria-label", function()
    local s = inline_edit.editor({
        value = "x", save_url = "/x", cancel_url = "/x/view",
        label = "Asset name",
    })
    assert_match(s, 'aria-label="Asset name"')
end)

-- ── Edge cases ─────────────────────────────────────────────────────

test("cell with omitted value renders empty span (no 'nil' text)", function()
    local s = inline_edit.cell({ edit_url = "/x/edit" })
    -- The display text should be empty, NOT the literal "nil".
    if s:find(">nil<", 1, true) then
        error("nil collapsed to literal 'nil' instead of empty")
    end
    assert_match(s, '<span')
    assert_match(s, '></span>')  -- empty body
end)

test("cell with table-typed value raises a clear error", function()
    local ok, err = pcall(inline_edit.cell, { value = {}, edit_url = "/x/edit" })
    assert_eq(ok, false, "should have errored")
    if not tostring(err):find("must be a string or number", 1, true) then
        error("expected type-narrowing error, got: " .. tostring(err))
    end
end)

test("editor with table-typed save_url raises a clear error", function()
    local ok, err = pcall(inline_edit.editor, {
        value = "x", save_url = {}, cancel_url = "/x/view",
    })
    assert_eq(ok, false, "should have errored")
    if not tostring(err):find("must be a string or number", 1, true) then
        error("expected type-narrowing error, got: " .. tostring(err))
    end
end)

print(string.format("\n%d/%d inline-edit tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end

-- test_htmx_table.lua. Tests for hull.web.htmx.table.render.

local tbl = require('hull.web.htmx.table')

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
local function assert_no_match(s, sub, msg)
    if s:find(sub, 1, true) then
        error((msg or "") .. " did not expect '" .. sub .. "' in '" .. s .. "'")
    end
end

test("render requires opts.base_url", function()
    local ok, err = pcall(tbl.render, {}, {}, {})
    assert_eq(ok, false, "should have errored")
    if not tostring(err):find("base_url is required", 1, true) then
        error("expected required-opt error, got: " .. tostring(err))
    end
end)

test("render emits a <table> with <thead> + <tbody>", function()
    local s = tbl.render({}, {}, { base_url = "/x" })
    assert_match(s, '<table class="hull-table">')
    assert_match(s, '<thead>')
    assert_match(s, '<tbody>')
end)

test("render emits empty-row marker when rows is empty", function()
    local s = tbl.render({}, { { name = "id", label = "ID" } },
        { base_url = "/x" })
    assert_match(s, 'hull-table-empty')
    assert_match(s, 'colspan="1"')
    assert_match(s, 'No rows.')
end)

test("render custom empty_label", function()
    local s = tbl.render({}, {}, { base_url = "/x", empty_label = "Nothing here yet" })
    assert_match(s, 'Nothing here yet')
end)

test("render plain (non-sortable, non-editable) cell is HTML-escaped", function()
    local s = tbl.render(
        { { name = "<script>" } },
        { { name = "name", label = "Name" } },
        { base_url = "/x" })
    assert_no_match(s, '<script>')
    assert_match(s, '&lt;script&gt;')
end)

test("render sortable header includes sort attrs", function()
    local s = tbl.render(
        { { id = 1, name = "A" } },
        { { name = "id", label = "ID" },
          { name = "name", label = "Name", sortable = true } },
        { base_url = "/x" })
    -- Sortable headers get hx-get + role=button + aria-sort.
    assert_match(s, 'hx-get="/x?sort=name:asc"')
    assert_match(s, 'aria-sort="none"')
    -- Non-sortable headers do NOT get those.
    if s:find('hx-get="/x?sort=id', 1, true) then
        error("non-sortable column should not emit sort attrs")
    end
end)

test("render editable cell wraps value in inline_edit.cell", function()
    local s = tbl.render(
        { { id = 1, name = "Original" } },
        { { name = "id", label = "ID" },
          { name = "name", label = "Name", editable = true } },
        { base_url = "/x",
          edit_url_for = function(row, col)
              return "/x/" .. row.id .. "/" .. col .. "/edit"
          end })
    -- Inline-edit cell uses hx-get to fetch the edit form.
    assert_match(s, 'hx-get="/x/1/name/edit"')
    assert_match(s, 'hull-inline-edit-view')
    assert_match(s, '>Original<')
end)

test("render editable column without edit_url_for raises", function()
    local ok, err = pcall(tbl.render,
        { { id = 1 } },
        { { name = "id", editable = true } },
        { base_url = "/x" })
    assert_eq(ok, false, "should have errored")
    if not tostring(err):find("edit_url_for is required", 1, true) then
        error("expected error about missing edit_url_for, got: " .. tostring(err))
    end
end)

test("render custom render() callback overrides default", function()
    local s = tbl.render(
        { { active = true } },
        { { name = "active", label = "Active",
            render = function(v) return v and "<b>YES</b>" or "no" end } },
        { base_url = "/x" })
    -- Custom render output goes through raw; no escaping by widget.
    -- App is responsible for the safety of what it returns from render.
    assert_match(s, '<b>YES</b>')
end)

test("render with current_sort reflects direction on the sorted column", function()
    local s = tbl.render(
        { { name = "A" } },
        { { name = "name", label = "Name", sortable = true } },
        { base_url = "/x",
          current_sort = { column = "name", direction = "desc" } })
    assert_match(s, 'aria-sort="descending"')
    -- The header's link toggles to asc (the opposite of current).
    assert_match(s, 'hx-get="/x?sort=name:asc"')
end)

test("render escapes col.label on both sortable and plain headers", function()
    -- A col with HTML metacharacters in its label must escape on
    -- both paths. The sortable path has additional attrs around
    -- the body; verify the label body is escaped regardless.
    local s1 = tbl.render({},
        { { name = "n", label = "<Name>", sortable = true } },
        { base_url = "/x" })
    assert_no_match(s1, '><Name><')
    assert_match(s1, '&lt;Name&gt;')

    local s2 = tbl.render({},
        { { name = "n", label = "<Name>" } },
        { base_url = "/x" })
    assert_match(s2, '&lt;Name&gt;')
end)

print(string.format("\n%d/%d htmx-table tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end

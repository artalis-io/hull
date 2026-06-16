-- test_htmx_sort.lua. Tests for hull.web.htmx.sort.

local sort = require('hull.web.htmx.sort')

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
local function mock_req(sort_param)
    return { query = sort_param and { sort = sort_param } or {} }
end

-- ── parse() ────────────────────────────────────────────────────────

test("parse nil/empty returns nil", function()
    assert_eq(sort.parse(mock_req(nil)), nil)
    assert_eq(sort.parse(mock_req("")), nil)
end)

test("parse bare column defaults direction to asc", function()
    local r = sort.parse(mock_req("name"))
    assert_eq(r.column, "name")
    assert_eq(r.direction, "asc")
end)

test("parse col:asc and col:desc round-trip", function()
    assert_eq(sort.parse(mock_req("name:asc")).direction, "asc")
    assert_eq(sort.parse(mock_req("name:desc")).direction, "desc")
end)

test("parse unknown direction defaults to asc", function()
    assert_eq(sort.parse(mock_req("name:weird")).direction, "asc")
end)

test("parse strips non-[A-Za-z0-9_-] from column name", function()
    -- URL-injection / SQL-injection defense: column name is
    -- always allowlist-sanitized before reaching SQL.
    assert_eq(sort.parse(mock_req("name; DROP TABLE assets")).column, "nameDROPTABLEassets")
end)

test("parse with opts.allowed rejects non-allowed column", function()
    local r = sort.parse(mock_req("password"), { allowed = { "name", "email" } })
    assert_eq(r, nil)
end)

test("parse with opts.allowed admits allowed column", function()
    local r = sort.parse(mock_req("name"), { allowed = { "name", "email" } })
    assert_eq(r.column, "name")
end)

test("parse falls back to opts.default when no param", function()
    local r = sort.parse(mock_req(), { default = "id" })
    assert_eq(r.column, "id")
    assert_eq(r.direction, "asc")
end)

test("parse falls back to opts.default when param rejected by allowlist", function()
    local r = sort.parse(mock_req("password"),
        { allowed = { "id" }, default = "id" })
    assert_eq(r.column, "id")
end)

-- ── header_attrs() ─────────────────────────────────────────────────

test("header_attrs unsorted column emits direction=none", function()
    local s = sort.header_attrs("name", nil, { url = "/x" })
    assert_match(s, 'data-sort-direction="none"')
    assert_match(s, 'aria-sort="none"')
    assert_match(s, 'class="hull-sort-header hull-sort-none"')
end)

test("header_attrs currently-asc column toggles to desc in URL", function()
    local s = sort.header_attrs("name",
        { column = "name", direction = "asc" }, { url = "/x" })
    assert_match(s, 'hx-get="/x?sort=name:desc"')
    assert_match(s, 'data-sort-direction="asc"')
    assert_match(s, 'aria-sort="ascending"')
end)

test("header_attrs currently-desc column toggles to asc in URL", function()
    local s = sort.header_attrs("name",
        { column = "name", direction = "desc" }, { url = "/x" })
    assert_match(s, 'hx-get="/x?sort=name:asc"')
    assert_match(s, 'data-sort-direction="desc"')
end)

test("header_attrs other-column click goes to asc by default", function()
    -- Currently sorted by 'name'; clicking 'email' should
    -- start at asc (the first-click default).
    local s = sort.header_attrs("email",
        { column = "name", direction = "desc" }, { url = "/x" })
    assert_match(s, 'hx-get="/x?sort=email:asc"')
end)

test("header_attrs preserves existing ?query in base url", function()
    -- If the base URL already has a query string, use & not ?.
    local s = sort.header_attrs("name", nil, { url = "/x?q=foo" })
    assert_match(s, 'hx-get="/x?q=foo&sort=name:asc"')
end)

test("header_attrs custom target", function()
    local s = sort.header_attrs("name", nil,
        { url = "/x", target = "#asset-table" })
    assert_match(s, 'hx-target="#asset-table"')
end)

test("header_attrs default target is 'closest table'", function()
    local s = sort.header_attrs("name", nil, { url = "/x" })
    assert_match(s, 'hx-target="closest table"')
end)

test("header_attrs default push_url=true", function()
    local s = sort.header_attrs("name", nil, { url = "/x" })
    assert_match(s, 'hx-push-url="true"')
end)

test("header_attrs push_url=false omits hx-push-url", function()
    local s = sort.header_attrs("name", nil,
        { url = "/x", push_url = false })
    if s:find("hx-push-url", 1, true) then
        error("hx-push-url should not be present")
    end
end)

test("header_attrs is keyboard-activatable", function()
    local s = sort.header_attrs("name", nil, { url = "/x" })
    assert_match(s, 'role="button"')
    assert_match(s, 'tabindex="0"')
    assert_match(s, "Enter")
    assert_match(s, "key==' '")
end)

-- ── M1 (audit): opts.default is sanitized + allowlist-checked ─────

test("parse sanitizes opts.default through safe_column", function()
    -- If an app passes a default with SQL meta-chars (config typo
    -- / user-controlled config), the result must still be safe.
    local r = sort.parse(mock_req(), { default = "name; DROP TABLE" })
    assert_eq(r.column, "nameDROPTABLE")
end)

test("parse rejects opts.default that fails the allowlist", function()
    -- Defense in depth: a typo'd default that isn't on the allowlist
    -- should not silently propagate (could let unintended columns
    -- reach SQL).
    local r = sort.parse(mock_req(), {
        allowed = { "name", "email" }, default = "password",
    })
    assert_eq(r, nil)
end)

test("parse honors opts.default that IS on the allowlist", function()
    local r = sort.parse(mock_req(), {
        allowed = { "name", "email" }, default = "name",
    })
    assert_eq(r.column, "name")
end)

test("parse falls back to nil when input + default both rejected", function()
    -- ?sort=admin not in allowlist, default also not — both rejected.
    local r = sort.parse(mock_req("admin"), {
        allowed = { "id" }, default = "secret",
    })
    assert_eq(r, nil)
end)

test("header_attrs escapes base URL containing quotes/ampersands", function()
    -- Base URL is spliced into hx-get value; the attr-escape must
    -- prevent it from breaking out of the attribute context.
    local s = sort.header_attrs("name", nil, { url = '/x?a="b"&c=d' })
    assert_match(s, '&quot;b&quot;')
    assert_match(s, '&amp;c=d')
end)

test("header_attrs escapes column-name when emitting data-attrs", function()
    -- safe_column already strips most meta-chars; verify what does
    -- make it through (alphanumerics + - + _) is still escape-safe.
    -- The interesting case: a column with only sanitization-safe
    -- chars also gets attr_escape applied (defense in depth).
    local s = sort.header_attrs("a-b_c", nil, { url = "/x" })
    assert_match(s, 'data-sort-column="a-b_c"')
end)

print(string.format("\n%d/%d sort tests passed", pass, pass + fail))
if fail > 0 then os.exit(1) end

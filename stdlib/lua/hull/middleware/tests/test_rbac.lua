-- test_rbac.lua — Tests for hull.middleware.rbac
--
-- Tests role-based access control.
-- Requires db global (run via C test harness with caps).

local rbac = require('hull.middleware.rbac')

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

local function assert_true(v, msg)
    if not v then
        error((msg or "") .. " expected true, got " .. tostring(v))
    end
end

-- ── init ────────────────────────────────────────────────────────────

test("init: creates tables", function()
    rbac.init()
end)

-- ── define_role and define_permission ───────────────────────────────

test("define_role: creates role", function()
    rbac.define_role("admin")
    rbac.define_role("editor")
    rbac.define_role("viewer")
end)

test("define_permission: creates permission", function()
    rbac.define_permission("users.read")
    rbac.define_permission("users.write")
    rbac.define_permission("posts.read")
    rbac.define_permission("posts.write")
    rbac.define_permission("posts.delete")
end)

test("define_role: with permissions", function()
    rbac.define_role("moderator", {"posts.read", "posts.write", "posts.delete"})
end)

-- ── grant ───────────────────────────────────────────────────────────

test("grant: adds permission to role", function()
    rbac.grant("admin", "users.read")
    rbac.grant("admin", "users.write")
    rbac.grant("admin", "posts.read")
    rbac.grant("admin", "posts.write")
    rbac.grant("admin", "posts.delete")
    rbac.grant("editor", "posts.read")
    rbac.grant("editor", "posts.write")
    rbac.grant("viewer", "posts.read")
end)

-- ── assign and query roles ──────────────────────────────────────────

test("assign: grants role to user", function()
    rbac.assign("user1", "admin")
    rbac.assign("user2", "editor")
    rbac.assign("user3", "viewer")
end)

test("roles: returns user's roles", function()
    local r = rbac.roles("user1")
    assert_eq(#r, 1)
    assert_eq(r[1], "admin")
end)

test("has_role: returns true for assigned role", function()
    assert_true(rbac.has_role("user1", "admin"))
end)

test("has_role: returns false for unassigned role", function()
    assert_eq(rbac.has_role("user1", "editor"), false)
end)

test("has_any_role: returns true if user has one of the roles", function()
    assert_true(rbac.has_any_role("user1", {"admin", "editor"}))
end)

test("has_any_role: returns false if user has none", function()
    assert_eq(rbac.has_any_role("user3", {"admin", "editor"}), false)
end)

-- ── permissions ─────────────────────────────────────────────────────

test("permissions: returns user's permissions via roles", function()
    local perms = rbac.permissions("user1")
    assert_true(#perms >= 5, "admin should have at least 5 permissions")
end)

test("has_permission: true for granted permission", function()
    assert_true(rbac.has_permission("user1", "users.write"))
end)

test("has_permission: false for ungranted permission", function()
    assert_eq(rbac.has_permission("user3", "users.write"), false)
end)

test("has_any_permission: true if user has one", function()
    assert_true(rbac.has_any_permission("user2", {"users.write", "posts.write"}))
end)

test("has_any_permission: false if user has none", function()
    assert_eq(rbac.has_any_permission("user3", {"users.read", "users.write"}), false)
end)

-- ── revoke and ungrant ──────────────────────────────────────────────

test("revoke: removes role from user", function()
    rbac.assign("user4", "admin")
    assert_true(rbac.has_role("user4", "admin"))
    rbac.revoke("user4", "admin")
    assert_eq(rbac.has_role("user4", "admin"), false)
end)

test("ungrant: removes permission from role", function()
    rbac.define_role("temp_role")
    rbac.define_permission("temp.perm")
    rbac.grant("temp_role", "temp.perm")
    rbac.assign("user5", "temp_role")
    assert_true(rbac.has_permission("user5", "temp.perm"))
    rbac.ungrant("temp_role", "temp.perm")
    assert_eq(rbac.has_permission("user5", "temp.perm"), false)
end)

-- ── middleware ──────────────────────────────────────────────────────

test("require_role: returns function", function()
    local mw = rbac.require_role("admin")
    assert_eq(type(mw), "function")
end)

test("require_permission: returns function", function()
    local mw = rbac.require_permission("users.read")
    assert_eq(type(mw), "function")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

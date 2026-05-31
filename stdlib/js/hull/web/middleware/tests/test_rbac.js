// test_rbac.js — Tests for hull:web:middleware:rbac
//
// Tests role-based access control.
// Requires db (run via C test harness with caps).

import { rbac } from "hull:web:middleware:rbac";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        print("FAIL: " + name + ": " + e.message);
    }
}

function assertEq(a, b, msg) {
    if (a !== b)
        throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function assertTrue(v, msg) {
    if (!v)
        throw new Error((msg || "") + " expected truthy, got " + v);
}

// ── init ────────────────────────────────────────────────────────────

test("init: creates tables", () => {
    rbac.init();
});

// ── defineRole and definePermission ─────────────────────────────────

test("defineRole: creates role", () => {
    rbac.defineRole("admin");
    rbac.defineRole("editor");
    rbac.defineRole("viewer");
});

test("definePermission: creates permission", () => {
    rbac.definePermission("users.read");
    rbac.definePermission("users.write");
    rbac.definePermission("posts.read");
    rbac.definePermission("posts.write");
    rbac.definePermission("posts.delete");
});

test("defineRole: with permissions", () => {
    rbac.defineRole("moderator", ["posts.read", "posts.write", "posts.delete"]);
});

// ── grant ───────────────────────────────────────────────────────────

test("grant: adds permission to role", () => {
    rbac.grant("admin", "users.read");
    rbac.grant("admin", "users.write");
    rbac.grant("admin", "posts.read");
    rbac.grant("admin", "posts.write");
    rbac.grant("admin", "posts.delete");
    rbac.grant("editor", "posts.read");
    rbac.grant("editor", "posts.write");
    rbac.grant("viewer", "posts.read");
});

// ── assign and query roles ──────────────────────────────────────────

test("assign: grants role to user", () => {
    rbac.assign("user1", "admin");
    rbac.assign("user2", "editor");
    rbac.assign("user3", "viewer");
});

test("roles: returns user's roles", () => {
    const r = rbac.roles("user1");
    assertEq(r.length, 1);
    assertEq(r[0], "admin");
});

test("hasRole: returns true for assigned role", () => {
    assertTrue(rbac.hasRole("user1", "admin"));
});

test("hasRole: returns false for unassigned role", () => {
    assertEq(rbac.hasRole("user1", "editor"), false);
});

test("hasAnyRole: returns true if user has one of the roles", () => {
    assertTrue(rbac.hasAnyRole("user1", ["admin", "editor"]));
});

test("hasAnyRole: returns false if user has none", () => {
    assertEq(rbac.hasAnyRole("user3", ["admin", "editor"]), false);
});

// ── permissions ─────────────────────────────────────────────────────

test("permissions: returns user's permissions via roles", () => {
    const perms = rbac.permissions("user1");
    assertTrue(perms.length >= 5, "admin should have at least 5 permissions");
});

test("hasPermission: true for granted permission", () => {
    assertTrue(rbac.hasPermission("user1", "users.write"));
});

test("hasPermission: false for ungranted permission", () => {
    assertEq(rbac.hasPermission("user3", "users.write"), false);
});

test("hasAnyPermission: true if user has one", () => {
    assertTrue(rbac.hasAnyPermission("user2", ["users.write", "posts.write"]));
});

test("hasAnyPermission: false if user has none", () => {
    assertEq(rbac.hasAnyPermission("user3", ["users.read", "users.write"]), false);
});

// ── revoke and ungrant ──────────────────────────────────────────────

test("revoke: removes role from user", () => {
    rbac.assign("user4", "admin");
    assertTrue(rbac.hasRole("user4", "admin"));
    rbac.revoke("user4", "admin");
    assertEq(rbac.hasRole("user4", "admin"), false);
});

test("ungrant: removes permission from role", () => {
    rbac.defineRole("temp_role");
    rbac.definePermission("temp.perm");
    rbac.grant("temp_role", "temp.perm");
    rbac.assign("user5", "temp_role");
    assertTrue(rbac.hasPermission("user5", "temp.perm"));
    rbac.ungrant("temp_role", "temp.perm");
    assertEq(rbac.hasPermission("user5", "temp.perm"), false);
});

// ── middleware ──────────────────────────────────────────────────────

test("requireRole: returns function", () => {
    const mw = rbac.requireRole("admin");
    assertEq(typeof mw, "function");
});

test("requirePermission: returns function", () => {
    const mw = rbac.requirePermission("users.read");
    assertEq(typeof mw, "function");
});

export default { pass, fail };

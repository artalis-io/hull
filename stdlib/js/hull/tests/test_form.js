// test_form.js — Tests for hull:form
//
// Tests pure-function URL-encoded body parsing (no runtime globals needed).

import { form } from "hull:form";

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

// ── basic parsing ────────────────────────────────────────────────────

test("simple key-value pair", () => {
    const r = form.parse("name=alice");
    assertEq(r.name, "alice");
});

test("multiple pairs", () => {
    const r = form.parse("email=a%40b.com&pass=secret");
    assertEq(r.email, "a@b.com");
    assertEq(r.pass, "secret");
});

test("plus decoded to space", () => {
    const r = form.parse("msg=hello+world");
    assertEq(r.msg, "hello world");
});

// ── edge cases ───────────────────────────────────────────────────────

test("null returns empty object", () => {
    const r = form.parse(null);
    assertEq(Object.keys(r).length, 0);
});

test("empty string returns empty object", () => {
    const r = form.parse("");
    assertEq(Object.keys(r).length, 0);
});

test("non-string returns empty object", () => {
    const r = form.parse(42);
    assertEq(Object.keys(r).length, 0);
});

test("pair without = is skipped", () => {
    const r = form.parse("noequals&key=val");
    assertEq(r.key, "val");
    assertEq(r.noequals, undefined);
});

test("empty key is skipped", () => {
    const r = form.parse("=value&key=val");
    assertEq(r[""], undefined);
    assertEq(r.key, "val");
});

test("empty value is preserved", () => {
    const r = form.parse("key=");
    assertEq(r.key, "");
});

test("duplicate keys: last wins", () => {
    const r = form.parse("a=1&a=2&a=3");
    assertEq(r.a, "3");
});

test("value with = sign", () => {
    const r = form.parse("data=a=b=c");
    assertEq(r.data, "a=b=c");
});

test("multiple & separators", () => {
    const r = form.parse("a=1&&b=2");
    assertEq(r.a, "1");
    assertEq(r.b, "2");
});

test("key with percent encoding", () => {
    const r = form.parse("my%20key=value");
    assertEq(r["my key"], "value");
});

export default { pass, fail };

// test_validate.js — Tests for hull:validate
//
// Tests pure-function schema validation (no runtime globals needed).

import { validate } from "hull:validate";

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

// ── required ─────────────────────────────────────────────────────────

test("required: missing field fails", () => {
    const [ok, errors] = validate.check({}, { name: { required: true } });
    assertEq(ok, false);
    assertEq(errors.name, "is required");
});

test("required: empty string fails", () => {
    const [ok, errors] = validate.check({ name: "" }, { name: { required: true } });
    assertEq(ok, false);
    assertEq(errors.name, "is required");
});

test("required: present value passes", () => {
    const [ok, errors] = validate.check({ name: "alice" }, { name: { required: true } });
    assertEq(ok, true);
    assertEq(errors, null);
});

test("required: false passes", () => {
    const [ok] = validate.check({ active: false }, { active: { required: true } });
    assertEq(ok, true);
});

test("required: zero passes", () => {
    const [ok] = validate.check({ count: 0 }, { count: { required: true } });
    assertEq(ok, true);
});

// ── optional ─────────────────────────────────────────────────────────

test("optional: undefined skips all rules", () => {
    const [ok] = validate.check({}, { role: { oneof: ["admin", "user"] } });
    assertEq(ok, true);
});

// ── trim ─────────────────────────────────────────────────────────────

test("trim: strips whitespace in-place", () => {
    const data = { name: "  alice  " };
    validate.check(data, { name: { trim: true } });
    assertEq(data.name, "alice");
});

test("trim + required: whitespace-only fails", () => {
    const [ok, errors] = validate.check({ name: "   " }, { name: { required: true, trim: true } });
    assertEq(ok, false);
    assertEq(errors.name, "is required");
});

// ── type ─────────────────────────────────────────────────────────────

test("type string: passes", () => {
    const [ok] = validate.check({ s: "hi" }, { s: { type: "string" } });
    assertEq(ok, true);
});

test("type string: number fails", () => {
    const [ok, errors] = validate.check({ s: 42 }, { s: { type: "string" } });
    assertEq(ok, false);
    assertEq(errors.s, "must be a string");
});

test("type number: passes", () => {
    const [ok] = validate.check({ n: 3.14 }, { n: { type: "number" } });
    assertEq(ok, true);
});

test("type integer: float fails", () => {
    const [ok, errors] = validate.check({ n: 3.5 }, { n: { type: "integer" } });
    assertEq(ok, false);
    assertEq(errors.n, "must be an integer");
});

test("type integer: integer passes", () => {
    const [ok] = validate.check({ n: 42 }, { n: { type: "integer" } });
    assertEq(ok, true);
});

test("type boolean: passes", () => {
    const [ok] = validate.check({ b: true }, { b: { type: "boolean" } });
    assertEq(ok, true);
});

// ── min / max (string) ──────────────────────────────────────────────

test("min string: too short fails", () => {
    const [ok, errors] = validate.check({ pw: "abc" }, { pw: { min: 8 } });
    assertEq(ok, false);
    assertEq(errors.pw, "must be at least 8 characters");
});

test("min string: exact length passes", () => {
    const [ok] = validate.check({ pw: "12345678" }, { pw: { min: 8 } });
    assertEq(ok, true);
});

test("max string: too long fails", () => {
    const [ok, errors] = validate.check({ name: "a very long name indeed" }, { name: { max: 10 } });
    assertEq(ok, false);
    assertEq(errors.name, "must be at most 10 characters");
});

test("max string: exact length passes", () => {
    const [ok] = validate.check({ name: "1234567890" }, { name: { max: 10 } });
    assertEq(ok, true);
});

// ── min / max (number) ──────────────────────────────────────────────

test("min number: too small fails", () => {
    const [ok, errors] = validate.check({ age: 5 }, { age: { min: 18 } });
    assertEq(ok, false);
    assertEq(errors.age, "must be at least 18");
});

test("max number: too large fails", () => {
    const [ok, errors] = validate.check({ age: 200 }, { age: { max: 150 } });
    assertEq(ok, false);
    assertEq(errors.age, "must be at most 150");
});

// ── pattern ──────────────────────────────────────────────────────────

test("pattern: match passes", () => {
    const [ok] = validate.check({ code: "ABC-123" }, { code: { pattern: /^[A-Z]+-\d+$/ } });
    assertEq(ok, true);
});

test("pattern: no match fails", () => {
    const [ok, errors] = validate.check({ code: "abc" }, { code: { pattern: /^[A-Z]+-\d+$/ } });
    assertEq(ok, false);
    assertEq(errors.code, "does not match the required pattern");
});

// ── oneof ────────────────────────────────────────────────────────────

test("oneof: valid value passes", () => {
    const [ok] = validate.check({ role: "admin" }, { role: { oneof: ["admin", "user"] } });
    assertEq(ok, true);
});

test("oneof: invalid value fails", () => {
    const [ok, errors] = validate.check({ role: "root" }, { role: { oneof: ["admin", "user"] } });
    assertEq(ok, false);
    assertEq(errors.role, "must be one of: admin, user");
});

// ── email ────────────────────────────────────────────────────────────

test("email: valid passes", () => {
    const [ok] = validate.check({ e: "a@b.com" }, { e: { email: true } });
    assertEq(ok, true);
});

test("email: no @ fails", () => {
    const [ok, errors] = validate.check({ e: "notanemail" }, { e: { email: true } });
    assertEq(ok, false);
    assertEq(errors.e, "is not a valid email");
});

test("email: no domain fails", () => {
    const [ok] = validate.check({ e: "a@" }, { e: { email: true } });
    assertEq(ok, false);
});

// ── custom fn ────────────────────────────────────────────────────────

test("fn: null return passes", () => {
    const [ok] = validate.check({ x: "ok" }, { x: { fn: () => null } });
    assertEq(ok, true);
});

test("fn: string return fails", () => {
    const [ok, errors] = validate.check({ x: "bad" }, { x: { fn: () => "custom error" } });
    assertEq(ok, false);
    assertEq(errors.x, "custom error");
});

// ── custom message override ─────────────────────────────────────────

test("message override", () => {
    const [ok, errors] = validate.check({}, { name: { required: true, message: "Name needed" } });
    assertEq(ok, false);
    assertEq(errors.name, "Name needed");
});

// ── multiple errors ──────────────────────────────────────────────────

test("multiple fields fail independently", () => {
    const [ok, errors] = validate.check({}, {
        email: { required: true },
        name: { required: true },
    });
    assertEq(ok, false);
    assertEq(errors.email, "is required");
    assertEq(errors.name, "is required");
});

// ── first error wins per field ───────────────────────────────────────

test("first rule error wins", () => {
    const [ok, errors] = validate.check({ pw: "" }, {
        pw: { required: true, min: 8 },
    });
    assertEq(ok, false);
    assertEq(errors.pw, "is required");
});

export default { pass, fail };

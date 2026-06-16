// test_toast.js. Tests for hull:web:htmx:toast.
//
// Verifies the helper sets the HX-Trigger header to the expected
// JSON payload shape. Doesn't exercise the client-side rendering.

import { toast } from "hull:web:htmx:toast";

let pass = 0, fail = 0;
function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + e.message); }
}
function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function mockRes() {
    const headersSet = {};
    return {
        headersSet,
        header(name, value) { headersSet[name] = value; },
    };
}
function decoded(res) {
    const v = res.headersSet["HX-Trigger"];
    return v ? JSON.parse(v) : null;
}

test("show emits HX-Trigger with default level info", () => {
    const res = mockRes();
    toast.show(res, "hello");
    const d = decoded(res);
    assertEq(d.toast.message, "hello");
    assertEq(d.toast.level, "info");
});

test("show coerces unknown level to info", () => {
    const res = mockRes();
    toast.show(res, "x", { level: "marquee" });
    assertEq(decoded(res).toast.level, "info");
});

test("success shorthand emits level=success", () => {
    const res = mockRes();
    toast.success(res, "saved");
    assertEq(decoded(res).toast.level, "success");
});

test("error shorthand emits level=error", () => {
    const res = mockRes();
    toast.error(res, "denied");
    assertEq(decoded(res).toast.level, "error");
});

test("warning shorthand emits level=warning", () => {
    const res = mockRes();
    toast.warning(res, "deprecated");
    assertEq(decoded(res).toast.level, "warning");
});

test("info shorthand emits level=info", () => {
    const res = mockRes();
    toast.info(res, "fyi");
    assertEq(decoded(res).toast.level, "info");
});

test("show passes duration through", () => {
    const res = mockRes();
    toast.show(res, "x", { duration: 8000 });
    assertEq(decoded(res).toast.duration, 8000);
});

test("show passes id through", () => {
    const res = mockRes();
    toast.show(res, "x", { id: "perm-denied" });
    assertEq(decoded(res).toast.id, "perm-denied");
});

test("show coerces null message to empty string", () => {
    const res = mockRes();
    toast.show(res, null);
    assertEq(decoded(res).toast.message, "");
});

test("shorthand opts override level if provided", () => {
    // toast.success(res, msg, { level: "error" }) — shorthand wins.
    const res = mockRes();
    toast.success(res, "x", { level: "error" });
    assertEq(decoded(res).toast.level, "success");
});

// ── Wire-payload type narrowing (M2 from the audit) ─────────────

test("show drops non-number duration silently", () => {
    const res = mockRes();
    toast.show(res, "x", { duration: "8000" });
    const d = decoded(res);
    if (d.toast.duration !== undefined) {
        throw new Error("non-number duration should not reach the wire payload");
    }
});

test("show coerces non-string id to string", () => {
    const res = mockRes();
    toast.show(res, "x", { id: 42 });
    assertEq(decoded(res).toast.id, "42");
});

console.log(`\n${pass}/${pass + fail} toast tests passed`);
if (fail > 0) throw new Error("toast tests failed");

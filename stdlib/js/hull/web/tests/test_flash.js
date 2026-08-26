// test_flash.js - Tests for hull:web:flash
//
// Covers input validation and the HX-Trigger path. The session-backed
// paths (set / consume) require a live SQLite session store; they are
// exercised end-to-end in examples/hypermedia_todo/tests/test_app.js.

import { flash } from "hull:web:flash";
import { json }  from "hull:json";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log(`FAIL: ${name}: ${e.message || e}`);
    }
}

function assertEq(a, b, msg) {
    if (a !== b) {
        throw new Error(`${msg || ""} expected ${b}, got ${a}`);
    }
}

// Fake res object that captures headers in a plain object.
function fakeRes() {
    const headers = {};
    return {
        header(k, v) { headers[k] = v; },
        _headers: headers,
    };
}

// ── trigger ──────────────────────────────────────────────────────────

test("trigger sets HX-Trigger header with default kind=info", () => {
    const res = fakeRes();
    flash.trigger(res, "Saved.");
    const val = res._headers["HX-Trigger"];
    if (!val) throw new Error("header not set");
    const payload = json.decode(val);
    assertEq(payload.flash.text, "Saved.");
    assertEq(payload.flash.kind, "info");
});

test("trigger honors custom kind", () => {
    const res = fakeRes();
    flash.trigger(res, "Bad input", "error");
    const payload = json.decode(res._headers["HX-Trigger"]);
    assertEq(payload.flash.text, "Bad input");
    assertEq(payload.flash.kind, "error");
});

test("trigger coerces non-string text", () => {
    const res = fakeRes();
    flash.trigger(res, 42);
    const payload = json.decode(res._headers["HX-Trigger"]);
    assertEq(payload.flash.text, "42");
});

test("trigger rejects missing res", () => {
    try { flash.trigger(null, "x"); } catch (_) { return; }
    throw new Error("should have thrown");
});

test("trigger rejects missing text", () => {
    try { flash.trigger(fakeRes(), null); } catch (_) { return; }
    throw new Error("should have thrown");
});

// ── set: input validation (no session needed for the error path) ────

test("set rejects missing text", () => {
    const req = { ctx: { session_id: "fake", session: {} } };
    try {
        flash.set(req, null);
    } catch (e) {
        if (!String(e.message).includes("text is required")) {
            throw new Error(`wrong error: ${e.message}`);
        }
        return;
    }
    throw new Error("should have thrown");
});

test("set errors when no session_id in req.ctx", () => {
    const req = { ctx: {} };
    try {
        flash.set(req, "hi");
    } catch (e) {
        if (!String(e.message).includes("no session")) {
            throw new Error(`wrong error: ${e.message}`);
        }
        return;
    }
    throw new Error("should have thrown");
});

test("set errors when req is null", () => {
    try { flash.set(null, "hi"); } catch (_) { return; }
    throw new Error("should have thrown");
});

// ── consume: safe-defaults ───────────────────────────────────────────

test("consume returns empty array when no session", () => {
    const msgs = flash.consume({ ctx: {} });
    assertEq(Array.isArray(msgs), true);
    assertEq(msgs.length, 0);
});

test("consume returns empty when req is null", () => {
    const msgs = flash.consume(null);
    assertEq(Array.isArray(msgs), true);
    assertEq(msgs.length, 0);
});

export default { pass, fail };

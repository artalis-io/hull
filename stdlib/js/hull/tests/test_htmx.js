// test_htmx.js. Tests for hull:htmx
//
// Lua parity: same coverage as stdlib/lua/hull/tests/test_htmx.lua.

import { htmx } from "hull:htmx";

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

// Mock response object recording header() / status() / send() / redirect()
// calls. Mirrors the subset of the Keel response API the helpers use.
function mockRes() {
    const headers = {};
    let statusCode;
    let body;
    return {
        headersSet: headers,
        header(name, value) { headers[name] = value; },
        status(code) { statusCode = code; },
        send(s) { body = s; },
        redirect(path) { headers.__redirect_to = path; },
        getStatus() { return statusCode; },
        getBody() { return body; },
    };
}

// ── Request-inspection helpers ────────────────────────────────────────

test("is() returns true for HX-Request: true", () => {
    assertEq(htmx.is({ headers: { "hx-request": "true" } }), true);
});

test("is() returns false when header absent", () => {
    assertEq(htmx.is({ headers: {} }), false);
});

test("is() handles null req gracefully", () => {
    assertEq(htmx.is(null), false);
    assertEq(htmx.is({}), false);
});

test("boosted() returns true for HX-Boosted: true", () => {
    assertEq(htmx.boosted({ headers: { "hx-boosted": "true" } }), true);
});

test("currentUrl() returns header value", () => {
    const req = { headers: { "hx-current-url": "https://example.com/x" } };
    assertEq(htmx.currentUrl(req), "https://example.com/x");
});

test("target() and triggerName()", () => {
    const req = { headers: {
        "hx-target": "#todo-list",
        "hx-trigger-name": "save-button",
    }};
    assertEq(htmx.target(req), "#todo-list");
    assertEq(htmx.triggerName(req), "save-button");
});

// ── Response-header helpers ──────────────────────────────────────────

test("retarget sets HX-Retarget", () => {
    const res = mockRes();
    htmx.retarget(res, "#errors");
    assertEq(res.headersSet["HX-Retarget"], "#errors");
});

test("reswap sets HX-Reswap", () => {
    const res = mockRes();
    htmx.reswap(res, "outerHTML");
    assertEq(res.headersSet["HX-Reswap"], "outerHTML");
});

test("refresh sets HX-Refresh: true", () => {
    const res = mockRes();
    htmx.refresh(res);
    assertEq(res.headersSet["HX-Refresh"], "true");
});

test("pushUrl sets HX-Push-Url", () => {
    const res = mockRes();
    htmx.pushUrl(res, "/items/42");
    assertEq(res.headersSet["HX-Push-Url"], "/items/42");
});

test("pushUrl(false) suppresses default push", () => {
    const res = mockRes();
    htmx.pushUrl(res, false);
    assertEq(res.headersSet["HX-Push-Url"], "false");
});

test("replaceUrl sets HX-Replace-Url", () => {
    const res = mockRes();
    htmx.replaceUrl(res, "/items/43");
    assertEq(res.headersSet["HX-Replace-Url"], "/items/43");
});

// ── HX-Trigger encoders ──────────────────────────────────────────────

test("trigger with bare event name sends string value", () => {
    const res = mockRes();
    htmx.trigger(res, "saved");
    assertEq(res.headersSet["HX-Trigger"], "saved");
});

test("trigger with event + payload encodes JSON object", () => {
    const res = mockRes();
    htmx.trigger(res, "saved", { id: 42 });
    const v = res.headersSet["HX-Trigger"];
    if (!v) throw new Error("trigger should set header");
    if (v.indexOf('"saved"') < 0) throw new Error("should contain event name");
    if (v.indexOf('"id"') < 0) throw new Error("should contain payload key");
    if (v.indexOf("42") < 0) throw new Error("should contain payload value");
});

test("trigger with object encodes directly", () => {
    const res = mockRes();
    htmx.trigger(res, { saved: { id: 1 }, refresh: true });
    const v = res.headersSet["HX-Trigger"];
    if (v.indexOf('"saved"') < 0) throw new Error("should contain saved");
    if (v.indexOf('"refresh"') < 0) throw new Error("should contain refresh");
});

test("triggerAfterSwap uses HX-Trigger-After-Swap", () => {
    const res = mockRes();
    htmx.triggerAfterSwap(res, "settled");
    assertEq(res.headersSet["HX-Trigger-After-Swap"], "settled");
});

test("triggerAfterSettle uses HX-Trigger-After-Settle", () => {
    const res = mockRes();
    htmx.triggerAfterSettle(res, "done");
    assertEq(res.headersSet["HX-Trigger-After-Settle"], "done");
});

// ── Location helpers ─────────────────────────────────────────────────

test("location with string path sends bare path", () => {
    const res = mockRes();
    htmx.location(res, "/dashboard");
    assertEq(res.headersSet["HX-Location"], "/dashboard");
});

test("location with object encodes as JSON context", () => {
    const res = mockRes();
    htmx.location(res, { path: "/x", target: "#main", swap: "outerHTML" });
    const v = res.headersSet["HX-Location"];
    if (v.indexOf('"/x"') < 0) throw new Error("should contain /x");
    if (v.indexOf('"#main"') < 0) throw new Error("should contain #main");
});

// ── Redirect (the dual-mode helper) ──────────────────────────────────

test("redirect on htmx request sets HX-Redirect + 204", () => {
    const req = { headers: { "hx-request": "true" } };
    const res = mockRes();
    htmx.redirect(req, res, "/after-login");
    assertEq(res.headersSet["HX-Redirect"], "/after-login");
    assertEq(res.getStatus(), 204);
    assertEq(res.getBody(), "");
});

test("redirect on plain request falls back to res.redirect", () => {
    const req = { headers: {} };
    const res = mockRes();
    htmx.redirect(req, res, "/after-login");
    assertEq(res.headersSet.__redirect_to, "/after-login");
    if (res.headersSet["HX-Redirect"] !== undefined)
        throw new Error("HX-Redirect should not be set on plain request");
});

// ── Done ─────────────────────────────────────────────────────────────

print(`hull:htmx: ${pass} passed, ${fail} failed`);
if (fail > 0) throw new Error("hull:htmx tests failed");

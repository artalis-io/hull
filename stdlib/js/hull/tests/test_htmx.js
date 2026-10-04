// test_htmx.js. Tests for hull:web:htmx
//
// Lua parity: same coverage as stdlib/lua/hull/tests/test_htmx.lua.

import { htmx } from "hull:web:htmx";
import { pagination as htmxPagination } from "hull:web:htmx:pagination";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + e.message);
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

test("trigger with opts.timing='swap' uses HX-Trigger-After-Swap", () => {
    const res = mockRes();
    htmx.trigger(res, "settled", null, { timing: "swap" });
    assertEq(res.headersSet["HX-Trigger-After-Swap"], "settled");
});

test("trigger with opts.timing='settle' uses HX-Trigger-After-Settle", () => {
    const res = mockRes();
    htmx.trigger(res, "done", null, { timing: "settle" });
    assertEq(res.headersSet["HX-Trigger-After-Settle"], "done");
});

test("trigger with opts as 3rd arg when event is an object", () => {
    const res = mockRes();
    htmx.trigger(res, { saved: { id: 1 } }, { timing: "swap" });
    const v = res.headersSet["HX-Trigger-After-Swap"];
    if (v.indexOf('"saved"') < 0) throw new Error("should contain saved");
});

test("trigger with opts.timing='swap' + payload encodes JSON", () => {
    const res = mockRes();
    htmx.trigger(res, "saved", { id: 42 }, { timing: "swap" });
    const v = res.headersSet["HX-Trigger-After-Swap"];
    if (v.indexOf('"saved"') < 0) throw new Error("should contain saved");
    if (v.indexOf("42") < 0) throw new Error("should contain 42");
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

// htmx assigns HX-Redirect to location.href: a javascript: URL ran script.
test("redirect refuses non-http(s) schemes and control chars", () => {
    const req = { headers: { "hx-request": "true" } };
    for (const bad of ["javascript:alert(1)", "JavaScript:x", "data:text/html,x",
                       " javascript:x", "java\tscript:x", "/a\nb", "", "\\evil", null]) {
        const res = mockRes();
        let threw = false;
        try { htmx.redirect(req, res, bad); } catch (e) { threw = true; }
        if (!threw) throw new Error("accepted " + bad);
        if (res.headersSet["HX-Redirect"] !== undefined) throw new Error("header set for " + bad);
    }
    for (const good of ["/x", "x/y", "?page=2", "#top", "https://a.example/p",
                        "HTTP://a.example", "/a:b"]) {
        const res = mockRes();
        htmx.redirect(req, res, good);
        assertEq(res.headersSet["HX-Redirect"], good);
    }
});

test("redirect on plain request falls back to res.redirect", () => {
    const req = { headers: {} };
    const res = mockRes();
    htmx.redirect(req, res, "/after-login");
    assertEq(res.headersSet.__redirect_to, "/after-login");
    if (res.headersSet["HX-Redirect"] !== undefined)
        throw new Error("HX-Redirect should not be set on plain request");
});

// ── htmx pagination nav ─────────────────────────────────────────────

// baseUrl / defaultPerPage reach the page math: they were passed as
// base_url / default_per_page, which render() ignores, so the links were
// "?page=2" (the current page's route) with per_page always appended.
test("pagination.nav honours baseUrl and defaultPerPage", () => {
    const s = htmxPagination.nav(100, { page: 1, perPage: 10, defaultPerPage: 10,
                                        baseUrl: "/todos/rows", target: "#rows" });
    if (s.indexOf('hx-get="/todos/rows?page=2"') < 0)
        throw new Error("expected a link to /todos/rows?page=2 in " + s);
    if (s.indexOf("per_page=") >= 0)
        throw new Error("per_page appended although it is the default: " + s);
});

// ── Done ─────────────────────────────────────────────────────────────

console.log(`hull:web:htmx: ${pass} passed, ${fail} failed`);
// Counts go back to the C harness (run_js_test in
// tests/hull/runtime/js/test_js.c), which reads them off the global object --
// a module's default export is not reachable from there.
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;

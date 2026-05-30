// test_csp.js. Tests for hull:middleware:csp
//
// Lua parity: same coverage as stdlib/lua/hull/middleware/tests/test_csp.lua.

import { csp } from "hull:middleware:csp";

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
    if (!v) throw new Error(msg || "expected truthy value");
}

function assertContains(haystack, needle, msg) {
    if (!haystack || haystack.indexOf(needle) < 0)
        throw new Error((msg || "") + " expected '" + needle +
                        "' in: " + String(haystack));
}

function assertNotContains(haystack, needle, msg) {
    if (haystack && haystack.indexOf(needle) >= 0)
        throw new Error((msg || "") + " expected '" + needle +
                        "' NOT in: " + String(haystack));
}

function mockReq() { return { ctx: {} }; }

function mockRes() {
    const headers = {};
    let statusCode;
    let body;
    return {
        headersSet: headers,
        header(name, value) { headers[name] = value; },
        status(code) { statusCode = code; },
        json(data) { body = data; },
        getStatus() { return statusCode; },
        getBody() { return body; },
    };
}

// ── nonce() helper ───────────────────────────────────────────────────

test("nonce() returns a non-empty base64url string", () => {
    const n = csp.nonce();
    assertTrue(n !== null && n !== undefined);
    assertTrue(n.length >= 20, "128-bit base64url should be 22 chars");
    assertTrue(/^[A-Za-z0-9_-]+$/.test(n), "must match base64url alphabet");
});

test("two nonces differ (RNG sanity)", () => {
    assertTrue(csp.nonce() !== csp.nonce(), "successive nonces must differ");
});

// ── csp.htmx() ───────────────────────────────────────────────────────

test("htmx() sets CSP header and exposes cspNonce in ctx", () => {
    const mw = csp.htmx();
    const req = mockReq(), res = mockRes();
    const rc = mw(req, res);
    assertEq(rc, 0, "middleware should pass through");
    assertTrue(req.ctx.cspNonce !== undefined, "ctx.cspNonce must be set");
    const hdr = res.headersSet["Content-Security-Policy"];
    assertTrue(hdr !== undefined, "CSP header must be set");
    assertContains(hdr, "default-src 'self'");
    assertContains(hdr, "script-src 'self' 'nonce-");
    assertContains(hdr, "style-src 'self' 'nonce-");
    assertContains(hdr, "style-src-attr 'unsafe-inline'");
    assertContains(hdr, "frame-ancestors 'none'");
    assertContains(hdr, "base-uri 'self'");
    assertContains(hdr, "'nonce-" + req.ctx.cspNonce + "'",
                   "header nonce must equal ctx.cspNonce");
});

test("htmx() generates a fresh nonce per request", () => {
    const mw = csp.htmx();
    const req1 = mockReq(), res1 = mockRes();
    const req2 = mockReq(), res2 = mockRes();
    mw(req1, res1);
    mw(req2, res2);
    assertTrue(req1.ctx.cspNonce !== req2.ctx.cspNonce,
               "per-request nonces must differ");
});

// ── csp.strict() ─────────────────────────────────────────────────────

test("strict() omits style-src-attr 'unsafe-inline'", () => {
    const mw = csp.strict();
    const req = mockReq(), res = mockRes();
    mw(req, res);
    const hdr = res.headersSet["Content-Security-Policy"];
    assertTrue(hdr !== undefined);
    assertContains(hdr, "style-src 'self' 'nonce-");
    assertNotContains(hdr, "style-src-attr",
                      "strict profile must not include style-src-attr");
});

// ── reportOnly ───────────────────────────────────────────────────────

test("reportOnly: true uses CSP-Report-Only header", () => {
    const mw = csp.htmx({ reportOnly: true });
    const req = mockReq(), res = mockRes();
    mw(req, res);
    assertTrue(res.headersSet["Content-Security-Policy-Report-Only"] !== undefined,
               "Report-Only header must be set");
    assertEq(res.headersSet["Content-Security-Policy"], undefined,
             "regular CSP header must NOT be set in report-only mode");
});

print(`hull:middleware:csp: ${pass} passed, ${fail} failed`);
if (fail > 0) throw new Error("csp tests failed");

// Regressions from the 2026-10-10 audit. Run by test_js.c.
import { template } from "hull:template";
import { etag } from "hull:web:middleware:etag";
import { config } from "hull:config";
import { encoding } from "hull:encoding";

let pass = 0, fail = 0;
function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + e); }
}
function eq(actual, expected) {
    if (actual !== expected) throw new Error("expected " + expected + ", got " + actual);
}

test("nested value loops restore the outer binding", () => {
    eq(template.renderString(
        "{% for item in items %}{{item.name}}:{% for item in item.children %}{{item}}{% end %}:{{item.name}}{% end %}",
        { items: [{ name: "outer", children: ["inner"] }], item: { name: "ROOT" } }),
        "outer:inner:outer");
});
test("nested key/value loops restore both outer bindings", () => {
    eq(template.renderString(
        "{% for k, v in items %}{{k}}:{{v.name}}:{% for k, v in v.children %}{{k}}:{{v}}{% end %}:{{k}}:{{v.name}}{% end %}",
        { items: { outer: { name: "parent", children: { inner: "child" } } },
          k: "ROOT", v: { name: "ROOT" } }), "outer:parent:inner:child:outer:parent");
});
test("include names never inherit visited keys", () => {
    // The C harness embeds these four templates in the application VFS.
    for (const name of ["toString", "constructor", "__proto__"]) {
        eq(template.renderString('{% if yes %}{% include "' + name + '" %}{% end %}',
                                 { yes: true }), "ok");
    }
    let rejected = false;
    try { template.renderString('{% if yes %}{% include "cycle" %}{% end %}', { yes: true }); }
    catch (e) { rejected = /circular include/.test(String(e)); }
    eq(rejected, true);
});

for (const helper of ["json", "text", "html"]) {
    for (const method of ["GET", "HEAD"]) {
        for (const status of [201, 301, 401, 404, 500]) {
            test(helper + " " + method + " preserves " + status, () => {
                const res = { code: 200, sent: false,
                    status(code) { this.code = code; return this; },
                    header() { return this; },
                    [helper]() { this.sent = true; return this; } };
                etag[helper]({ method, header: () => "*" }, res,
                             helper === "json" ? { error: "failed" } : "failed", status);
                eq(res.code, status); eq(res.sent, true);
            });
        }
        test(helper + " " + method + " still returns 304 for 200", () => {
            const res = { code: 200, sent: false,
                status(code) { this.code = code; return this; },
                header() { return this; },
                [helper]() { this.sent = true; return this; } };
            etag[helper]({ method, header: () => "*" }, res,
                         helper === "json" ? { ok: true } : "ok", 200);
            eq(res.code, 304); eq(res.sent, false);
        });
    }
}

test("dotenv preserves UTF-8 and rejects malformed input before applying keys", () => {
    // The env / fs exports are frozen (audit 18): config's own seam stands
    // in for them.
    const hooks = __testHooks("hull:config");
    try {
        const base = { get: () => null, allowed: name => name === "AUDIT_UTF8" };
        hooks.setIo(Object.assign({}, base, {
            read: () => encoding.bytes.toU8(encoding.utf8.encode("AUDIT_UTF8=árvíz 😀\nDENIED=bad\n")).buffer }));
        eq(config.loadDotenv("audit.env"), 1);
        eq(config.get("AUDIT_UTF8"), "árvíz 😀");
        hooks.setIo(Object.assign({}, base, {
            read: () => new Uint8Array([65, 61, 255]).buffer }));
        let rejected = false;
        try { config.loadDotenv("bad.env"); }
        catch (e) { rejected = /UTF-8/.test(String(e)); }
        eq(rejected, true);
        eq(config.get("AUDIT_UTF8"), "árvíz 😀");
    } finally { hooks.setIo(null); }
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;

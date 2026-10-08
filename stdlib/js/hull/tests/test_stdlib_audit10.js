// test_stdlib_audit10.js - stdlib regressions from audit 10 (JS).
//
// logx / _logfmt key + control-character escaping, cache.fetch not caching
// null or a rejected Promise, i18n Accept-Language order and date range /
// token replacement, qrcode option validation and UTF-8 byte mode, csv
// delimiters / BOM. Asserts the same outputs as the Lua twin
// (stdlib/lua/hull/tests/test_stdlib_audit10.lua). Runs in the caps-bearing
// context (run_js_test in tests/hull/runtime/js/test_js.c).

import { logx } from "hull:logx";
import { cache } from "hull:cache";
import { i18n } from "hull:i18n";
import { qrcode } from "hull:qrcode";
import { csv } from "hull:csv";

let pass = 0;
let fail = 0;

async function test(name, fn) {
    try {
        await fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

function raises(fn, want) {
    let err = null;
    try { fn(); } catch (e) { err = e; }
    if (err === null) throw new Error("expected an error");
    const m = String(err && err.message);
    if (want && m.indexOf(want) < 0)
        throw new Error("expected error containing '" + want + "', got: " + m);
}

// ── logx / _logfmt ─────────────────────────────────────────────────

await test("logx: keys are reduced to [A-Za-z0-9_.-]", () => {
    assertEq(logx.fields({ "a b": 1 }), " a_b=1");
    assertEq(logx.fields({ "x=y": "v" }), " x_y=v");
    assertEq(logx.fields({ "k\nforged=1": "v" }), " k_forged_1=v");
    assertEq(logx.fields({ "é": "v" }), " __=v", "one _ per UTF-8 byte, as Lua");
    assertEq(logx.fields({ "": "v" }), " _=v");
    assertEq(logx.fields({ "req.id-2_x": "v" }), " req.id-2_x=v");
});

await test("logx: control characters in values are escaped and quoted", () => {
    assertEq(logx.fields({ k: "a\nb" }), ' k="a\\nb"');
    assertEq(logx.fields({ k: "a\tb" }), ' k="a\\tb"');
    assertEq(logx.fields({ k: "\x1b[31mred" }), ' k="\\x1b[31mred"');
    assertEq(logx.fields({ k: "nul\0" }), ' k="nul\\x00"');
    assertEq(logx.fields({ k: "del\x7f" }), ' k="del\\x7f"');
    assertEq(logx.fields({ k: 'q"\\' }), ' k="q\\"\\\\"');
    assertEq(logx.fields({ k: "plain" }), " k=plain");
});

// ── cache.fetch ────────────────────────────────────────────────────

await test("cache.fetch: a null / undefined result is not cached", () => {
    const c = cache.new();
    let calls = 0;
    const f = () => { calls++; return null; };
    assertEq(c.fetch("k", 60, f), null);
    assertEq(c.fetch("k", 60, f), null);
    assertEq(calls, 2, "fn runs again after a null");
    assertEq(c.fetch("u", 60, () => undefined), undefined);
    assertEq(c.has("k"), false);
    assertEq(c.has("u"), false);
    assertEq(c.fetch("k", 60, () => "v"), "v");
    assertEq(c.fetch("k", 60, () => "other"), "v", "a value is cached");
});

await test("cache.fetch: a rejected Promise is dropped", async () => {
    const c = cache.new();
    const p = c.fetch("k", 60, () => Promise.reject(new Error("boom")));
    assertEq(c.has("k"), true, "shared while pending");
    let rejected = false;
    try { await p; } catch (_e) { rejected = true; }
    assertEq(rejected, true);
    await null;
    assertEq(c.has("k"), false, "the rejection is not cached");
    const ok = c.fetch("k", 60, async () => 7);
    assertEq(await ok, 7);
    await null;
    assertEq(c.has("k"), true, "a resolved Promise stays");
    c.fetch("n", 60, async () => null);
    await null; await null;
    assertEq(c.has("n"), false, "a Promise of null is a miss");
});

// ── i18n ───────────────────────────────────────────────────────────

await test("i18n.detect: equal q keeps header order, q=0 is skipped", () => {
    i18n.reset();
    i18n.load("de", { hi: "hallo" });
    i18n.load("fr", { hi: "salut" });
    i18n.load("hu", { hi: "szia" });
    assertEq(i18n.detect("fr;q=0.5, de;q=0.5, hu;q=0.5"), "fr");
    assertEq(i18n.detect("hu;q=0.5, de;q=0.5, fr;q=0.5"), "hu");
    assertEq(i18n.detect("de;q=0, fr;q=0.1"), "fr", "q=0 is not acceptable");
    assertEq(i18n.detect("de;q=0"), null);
    assertEq(i18n.detect("de;q=1.2.3, fr;q=0.1"), "fr", "an unparseable q is 0");
    i18n.reset();
});

await test("i18n.detect: the prefix fallback is deterministic", () => {
    i18n.reset();
    i18n.load("en-US", { hi: "hi" });
    i18n.load("en-GB", { hi: "hi" });
    i18n.load("end", { hi: "x" });
    assertEq(i18n.detect("en"), "en-GB", "sorted: en-GB before en-US");
    i18n.reset();
    i18n.load("end", { hi: "x" });
    assertEq(i18n.detect("en"), null, "en does not match end");
    i18n.reset();
});

await test("i18n.date: out-of-range timestamps come back as text", () => {
    i18n.reset();
    assertEq(i18n.dateIn(null, 0), "1970-01-01");
    assertEq(i18n.dateIn(null, 253402300799), "9999-12-31");
    assertEq(i18n.dateIn(null, -62167219200), "0000-01-01");
    assertEq(i18n.dateIn(null, -86400), "1969-12-31");
    assertEq(i18n.dateIn(null, -1), "1969-12-31", "floor modulo, as Lua");
    assertEq(i18n.dateIn(null, 1e300), "1e+300");
    assertEq(i18n.dateIn(null, -1e300), "-1e+300");
    assertEq(i18n.dateIn(null, 253402300800), "253402300800");
});

await test("i18n.date: every occurrence of a token is replaced", () => {
    i18n.reset();
    i18n.load("x", { format: { datePattern: "DD/MM/YYYY (YYYY) HH:mm:ss" } });
    i18n.load("y", { format: { datePattern: "YYYY$&" } });
    // 2001-02-03T04:05:06Z
    assertEq(i18n.dateIn("x", 981173106), "03/02/2001 (2001) 04:05:06");
    assertEq(i18n.dateIn("y", 981173106), "2001$&");
    i18n.reset();
});

// ── qrcode ─────────────────────────────────────────────────────────

await test("qrcode: scale / margin must be bounded integers", () => {
    raises(() => qrcode.svg("x", { scale: 2.5 }), "opts.scale");
    raises(() => qrcode.svg("x", { scale: 0 }), "opts.scale");
    raises(() => qrcode.svg("x", { scale: 65 }), "opts.scale");
    raises(() => qrcode.svg("x", { scale: '4" onload="x' }), "opts.scale");
    raises(() => qrcode.svg("x", { margin: -1 }), "opts.margin");
    raises(() => qrcode.svg("x", { margin: "1" }), "opts.margin");
    const s = qrcode.svg("x", { scale: 2, margin: 0 });
    assertEq(s.indexOf('width="42"') >= 0, true, "21 modules * 2, no margin");
});

await test("qrcode: mask must be 0..7", () => {
    raises(() => qrcode.encode("x", { mask: 8 }), "opts.mask");
    raises(() => qrcode.encode("x", { mask: -1 }), "opts.mask");
    raises(() => qrcode.encode("x", { mask: 1.5 }), "opts.mask");
    raises(() => qrcode.encode("x", { mask: "1" }), "opts.mask");
    assertEq(qrcode.encode("x", { mask: 3 }).mask, 3);
    raises(() => qrcode.encode(42), "text must be a string");
});

await test("qrcode: byte mode carries the UTF-8 bytes", () => {
    // 17 x U+00E9 = 34 UTF-8 bytes: version 3 at EC M, as in Lua (the low
    // byte of each character made it 17 bytes and version 2).
    assertEq(qrcode.encode("é".repeat(17)).version, 3);
    // ASCII is unchanged; a lone surrogate is U+FFFD (3 bytes), no throw.
    assertEq(qrcode.encode("hello").version, 1);
    assertEq(qrcode.encode("a\ud800b").version, 1);
});

// ── csv ────────────────────────────────────────────────────────────

await test("csv: separator / quote must be one ASCII character", () => {
    raises(() => csv.parse("a;;b", { separator: ";;" }), "opts.separator");
    raises(() => csv.parse("a", { quote: "''" }), "opts.quote");
    raises(() => csv.parse("a", { separator: "\n" }), "opts.separator");
    raises(() => csv.parse("a", { separator: "é" }), "opts.separator");
    raises(() => csv.encode([["a"]], { separator: ";;" }), "opts.separator");
    raises(() => csv.parse("a", { separator: "'", quote: "'" }), "must differ");
    assertEq(csv.parse("a\tb\n", { separator: "\t" })[0][1], "b");
});

await test("csv: a leading BOM is not data", () => {
    const r = csv.parse("﻿name,age\nann,3\n", { headers: true });
    assertEq(r[0].name, "ann");
    assertEq(csv.parse("﻿").length, 0);
});

await test("csv: maxRows is the exact cap", () => {
    assertEq(csv.parse("a\nb\n", { maxRows: 2 }).length, 2);
    assertEq(csv.parse("a\nb", { maxRows: 2 }).length, 2);
    raises(() => csv.parse("a\nb\nc", { maxRows: 2 }), "maxRows");
    raises(() => csv.parse("a\nb\n\n", { maxRows: 2 }), "maxRows");
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;

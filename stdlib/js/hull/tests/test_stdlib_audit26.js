// test_stdlib_audit26.js - stdlib regressions from audit 26 (JS stdlib).
//
// template: safe_url percent-encodes the characters that could end an
// attribute or open a tag, so raw output ({{{ u | safe_url }}}) is inert;
// a dot path never renders a function (parity with the Lua twin).
// search: snippet / highlight HTML-escape the indexed text; only the
// caller's before / after / ellipsis markers are inserted as given.
//
// Runs in the caps-bearing context (run_js_test in
// tests/hull/runtime/js/test_js.c): search needs the db.

import { template } from "hull:template";
import { search } from "hull:search";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try {
        fn();
        pass++;
    } catch (e) {
        fail++;
        console.log("FAIL: " + name + ": " + (e && e.message));
    }
}

function assertEq(a, b, msg) {
    if (a !== b) throw new Error((msg || "") + " expected " + b + ", got " + a);
}

// ── template: safe_url ─────────────────────────────────────────────

test("safe_url: raw output of an https URL cannot break out", () => {
    const out = template.renderString("{{{ u | safe_url }}}",
        { u: 'https://a"><script>x</script>' });
    assertEq(out, "https://a%22%3E%3Cscript%3Ex%3C/script%3E");
});

test("safe_url: quotes, backtick, whitespace and controls encoded", () => {
    const out = template.renderString("{{{ u | safe_url }}}",
        { u: "/p?a='b` c\t\n\u0001" });
    assertEq(out, "/p?a=%27b%60%20c%09%0A%01");
});

test("safe_url: escaped output of an encoded URL is unchanged", () => {
    const out = template.renderString("{{ u | safe_url }}",
        { u: 'mailto:x@y.test?subject="hi"' });
    assertEq(out, "mailto:x@y.test?subject=%22hi%22");
});

test("safe_url: an ordinary URL passes through", () => {
    assertEq(template.renderString("{{{ u | safe_url }}}",
        { u: "https://x.test/a?b=1&c=2#f" }), "https://x.test/a?b=1&c=2#f");
});

test("safe_url: javascript: still refused", () => {
    assertEq(template.renderString("{{{ u | safe_url }}}",
        { u: "javascript:alert(1)" }), "#");
});

// ── template: dot paths ────────────────────────────────────────────

test("dot path: a function value renders nothing", () => {
    const out = template.renderString("[{{ f }}][{{ o.g }}]",
        { f: function secret() { return 1; }, o: { g: () => 2 } });
    assertEq(out, "[][]");
});

test("dot path: own data still renders", () => {
    const out = template.renderString(
        "{{ user.name }}:{{ user.name.length }}:{% for it in items %}{{ it.v }}{% end %}",
        { user: { name: "ann" }, items: [{ v: 1 }, { v: 2 }] });
    assertEq(out, "ann:3:12");
});

// ── search: snippet / highlight escaping ───────────────────────────

search.createIndex("a26docs", ["title", "body"]);
search.index("a26docs", "1", {
    title: "t",
    body: "needle <img src=x onerror=alert(1)> & \"q\" 's'",
});

test("search snippet: indexed markup is escaped, markers inserted", () => {
    const rows = search.query("a26docs", "needle", { snippet: { column: 2, tokens: 32 } });
    assertEq(rows.length, 1);
    const s = rows[0].snippet;
    assertEq(s.indexOf("<img"), -1, "raw markup leaked: " + s);
    assertEq(s.indexOf("<b>needle</b>") >= 0, true, "markers: " + s);
    assertEq(s.indexOf("&lt;img src=x onerror=alert(1)&gt; &amp; &quot;q&quot; &#39;s&#39;") >= 0,
             true, "escaped text: " + s);
});

test("search snippet: caller markers are used as given", () => {
    const rows = search.query("a26docs", "needle",
        { snippet: { column: 2, tokens: 32, before: "<mark class=\"h\">", after: "</mark>" } });
    const s = rows[0].snippet;
    assertEq(s.indexOf("<mark class=\"h\">needle</mark>") >= 0, true, s);
    assertEq(/[-]/.test(s), false, "sentinel left: " + s);
});

test("search highlight: indexed markup is escaped", () => {
    const rows = search.query("a26docs", "needle", { highlight: { column: 2 } });
    const h = rows[0].highlight;
    assertEq(h.slice(0, 13), "<b>needle</b>", h);
    assertEq(h.indexOf("<img"), -1, "raw markup leaked: " + h);
});

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;
console.log("stdlib audit 26: " + pass + " passed, " + fail + " failed");

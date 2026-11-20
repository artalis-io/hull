// test_stdlib_audit49.js - stdlib regressions from audit 49 (JS stdlib).
//
// template: a loop variable bound to a function renders nothing, as a dot
// path does since audit 26 ({{ it }} printed the function's source text).
// search: snippet / highlight markers are drawn per query, so indexed text
// holding U+E000-U+E002 no longer gets the caller's markup.
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

function secret() { return "sk_live_123"; }

// ── template: loop variables ───────────────────────────────────────

test("template_audit49.loop_var_function", () => {
    const out = template.renderString(
        "{% for it in list %}[{{ it }}|{{{ it }}}]{% end %}",
        { list: [secret, 1, () => 2] });
    assertEq(out, "[|][1|1][|]");
});

test("template_audit49.loop_kv_function", () => {
    const out = template.renderString(
        "{% for k, v in obj %}<{{ k }}={{{ v }}}>{% end %}",
        { obj: { greet: secret } });
    assertEq(out, "<greet=>");
});

test("template_audit49.loop_var_filter_input", () => {
    const out = template.renderString(
        "{% for it in list %}[{{ it | upper }}|{{ it | default: \"d\" }}]{% end %}",
        { list: [secret] });
    assertEq(out, "[|d]");
});

test("template_audit49.loop_var_condition", () => {
    const out = template.renderString(
        "{% for it in list %}{% if it %}T{% else %}F{% end %}{% end %}",
        { list: [secret, "x"] });
    assertEq(out, "FT");
});

test("template_audit49.loop_var_values_still_render", () => {
    const out = template.renderString(
        "{% for it in list %}{{ it.name }}{% for x in it.tags %},{{ x }}{% end %};{% end %}",
        { list: [{ name: "a<", tags: ["t1", "t2"] }] });
    assertEq(out, "a&lt;,t1,t2;");
});

// ── search: planted markers ────────────────────────────────────────

search.createIndex("a49docs", ["body"]);
search.index("a49docs", "1",
    { body: "hello  world  more  tail" });

test("search_audit49.highlight_planted_marker", () => {
    const rows = search.query("a49docs", "hello",
        { highlight: { column: 1, before: "<mark>", after: "</mark>" } });
    assertEq(rows[0].highlight,
        "<mark>hello</mark>  world  more  tail");
});

test("search_audit49.snippet_planted_marker", () => {
    const rows = search.query("a49docs", "tail",
        { snippet: { column: 1, tokens: 64, before: "<b>", after: "</b>",
                     ellipsis: "~" } });
    assertEq(rows[0].snippet,
        "hello  world  more  <b>tail</b>");
});

search.dropIndex("a49docs");

globalThis.__test_pass = pass;
globalThis.__test_fail = fail;

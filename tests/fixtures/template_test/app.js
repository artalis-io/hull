/*
 * Template engine test app (JS)
 * Each route tests a different template feature and returns JSON pass/fail.
 */

import { app } from "hull:app";
import { template } from "hull:template";

// Helper: compare expected vs actual
function check(name, actual, expected) {
    if (actual === expected) {
        return { test: name, ok: true };
    } else {
        return { test: name, ok: false, expected, actual };
    }
}

app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
});

// Test 1: plain text passthrough
app.get("/test/text", (_req, res) => {
    const html = template.renderString("Hello World", {});
    res.json(check("text", html, "Hello World"));
});

// Test 2: variable interpolation with HTML escaping
app.get("/test/var-escape", (_req, res) => {
    const html = template.renderString("{{ name }}", { name: '<script>alert("xss")</script>' });
    res.json(check("var-escape", html, '&lt;script&gt;alert(&quot;xss&quot;)&lt;/script&gt;'));
});

// Test 3: raw output (no escaping)
app.get("/test/raw", (_req, res) => {
    const html = template.renderString("{{{ html }}}", { html: "<b>bold</b>" });
    res.json(check("raw", html, "<b>bold</b>"));
});

// Test 4: dot path access
app.get("/test/dot-path", (_req, res) => {
    const html = template.renderString("{{ user.name }}", { user: { name: "Alice" } });
    res.json(check("dot-path", html, "Alice"));
});

// Test 5: nil-safe dot path (missing intermediate)
app.get("/test/nil-path", (_req, res) => {
    const html = template.renderString("{{ user.name }}", {});
    res.json(check("nil-path", html, ""));
});

// Test 6: if/else (true branch)
app.get("/test/if-true", (_req, res) => {
    const html = template.renderString("{% if show %}YES{% else %}NO{% end %}", { show: true });
    res.json(check("if-true", html, "YES"));
});

// Test 7: if/else (false branch)
app.get("/test/if-false", (_req, res) => {
    const html = template.renderString("{% if show %}YES{% else %}NO{% end %}", { show: false });
    res.json(check("if-false", html, "NO"));
});

// Test 8: if/elif/else
app.get("/test/elif", (_req, res) => {
    const html = template.renderString(
        "{% if a %}A{% elif b %}B{% else %}C{% end %}",
        { a: false, b: true });
    res.json(check("elif", html, "B"));
});

// Test 9: if not
app.get("/test/if-not", (_req, res) => {
    const html = template.renderString("{% if not hide %}VISIBLE{% end %}", { hide: false });
    res.json(check("if-not", html, "VISIBLE"));
});

// Test 10: for loop with array
app.get("/test/for", (_req, res) => {
    const html = template.renderString(
        "{% for item in items %}{{ item }},{% end %}",
        { items: ["a", "b", "c"] });
    res.json(check("for", html, "a,b,c,"));
});

// Test 11: for loop with dot path on loop variable
app.get("/test/for-dot", (_req, res) => {
    const html = template.renderString(
        "{% for u in users %}{{ u.name }},{% end %}",
        { users: [{ name: "Alice" }, { name: "Bob" }] });
    res.json(check("for-dot", html, "Alice,Bob,"));
});

// Test 12: for loop over nil (should produce empty)
app.get("/test/for-nil", (_req, res) => {
    const html = template.renderString("{% for x in missing %}{{ x }}{% end %}", {});
    res.json(check("for-nil", html, ""));
});

// Test 13: nested for + if
app.get("/test/nested", (_req, res) => {
    const html = template.renderString(
        "{% for u in users %}{% if u.active %}{{ u.name }},{% end %}{% end %}",
        { users: [{ name: "Alice", active: true }, { name: "Bob", active: false }, { name: "Carol", active: true }] });
    res.json(check("nested", html, "Alice,Carol,"));
});

// Test 14: filters — upper, lower, trim, length
app.get("/test/filters", (_req, res) => {
    const results = [];
    results.push(check("upper", template.renderString("{{ x | upper }}", { x: "hello" }), "HELLO"));
    results.push(check("lower", template.renderString("{{ x | lower }}", { x: "HELLO" }), "hello"));
    results.push(check("trim", template.renderString("{{ x | trim }}", { x: "  hi  " }), "hi"));
    results.push(check("length", template.renderString("{{ items | length }}", { items: [1, 2, 3] }), "3"));
    results.push(check("default-nil", template.renderString('{{ x | default: "fallback" }}', {}), "fallback"));
    results.push(check("default-set", template.renderString('{{ x | default: "fallback" }}', { x: "value" }), "value"));
    results.push(check("json", template.renderString("{{ x | json }}", { x: [1, 2] }), "[1,2]"));
    res.json(results);
});

// Test 15: comment stripping
app.get("/test/comment", (_req, res) => {
    const html = template.renderString("A{# this is a comment #}B", {});
    res.json(check("comment", html, "AB"));
});

// Test 16: template inheritance (extends + block)
app.get("/test/extends", (_req, res) => {
    const html = template.render("pages/child.html", { year: "2026" });
    res.json(check("extends", html, "HEADER-Child Title-Child Body-FOOTER 2026"));
});

// Test 17: include
app.get("/test/include", (_req, res) => {
    const html = template.render("pages/with_include.html", { name: "World" });
    res.json(check("include", html, "Before-Hello World!-After"));
});

// Test 18: for_kv (key, value pairs)
app.get("/test/for-kv", (_req, res) => {
    // Note: object key order is insertion order in JS
    const html = template.renderString(
        "{% for k, v in data %}{{ k }}={{ v }};{% end %}",
        { data: { x: 1 } });
    res.json(check("for-kv", html, "x=1;"));
});

// Test 19: if/else with for inside if
app.get("/test/if-for-else", (_req, res) => {
    // With items
    const html1 = template.renderString(
        "{% if show %}{% for i in items %}{{ i }}{% end %}{% else %}empty{% end %}",
        { show: true, items: ["a", "b"] });
    // Without items
    const html2 = template.renderString(
        "{% if show %}content{% else %}empty{% end %}",
        { show: false });
    const results = [];
    results.push(check("if-for-else-true", html1, "ab"));
    results.push(check("if-for-else-false", html2, "empty"));
    res.json(results);
});

// Test 20: XSS safety — all {{ }} output is escaped
app.get("/test/xss", (_req, res) => {
    const html = template.renderString("{{ input }}", { input: '"><img src=x onerror=alert(1)>' });
    const safe = !html.includes("<img");
    res.json(check("xss", safe, true));
});

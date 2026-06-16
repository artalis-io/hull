// test_htmx_search.js. Tests for hull:web:htmx:search helpers.

import { search } from "hull:web:htmx:search";

let pass = 0, fail = 0;
function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log("FAIL: " + name + ": " + e.message); }
}
function assertMatch(s, sub, msg) {
    if (s.indexOf(sub) < 0) throw new Error((msg || "") + " expected '" + sub + "' in '" + s + "'");
}
function assertNoMatch(s, sub, msg) {
    if (s.indexOf(sub) >= 0) throw new Error((msg || "") + " did not expect '" + sub + "' in '" + s + "'");
}

// ── inputAttrs() ────────────────────────────────────────────────────

test("inputAttrs emits type=search + autocomplete=off", () => {
    const s = search.inputAttrs({ url: "/x" });
    assertMatch(s, 'type="search"');
    assertMatch(s, 'autocomplete="off"');
});

test("inputAttrs default method is GET", () => {
    const s = search.inputAttrs({ url: "/search" });
    assertMatch(s, 'hx-get="/search"');
    assertNoMatch(s, "hx-post");
});

test("inputAttrs method=post emits hx-post", () => {
    const s = search.inputAttrs({ url: "/search", method: "post" });
    assertMatch(s, 'hx-post="/search"');
    assertNoMatch(s, "hx-get");
});

test("inputAttrs default debounce is 300ms", () => {
    assertMatch(search.inputAttrs({ url: "/x" }), 'hx-trigger="input changed delay:300ms"');
});

test("inputAttrs custom delayMs takes effect", () => {
    assertMatch(search.inputAttrs({ url: "/x", delayMs: 500 }), "delay:500ms");
});

test("inputAttrs default name is q", () => {
    assertMatch(search.inputAttrs({ url: "/x" }), 'name="q"');
});

test("inputAttrs custom name overrides q", () => {
    assertMatch(search.inputAttrs({ url: "/x", name: "query" }), 'name="query"');
});

test("inputAttrs default target is hull-search-results", () => {
    assertMatch(search.inputAttrs({ url: "/x" }), 'hx-target="#hull-search-results"');
});

test("inputAttrs custom target", () => {
    assertMatch(
        search.inputAttrs({ url: "/x", target: "#asset-rows" }),
        'hx-target="#asset-rows"',
    );
});

test("inputAttrs pushUrl=true emits hx-push-url", () => {
    assertMatch(search.inputAttrs({ url: "/x", pushUrl: true }), 'hx-push-url="true"');
});

test("inputAttrs pushUrl=false (default) omits hx-push-url", () => {
    assertNoMatch(search.inputAttrs({ url: "/x" }), "hx-push-url");
});

test("inputAttrs indicator emits hx-indicator", () => {
    assertMatch(
        search.inputAttrs({ url: "/x", indicator: "#spinner" }),
        'hx-indicator="#spinner"',
    );
});

test("inputAttrs escapes url (no attribute break)", () => {
    assertMatch(
        search.inputAttrs({ url: '/x?a="b"' }),
        'hx-get="/x?a=&quot;b&quot;"',
    );
});

// ── resultsAttrs() ──────────────────────────────────────────────────

test("resultsAttrs default id matches inputAttrs default target", () => {
    assertMatch(search.resultsAttrs(), 'id="hull-search-results"');
});

test("resultsAttrs always sets aria-live=polite", () => {
    assertMatch(search.resultsAttrs(), 'aria-live="polite"');
});

test("resultsAttrs custom id", () => {
    assertMatch(search.resultsAttrs({ id: "asset-rows" }), 'id="asset-rows"');
});

// ── pair contract

test("default input target and default results id pair correctly", () => {
    assertMatch(search.inputAttrs({ url: "/x" }),  'hx-target="#hull-search-results"');
    assertMatch(search.resultsAttrs(),             'id="hull-search-results"');
});

// ── Edge cases ──────────────────────────────────────────────────────

test("inputAttrs with no opts is well-defined (url defaults to empty)", () => {
    const s = search.inputAttrs();
    assertMatch(s, 'type="search"');
    assertMatch(s, 'hx-get=""');
});

test("inputAttrs method='POST' (uppercase) normalizes to post", () => {
    const s = search.inputAttrs({ url: "/x", method: "POST" });
    assertMatch(s, 'hx-post="/x"');
    assertNoMatch(s, "hx-get");
});

test("inputAttrs invalid method throws a clear error", () => {
    let err;
    try { search.inputAttrs({ url: "/x", method: "put" }); }
    catch (e) { err = e; }
    if (!err) throw new Error("should have thrown");
    if (err.message.indexOf("'get' or 'post'") < 0) {
        throw new Error("expected error to mention valid options, got: " + err.message);
    }
});

console.log(`\n${pass}/${pass + fail} htmx-search tests passed`);
if (fail > 0) throw new Error("htmx-search tests failed");

// test_search.js — Tests for hull:search
//
// Tests FTS5 full-text search wrapper.
// Requires db (run via C test harness with caps).

import { search } from "hull:search";

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
    if (!v)
        throw new Error((msg || "") + " expected truthy, got " + v);
}

// ── createIndex and basic operations ────────────────────────────────

test("createIndex: creates FTS5 table", () => {
    search.createIndex("articles", ["title", "body"]);
});

test("index: insert document", () => {
    search.index("articles", "1", { title: "Hello World", body: "This is a test article about searching" });
    search.index("articles", "2", { title: "Lua Programming", body: "Learn Lua programming language" });
    search.index("articles", "3", { title: "World News", body: "Breaking news from around the world" });
});

test("query: basic text search", () => {
    const results = search.query("articles", "world");
    assertTrue(results.length >= 2, "expected at least 2 results");
    assertTrue(results[0].id !== undefined, "expected id field");
    assertTrue(results[0].rank !== undefined, "expected rank field");
});

test("query: specific term", () => {
    const results = search.query("articles", "lua");
    assertEq(results.length, 1);
    assertEq(results[0].id, "2");
});

test("query: no results", () => {
    const results = search.query("articles", "xyznonexistent");
    assertEq(results.length, 0);
});

test("query: with limit", () => {
    const results = search.query("articles", "world", { limit: 1 });
    assertEq(results.length, 1);
});

// ── remove ──────────────────────────────────────────────────────────

test("remove: deletes document", () => {
    search.remove("articles", "3");
    const results = search.query("articles", "news");
    assertEq(results.length, 0);
});

// ── dropIndex ───────────────────────────────────────────────────────

test("dropIndex: removes table", () => {
    search.dropIndex("articles");
    let threw = false;
    try { search.query("articles", "test"); } catch (e) { threw = true; }
    assertTrue(threw, "expected error after drop");
});

// ── validation ──────────────────────────────────────────────────────

test("createIndex: rejects invalid name", () => {
    let threw = false;
    try { search.createIndex("bad name!", ["col"]); } catch (e) { threw = true; }
    assertTrue(threw, "expected error for invalid name");
});

test("createIndex: rejects _hull_ prefix", () => {
    let threw = false;
    try { search.createIndex("_hull_test", ["col"]); } catch (e) { threw = true; }
    assertTrue(threw, "expected error for _hull_ prefix");
});

test("createIndex: rejects invalid column names", () => {
    let threw = false;
    try { search.createIndex("test", ["bad col!"]); } catch (e) { threw = true; }
    assertTrue(threw, "expected error for invalid column");
});

// ── snippet ─────────────────────────────────────────────────────────

test("query: with snippet", () => {
    search.createIndex("docs", ["title", "content"]);
    search.index("docs", "1", { title: "Guide", content: "This is a comprehensive guide to full text searching in databases" });
    const results = search.query("docs", "guide", {
        snippet: { column: 2, tokens: 10, before: "<b>", after: "</b>" }
    });
    assertTrue(results.length >= 1, "expected at least 1 result");
    search.dropIndex("docs");
});

export default { pass, fail };

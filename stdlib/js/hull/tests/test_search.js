// test_search.js - Tests for hull:search
//
// Tests FTS5 full-text search wrapper.
// Requires db (run via C test harness with caps).

import { search } from "hull:search";
import { db as dbModule } from "hull:db";

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

// A SQL keyword passes IDENT_RE -- it is a plain identifier -- so nothing
// above catches it, and `reindex` interpolates the source table and column
// names UNPREFIXED. Without this branch the failure surfaces as a bare
// SQLite syntax error naming neither the caller nor the word. Case-
// insensitive, because SQL's grammar is.
test("createIndex: rejects a SQL keyword as a column name", () => {
    let threw = false;
    try { search.createIndex("posts", ["from"]); } catch (e) { threw = true; }
    assertTrue(threw, "expected error for keyword column name");
});

test("reindex: rejects a SQL keyword as the source table", () => {
    let threw = false;
    try {
        search.reindex("posts", "order", { columns: { title: "title" } });
    } catch (e) { threw = true; }
    assertTrue(threw, "expected error for keyword source table");
});

test("createIndex: keyword check is case-insensitive", () => {
    let threw = false;
    try { search.createIndex("Select", ["col"]); } catch (e) { threw = true; }
    assertTrue(threw, "expected error for lower/mixed-case keyword");
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

// ── Audit 19: every caller value is read once ────────────────────────
// The module's SQL runs from a stdlib frame, past the C-level _hull_*
// guard. A getter or Proxy answered the validation with a safe name and the
// SQL with "_hull_sessions" (or any text), so reindex copied a reserved
// table into an index the app could query. Each value must be read exactly
// once, and the validated copy is what the SQL gets.

test("createIndex: reads each column once", () => {
    const reads = { 0: 0, 1: 0 };
    const cols = new Proxy(["title", "body"], {
        get(t, k) {
            if (k === "0" || k === "1") {
                reads[k]++;
                return reads[k] === 1 ? t[k] : "x) ; DROP TABLE nothing; --";
            }
            return t[k];
        },
    });
    search.createIndex("once_cols", cols);
    assertEq(reads[0], 1, "column 0 reads");
    assertEq(reads[1], 1, "column 1 reads");
    search.dropIndex("once_cols");
});

test("createIndex: reads content / contentRowid once", () => {
    let c = 0, r = 0;
    const opts = {
        get content() { c++; return c === 1 ? "once_src" : "_hull_sessions"; },
        get contentRowid() { r++; return r === 1 ? "id" : "_hull_x"; },
    };
    search.createIndex("once_content", ["title"], opts);
    assertEq(c, 1, "content reads");
    assertEq(r, 1, "contentRowid reads");
    search.dropIndex("once_content");
});

test("reindex: reads each source column once", () => {
    const conn = dbModule.default();
    conn.exec("CREATE TABLE IF NOT EXISTS once_src (id INTEGER PRIMARY KEY, title TEXT)");
    conn.exec("INSERT INTO once_src (id, title) VALUES (1, 'hello once')");
    search.createIndex("once_re", ["title"]);
    let n = 0;
    const columns = {
        get title() { n++; return n === 1 ? "title" : "_hull_sessions"; },
    };
    search.reindex("once_re", "once_src", { columns });
    assertEq(n, 1, "source column reads");
    assertEq(search.query("once_re", "hello").length, 1, "reindexed row");
    search.dropIndex("once_re");
    conn.exec("DROP TABLE once_src");
});

// Counts go back to the C harness (run_js_test in
// tests/hull/runtime/js/test_js.c), which reads them off the global object --
// a module's default export is not reachable from there.
globalThis.__test_pass = pass;
globalThis.__test_fail = fail;

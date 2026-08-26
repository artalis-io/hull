// test_pagination.js - Tests for hull:web:pagination

import { pagination } from "hull:web:pagination";

let pass = 0;
let fail = 0;

function test(name, fn) {
    try { fn(); pass++; }
    catch (e) { fail++; console.log(`FAIL: ${name}: ${e.message || e}`); }
}

function assertEq(a, b, msg) {
    if (a !== b) {
        throw new Error(`${msg || ""} expected ${b}, got ${a}`);
    }
}

// ── fromQuery ──────────────────────────────────────────────────────

test("fromQuery defaults to page=1 per_page=20 when no query", () => {
    const p = pagination.fromQuery({});
    assertEq(p.page, 1);
    assertEq(p.per_page, 20);
    assertEq(p.offset, 0);
    assertEq(p.limit, 20);
});

test("fromQuery reads ?page=3", () => {
    const p = pagination.fromQuery({ query: { page: "3" } });
    assertEq(p.page, 3);
    assertEq(p.offset, 40);
});

test("fromQuery reads ?per_page=5", () => {
    const p = pagination.fromQuery({ query: { per_page: "5" } });
    assertEq(p.per_page, 5);
    assertEq(p.limit, 5);
});

test("fromQuery caps per_page at maxPerPage", () => {
    const p = pagination.fromQuery(
        { query: { per_page: "5000" } },
        { maxPerPage: 50 }
    );
    assertEq(p.per_page, 50);
});

test("fromQuery honors defaultPerPage", () => {
    const p = pagination.fromQuery({}, { defaultPerPage: 10 });
    assertEq(p.per_page, 10);
});

test("fromQuery rejects invalid page values, falls back", () => {
    assertEq(pagination.fromQuery({ query: { page: "abc" } }).page, 1);
    assertEq(pagination.fromQuery({ query: { page: "-5" } }).page, 1);
    assertEq(pagination.fromQuery({ query: { page: "3.5" } }).page, 1);
});

test("fromQuery clamps page to at least 1", () => {
    assertEq(pagination.fromQuery({ query: { page: "0" } }).page, 1);
});

// ── render ─────────────────────────────────────────────────────────

test("render with total=0 has show=false, pages=1", () => {
    const r = pagination.render(0, { page: 1, per_page: 20 });
    assertEq(r.show, false);
    assertEq(r.pages, 1);
    assertEq(r.has_prev, false);
    assertEq(r.has_next, false);
});

test("render single page (total < per_page) has show=false", () => {
    const r = pagination.render(5, { page: 1, per_page: 20 });
    assertEq(r.pages, 1);
    assertEq(r.show, false);
});

test("render computes pages from ceiling", () => {
    const r = pagination.render(21, { page: 1, per_page: 10 });
    assertEq(r.pages, 3);
});

test("render page=1 of 5: links are [1*,2,3,4,5], no ellipsis", () => {
    const r = pagination.render(100, { page: 1, per_page: 20, window: 2 });
    assertEq(r.pages, 5);
    assertEq(r.links.length, 5);
    assertEq(r.links[0].active, true);
    assertEq(r.links[0].page, 1);
    assertEq(r.links[4].page, 5);
    for (const link of r.links) {
        if (link.ellipsis) throw new Error("no ellipsis expected");
    }
});

test("render large total inserts ellipses around current", () => {
    const r = pagination.render(400, { page: 10, per_page: 20, window: 2 });
    assertEq(r.pages, 20);
    // Expected: 1 ... 8 9 10* 11 12 ... 20 = 9 entries
    assertEq(r.links.length, 9);
    assertEq(r.links[0].page, 1);
    assertEq(r.links[1].ellipsis, true);
    const activeIdx = r.links.findIndex(l => l.active);
    if (activeIdx < 0) throw new Error("active link present");
    assertEq(r.links[activeIdx].page, 10);
});

test("render has_prev/has_next reflect boundaries", () => {
    const first = pagination.render(100, { page: 1, per_page: 20 });
    assertEq(first.has_prev, false);
    assertEq(first.has_next, true);
    const last = pagination.render(100, { page: 5, per_page: 20 });
    assertEq(last.has_prev, true);
    assertEq(last.has_next, false);
});

test("render prev/next URLs include baseUrl", () => {
    const r = pagination.render(100, {
        page: 3, per_page: 20, baseUrl: "/todos",
    });
    assertEq(r.prev, "/todos?page=2");
    assertEq(r.next, "/todos?page=4");
});

test("render URL preserves existing ? in baseUrl", () => {
    const r = pagination.render(100, {
        page: 3, per_page: 20, baseUrl: "/search?q=foo",
    });
    assertEq(r.next, "/search?q=foo&page=4");
});

test("render URL omits per_page when matching defaultPerPage", () => {
    const r = pagination.render(100, {
        page: 2, per_page: 10, defaultPerPage: 10, baseUrl: "/x",
    });
    assertEq(r.next, "/x?page=3");
});

test("render URL includes per_page when differing from default", () => {
    const r = pagination.render(100, {
        page: 2, per_page: 5, defaultPerPage: 20, baseUrl: "/x",
    });
    assertEq(r.next, "/x?page=3&per_page=5");
});

test("render clamps page to [1, pages]", () => {
    assertEq(pagination.render(50, { page: 99, per_page: 10 }).page, 5);
    assertEq(pagination.render(50, { page: 0, per_page: 10 }).page, 1);
});

export default { pass, fail };

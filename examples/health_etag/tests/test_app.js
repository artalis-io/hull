// Tests for health check + ETag middleware
// Run: hull test examples/health_etag

import { etag } from "hull:web:middleware:etag";
import { health } from "hull:web:middleware:health";

// ═══════════════════════════════════════════════════════════════════
// Health Check Tests
// ═══════════════════════════════════════════════════════════════════

// Note: health middleware routes don't run in hull test. Test module API directly.

test("health.runChecks includes DB", () => {
    const result = health.runChecks();
    test.ok(result.checks.db, "has db check");
    test.eq(result.checks.db.status, "ok");
    test.eq(result.allOk, true);
});

test("health.register failing check", () => {
    health.register("bad_check", () => { throw new Error("connection refused"); });
    const result = health.runChecks();
    test.eq(result.checks.bad_check.status, "fail");
    test.ok(result.checks.bad_check.error.includes("connection refused"), "has error message");
    test.eq(result.allOk, false);
    health.unregister("bad_check");
});

test("health.register check returning false", () => {
    health.register("false_check", () => false);
    const result = health.runChecks();
    test.eq(result.checks.false_check.status, "fail");
    test.eq(result.allOk, false);
    health.unregister("false_check");
});

// ═══════════════════════════════════════════════════════════════════
// ETag Tests
// ═══════════════════════════════════════════════════════════════════

test("etag.compute returns weak ETag", () => {
    const tag = etag.compute("hello world");
    test.ok(tag, "returns a value");
    test.ok(tag.startsWith('W/"'), "starts with W/\"");
    test.ok(tag.endsWith('"'), "ends with quote");
    test.eq(tag.length, 20); // W/" + 16 hex + "
});

test("etag.compute is deterministic", () => {
    const tag1 = etag.compute("test data");
    const tag2 = etag.compute("test data");
    test.eq(tag1, tag2);
});

test("etag.compute returns null for empty", () => {
    test.eq(etag.compute(""), null);
    test.eq(etag.compute(null), null);
});

test("GET /api/items returns ETag header", () => {
    const res = test.get("/api/items");
    test.eq(res.status, 200);
    test.ok(res.json.items, "has items");
    const tag = res.headers.etag;
    test.ok(tag, "has ETag header");
});

test("GET /api/items with matching If-None-Match returns 304", () => {
    const res1 = test.get("/api/items");
    const tag = res1.headers.etag;
    test.ok(tag, "first request has ETag");

    const res2 = test.get("/api/items", {
        headers: { "If-None-Match": tag }
    });
    test.eq(res2.status, 304);
});

test("GET /api/items with wrong If-None-Match returns 200", () => {
    const res = test.get("/api/items", {
        headers: { "If-None-Match": 'W/"wrongwrongwrong1"' }
    });
    test.eq(res.status, 200);
    test.ok(res.json.items, "has body");
});

test("GET /api/greeting returns ETag", () => {
    const res = test.get("/api/greeting?name=Test");
    test.eq(res.status, 200);
    test.ok(res.headers.etag, "has ETag");
});

test("ETag changes when data changes", () => {
    const res1 = test.get("/api/items");
    const tag1 = res1.headers.etag;

    test.post("/api/items", {
        body: '{"name":"JSItem"}',
        headers: { "Content-Type": "application/json" },
    });

    const res2 = test.get("/api/items");
    const tag2 = res2.headers.etag;

    test.ok(tag1 !== tag2, "ETag changed after data modification");
});

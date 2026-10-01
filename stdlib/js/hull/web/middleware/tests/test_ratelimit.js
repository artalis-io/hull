// test_ratelimit.js - Tests for hull:web:middleware:ratelimit
//
// Tests pure-function helpers (no runtime globals needed).

import { ratelimit } from "hull:web:middleware:ratelimit";
import { cache } from "hull:cache";  // bucket store is a cache instance now

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

// ── check ────────────────────────────────────────────────────────────

test("check allows first request", () => {
    const buckets = cache.new();
    const r = ratelimit.check(buckets, "ip1", 5, 60, 1000);
    assertEq(r.allowed, true);
    assertEq(r.remaining, 4);
});

test("check blocks after limit", () => {
    const buckets = cache.new();
    for (let i = 0; i < 5; i++)
        ratelimit.check(buckets, "ip2", 5, 60, 1000);
    const r = ratelimit.check(buckets, "ip2", 5, 60, 1000);
    assertEq(r.allowed, false);
    assertEq(r.remaining, 0);
});

test("check resets after window", () => {
    const buckets = cache.new();
    for (let i = 0; i < 5; i++)
        ratelimit.check(buckets, "ip3", 5, 60, 1000);
    const r = ratelimit.check(buckets, "ip3", 5, 60, 1061);
    assertEq(r.allowed, true);
    assertEq(r.remaining, 4);
});

test("check returns remaining count", () => {
    const buckets = cache.new();
    ratelimit.check(buckets, "ip4", 10, 60, 1000);
    ratelimit.check(buckets, "ip4", 10, 60, 1000);
    const r = ratelimit.check(buckets, "ip4", 10, 60, 1000);
    assertEq(r.remaining, 7);
});

test("check returns reset timestamp", () => {
    const buckets = cache.new();
    const r = ratelimit.check(buckets, "ip5", 5, 60, 1000);
    assertEq(r.reset, 1060);
});

// ── middleware factory ───────────────────────────────────────────────

test("middleware returns a function", () => {
    const mw = ratelimit.middleware({ limit: 10, window: 60 });
    assertEq(typeof mw, "function");
});

// The default key is the client, not one bucket for everybody: one client
// spending the limit must not lock out another.
test("default key is per client", () => {
    const mw = ratelimit.middleware({ limit: 1, window: 60 });
    const res = () => {
        const r = { code: 200 };
        r.header = () => r;
        r.status = (c) => { r.code = c; return r; };
        r.json = () => r;
        return r;
    };
    const a = { headers: {}, remote_addr: "198.51.100.1" };
    const b = { headers: {}, remote_addr: "198.51.100.2" };
    assertEq(mw(a, res()), 0);
    assertEq(mw(a, res()), 1);
    assertEq(mw(b, res()), 0);
    // X-Forwarded-For is ignored unless trustProxy is set.
    const spoof = { headers: { "x-forwarded-for": "203.0.113.9" },
                    remote_addr: "198.51.100.1" };
    assertEq(mw(spoof, res()), 1);
});

export default { pass, fail };

-- Valkey/Redis KV backend E2E fixture (Lua). Exercises the full store surface
-- against a real server whose DSN arrives as ctx.args[1], and asserts the
-- borrow-copy guard: a value returned by get() must survive a later op on the
-- same connection (the binding copies the borrowed backend buffer into a
-- runtime-owned string before returning).
--
-- Prints "ok: <name>" per check and "ALL OK" at the end; a failed assertion
-- exits non-zero via error(). SPDX-License-Identifier: AGPL-3.0-or-later

app.manifest({
    modules = { "hull/kv@1", "hull/cache@1" },
    kv = { dynamic = { schemes = { "redis", "valkey" }, hosts = { "127.0.0.1" } } },
})

local kv_mod    = require("hull.kv")
local cache_mod = require("hull.cache")

local function check(name, cond)
    if not cond then error("FAIL: " .. name) end
    print("ok: " .. name)
end

app.main(function(ctx)
    local dsn = ctx.args[1]
    if not dsn or dsn == "" then error("usage: app.lua <dsn>") end

    local kv = kv_mod.open({ backend = "valkey", dsn = dsn, namespace = "t" })

    -- Clean slate for a repeatable run.
    kv:clear()

    -- Basic set/get, including a binary value with embedded NUL.
    kv:set("a", "AAAA")
    check("get hit", kv:get("a") == "AAAA")
    check("get miss", kv:get("nope") == nil)
    kv:set("bin", "x\0y\1z")
    check("binary roundtrip", kv:get("bin") == "x\0y\1z")

    -- Borrow-copy guard: v1 must not change when a later op reuses the
    -- connection's receive buffer (a large value forces a realloc/reset).
    local v1 = kv:get("a")
    kv:set("big", string.rep("Z", 6000))
    local v2 = kv:get("big")
    check("borrow-copy: first value unchanged after another op", v1 == "AAAA")
    check("borrow-copy: second value correct", v2 == string.rep("Z", 6000))

    -- exists / delete.
    check("exists true", kv:exists("a") == true)
    check("delete existing", kv:delete("a") == true)
    check("delete missing", kv:delete("a") == false)
    check("exists false after delete", kv:exists("a") == false)

    -- TTL (seconds) honored.
    kv:set("ttlk", "v", { ttl = 100 })
    check("ttl key present", kv:get("ttlk") == "v")

    -- Atomic increment (fresh starts at 0).
    check("incr fresh", kv:incr("ctr", 5) == 5)
    check("incr again", kv:incr("ctr", 3) == 8)

    -- CAS: set-if-absent, mismatch, match.
    check("cas set-if-absent", kv:cas("cx", nil, "v1") == true)
    check("cas absent again -> mismatch", kv:cas("cx", nil, "v2") == false)
    check("cas wrong expected -> false", kv:cas("cx", "WRONG", "v3") == false)
    check("cas right expected -> true", kv:cas("cx", "v1", "v2") == true)
    check("cas took effect", kv:get("cx") == "v2")

    -- Scan returns full app keys under a prefix; namespace stripped.
    kv:clear()
    kv:set("run:1", "a"); kv:set("run:2", "b"); kv:set("other", "c")
    local found = {}
    for _, k in ipairs(kv:scan("run:")) do found[k] = true end
    check("scan matched run:1", found["run:1"] == true)
    check("scan matched run:2", found["run:2"] == true)
    check("scan excluded other", found["other"] == nil)
    local all = kv:scan("")
    check("scan-all count", #all == 3)

    -- clear wipes only this namespace.
    kv:clear()
    check("cleared", #kv:scan("") == 0)

    -- caps advertise the Redis-shaped feature set.
    check("cap ttl", kv.caps.ttl == true)
    check("cap incr", kv.caps.atomic_increment == true)
    check("cap cas", kv.caps.compare_exchange == true)
    check("cap scan", kv.caps.scan == true)
    check("cap shared", kv.caps.shared == true)
    check("backend name", kv.backend == "valkey")

    kv:close()

    -- A cache handle over the same server, isolated by the "cache:" prefix from
    -- the "kv:" namespaces: a shared name must not collide.
    local c = cache_mod.open({ backend = "valkey", dsn = dsn, namespace = "t" })
    c:set("shared", "cacheval")
    check("cache set/get", c:get("shared") == "cacheval")
    local fetched = c:fetch("memo", 60, function() return "computed" end)
    check("cache fetch computes", fetched == "computed")
    check("cache fetch caches", c:get("memo") == "computed")
    c:close()

    -- Prove no collision: reopen the kv namespace "t"; its "shared" key (deleted
    -- by the clear above) is absent even though the cache wrote "shared".
    local kv2 = kv_mod.open({ backend = "valkey", dsn = dsn, namespace = "t" })
    check("kv/cache namespace isolation", kv2:get("shared") == nil)
    kv2:clear()
    kv2:close()

    print("ALL OK")
    return 0
end)

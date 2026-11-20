-- test_stdlib_audit50.lua - stdlib regressions from audit 50 (Lua stdlib).
--
-- pwned: a "[hull:denied] " refusal (a host manifest.hosts does not admit)
-- raises instead of failing open, whatever the hosts list holds.
-- retry: a denied error is re-raised at once, not retried.
-- sftp: list() bounds the bytes of a listing, not only its entry count.
-- ratelimit: a sweep at the cap that leaves more than 90% sheds down to it.
-- The JS twin is test_stdlib_audit50.js.
--
-- Runs in the caps-bearing context (run_lua_test_in_runtime in
-- tests/hull/runtime/lua/test_lua.c).

local pass = 0
local fail = 0

-- __audit50_case names the one case to run (one UTEST per case in
-- test_lua.c); unset runs them all.
local function test(name, fn)
    if __audit50_case and __audit50_case ~= name then return end
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a), 2)
    end
end

-- ── pwned: denied fails closed ─────────────────────────────────────

local pwned = require("hull.web.pwned")
local P = __test_hooks("hull.web.pwned")

-- What the runtime raises for a host it denies (mod_http_client.c).
local function denied(host)
    return function()
        error("[hull:denied] http.async.get: host '" .. host
            .. "' is not in the manifest hosts allowlist", 0)
    end
end

-- A password the local blocklist does not hold, so the lookup goes to HIBP.
local PW = "audit50-not-in-the-blocklist-zq7"

test("pwned_audit50.denied_unset_var_throws", function()
    -- With hosts = { "$UNSET_VAR" } the runtime denies every HIBP fetch, and
    -- hosts_may_admit took the "$" entry for "may admit": check() returned
    -- false (not pwned) behind one WARN.
    P.set_get(denied("api.pwnedpasswords.com"))
    local ok, err = pcall(pwned.check, PW)
    P.set_get(nil)
    assert_eq(ok, false, "check raises")
    assert_eq(tostring(err):find("manifest.hosts does not admit", 1, true) ~= nil, true,
              "the misconfiguration is named: " .. tostring(err))
end)

test("pwned_audit50.denied_cidr_throws", function()
    -- A CIDR entry matches no DNS name; the endpoint is denied the same way.
    P.set_get(denied("hibp.internal.example"))
    local ok, err = pcall(pwned.check, PW, { endpoint = "https://hibp.internal.example/range/" })
    P.set_get(nil)
    assert_eq(ok, false, "check raises")
    assert_eq(tostring(err):find("hibp.internal.example", 1, true) ~= nil, true,
              "the endpoint is named: " .. tostring(err))
end)

test("pwned_audit50.outage_still_fails_open", function()
    -- A plain fetch failure is an outage, not a denial: unchanged.
    P.set_get(function() error("connection refused", 0) end)
    local ok, r = pcall(pwned.check, PW)
    P.set_get(nil)
    assert_eq(ok, true, "no raise: " .. tostring(r))
    assert_eq(r, false, "fails open")
end)

-- ── retry: denied is not retried ───────────────────────────────────

local retry = require("hull.retry")

test("retry_audit50.denied_not_retried", function()
    local calls = 0
    local msg = "[hull:denied] http.get: host 'x.test' is not in the manifest hosts allowlist"
    local ok, err = pcall(retry.run, function()
        calls = calls + 1
        error(msg, 0)
    end, { max_attempts = 3, base_ms = 0, cap_ms = 0 })
    assert_eq(ok, false, "raises")
    assert_eq(calls, 1, "called once")
    assert_eq(err, msg, "the error unchanged")
end)

-- ── sftp: list() byte bound ────────────────────────────────────────

local S = __test_hooks("hull.ssh.sftp_client")

-- A session whose server answers OPENDIR with a handle and then `replies`
-- READDIR replies of `per` names `name` each, then EOF.
local function fake_session(name, per, replies)
    local sent, closed = 0, 0
    local s = setmetatable({}, { __index = S.Sftp })
    s.request = function(_, build)
        if sent == 0 then sent = 1; return { type = "handle", handle = "H" } end
        if sent <= replies then
            sent = sent + 1
            local names = {}
            for i = 1, per do names[i] = { filename = name, longname = "", attrs = {} } end
            return { type = "name", names = names }
        end
        return { type = "status", eof = true }
    end
    s.close_handle = function() closed = closed + 1; return true end
    return s, function() return closed end
end

test("ssh_audit50.list_byte_cap", function()
    -- max_bytes given: 4 names of 600 bytes pass 2000.
    local s, closed = fake_session(("n"):rep(600), 2, 2)
    local names, err = s:list("/d", { max_bytes = 2000 })
    assert_eq(names, nil, "no listing")
    assert_eq(err and err.code, "too_large", "too_large")
    assert_eq(err.limit, 2000, "the limit")
    assert_eq(closed(), 1, "handle closed")
    -- The default bound (16 MiB): 100 names of 256 KiB, well under the
    -- entry cap, were returned whole.
    local s2, closed2 = fake_session(("n"):rep(256 * 1024), 10, 10)
    local n2, e2 = s2:list("/d")
    assert_eq(n2, nil, "no listing")
    assert_eq(e2 and e2.code, "too_large", "too_large by default")
    assert_eq(e2.limit, 16 * 1024 * 1024, "the default limit")
    assert_eq(closed2(), 1, "handle closed")
    -- Under the bound the listing is whole.
    local s3 = fake_session("ok", 3, 1)
    local n3 = s3:list("/d", { max_bytes = 2000 })
    assert_eq(type(n3), "table", "a listing")
    assert_eq(#n3, 3, "three names")
    -- A bad bound is a bad_argument.
    local _, e4 = fake_session("ok", 1, 1):list("/d", { max_bytes = -1 })
    assert_eq(e4 and e4.code, "bad_argument", "bad_argument")
end)

-- ── ratelimit: amortised sweep at the cap ──────────────────────────

local ratelimit = require("hull.web.middleware.ratelimit")
local cache = require("hull.cache")

test("ratelimit_audit50.sweep_amortised_at_cap", function()
    local MAX = 10000
    local now = 1000000
    local buckets = cache.new({ max_entries = MAX })
    local saturated = { map = {}, n = 0 }
    -- MAX saturated keys, ten of them expired, the rest live.
    for i = 1, MAX do
        local ws = (i <= 10) and (now - 120) or now
        saturated.map["k" .. i] = { count = 1, window_start = ws }
    end
    saturated.n = MAX
    -- One more saturated key pushes the map past the cap and sweeps. Before
    -- the fix the sweep freed the ten expired entries only, leaving 9991, so
    -- the next keys each paid another O(MAX) sweep.
    ratelimit.check(buckets, "new", 1, 60, now + 1, saturated)
    local n = 0
    for _ in pairs(saturated.map) do n = n + 1 end
    assert_eq(n <= math.floor(MAX * 0.9), true, "shed to 90%, got " .. n)
    assert_eq(saturated.n, n, "the count follows")
    assert_eq(saturated.map["new"] ~= nil, true, "the new key is kept")
end)

return { pass = pass, fail = fail }

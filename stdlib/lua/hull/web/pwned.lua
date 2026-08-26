-- hull.web.pwned - k-anonymity pwned-password check via the HIBP
-- range API. Uses SHA-1's first 5 hex chars as a query prefix so
-- the password never crosses the wire; only the prefix and a 35-
-- char suffix scan happen client-side.
--
-- Two-tier check:
--   1. Embedded local blocklist (SecLists top 10K, ~80KB, binary-
--      searched in-process). A hit short-circuits - no network round-
--      trip needed. Safe for air-gapped deployments: the most
--      egregious passwords are blocked without HIBP.
--   2. HIBP range API for the remaining ~half-billion known leaks.
--      Fail-open on outage (a hardening layer must not become an
--      availability dependency), with a once-per-process log.warn
--      so the operator sees the gap.
--
-- API:
--   pwned.check(password)             -> boolean
--   pwned.check(password, { endpoint = "https://.../range/" })
--   pwned.health()                    -> { ok, last_check_at, last_error }
--
-- Returns from check():
--   true   - the password's SHA-1 hash appears in HIBP's set (or
--            in the embedded blocklist).
--   false  - the password does not appear, OR HIBP was unreachable
--            AND the embedded blocklist did not hit (fail-open).
--
-- Apps consuming this module MUST add the HIBP host (or their
-- override) to manifest.hosts:
--
--   app.manifest({
--       modules = { "hull/web/pwned@1", ... },
--       hosts   = { "api.pwnedpasswords.com" },
--   })

local crypto      = require("hull.crypto")
local http_client = require("hull.http-client")
local log         = require("hull.log")
local time        = require("hull.time")
local blocklist   = require("hull.web._pwned_blocklist")

local DEFAULT_ENDPOINT = "https://api.pwnedpasswords.com/range/"

local M = {}

-- Health state. Updated after every HIBP attempt. `ok=true` only
-- when the last attempt produced a real answer (any 200 response).
-- `last_error` is the textual reason for the most recent failure;
-- nil after a success. Use pwned.health() to read this from
-- middleware health checks.
local _health = {
    ok            = nil,    -- nil until the first attempt
    last_check_at = nil,
    last_error    = nil,
}
local _warned_failopen = false

-- SHA-1 of the password, uppercase hex (HIBP wire format). Delegates
-- to crypto.sha1 - a legacy-interop primitive surfaced specifically
-- for protocols like HIBP that hardcode SHA-1. Do NOT use SHA-1
-- for any new cryptographic purpose.
local function sha1_hex(msg)
    return crypto.hex_encode(crypto.sha1(msg)):upper()
end

-- Binary search the embedded blocklist. The hashes string is a
-- packed sort of 8-char uppercase-hex SHA-1 prefixes; each stride
-- is exactly `blocklist.stride` (= 8) bytes. Returns true if the
-- first 8 hex chars of the SHA-1 match a known entry.
local function in_local_blocklist(hash_hex_upper)
    local stride = blocklist.stride
    local needle = hash_hex_upper:sub(1, stride)
    local hashes = blocklist.hashes
    local lo, hi = 0, blocklist.count - 1
    while lo <= hi do
        local mid = (lo + hi) // 2
        local pos = mid * stride + 1
        local s = hashes:sub(pos, pos + stride - 1)
        if s == needle then return true
        elseif s < needle then lo = mid + 1
        else hi = mid - 1 end
    end
    return false
end

--- Check whether a password's SHA-1 appears in HIBP or the local
-- top-10K blocklist.
-- @tparam string password    Plaintext password.
-- @tparam ?table opts        { endpoint = string }. For tests.
-- @treturn boolean           true if pwned, false otherwise.
function M.check(password, opts)
    if type(password) ~= "string" or password == "" then return false end
    opts = opts or {}
    local endpoint = opts.endpoint or DEFAULT_ENDPOINT

    local hash = sha1_hex(password)

    -- Local blocklist first. Cheap (~14 comparisons) and works
    -- when HIBP is unreachable. A hit here is conclusive; we
    -- don't bother with the network round-trip.
    if in_local_blocklist(hash) then return true end

    local prefix = hash:sub(1, 5)
    local suffix = hash:sub(6)

    local ok, resp = pcall(http_client.async.get, endpoint .. prefix, {
        headers = { ["User-Agent"] = "hull-pwned-check/1" },
    })
    if not ok or not resp or resp.status ~= 200 or not resp.body then
        _health.ok            = false
        _health.last_check_at = time.now()
        _health.last_error    = "HIBP fetch failed"
        if not _warned_failopen then
            _warned_failopen = true
            log.warn("pwned: HIBP fetch failed; failing open after local "
                  .. "blocklist miss. Subsequent failures will be silent. "
                  .. "Check connectivity to " .. endpoint)
        end
        return false
    end

    _health.ok            = true
    _health.last_check_at = time.now()
    _health.last_error    = nil
    _warned_failopen      = false  -- re-arm warning for the next outage

    -- Response body is CRLF-separated "SUFFIX:count" lines.
    for line in resp.body:gmatch("[^\r\n]+") do
        local sep = line:find(":", 1, true)
        if sep then
            local s = line:sub(1, sep - 1)
            if s == suffix then return true end
        end
    end
    return false
end

--- Last-attempt status of the HIBP backend. Apps can poll this from
-- a startup probe, a health endpoint, or a periodic timer to detect
-- silent fail-open conditions (air-gapped deployments, blocked
-- egress, HIBP outages).
-- @treturn table  { ok = bool|nil, last_check_at = int|nil,
--                   last_error = string|nil }
function M.health()
    return {
        ok            = _health.ok,
        last_check_at = _health.last_check_at,
        last_error    = _health.last_error,
    }
end

-- Exposed for tests + ad-hoc uses (e.g. probing whether a
-- password the app already has access to is in HIBP, separate
-- from the registration path).
M._sha1_hex = sha1_hex
M._in_local_blocklist = in_local_blocklist

return M

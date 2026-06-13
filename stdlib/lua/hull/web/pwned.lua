-- hull.web.pwned — k-anonymity pwned-password check via the HIBP
-- range API. Uses SHA-1's first 5 hex chars as a query prefix so
-- the password never crosses the wire; only the prefix and a 35-
-- char suffix scan happen client-side.
--
-- API:
--   pwned.check(password)             -> boolean
--   pwned.check(password, { endpoint = "https://.../range/" })
--
-- Returns:
--   true   - the password's SHA-1 hash appears in HIBP's set.
--   false  - the password does not appear, OR HIBP was
--            unreachable / returned an unexpected response
--            (fail-open: a hardening layer should not become an
--            availability dependency; logged via log.warn).
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

local DEFAULT_ENDPOINT = "https://api.pwnedpasswords.com/range/"

local M = {}

-- SHA-1 hex of the password's UTF-8 bytes. Hull's cap layer
-- exposes SHA-256 and SHA-512 but not SHA-1 directly; the HIBP
-- protocol is fixed at SHA-1, so we drive it through the HMAC
-- backend using a zero-length key as an HMAC-of-SHA1 trick.
--
-- Actually no — we just need the raw SHA-1 digest. HMAC-SHA1
-- with key="" is NOT equal to SHA-1(msg). For now route through
-- a tiny inline SHA-1 over the password. Since this is a single
-- 50-line consumer, an in-stdlib SHA-1 of the password is
-- acceptable; it does NOT need to be constant-time (password
-- already hit hash_password via PBKDF2 elsewhere) and never
-- leaves this module.

-- Inline SHA-1 (RFC 3174). Pure Lua, ~60 LOC. Used only for the
-- HIBP query — never for password storage or authentication.
local function sha1_hex(msg)
    -- Bit ops require Lua 5.3+ (Hull is Lua 5.4).
    local function rol(n, b)
        return ((n << b) | (n >> (32 - b))) & 0xFFFFFFFF
    end
    local h0, h1, h2, h3, h4 =
        0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0
    local len = #msg
    local bit_len = len * 8
    -- Pad: append 0x80, then zeros, then 64-bit length BE.
    msg = msg .. "\x80"
    while (#msg % 64) ~= 56 do msg = msg .. "\0" end
    for i = 7, 0, -1 do
        msg = msg .. string.char((bit_len >> (i * 8)) & 0xFF)
    end
    for chunk = 1, #msg, 64 do
        local w = {}
        for j = 0, 15 do
            local i = chunk + j * 4
            w[j] = (string.byte(msg, i)     << 24)
                 | (string.byte(msg, i + 1) << 16)
                 | (string.byte(msg, i + 2) << 8)
                 |  string.byte(msg, i + 3)
        end
        for j = 16, 79 do
            w[j] = rol(w[j-3] ~ w[j-8] ~ w[j-14] ~ w[j-16], 1)
        end
        local a, b, c, d, e = h0, h1, h2, h3, h4
        for j = 0, 79 do
            local f, k
            if j < 20 then
                f = (b & c) | ((~b) & d); k = 0x5A827999
            elseif j < 40 then
                f = b ~ c ~ d; k = 0x6ED9EBA1
            elseif j < 60 then
                f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC
            else
                f = b ~ c ~ d; k = 0xCA62C1D6
            end
            local temp = (rol(a, 5) + f + e + k + w[j]) & 0xFFFFFFFF
            e, d, c, b, a = d, c, rol(b, 30), a, temp
        end
        h0 = (h0 + a) & 0xFFFFFFFF
        h1 = (h1 + b) & 0xFFFFFFFF
        h2 = (h2 + c) & 0xFFFFFFFF
        h3 = (h3 + d) & 0xFFFFFFFF
        h4 = (h4 + e) & 0xFFFFFFFF
    end
    return string.format("%08X%08X%08X%08X%08X", h0, h1, h2, h3, h4)
end

--- Check whether a password's SHA-1 appears in HIBP.
-- @tparam string password    Plaintext password.
-- @tparam ?table opts        { endpoint = string }. For tests.
-- @treturn boolean           true if pwned, false otherwise
--                            (including all error paths).
function M.check(password, opts)
    if type(password) ~= "string" or password == "" then return false end
    opts = opts or {}
    local endpoint = opts.endpoint or DEFAULT_ENDPOINT

    local hash = sha1_hex(password)
    local prefix = hash:sub(1, 5)
    local suffix = hash:sub(6)

    local ok, resp = pcall(http_client.async.get, endpoint .. prefix, {
        headers = { ["User-Agent"] = "hull-pwned-check/1" },
    })
    if not ok or not resp or resp.status ~= 200 or not resp.body then
        log.warn("pwned: HIBP fetch failed; failing open")
        return false
    end

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

-- Exposed for tests + ad-hoc uses (e.g. probing whether a
-- password the app already has access to is in HIBP, separate
-- from the registration path).
M._sha1_hex = sha1_hex

return M

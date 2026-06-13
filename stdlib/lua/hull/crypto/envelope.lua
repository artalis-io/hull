-- hull.crypto.envelope — HMAC-signed, JSON-payload, stateless tokens.
--
-- One bit of bookkeeping shared by every Hull module that issues
-- a self-contained token: hull/web/auth-flows (verify-email,
-- magic-link, password-reset, email-change, totp-pending) and
-- hull/web/middleware/oauth (state cookie). Format:
--
--     base64url(JSON-payload) "." hex(HMAC-SHA256(secret_hex, body))
--
-- The HMAC is over the base64url body bytes, NOT over the raw
-- JSON, so verification only needs the body string + tag from
-- the wire. The body uses base64url (no padding) so it survives
-- being passed through URL query parameters and HTTP-only
-- cookies without escaping.
--
-- This module intentionally does NOT enforce semantic checks
-- (action match, expiry, single-use). Those vary per caller —
-- auth-flows tracks single-use via a SQL table; oauth doesn't.
-- Each caller checks `payload.exp`, `payload.action`, etc.
-- itself; the helper here only frames the signature.
--
-- API:
--   envelope.sign(payload_table, secret_hex)   -> token_string
--   envelope.verify(token_string, secret_hex)  -> payload_table, nil
--                                              | nil, error_reason
--
-- Error reasons are intentionally vague (so an attacker can't
-- distinguish "tampered tag" from "bad encoding" at the response
-- layer): "missing", "malformed", "bad tag", "bad encoding",
-- "bad json".

local crypto = require("hull.crypto")
local json   = require("hull.json")

local M = {}

--- Build a signed token from a payload table.
-- @tparam table  payload     Arbitrary JSON-serializable fields. Callers
--                            typically include `sub`, `action`, `exp`,
--                            and a random `nonce` to defend against
--                            guessable collisions.
-- @tparam string secret_hex  HMAC-SHA256 key as a hex string.
-- @treturn string            `body.tag` token, URL-safe.
function M.sign(payload, secret_hex)
    local body = crypto.base64url_encode(json.encode(payload))
    local tag  = crypto.hmac_sha256(body, secret_hex)
    return body .. "." .. tag
end

--- Verify a token's signature and decode its payload.
-- Returns (payload_table, nil) on success or (nil, reason) on
-- failure. crypto.hmac_sha256_verify raises on malformed-hex
-- inputs (programmer error at the cap layer); we pcall it so a
-- user-supplied junk token returns a clean "bad tag" result
-- instead of crashing the request handler.
-- @tparam string token       Signed token returned by sign().
-- @tparam string secret_hex  Same key the token was signed with.
function M.verify(token, secret_hex)
    if type(token) ~= "string" or token == "" then
        return nil, "missing"
    end
    local dot = token:find(".", 1, true)
    if not dot then return nil, "malformed" end
    local body = token:sub(1, dot - 1)
    local tag  = token:sub(dot + 1)

    local ok, valid = pcall(crypto.hmac_sha256_verify, body,
                             secret_hex, tag)
    if not ok or not valid then return nil, "bad tag" end

    local raw = crypto.base64url_decode(body)
    if not raw then return nil, "bad encoding" end
    local payload = json.decode(raw)
    if type(payload) ~= "table" then return nil, "bad json" end
    return payload, nil
end

return M

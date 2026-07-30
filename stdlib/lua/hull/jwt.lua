--- JWT sign / verify / decode.
--
-- @module hull.jwt
-- @author Hull contributors
-- @license AGPL-3.0-or-later
--
-- Sign: HS256 only (Hull doesn't ship asymmetric signing keys).
-- Verify: HS256 + RS256/384/512 + PS256 + ES256/384, dispatched by
-- the token's `alg` header against a caller-supplied allowlist.
--
-- Uses `crypto.hmac_sha256`, `crypto.verify` (asymmetric),
-- `crypto.base64url_encode/decode`, `json`, and `time`.
--
-- Asym-confusion protection: callers MUST pass `opts.algs` (or
-- accept the default `{"HS256"}`) to gate which `alg` values are
-- acceptable. A token claiming RS256 against an HS256-only allowlist
-- is rejected before the key resolver runs. `"none"` is rejected
-- unconditionally.
--
-- @usage HS256 (today's API, unchanged):
--   local token = jwt.sign({ sub = user_id, exp = 3600 }, secret)
--   local payload, err = jwt.verify(token, secret)
--
-- @usage RS256/ES256 with a static PEM:
--   local payload, err = jwt.verify(token, pem, { algs = {"RS256"} })
--
-- @usage Asym with JWKS-style key resolver:
--   local function resolver(kid, alg)
--       return jwks_cache:lookup_pem(kid)  -- nil if unknown kid
--   end
--   local payload, err = jwt.verify(token, resolver,
--                                   { algs = {"RS256", "ES256"} })

local json = require("hull.json")
local crypto = require("hull.crypto")
local time = require("hull.time")

local jwt = {}

-- Pre-computed base64url encoding of {"alg":"HS256","typ":"JWT"} used
-- by jwt.sign (HS256 is the only signing path).
local HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

local EXP_RELATIVE_THRESHOLD = 2e9

-- Allowlist of algorithms this module understands. Anything not in
-- this set is rejected before the key resolver runs. `"none"` is
-- deliberately absent.
local SUPPORTED_ALGS = {
    HS256 = true,
    RS256 = true, RS384 = true, RS512 = true,
    PS256 = true,
    ES256 = true, ES384 = true,
}

local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

-- Constant-time comparison, done in C (crypto.constant_time_eq) so timing is
-- not subject to interpreter/bytecode-dispatch variance, matching
-- crypto/envelope. Length leak is acceptable: both inputs are fixed-length
-- HMAC outputs of the same size.
local function constant_time_compare(a, b)
    return crypto.constant_time_eq(a, b)
end

-- HS256 signature: HMAC-SHA256 over the signing input, returning the
-- raw 32-byte digest. The crypto cap returns hex; convert back.
local function hs256_signature(data, secret)
    local key_hex = str_to_hex(secret)
    local sig_hex = crypto.hmac_sha256(data, key_hex)
    local raw = {}
    for i = 1, #sig_hex, 2 do
        raw[#raw + 1] = string.char(tonumber(sig_hex:sub(i, i + 1), 16))
    end
    return table.concat(raw)
end

--- Sign a payload and return a JWT string (HS256 only).
--
-- @tparam table payload  Claims to encode.
-- @tparam string secret  HMAC-SHA256 key.
-- @treturn string  JWT in compact form.
function jwt.sign(payload, secret)
    if not payload or not secret then
        return nil, "payload and secret are required"
    end
    -- SECURITY: use a 32+ byte random HMAC key. A short (<16 byte) HS256 secret
    -- is brute-forceable. Not hard-rejected here (would break existing apps),
    -- but strongly recommended; oauth.init enforces >=16 for its state secret.

    local claims = {}
    for k, v in pairs(payload) do claims[k] = v end

    if not claims.iat then claims.iat = time.now() end
    if claims.exp and claims.exp < EXP_RELATIVE_THRESHOLD then
        claims.exp = time.now() + claims.exp
    end

    local payload_json = json.encode(claims)
    local payload_b64 = crypto.base64url_encode(payload_json)

    local signing_input = HEADER_B64 .. "." .. payload_b64
    local sig_raw = hs256_signature(signing_input, secret)
    local sig_b64 = crypto.base64url_encode(sig_raw)

    return signing_input .. "." .. sig_b64
end

-- Resolve key_or_resolver into the actual key bytes for this token.
-- If callable, invoke with (kid, alg) so JWKS callers can route by
-- the token's key-id header. If string, return as-is.
local function resolve_key(key_or_resolver, kid, alg)
    if type(key_or_resolver) == "function" then
        return key_or_resolver(kid, alg)
    end
    return key_or_resolver
end

-- Verify the per-alg signature. Returns true on valid, false on any
-- failure. HS256 uses constant-time HMAC compare; asym uses
-- crypto.verify (which itself is constant-time inside the cap layer).
local function verify_signature(alg, key, signing_input, sig_raw, sig_b64)
    if alg == "HS256" then
        if type(key) ~= "string" or #key == 0 then return false end
        local expected_raw = hs256_signature(signing_input, key)
        local expected_b64 = crypto.base64url_encode(expected_raw)
        return constant_time_compare(sig_b64, expected_b64)
    end
    -- Asym: crypto.verify takes the raw r||s sig (for ECDSA), which
    -- is exactly what's encoded in the JWS token (per RFC 7515 §3.1).
    if type(key) ~= "string" or #key == 0 then return false end
    return crypto.verify(alg, key, signing_input, sig_raw)
end

--- Verify a JWT and return the decoded payload.
--
-- @tparam string token            JWT in compact form.
-- @tparam string|function key_or_resolver  Either:
--   - HMAC secret (HS256) or PEM-encoded SubjectPublicKeyInfo (asym);
--   - a function `(kid, alg) -> key` for JWKS-style resolution.
--     Returning `nil` from the resolver fails the verify (no oracle).
-- @tparam[opt] table opts
--   - `algs`        Allowlist of acceptable `alg` values. Defaults to
--                   `{"HS256"}` for back-compat with the pre-asym API.
--                   MUST be set to include asym algs for asym tokens.
--   - `require_exp` Reject tokens lacking an `exp` claim.
-- @treturn ?table payload  Decoded claims on success.
-- @treturn ?string err     Reason on failure (when payload is nil).
function jwt.verify(token, key_or_resolver, opts)
    if not token or not key_or_resolver then
        return nil, "token and key are required"
    end
    opts = opts or {}
    local allowed = opts.algs or { "HS256" }

    -- Split into the three b64-encoded parts.
    local parts = {}
    for part in token:gmatch("[^%.]+") do
        parts[#parts + 1] = part
    end
    if #parts ~= 3 then return nil, "invalid token format" end

    local header_b64, payload_b64, sig_b64 = parts[1], parts[2], parts[3]

    -- Parse the header to discover the alg + kid. We can't just match
    -- a fixed header string anymore now that we support multiple algs.
    local header_json = crypto.base64url_decode(header_b64)
    if not header_json then return nil, "invalid header encoding" end
    local header = json.decode(header_json)
    if type(header) ~= "table" then return nil, "invalid header JSON" end

    local alg = header.alg
    if not alg or alg == "none" then
        return nil, "alg 'none' rejected"
    end
    if not SUPPORTED_ALGS[alg] then
        return nil, "unsupported algorithm: " .. tostring(alg)
    end

    -- Allowlist enforcement BEFORE the key resolver runs, so a hostile
    -- token claiming an unexpected alg can't trigger key-resolution
    -- side effects.
    local in_allowed = false
    for _, a in ipairs(allowed) do
        if a == alg then in_allowed = true; break end
    end
    if not in_allowed then
        return nil, "alg " .. alg .. " not in allowed list"
    end

    local key = resolve_key(key_or_resolver, header.kid, alg)
    if not key then return nil, "no key for kid/alg" end

    local signing_input = header_b64 .. "." .. payload_b64
    local sig_raw = crypto.base64url_decode(sig_b64)
    if not sig_raw then return nil, "invalid signature encoding" end

    if not verify_signature(alg, key, signing_input, sig_raw, sig_b64) then
        return nil, "invalid signature"
    end

    local payload_json = crypto.base64url_decode(payload_b64)
    if not payload_json then return nil, "invalid payload encoding" end
    local payload = json.decode(payload_json)
    if not payload then return nil, "invalid payload JSON" end

    -- exp / nbf must be numbers per RFC 7519 §4.1.4 / §4.1.5. A token
    -- crafted with `"exp": "not-a-number"` would otherwise raise on
    -- `time.now() >= payload.exp` (string vs number compare) and the
    -- handler would surface as a 500 instead of a clean reject.
    if payload.exp ~= nil then
        if type(payload.exp) ~= "number" then
            return nil, "invalid exp claim"
        end
        if time.now() >= payload.exp then return nil, "token expired" end
    elseif opts.require_exp then
        return nil, "token missing required exp claim"
    end

    if payload.nbf ~= nil then
        if type(payload.nbf) ~= "number" then
            return nil, "invalid nbf claim"
        end
        if time.now() < payload.nbf then
            return nil, "token not yet valid"
        end
    end

    return payload
end

--- Decode a JWT payload WITHOUT verifying the signature. Debug use.
function jwt.decode(token)
    if not token then return nil end
    local parts = {}
    for part in token:gmatch("[^%.]+") do
        parts[#parts + 1] = part
    end
    if #parts ~= 3 then return nil end
    local payload_json = crypto.base64url_decode(parts[2])
    if not payload_json then return nil end
    return json.decode(payload_json)
end

return jwt

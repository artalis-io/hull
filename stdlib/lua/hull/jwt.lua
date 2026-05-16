--- JWT HS256 sign / verify / decode.
--
-- @module hull.jwt
-- @author Hull contributors
-- @license AGPL-3.0-or-later
--
-- Uses `crypto.hmac_sha256`, `crypto.base64url_encode/decode`, `json`,
-- and `time` globals. Only HS256 is supported — the `"alg":"none"` family
-- is rejected, and there is no asymmetric-key path here.
--
-- All comparisons of HMAC digests are constant-time.
--
-- @usage
-- local jwt = require("hull.jwt")
-- local token = jwt.sign({ sub = user_id, exp = 3600 }, secret)  -- 1h from now
-- local payload, err = jwt.verify(token, secret, { require_exp = true })

local jwt = {}

-- Pre-computed base64url encoding of {"alg":"HS256","typ":"JWT"}
local HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

-- Threshold for distinguishing relative exp (seconds from now) from absolute
-- Unix timestamps. Values below this (~May 2033) are treated as relative.
local EXP_RELATIVE_THRESHOLD = 2e9

--- Convert a raw string to hex representation.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

--- Constant-time comparison of two strings.
-- Note: length leak is acceptable — both inputs are always fixed-length HMAC outputs
local function constant_time_compare(a, b)
    if #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff | (string.byte(a, i) ~ string.byte(b, i))
    end
    return diff == 0
end

--- Compute HMAC-SHA256 signature for the given data using a secret string.
-- Returns the raw signature bytes (decoded from hex).
local function compute_signature(data, secret)
    local key_hex = str_to_hex(secret)
    local sig_hex = crypto.hmac_sha256(data, key_hex)
    -- Decode hex to raw bytes for base64url encoding
    local raw = {}
    for i = 1, #sig_hex, 2 do
        raw[#raw + 1] = string.char(tonumber(sig_hex:sub(i, i + 1), 16))
    end
    return table.concat(raw)
end

--- Sign a payload and return a JWT string.
--
-- @tparam table payload  Claims to encode. Any JSON-serializable values.
--   Special handling:
--     - `iat` is auto-set to `time.now()` if absent.
--     - `exp` < 2e9 is treated as seconds-from-now and `time.now()` is added.
--       Values ≥ 2e9 are taken as absolute Unix timestamps.
-- @tparam string secret  HMAC-SHA256 key. ASCII bytes; **must not** be empty.
-- @treturn string  Compact-encoded JWT (`base64url(header).base64url(payload).base64url(sig)`).
-- @raise Errors if `payload` or `secret` is missing.
-- @usage
-- local token = jwt.sign({ sub = "alice", exp = 3600 }, "my-secret")
function jwt.sign(payload, secret)
    if not payload or not secret then
        return nil, "payload and secret are required"
    end

    -- Make a shallow copy to avoid mutating the original
    local claims = {}
    for k, v in pairs(payload) do
        claims[k] = v
    end

    -- Auto-set iat
    if not claims.iat then
        claims.iat = time.now()
    end

    -- If exp < threshold, treat as relative (seconds from now)
    if claims.exp and claims.exp < EXP_RELATIVE_THRESHOLD then
        claims.exp = time.now() + claims.exp
    end

    local payload_json = json.encode(claims)
    local payload_b64 = crypto.base64url_encode(payload_json)

    local signing_input = HEADER_B64 .. "." .. payload_b64
    local sig_raw = compute_signature(signing_input, secret)
    local sig_b64 = crypto.base64url_encode(sig_raw)

    return signing_input .. "." .. sig_b64
end

--- Verify a JWT and return the decoded payload.
--
-- Steps performed:
--   1. Split the token, check it has exactly three parts.
--   2. Compare the header (only `eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9` is accepted).
--   3. Recompute the HMAC-SHA256 signature and constant-time-compare.
--   4. Decode the payload JSON.
--   5. Reject expired (`exp` ≤ now) or not-yet-valid (`nbf` > now) tokens.
--   6. If `opts.require_exp` is true, reject tokens lacking an `exp` claim.
--
-- @tparam string token   JWT in compact form.
-- @tparam string secret  Same secret used at sign time.
-- @tparam[opt] table opts  `{ require_exp = boolean }`.
-- @treturn ?table payload  Decoded claims on success.
-- @treturn ?string err     Reason on failure (when payload is nil):
--   `"token and secret are required"`, `"invalid token format"`,
--   `"unsupported algorithm"`, `"invalid signature"`,
--   `"invalid payload encoding"`, `"invalid payload JSON"`,
--   `"token expired"`, `"token missing required exp claim"`,
--   `"token not yet valid"`.
-- @usage
-- local payload, err = jwt.verify(token, secret, { require_exp = true })
-- if not payload then return res:status(401):json({ error = err }) end
function jwt.verify(token, secret, opts)
    if not token or not secret then
        return nil, "token and secret are required"
    end
    opts = opts or {}

    -- Split token into parts
    local parts = {}
    for part in token:gmatch("[^%.]+") do
        parts[#parts + 1] = part
    end

    if #parts ~= 3 then
        return nil, "invalid token format"
    end

    local header_b64 = parts[1]
    local payload_b64 = parts[2]
    local sig_b64 = parts[3]

    -- Verify header matches expected HS256 header
    if header_b64 ~= HEADER_B64 then
        return nil, "unsupported algorithm"
    end

    -- Recompute signature
    local signing_input = header_b64 .. "." .. payload_b64
    local expected_sig_raw = compute_signature(signing_input, secret)
    local expected_sig_b64 = crypto.base64url_encode(expected_sig_raw)

    -- Constant-time comparison of signatures
    if not constant_time_compare(sig_b64, expected_sig_b64) then
        return nil, "invalid signature"
    end

    -- Decode payload
    local payload_json = crypto.base64url_decode(payload_b64)
    if not payload_json then
        return nil, "invalid payload encoding"
    end

    local payload = json.decode(payload_json)
    if not payload then
        return nil, "invalid payload JSON"
    end

    -- Check expiration
    if payload.exp then
        if time.now() >= payload.exp then
            return nil, "token expired"
        end
    elseif opts.require_exp then
        return nil, "token missing required exp claim"
    end

    -- Check not-before
    if payload.nbf then
        if time.now() < payload.nbf then
            return nil, "token not yet valid"
        end
    end

    return payload
end

--- Decode a JWT payload WITHOUT verifying the signature. Debug use only.
--
-- @tparam string token  JWT in compact form.
-- @treturn ?table  Decoded claims, or `nil` on malformed / unparsable input.
-- @warning Never use this for authentication. Always pair with @{jwt.verify}.
function jwt.decode(token)
    if not token then
        return nil
    end

    local parts = {}
    for part in token:gmatch("[^%.]+") do
        parts[#parts + 1] = part
    end

    if #parts ~= 3 then
        return nil
    end

    local payload_json = crypto.base64url_decode(parts[2])
    if not payload_json then
        return nil
    end

    return json.decode(payload_json)
end

return jwt

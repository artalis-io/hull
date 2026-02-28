--
-- hull.jwt -- JWT creation and verification (HS256)
--
-- Uses crypto.hmac_sha256, crypto.base64url_encode/decode, json, and time globals.
--
-- SPDX-License-Identifier: AGPL-3.0-or-later
--

local jwt = {}

-- Pre-computed base64url encoding of {"alg":"HS256","typ":"JWT"}
local HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

--- Convert a raw string to hex representation.
local function str_to_hex(s)
    local hex = {}
    for i = 1, #s do
        hex[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(hex)
end

--- Constant-time comparison of two strings.
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

--- Sign a payload table and return a JWT string.
-- If payload.exp is set and < 2e9, treat it as seconds-from-now.
-- Automatically sets payload.iat if missing.
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

    -- If exp < 2e9, treat as relative (seconds from now)
    if claims.exp and claims.exp < 2e9 then
        claims.exp = time.now() + claims.exp
    end

    local payload_json = json.encode(claims)
    local payload_b64 = crypto.base64url_encode(payload_json)

    local signing_input = HEADER_B64 .. "." .. payload_b64
    local sig_raw = compute_signature(signing_input, secret)
    local sig_b64 = crypto.base64url_encode(sig_raw)

    return signing_input .. "." .. sig_b64
end

--- Verify a JWT token and return the payload table on success.
-- Returns nil, "error reason" on failure.
function jwt.verify(token, secret)
    if not token or not secret then
        return nil, "token and secret are required"
    end

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
    end

    return payload
end

--- Decode a JWT token without verification (for debugging only).
-- Returns the payload table, or nil on decode failure.
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

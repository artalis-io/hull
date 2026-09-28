-- hull.encoding - byte <-> text codecs: hex, base64 (standard and url-safe),
-- base32, and UTF-8. The one home for them in the stdlib; the JS module
-- (hull:encoding) produces and accepts exactly the same text.
--
--   local enc = require("hull.encoding")
--   enc.hex.encode(bytes)                        -- lowercase
--   enc.hex.decode(text)                         -- bytes | nil, reason
--   enc.base64.encode(bytes, { url = true })     -- -_ alphabet, unpadded
--   enc.base64.decode(text, { lenient = true })  -- skips whitespace
--   enc.base32.encode(bytes)                     -- RFC 4648, unpadded
--
-- Values are byte strings. Decoding is strict unless asked otherwise: a byte
-- outside the alphabet, a misplaced or wrong amount of padding, or an
-- impossible length is refused, because silently skipping junk lets damaged
-- input decode to something plausible. A decoder returns nil and a reason -
-- "invalid_char", "bad_padding" or "bad_length" - on bad input, and raises
-- only when handed something that is not a string. The unused low bits of the
-- last base64 / base32 character must be zero ("non_canonical" otherwise), so
-- every value has exactly one encoding - a signature that decodes the same
-- from two different strings is malleable.
--
-- Pure: no capabilities and no other module, so anything may require it.

local M = {}

local sbyte, schar, concat = string.byte, string.char, table.concat

local function check_string(fname, s)
    if type(s) ~= "string" then
        error("encoding." .. fname .. ": expected a string, got " .. type(s), 3)
    end
end

local WS = { [32] = true, [9] = true, [10] = true, [13] = true }

-- Hex ---------------------------------------------------------------------

M.hex = {}

local HEX_OUT = {}
for i = 0, 255 do HEX_OUT[i] = string.format("%02x", i) end

local HEX_IN = {}
for i = 0, 9 do HEX_IN[48 + i] = i end
for i = 0, 5 do HEX_IN[97 + i] = 10 + i; HEX_IN[65 + i] = 10 + i end

--- Lowercase hex, two characters per byte. hex(prefix) is a prefix of
--- hex(key), which the SQL kv backend relies on for range scans.
function M.hex.encode(bytes)
    check_string("hex.encode", bytes)
    local out = {}
    for i = 1, #bytes do out[i] = HEX_OUT[sbyte(bytes, i)] end
    return concat(out)
end

--- Bytes from hex; either case is accepted.
function M.hex.decode(text)
    check_string("hex.decode", text)
    if #text % 2 ~= 0 then return nil, "bad_length" end
    local out = {}
    for i = 1, #text, 2 do
        local hi, lo = HEX_IN[sbyte(text, i)], HEX_IN[sbyte(text, i + 1)]
        if not hi or not lo then return nil, "invalid_char" end
        out[#out + 1] = schar(hi * 16 + lo)
    end
    return concat(out)
end

-- Base64 ------------------------------------------------------------------

M.base64 = {}

local STD = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

local function table_of(alphabet)
    local enc, dec = {}, {}
    for i = 1, #alphabet do
        enc[i - 1] = alphabet:sub(i, i)
        dec[alphabet:byte(i)] = i - 1
    end
    return enc, dec
end
local STD_ENC, STD_DEC = table_of(STD)
local URL_ENC, URL_DEC = table_of(URL)

--- Base64 of `bytes`. opts.url selects the url-safe alphabet (-_). Padding
--- defaults to on for the standard alphabet and off for the url-safe one,
--- the forms each is normally used in; opts.pad overrides.
function M.base64.encode(bytes, opts)
    check_string("base64.encode", bytes)
    local url = opts and opts.url
    local pad = opts and opts.pad
    if pad == nil then pad = not url end
    local E = url and URL_ENC or STD_ENC
    local out, n = {}, #bytes
    local i = 1
    while i + 2 <= n do
        local a, b, c = sbyte(bytes, i, i + 2)
        local v = (a << 16) | (b << 8) | c
        out[#out + 1] = E[v >> 18] .. E[(v >> 12) & 63] .. E[(v >> 6) & 63] .. E[v & 63]
        i = i + 3
    end
    local rem = n - i + 1
    if rem == 1 then
        local v = sbyte(bytes, i) << 4
        out[#out + 1] = E[v >> 6] .. E[v & 63] .. (pad and "==" or "")
    elseif rem == 2 then
        local a, b = sbyte(bytes, i, i + 1)
        local v = (a << 10) | (b << 2)
        out[#out + 1] = E[v >> 12] .. E[(v >> 6) & 63] .. E[v & 63] .. (pad and "=" or "")
    end
    return concat(out)
end

--- Bytes from base64. opts.url selects the url-safe alphabet, which takes no
--- padding; the standard alphabet takes padding or none, but if present it
--- must be exactly right. opts.lenient skips whitespace (key files wrap their
--- base64 at 70 columns).
function M.base64.decode(text, opts)
    check_string("base64.decode", text)
    local url = opts and opts.url
    local lenient = opts and opts.lenient
    local D = url and URL_DEC or STD_DEC
    local out = {}
    local acc, bits, count, padding = 0, 0, 0, 0
    for i = 1, #text do
        local c = sbyte(text, i)
        local v = D[c]
        if v and padding == 0 then
            acc = ((acc << 6) | v) & 0xFFFFFF
            bits = bits + 6
            count = count + 1
            if bits >= 8 then
                bits = bits - 8
                out[#out + 1] = schar((acc >> bits) & 0xFF)
            end
        elseif c == 61 and not url then          -- '='
            padding = padding + 1
        elseif not (lenient and WS[c]) then
            return nil, (v or c == 61) and "bad_padding" or "invalid_char"
        end
    end
    if count % 4 == 1 then return nil, "bad_length" end
    if padding > 0 and (padding > 2 or (count + padding) % 4 ~= 0) then
        return nil, "bad_padding"
    end
    if acc & ((1 << bits) - 1) ~= 0 then return nil, "non_canonical" end
    return concat(out)
end

-- Base32 ------------------------------------------------------------------

M.base32 = {}

local B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"
local B32_ENC, B32_DEC = table_of(B32)
for i = 1, 26 do B32_DEC[B32:byte(i) + 32] = i - 1 end   -- lowercase too

--- RFC 4648 base32, uppercase, unpadded (the form authenticator apps take).
function M.base32.encode(bytes)
    check_string("base32.encode", bytes)
    local out = {}
    local buf, bits = 0, 0
    for i = 1, #bytes do
        buf = ((buf << 8) | sbyte(bytes, i)) & 0xFFFF
        bits = bits + 8
        while bits >= 5 do
            bits = bits - 5
            out[#out + 1] = B32_ENC[(buf >> bits) & 31]
        end
    end
    if bits > 0 then out[#out + 1] = B32_ENC[(buf << (5 - bits)) & 31] end
    return concat(out)
end

-- Base32 lengths (mod 8) that some byte count encodes to: 0, 2, 4, 5 and 7.
local B32_LEN_OK = { [0] = true, [2] = true, [4] = true, [5] = true, [7] = true }

--- Bytes from base32, either case. Strictly, only the alphabet, a length some
--- input encodes to, and zero unused low bits; opts.lenient also skips
--- whitespace and '=', which people and apps copy along with a secret.
function M.base32.decode(text, opts)
    check_string("base32.decode", text)
    local lenient = opts and opts.lenient
    local out = {}
    local buf, bits, count = 0, 0, 0
    for i = 1, #text do
        local c = sbyte(text, i)
        local v = B32_DEC[c]
        if v then
            buf = ((buf << 5) | v) & 0xFFFF
            bits = bits + 5
            count = count + 1
            if bits >= 8 then
                bits = bits - 8
                out[#out + 1] = schar((buf >> bits) & 0xFF)
            end
        elseif not (lenient and (WS[c] or c == 61)) then
            return nil, "invalid_char"
        end
    end
    if not B32_LEN_OK[count % 8] then return nil, "bad_length" end
    if buf & ((1 << bits) - 1) ~= 0 then return nil, "non_canonical" end
    return concat(out)
end

-- UTF-8 ---------------------------------------------------------------------
--
-- Text and bytes are the same thing in Lua, so this is the half of the JS
-- family that matters here: checking that bytes really are UTF-8 before they
-- are used as text (a decoded token payload, say). It exists in Lua so the
-- two runtimes accept exactly the same input.

M.utf8 = {}

--- The UTF-8 bytes of `text`. A Lua string already is its bytes.
function M.utf8.encode(text)
    check_string("utf8.encode", text)
    return text
end

--- `bytes` as text if they are well-formed UTF-8 (no overlong forms, no
--- surrogates, nothing above U+10FFFF); otherwise nil, "invalid_utf8".
function M.utf8.decode(bytes)
    check_string("utf8.decode", bytes)
    if not utf8.len(bytes) then return nil, "invalid_utf8" end
    return bytes
end

return M

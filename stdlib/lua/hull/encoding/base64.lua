-- hull.encoding.base64 - standard base64, both directions.
--
-- Standard alphabet (+/), not the URL-safe one hull.crypto offers. SSH uses
-- standard base64 in two places that must match OpenSSH byte for byte: the
-- SHA256: fingerprint (unpadded) and the PEM-style armour around key files
-- (padded). A near-miss in either is worse than an outright failure, because
-- it produces something that looks right to a human comparing it.

local M = {}

local ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

-- Reverse table, built once. -1 marks a byte that is not base64 at all.
local REV = {}
for i = 0, 255 do REV[i] = -1 end
for i = 1, #ALPHABET do REV[ALPHABET:byte(i)] = i - 1 end

--- Encode without padding. This is the fingerprint form.
function M.encode_nopad(data)
    local out = {}
    local n = #data
    local i = 1
    while i + 2 <= n do
        local a, b, c = data:byte(i, i + 2)
        local v = a * 65536 + b * 256 + c
        out[#out + 1] = ALPHABET:sub((v >> 18) + 1, (v >> 18) + 1)
                     .. ALPHABET:sub(((v >> 12) & 63) + 1, ((v >> 12) & 63) + 1)
                     .. ALPHABET:sub(((v >> 6) & 63) + 1, ((v >> 6) & 63) + 1)
                     .. ALPHABET:sub((v & 63) + 1, (v & 63) + 1)
        i = i + 3
    end
    local rem = n - i + 1
    if rem == 1 then
        local v = data:byte(i) * 16
        out[#out + 1] = ALPHABET:sub((v >> 6) + 1, (v >> 6) + 1)
                     .. ALPHABET:sub((v & 63) + 1, (v & 63) + 1)
    elseif rem == 2 then
        local a, b = data:byte(i, i + 1)
        local v = a * 1024 + b * 4
        out[#out + 1] = ALPHABET:sub((v >> 12) + 1, (v >> 12) + 1)
                     .. ALPHABET:sub(((v >> 6) & 63) + 1, ((v >> 6) & 63) + 1)
                     .. ALPHABET:sub((v & 63) + 1, (v & 63) + 1)
    end
    return table.concat(out)
end

--- Encode with padding. This is the form inside a key file or a .pub line.
function M.encode(data)
    local s = M.encode_nopad(data)
    while #s % 4 ~= 0 do s = s .. "=" end
    return s
end

--- Decode. Whitespace is skipped, since armoured key files wrap at 70
--- columns; anything else outside the alphabet is refused rather than
--- ignored, because silently skipping junk would let a corrupted key file
--- decode to something plausible.
function M.decode(s)
    if type(s) ~= "string" then
        error("encoding.base64: decode expects a string", 2)
    end
    local acc, bits = 0, 0
    local out = {}
    for i = 1, #s do
        local c = s:byte(i)
        if c == 61 then break end          -- '=' ends the data
        local v = REV[c]
        if v >= 0 then
            acc = (acc << 6) | v
            bits = bits + 6
            if bits >= 8 then
                bits = bits - 8
                out[#out + 1] = string.char((acc >> bits) & 0xFF)
            end
        elseif not (c == 32 or c == 9 or c == 10 or c == 13) then
            -- Whitespace is the only thing skipped; anything else is refused
            -- rather than ignored.
            error("encoding.base64: invalid character at offset " .. tostring(i), 2)
        end
    end
    return table.concat(out)
end

return M

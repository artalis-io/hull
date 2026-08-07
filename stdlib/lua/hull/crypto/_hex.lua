--- Internal raw-byte hex helper shared across the crypto/auth stdlib.
--
-- @module hull.crypto._hex
-- @license AGPL-3.0-or-later
--
-- Contributor-only (the `_` prefix). Encodes a byte string as lowercase hex,
-- ONE hex pair per byte.
--
-- This deliberately does NOT use crypto.hex_encode. That cap hex-encodes the
-- bytes it receives across the C boundary - and the two runtimes marshal a
-- string differently: Lua strings are byte strings (so crypto.hex_encode is
-- raw there), but a JS string is UTF-8-encoded at the boundary, so
-- crypto.hexEncode inflates any code unit >= 0x80 (0xFF -> "c3bf", not "ff").
-- For hashing / HMAC keys / token wire formats over arbitrary secret bytes
-- that difference silently derives different digests per runtime. This helper
-- iterates raw bytes identically in both runtimes, so it is the ONE correct
-- home for byte-string hex. See docs/stdlib_style.md §4.

local M = {}

--- Lowercase hex of a byte string (2 chars per byte).
-- @tparam string s  raw bytes
-- @treturn string
function M.to_hex(s)
    local out = {}
    for i = 1, #s do
        out[i] = string.format("%02x", string.byte(s, i))
    end
    return table.concat(out)
end

return M

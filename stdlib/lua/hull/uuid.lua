--- RFC 9562 UUID generation (v4 random, v7 time-ordered).
--
-- Both variants are 36-char canonical strings
-- (`xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx`, `M` the version nibble, `N` the
-- variant). Built on `crypto.random` (CSPRNG) + `time.now_ms`; no new
-- authority. Apps that previously minted ad-hoc ids from
-- `crypto.base64url_encode(crypto.random(16))` should prefer these - they are
-- canonical, interoperable with external systems, and v7 is lexically sortable.
--
-- @module hull.uuid
-- @license AGPL-3.0-or-later
-- @usage
--   local uuid = require("hull.uuid")
--   local id = uuid.v7()   -- time-ordered; ideal for DB primary keys
--   local r  = uuid.v4()   -- fully random

local crypto = require("hull.crypto")
local time = require("hull.time")

local uuid = {}

local FMT = "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"

--- Random (version 4) UUID.
-- @treturn string  36-char canonical UUID.
function uuid.v4()
    local b = { crypto.random(16):byte(1, 16) }
    b[7] = (b[7] & 0x0f) | 0x40   -- version 4
    b[9] = (b[9] & 0x3f) | 0x80   -- variant 10
    return string.format(FMT, table.unpack(b))
end

--- Time-ordered (version 7) UUID: 48-bit big-endian Unix-ms timestamp followed
-- by 74 random bits. Lexically sortable by creation time - good for database
-- primary keys (better index locality than v4).
-- @treturn string  36-char canonical UUID.
function uuid.v7()
    local ms = time.now_ms()
    local r = { crypto.random(10):byte(1, 10) }
    local b = {
        (ms >> 40) & 0xff, (ms >> 32) & 0xff, (ms >> 24) & 0xff,
        (ms >> 16) & 0xff, (ms >> 8) & 0xff, ms & 0xff,
        r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
    }
    b[7] = (b[7] & 0x0f) | 0x70   -- version 7
    b[9] = (b[9] & 0x3f) | 0x80   -- variant 10
    return string.format(FMT, table.unpack(b))
end

return uuid

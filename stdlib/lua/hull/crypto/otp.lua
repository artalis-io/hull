-- hull.crypto.otp - one-time passwords: HOTP (RFC 4226), which TOTP (RFC 6238)
-- is at a time-derived counter.
--
-- The algorithm, and nothing about enrolment, storage, windows or lockout -
-- that policy is hull/web/middleware/totp's. The JS module (hull:crypto:otp)
-- produces the same codes.
--
--   1. digest = HMAC-SHA1(key, counter as 8-byte big-endian)
--   2. offset = low 4 bits of digest[19]
--   3. P = digest[offset .. offset+3] as a big-endian u32, top bit cleared
--   4. code = P mod 10^digits, zero-padded to `digits`

local crypto = require('hull.crypto')

local M = {}

--- The HOTP code for `key` (raw bytes) at `counter` (a non-negative integer),
--- `digits` long (6 by default; 6 to 8 per RFC 4226).
function M.hotp(key, counter, digits)
    -- Integral floats (a period given as 30.0, a count read back as a
    -- float) are accepted; anything fractional is not.
    counter, digits = math.tointeger(counter), math.tointeger(digits or 6)
    if type(key) ~= "string" then error("otp.hotp: key must be a byte string", 2) end
    if not counter or counter < 0 then
        error("otp.hotp: counter must be a non-negative integer", 2)
    end
    if not digits or digits < 6 or digits > 8 then
        error("otp.hotp: digits must be 6, 7 or 8", 2)
    end
    local mac = crypto.hmac_sha1(string.pack(">I8", counter), key)
    local offset = (mac:byte(20) & 0x0F) + 1
    local p = string.unpack(">I4", mac, offset) & 0x7FFFFFFF
    return string.format("%0" .. digits .. "d", p % math.tointeger(10 ^ digits))
end

--- The TOTP time step for unix time `now` and a period in seconds.
function M.step(now, period)
    if period == nil then period = 30 end
    if type(now) ~= "number" or now < 0 or type(period) ~= "number" or period <= 0 then
        error("otp.step: time must be >= 0 and period > 0", 2)
    end
    return math.tointeger(now // period) or error("otp.step: bad time or period", 2)
end

return M

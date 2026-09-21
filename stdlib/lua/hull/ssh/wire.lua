-- hull.ssh.wire - the SSH binary data types (RFC 4251 section 5).
--
-- Every byte an SSH peer sends arrives through this module, most of it before
-- authentication has happened. That is the whole reason the protocol lives in
-- Lua: a bounds mistake here is a wrong answer, not a corrupted heap.
--
-- Readers RAISE on malformed input rather than returning nil. A protocol
-- parser threads dozens of reads together, and a single missed nil check is
-- precisely the bug this layer exists to make impossible, so the caller pcalls
-- once at the packet boundary and cannot forget in between.
--
-- All values are binary-safe Lua strings; nothing here interprets text.

local M = {}

local schar, ssub, sbyte, srep = string.char, string.sub, string.byte, string.rep
local spack, sunpack = string.pack, string.unpack
local concat = table.concat

-- Writer -------------------------------------------------------------

local Writer = {}
Writer.__index = Writer

-- A new packet builder. Parts accumulate in a table and are joined once, so
-- building a packet is linear rather than quadratic in its length.
function M.writer()
    return setmetatable({ parts = {}, n = 0 }, Writer)
end

function Writer:raw(s)
    self.n = self.n + 1
    self.parts[self.n] = s
    return self
end

function Writer:byte(v)
    if type(v) ~= "number" or v < 0 or v > 255 or v % 1 ~= 0 then
        error("ssh.wire: byte out of range: " .. tostring(v), 2)
    end
    return self:raw(schar(v))
end

function Writer:boolean(v)
    -- RFC 4251: any non-zero value reads as true, but a sender MUST emit 1.
    return self:raw(v and "\1" or "\0")
end

function Writer:uint32(v)
    if type(v) ~= "number" or v % 1 ~= 0 or v < 0 or v > 0xFFFFFFFF then
        error("ssh.wire: uint32 out of range: " .. tostring(v), 2)
    end
    return self:raw(spack(">I4", v))
end

function Writer:uint64(v)
    -- Lua integers are signed 64-bit, so values above 2^63-1 cannot be
    -- represented here at all. Refuse rather than silently wrapping negative.
    if type(v) ~= "number" or v % 1 ~= 0 or v < 0 then
        error("ssh.wire: uint64 out of range: " .. tostring(v), 2)
    end
    return self:raw(spack(">I8", v))
end

-- A length-prefixed binary blob. Not text: no encoding is applied.
function Writer:string(s)
    if type(s) ~= "string" then
        error("ssh.wire: string expected, got " .. type(s), 2)
    end
    return self:raw(spack(">I4", #s)):raw(s)
end

-- A multiple-precision integer, given as a big-endian unsigned magnitude.
-- Only non-negative values are supported: SSH uses mpint for key material
-- (RSA modulus and exponent, DH values), all of which is positive.
function Writer:mpint(mag)
    if type(mag) ~= "string" then
        error("ssh.wire: mpint expects a magnitude string", 2)
    end
    -- RFC 4251: unnecessary leading zero bytes MUST NOT be included.
    local i = 1
    while i <= #mag and sbyte(mag, i) == 0 do i = i + 1 end
    local trimmed = ssub(mag, i)

    -- ...and zero is the empty string, not a zero byte.
    if #trimmed == 0 then
        return self:raw(spack(">I4", 0))
    end
    -- A leading byte of 0x80 or more would read as negative in two's
    -- complement, so a positive value pads with one zero byte.
    if sbyte(trimmed, 1) >= 0x80 then
        trimmed = "\0" .. trimmed
    end
    return self:raw(spack(">I4", #trimmed)):raw(trimmed)
end

-- A comma-separated algorithm list, length-prefixed as one string.
function Writer:namelist(names)
    if type(names) ~= "table" then
        error("ssh.wire: namelist expects a table", 2)
    end
    for _, n in ipairs(names) do
        if type(n) ~= "string" or n == "" then
            error("ssh.wire: namelist entries must be non-empty strings", 2)
        end
        if n:find(",", 1, true) then
            -- A comma inside a name splits into two names on the wire, which
            -- is how a caller could unknowingly offer an algorithm it did not
            -- mean to.
            error("ssh.wire: namelist entry contains a comma: " .. n, 2)
        end
    end
    return self:string(concat(names, ","))
end

function Writer:build()
    return concat(self.parts)
end

-- Bytes written so far, without building.
function Writer:len()
    local t = 0
    for i = 1, self.n do t = t + #self.parts[i] end
    return t
end

-- Reader -------------------------------------------------------------

local Reader = {}
Reader.__index = Reader

-- A cursor over buf. pos is 1-based and points at the next unread byte.
function M.reader(buf)
    if type(buf) ~= "string" then
        error("ssh.wire: reader expects a string", 2)
    end
    return setmetatable({ buf = buf, pos = 1 }, Reader)
end

function Reader:remaining()
    return #self.buf - self.pos + 1
end

-- Every read funnels through here, so the bounds check exists exactly once.
function Reader:need(n)
    if n < 0 or self:remaining() < n then
        error("ssh.wire: truncated: need " .. tostring(n)
              .. " byte(s), have " .. tostring(self:remaining()), 3)
    end
end

function Reader:raw(n)
    self:need(n)
    local s = ssub(self.buf, self.pos, self.pos + n - 1)
    self.pos = self.pos + n
    return s
end

function Reader:byte()
    self:need(1)
    local v = sbyte(self.buf, self.pos)
    self.pos = self.pos + 1
    return v
end

function Reader:boolean()
    -- RFC 4251: zero is false and EVERY non-zero value is true, not just 1.
    -- Treating 2 as malformed would reject a conformant sender.
    return self:byte() ~= 0
end

function Reader:uint32()
    self:need(4)
    local v = sunpack(">I4", self.buf, self.pos)
    self.pos = self.pos + 4
    return v
end

function Reader:uint64()
    self:need(8)
    local v = sunpack(">I8", self.buf, self.pos)
    self.pos = self.pos + 8
    if v < 0 then
        -- Lua integers are signed, so a value with the top bit set has
        -- wrapped. Refuse rather than hand back a negative length or offset.
        error("ssh.wire: uint64 exceeds the representable range", 2)
    end
    return v
end

function Reader:string()
    local n = self:uint32()
    -- Bounds are checked BEFORE anything of size n is touched, so a peer
    -- claiming a 4 GB string on a 40-byte packet costs nothing.
    self:need(n)
    return self:raw(n)
end

-- Returns the big-endian unsigned magnitude with leading zeros stripped.
function Reader:mpint()
    local n = self:uint32()
    self:need(n)
    local s = self:raw(n)
    if #s == 0 then return "" end
    if sbyte(s, 1) >= 0x80 then
        error("ssh.wire: negative mpint is not supported", 2)
    end
    local i = 1
    while i < #s and sbyte(s, i) == 0 do i = i + 1 end
    return ssub(s, i)
end

function Reader:namelist()
    local s = self:string()
    local out = {}
    if s == "" then return out end      -- an empty list is legal
    local i = 1
    while true do
        local j = s:find(",", i, true)
        local name = j and ssub(s, i, j - 1) or ssub(s, i)
        if name == "" then
            error("ssh.wire: empty name in namelist", 2)
        end
        out[#out + 1] = name
        if not j then break end
        i = j + 1
    end
    return out
end

-- Convenience --------------------------------------------------------

-- Encode one length-prefixed string without building a whole writer.
function M.string(s)
    return spack(">I4", #s) .. s
end

-- Encode a uint32 on its own.
function M.uint32(v)
    return spack(">I4", v)
end

-- Zero padding. Callers needing RANDOM padding (the binary packet protocol
-- does) must supply it themselves: this module has no capability access and
-- must not pretend to produce entropy.
function M.zeros(n)
    return srep("\0", n)
end

return M

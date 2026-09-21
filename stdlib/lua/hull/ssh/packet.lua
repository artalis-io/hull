-- hull.ssh.packet - version exchange and the binary packet protocol.
--
-- RFC 4253 section 4.2 (identification string) and section 6 (binary packet
-- protocol). Pure functions over byte strings: nothing here does I/O, holds a
-- key, or knows a cipher. The transport layer feeds it bytes and supplies
-- randomness; the cipher layer wraps it once keys exist.
--
-- Framing is where a peer first gets to choose a number that Hull then
-- allocates against, so the length checks below are the load-bearing part of
-- this file, not the shape of the header.

local M = {}

local ssub, sbyte, srep = string.sub, string.byte, string.rep
local spack, sunpack = string.pack, string.unpack

-- RFC 4253 section 6.1: an implementation MUST support 32768 bytes of payload
-- and a total packet of 35000. Accepting more than a peer could legitimately
-- need only widens what a hostile length field can make us hold, so this is a
-- ceiling rather than a target.
M.MAX_PACKET  = 35000
M.MIN_PADDING = 4          -- RFC 4253 section 6
M.MIN_BLOCK   = 8          -- padding aligns to max(8, cipher block size)

-- Identification string ----------------------------------------------

-- RFC 4253 section 4.2: at most 255 bytes including the trailing CR LF, and
-- the version and comment fields carry no SP, CR or LF.
M.MAX_IDENT = 255

-- Build our identification line. `software` must be printable US-ASCII with
-- no space or minus, because the field is delimited by exactly those.
function M.build_ident(software)
    if type(software) ~= "string" or software == "" then
        error("ssh.packet: software version must be a non-empty string", 2)
    end
    if software:find("[^%w%._]") then
        error("ssh.packet: software version may use only letters, digits, dot and underscore", 2)
    end
    local line = "SSH-2.0-" .. software .. "\r\n"
    if #line > M.MAX_IDENT then
        error("ssh.packet: identification string too long", 2)
    end
    return line
end

-- Parse one identification line, WITHOUT its CR LF.
--
-- Returns a table on success, or nil plus a reason. A server may send any
-- number of other lines first (RFC 4253 section 4.2 allows it, for banners),
-- so the caller decides what to do with a line that is not an identification
-- string rather than this deciding for it.
function M.parse_ident(line)
    if type(line) ~= "string" then
        return nil, "not a string"
    end
    if #line > M.MAX_IDENT then
        return nil, "identification string too long"
    end
    local rest = line:match("^SSH%-([^\r\n]*)$")
    if not rest then
        return nil, "not an identification string"
    end
    -- protoversion "-" softwareversion [SP comments]
    local proto, tail = rest:match("^([^%-]+)%-(.*)$")
    if not proto or tail == "" then
        return nil, "malformed identification string"
    end
    if proto ~= "2.0" then
        -- 1.99 means a server willing to speak either; Hull speaks only 2.0.
        return nil, "unsupported protocol version: " .. proto
    end
    local software, comments = tail:match("^([^ ]+) (.*)$")
    if not software then
        software, comments = tail, nil
    end
    return { protoversion = proto, software = software, comments = comments,
             line = line }
end

-- Binary packet protocol ----------------------------------------------

-- How much padding a payload needs.
--
-- The padded region is padding_length(1) + payload + padding, and the whole
-- of it must be a multiple of the block size. In the plaintext and
-- non-AEAD cases the 4-byte length field is inside that region too; with an
-- AEAD like aes256-gcm the length travels as associated data and is NOT
-- counted, which is why the caller says which it wants.
function M.padding_for(payload_len, block, length_in_block)
    local b = block or M.MIN_BLOCK
    if b < M.MIN_BLOCK then b = M.MIN_BLOCK end
    local base = payload_len + 1 + (length_in_block and 4 or 0)
    local pad = b - (base % b)
    if pad < M.MIN_PADDING then pad = pad + b end
    return pad
end

-- Frame a payload into an unencrypted packet.
--
-- `random_bytes` is a function(n) returning n random bytes. It is passed in
-- rather than reached for: this module has no capability access, and padding
-- that is not random leaks plaintext structure in the encrypted case.
function M.frame(payload, block, random_bytes)
    if type(payload) ~= "string" then
        error("ssh.packet: payload must be a string", 2)
    end
    if type(random_bytes) ~= "function" then
        error("ssh.packet: a random_bytes function is required", 2)
    end
    local pad = M.padding_for(#payload, block, true)
    local packet_length = #payload + pad + 1
    if packet_length + 4 > M.MAX_PACKET then
        error("ssh.packet: packet too large", 2)
    end
    local padding = random_bytes(pad)
    if type(padding) ~= "string" or #padding ~= pad then
        error("ssh.packet: random_bytes returned the wrong length", 2)
    end
    return spack(">I4", packet_length) .. string.char(pad) .. payload .. padding
end

-- Parse one unencrypted packet from the front of `buf`.
--
-- Returns payload, bytes_consumed on success.
-- Returns nil, "need_more" when buf holds an incomplete packet - the normal
-- case while reading from a socket, not an error.
-- Raises on a packet that cannot be valid whatever arrives next.
function M.parse(buf, block)
    if type(buf) ~= "string" then
        error("ssh.packet: buf must be a string", 2)
    end
    if #buf < 4 then return nil, "need_more" end

    local packet_length = sunpack(">I4", buf, 1)

    -- Bound the claim BEFORE waiting for that many bytes, or a peer sending a
    -- 4 GB length would have us buffer forever waiting for a packet that will
    -- never be valid.
    if packet_length + 4 > M.MAX_PACKET then
        error("ssh.packet: declared packet length " .. tostring(packet_length)
              .. " exceeds the maximum")
    end
    -- The smallest legal packet is padding_length(1) + 0 payload + 4 padding.
    if packet_length < 1 + M.MIN_PADDING then
        error("ssh.packet: declared packet length " .. tostring(packet_length)
              .. " is below the minimum")
    end
    local b = block or M.MIN_BLOCK
    if b < M.MIN_BLOCK then b = M.MIN_BLOCK end
    if (packet_length + 4) % b ~= 0 then
        error("ssh.packet: packet length is not a multiple of the block size")
    end

    if #buf < packet_length + 4 then return nil, "need_more" end

    local padding_length = sbyte(buf, 5)
    if padding_length < M.MIN_PADDING then
        error("ssh.packet: padding of " .. tostring(padding_length)
              .. " is below the minimum")
    end
    local payload_len = packet_length - padding_length - 1
    if payload_len < 0 then
        -- Padding longer than the packet claiming to contain it.
        error("ssh.packet: padding exceeds the packet")
    end
    local payload = ssub(buf, 6, 5 + payload_len)
    return payload, packet_length + 4
end

-- Zero padding, for tests and for callers that do their own randomness.
function M.zero_padding(n)
    return srep("\0", n)
end

return M

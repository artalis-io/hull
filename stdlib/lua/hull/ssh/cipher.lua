-- hull.ssh.cipher - the aes256-gcm@openssh.com packet layer.
--
-- RFC 5647 as OpenSSH implements it. This is the slice where getting the IV
-- wrong is unrecoverable rather than merely broken: two packets encrypted
-- under one (key, IV) leak their XOR AND the GHASH authentication subkey,
-- which lets an attacker forge tags for that key for the rest of the
-- connection. So the counter lives inside this object, advances exactly once
-- per packet, and no caller can supply an IV.
--
-- The framing differs from the generic binary packet protocol in one way that
-- matters: the 4-byte length travels in the CLEAR and is authenticated as
-- associated data. That is what lets a receiver know how much to read before
-- it can decrypt anything - and it is also why the padding alignment excludes
-- the length field (see hull.ssh.packet.padding_for).
--
--   uint32   packet_length      plaintext, authenticated as AAD
--   byte     padding_length  \
--   byte[]   payload          |  encrypted
--   byte[]   padding         /
--   byte[16] tag
--
-- The AEAD itself is injected: hull.crypto reaches AES-GCM only through the
-- composed TLS feature, and this module has no capability access.

local packet = require('hull.ssh.packet')

local M = {}

M.KEY_LEN   = 32
M.IV_LEN    = 12
M.TAG_LEN   = 16
M.BLOCK     = 16      -- the encrypted region aligns to the AES block
M.LENGTH_LEN = 4

-- How many packets one key may protect before this refuses to continue.
--
-- NIST SP 800-38D allows 2^32 invocations for a 96-bit deterministic IV, so
-- this is a conservative backstop rather than the real bound. It should never
-- be reached: a server rekeys long before (OpenSSH at about a gigabyte, which
-- is tens of thousands of packets), and hull.ssh.transport now absorbs that
-- rekey instead of dying on it. Reaching this means no rekey ever happened,
-- and the honest response is to stop rather than keep encrypting past the
-- limit the key was chosen for.
M.MAX_PACKETS = 0x80000000

-- When a rekey is DUE - the point at which this client asks for new keys
-- rather than waiting to be asked.
--
-- RFC 4253 section 9 says "after each gigabyte of transmitted data or after
-- each hour", and recommends it be done by whichever side reaches the limit
-- first. OpenSSH's default RekeyLimit is the same gigabyte. Matching it means
-- a Hull client talking to something that never initiates - an embedded
-- server, a jump host, a router - still gets fresh keys, instead of running a
-- single key out to MAX_PACKETS and then dying.
--
-- The packet trigger is the same bound one order of magnitude earlier, so
-- MAX_PACKETS stays what it is meant to be: the thing that never happens.
--
-- There is deliberately NO time trigger. This module is handed a stream and
-- an AEAD and nothing else; it has no clock, and inventing one by threading a
-- time capability through the transport would give the SSH stdlib an
-- authority it does not otherwise need, for a bound the byte counter already
-- covers on any connection actually moving data.
M.REKEY_BYTES   = 1024 * 1024 * 1024        -- 1 GiB, per direction
M.REKEY_PACKETS = 0x08000000                -- 2^27, a sixteenth of MAX_PACKETS

local Cipher = {}
Cipher.__index = Cipher

-- One direction of the connection. c2s and s2c each get their own object,
-- because sharing one counter across directions would reuse IVs immediately.
function M.new(key, iv)
    if type(key) ~= "string" or #key ~= M.KEY_LEN then
        error("ssh.cipher: key must be 32 bytes", 2)
    end
    if type(iv) ~= "string" or #iv ~= M.IV_LEN then
        error("ssh.cipher: iv must be 12 bytes", 2)
    end
    -- RFC 5647 section 7.1: the IV splits into a fixed field and an
    -- invocation counter. Only the counter moves.
    return setmetatable({
        key = key,
        fixed = iv:sub(1, 4),
        counter = iv:sub(5, 12),
        packets = 0,
        bytes = 0,
    }, Cipher)
end

-- Increment the 8-byte big-endian invocation counter.
local function bump(counter)
    local b = { counter:byte(1, 8) }
    for i = 8, 1, -1 do
        b[i] = b[i] + 1
        if b[i] <= 255 then break end
        b[i] = 0
        -- carry continues; a full wrap needs 2^64 packets, which is not
        -- reachable, but wrapping silently would be the one unrecoverable
        -- bug in this file, so it is not left implicit.
        if i == 1 then
            error("ssh.cipher: invocation counter wrapped; the key must not be reused")
        end
    end
    return string.char(table.unpack(b))
end

function Cipher:iv()
    return self.fixed .. self.counter
end

-- Advance to the next packet. Called on BOTH seal and open, so the two ends
-- stay in step.
function Cipher:advance()
    self.counter = bump(self.counter)
    self.packets = self.packets + 1
    if self.packets >= M.MAX_PACKETS then
        error("ssh.cipher: " .. tostring(self.packets)
              .. " packets under one key without a rekey; refusing to continue")
    end
end

function Cipher:packets_sent()
    return self.packets
end

-- Bytes this key has protected, counted as they go on the wire: the whole
-- frame, length field and tag included, not just the payload. That is what
-- the limit is about - what an attacker has collected under one key - and it
-- is also the number that matches what OpenSSH's RekeyLimit counts.
function Cipher:bytes_processed()
    return self.bytes
end

-- Whether this key has protected enough that a rekey is DUE.
--
-- Advisory, and deliberately so: this object knows nothing about when it is
-- safe to start a key exchange. It answers the question; hull.ssh.transport
-- decides where to act on the answer.
function Cipher:rekey_due(limits)
    limits = limits or {}
    local max_bytes   = limits.bytes   or M.REKEY_BYTES
    local max_packets = limits.packets or M.REKEY_PACKETS
    return self.bytes >= max_bytes or self.packets >= max_packets
end

-- Encrypt one payload into a wire packet.
--
-- `aead` is { seal = function(key, iv, aad, plaintext) -> ciphertext, tag }.
function Cipher:seal(aead, payload, random_bytes)
    if type(random_bytes) ~= "function" then
        error("ssh.cipher: a random_bytes function is required", 2)
    end
    -- The length field is NOT part of the aligned region for an AEAD.
    local pad = packet.padding_for(#payload, M.BLOCK, false)
    local padding = random_bytes(pad)
    if #padding ~= pad then
        error("ssh.cipher: random_bytes returned the wrong length", 2)
    end

    local plain = string.char(pad) .. payload .. padding
    if #plain % M.BLOCK ~= 0 then
        error("ssh.cipher: encrypted region is not block aligned")
    end
    local packet_length = #plain
    local aad = string.pack(">I4", packet_length)

    local ct, tag = aead.seal(self.key, self:iv(), aad, plain)
    if not ct or #ct ~= #plain or #tag ~= M.TAG_LEN then
        error("ssh.cipher: AEAD seal returned the wrong shape")
    end
    self:advance()
    local frame = aad .. ct .. tag
    self.bytes = self.bytes + #frame
    return frame
end

-- How many bytes a full packet occupies, given its plaintext length field.
function M.frame_size(packet_length)
    return M.LENGTH_LEN + packet_length + M.TAG_LEN
end

-- Decrypt one packet from the front of `buf`.
--
-- Returns payload, consumed.
-- Returns nil, "need_more" while incomplete.
-- Raises on a length that cannot be valid, or on authentication failure -
-- which is NOT recoverable: a packet that does not authenticate means the
-- stream is no longer trustworthy, so the caller must drop the connection
-- rather than resynchronise.
function Cipher:open(aead, buf)
    if #buf < M.LENGTH_LEN then return nil, "need_more" end

    local packet_length = string.unpack(">I4", buf, 1)
    -- Bounded before waiting, as everywhere else. The length is plaintext and
    -- attacker-controlled, and it is only AUTHENTICATED once the tag checks -
    -- so a hostile length is refused on plausibility first.
    if packet_length > packet.MAX_PACKET then
        error("ssh.cipher: declared packet length " .. tostring(packet_length)
              .. " exceeds the maximum")
    end
    if packet_length < M.BLOCK or packet_length % M.BLOCK ~= 0 then
        error("ssh.cipher: declared packet length " .. tostring(packet_length)
              .. " is not a whole number of blocks")
    end

    local total = M.frame_size(packet_length)
    if #buf < total then return nil, "need_more" end

    local aad = buf:sub(1, M.LENGTH_LEN)
    local ct  = buf:sub(M.LENGTH_LEN + 1, M.LENGTH_LEN + packet_length)
    local tag = buf:sub(M.LENGTH_LEN + packet_length + 1, total)

    local plain = aead.open(self.key, self:iv(), aad, ct, tag)
    if not plain then
        error("ssh.cipher: packet failed authentication")
    end
    self:advance()
    self.bytes = self.bytes + total

    local pad = plain:byte(1)
    if pad < packet.MIN_PADDING or pad + 1 > #plain then
        -- Reached only for a packet that DID authenticate, so this is a peer
        -- sending something malformed rather than an attacker.
        error("ssh.cipher: padding of " .. tostring(pad) .. " is not usable")
    end
    return plain:sub(2, #plain - pad), total
end

return M

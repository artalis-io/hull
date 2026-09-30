-- hull.ssh.chacha - the chacha20-poly1305@openssh.com packet layer.
--
-- OpenSSH's construction (PROTOCOL.chacha20poly1305), not the IETF AEAD. The
-- 64 bytes of key material split in two: the SECOND half (K_1) encrypts only
-- the 4-byte length, the FIRST (K_2) everything else. The nonce is the
-- packet's sequence number, which is why every call here takes it:
--
--   uint32   packet_length      ChaCha20(K_1, seq, counter 0)
--   byte     padding_length  \
--   byte[]   payload          |  ChaCha20(K_2, seq, counter 1)
--   byte[]   padding         /
--   byte[16] tag                Poly1305(key = ChaCha20(K_2, seq, counter 0)
--                                              over 32 zero bytes,
--                                        msg = the two ciphertexts above)
--
-- Encrypting the length is the difference that matters to the transport: it
-- cannot learn how much to read by peeking, so it asks `needed` instead.
--
-- The sequence number as nonce is also why this cipher is only ever
-- negotiated under strict KEX (hull.ssh.kexinit): Terrapin (CVE-2023-48795)
-- shifts sequence numbers during the unauthenticated handshake, and with the
-- nonce bound to them a deletion no longer desynchronises anything visible.
--
-- The primitives are injected, as the AEAD is for hull.ssh.cipher: this
-- module has no capability access. `prims` = { chacha20 = function(key,
-- nonce12, counter, data), poly1305 = function(key, msg), ct_eq =
-- function(a, b) } - hull.crypto's chacha20 / poly1305 / constant_time_eq.

local packet = require('hull.ssh.packet')
local cipher = require('hull.ssh.cipher')     -- the shared per-key limits

local M = {}

M.NAME       = "chacha20-poly1305@openssh.com"
M.KEY_LEN    = 64
M.TAG_LEN    = 16
M.BLOCK      = 8
M.LENGTH_LEN = 4

local ZEROS32 = string.rep("\0", 32)

local Chacha = {}
Chacha.__index = Chacha

-- One direction of the connection, like hull.ssh.cipher's objects.
function M.new(key, prims)
    if type(key) ~= "string" or #key ~= M.KEY_LEN then
        error("ssh.chacha: key must be 64 bytes", 2)
    end
    if type(prims) ~= "table" or type(prims.chacha20) ~= "function"
       or type(prims.poly1305) ~= "function" or type(prims.ct_eq) ~= "function" then
        error("ssh.chacha: chacha20, poly1305 and ct_eq are required", 2)
    end
    return setmetatable({
        k_main = key:sub(1, 32),
        k_len  = key:sub(33, 64),
        prims  = prims,
        packets = 0,
        bytes = 0,
    }, Chacha)
end

-- RFC 8439's 12-byte nonce carrying OpenSSH's 64-bit one: the high word of
-- the original 64-bit block counter is always zero here.
local function nonce(seq)
    if math.type(seq) ~= "integer" or seq < 0 or seq > 0xFFFFFFFF then
        error("ssh.chacha: a sequence number is required")
    end
    return "\0\0\0\0" .. string.pack(">I8", seq)
end

-- Advance to the next packet; the per-key limits are hull.ssh.cipher's.
function Chacha:advance()
    self.packets = self.packets + 1
    if self.packets >= cipher.MAX_PACKETS then
        error("ssh.chacha: " .. tostring(self.packets)
              .. " packets under one key without a rekey; refusing to continue")
    end
end

function Chacha:packets_sent()    return self.packets end
function Chacha:bytes_processed() return self.bytes end

function Chacha:rekey_due(limits)
    limits = limits or {}
    return self.bytes >= (limits.bytes or cipher.REKEY_BYTES)
        or self.packets >= (limits.packets or cipher.REKEY_PACKETS)
end

-- Encrypt one payload into a wire packet. `_aead` is hull.ssh.cipher's
-- argument, unused here; the primitives came with the key.
function Chacha:seal(_aead, payload, random_bytes, seq)
    if type(random_bytes) ~= "function" then
        error("ssh.chacha: a random_bytes function is required", 2)
    end
    local n = nonce(seq)
    -- As for the GCM layer, the (encrypted) length field is not part of the
    -- aligned region: OpenSSH excludes it for every cipher with a tag.
    local pad = packet.padding_for(#payload, M.BLOCK, false)
    local padding = random_bytes(pad)
    if #padding ~= pad then
        error("ssh.chacha: random_bytes returned the wrong length", 2)
    end
    local plain = string.char(pad) .. payload .. padding
    if #plain + M.LENGTH_LEN > packet.MAX_PACKET then
        error("ssh.chacha: packet too large", 2)
    end

    local p = self.prims
    local enc_len  = p.chacha20(self.k_len, n, 0, string.pack(">I4", #plain))
    local poly_key = p.chacha20(self.k_main, n, 0, ZEROS32)
    local enc_body = p.chacha20(self.k_main, n, 1, plain)
    local tag = p.poly1305(poly_key, enc_len .. enc_body)
    if #enc_len ~= 4 or #enc_body ~= #plain or #tag ~= M.TAG_LEN then
        error("ssh.chacha: a primitive returned the wrong shape")
    end
    self:advance()
    local frame = enc_len .. enc_body .. tag
    self.bytes = self.bytes + #frame
    return frame
end

-- The whole frame's size, from the (encrypted) length at the front of `buf`.
-- `buf` must hold at least 4 bytes; below that, 4 is what is needed.
--
-- The length is decrypted but NOT yet authenticated - the tag covers it, and
-- is checked only once the whole frame is here - so it is bounded on
-- plausibility first, exactly as the GCM layer bounds its plaintext one. A
-- hostile or corrupted length must not make the transport wait for 4 GiB.
function Chacha:needed(buf, seq)
    if #buf < M.LENGTH_LEN then return M.LENGTH_LEN end
    local len = self.prims.chacha20(self.k_len, nonce(seq), 0, buf:sub(1, M.LENGTH_LEN))
    local packet_length = string.unpack(">I4", len)
    if packet_length + M.LENGTH_LEN > packet.MAX_PACKET then
        error("ssh.chacha: declared packet length " .. tostring(packet_length)
              .. " exceeds the maximum")
    end
    if packet_length < M.BLOCK or packet_length % M.BLOCK ~= 0 then
        error("ssh.chacha: declared packet length " .. tostring(packet_length)
              .. " is not a whole number of blocks")
    end
    return M.LENGTH_LEN + packet_length + M.TAG_LEN
end

-- Decrypt one packet from the front of `buf`: payload, consumed; or nil,
-- "need_more" while incomplete. Raises on authentication failure, which, as
-- for GCM, the caller must treat as the end of the connection.
function Chacha:open(_aead, buf, seq)
    if #buf < M.LENGTH_LEN then return nil, "need_more" end
    local total = self:needed(buf, seq)
    if #buf < total then return nil, "need_more" end

    local n = nonce(seq)
    local p = self.prims
    local body_end = total - M.TAG_LEN
    local poly_key = p.chacha20(self.k_main, n, 0, ZEROS32)
    local want = p.poly1305(poly_key, buf:sub(1, body_end))
    -- Authenticate before decrypting anything past the length.
    if not p.ct_eq(want, buf:sub(body_end + 1, total)) then
        error("ssh.chacha: packet failed authentication")
    end
    local plain = p.chacha20(self.k_main, n, 1, buf:sub(M.LENGTH_LEN + 1, body_end))
    self:advance()
    self.bytes = self.bytes + total

    local pad = plain:byte(1)
    if pad < packet.MIN_PADDING or pad + 1 > #plain then
        error("ssh.chacha: padding of " .. tostring(pad) .. " is not usable")
    end
    return plain:sub(2, #plain - pad), total
end

return M

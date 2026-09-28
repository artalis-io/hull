-- hull.ssh.transport - the connection: handshake, encrypted packets, channels.
--
-- This is the piece that turns the other modules into a client. It owns the
-- sequence (identification, KEXINIT, key exchange, NEWKEYS, userauth) and the
-- packet loop that runs afterwards.
--
-- The stream and the crypto are BOTH injected. In production they are the
-- private byte stream (hull.ssh._stream) and hull.crypto; in tests they are a
-- buffer and a stub, which is what lets the sequence be exercised without a
-- server. The transport itself never reaches for a capability.
--
-- The stream interface it expects is exactly what the binding provides:
--
--   stream:read(n)   -> bytes (1..n), "" at EOF, or nil, err
--   stream:write(s)  -> true, or nil, err
--   stream:close()
--
-- read() returning SHORT is normal, not an error: the binding hands back
-- whatever arrived. Under Hull the call parks the coroutine and resumes when
-- the loop has more, so this code reads as though it were blocking while
-- never blocking the loop it runs on.

-- How much stdin exec will write to a command.
--
-- Measured against OpenSSH on Windows over loopback: 122 KiB in with 131 KiB
-- out completes, 305 KiB in with 330 KiB out stalls until the socket times
-- out, and 1.2 MiB in with SMALL output is instant. So the wall is the two
-- directions together, around 256 KiB, not the size of either one.
--
-- Interleaving reads with the writes does NOT lift it: instrumented against
-- that server, nothing is readable at any point during the write, so there is
-- nothing for a client to drain - the peer has stopped producing, and only
-- writing less relieves it. Hence a bound rather than a scheduling fix.
--
-- Bulk data belongs in sftp, which moves one direction at a time and has no
-- such limit.
local STDIN_MAX = 128 * 1024

-- How long closing a timed-out command's channel may take. The command has
-- already had its time; this only bounds reading its tail so the connection
-- can be reused.
local CHANNEL_DRAIN_MS = 5000

-- The most channel data held back while a rekey WE started waits for the
-- peer's KEXINIT (see defer_message). Far below the Lua heap limit.
local DEFER_MAX_BYTES = 8 * 1024 * 1024

local packet     = require('hull.ssh.packet')
local kexinit    = require('hull.ssh.kexinit')
local kex        = require('hull.ssh.kex')
local hostkey    = require('hull.ssh.hostkey')
local cipher     = require('hull.ssh.cipher')
local userauth   = require('hull.ssh.userauth')
local channel    = require('hull.ssh.channel')
local wire       = require('hull.ssh.wire')
local hex        = require('hull.encoding').hex

-- hull.crypto speaks hex; SSH speaks bytes. What comes back from crypto is
-- always well-formed hex, so a failure here is a defect, not input.
local function unhex(h)
    return (assert(hex.decode(h), "ssh.transport: crypto returned malformed hex"))
end

local M = {}

-- Transport-layer messages that can arrive at any time and mean nothing to
-- the layer above (RFC 4253 section 11).
local SSH_MSG_DISCONNECT    = 1
local SSH_MSG_IGNORE        = 2
local SSH_MSG_UNIMPLEMENTED = 3
local SSH_MSG_DEBUG         = 4
local SSH_MSG_GLOBAL_REQUEST = 80
local SSH_MSG_REQUEST_SUCCESS = 81
local SSH_MSG_REQUEST_FAILURE = 82
local SSH_MSG_KEXINIT       = 20

local Transport = {}
Transport.__index = Transport

--- @param stream  read/write/close, as above
--- @param crypto  sha256, x25519, x25519_keypair, ed25519_verify,
---                ed25519_sign, random, gcm_seal, gcm_open
-- Liveness defaults (see Transport:quiet). OpenSSH's ServerAliveInterval /
-- ServerAliveCountMax, and an idle bound on top of them.
M.KEEPALIVE_MS  = 30000
M.KEEPALIVE_MAX = 3
M.IDLE_MS       = 60000

function M.new(stream, crypto, opts)
    opts = opts or {}
    local function opt(v, default) if v == nil then return default end return v end
    local t = setmetatable({
        -- Liveness. Every read waits at most wait_ms (see apply_wait); each
        -- time one expires the silence is counted, and once authenticated a
        -- keepalive asks whether the server is still there.
        keepalive_ms  = opt(opts.keepalive_ms, M.KEEPALIVE_MS),
        keepalive_max = opt(opts.keepalive_max, M.KEEPALIVE_MAX),
        idle_ms       = opt(opts.idle_ms, M.IDLE_MS),
        quiet_ms      = 0,        -- silence so far, in wait_ms steps
        unanswered    = 0,        -- keepalives sent since the server last spoke
        stream = stream,
        crypto = crypto,
        -- build_ident validates the software string: it is sent in the clear
        -- AND hashed into the exchange, so a CR/LF or space in it would
        -- corrupt both. Stored without the CR LF, which is sent separately.
        ident = packet.build_ident(opts.software or "Hull"):sub(1, -3),
        inbuf = "",
        next_channel = 0,
        rekeys = 0,
        -- Packet sequence numbers (RFC 4253 section 6.4), one per direction,
        -- uint32 and wrapping. The GCM mode in use does not feed them into
        -- the MAC, so today they matter only to strict KEX; they are kept
        -- exactly anyway, because a MAC mode that does use them must find
        -- them already right rather than rediscover Terrapin.
        recv_seq = 0,
        send_seq = 0,
        -- Messages read while waiting for the peer's KEXINIT during a rekey
        -- WE started. See defer_message.
        deferred = {},
        deferred_head = 1,
        -- nil means hull.ssh.cipher's own limits; opts.rekey_limit is the
        -- escape hatch a test uses to reach them in milliseconds rather than
        -- gigabytes.
        rekey_limit = opts.rekey_limit,
        aead = {
            seal = function(k, iv, aad, p) return crypto.gcm_seal(k, iv, aad, p) end,
            open = function(k, iv, aad, c, t) return crypto.gcm_open(k, iv, aad, c, t) end,
        },
        raw_sha = kex.raw_hash(crypto.sha256),
    }, Transport)
    t:apply_wait()
    return t
end

-- Failures the layers above branch on carry a code: raised as a table, so
-- the facade can hand them back as they are rather than as "io_error".
--   "deadline"  a phase ran out of time (handshake, one command); the
--               connection itself may be fine
--   "timeout"   the server stopped answering; the connection is dead
local function fail(code, detail)
    error({ code = code, detail = detail }, 0)
end

-- How long any one read or write may wait: the keepalive interval when
-- keepalives are on, else the idle bound, else unbounded. Pushed down to the
-- stream, which reports each expiry as "timeout" without ending the stream.
function Transport:apply_wait()
    local w = (self.keepalive_ms > 0 and self.keepalive_ms) or self.idle_ms
    self.wait_ms = w
    if w > 0 and self.stream.wait then self.stream:wait(w) end
end

-- Bound the current phase: the stream's deadline, `ms` from now (0 clears).
function Transport:set_deadline(ms)
    if self.stream.deadline then self.stream:deadline(ms) end
end

local KEEPALIVE = wire.writer():byte(80):string("keepalive@openssh.com")
                               :boolean(true):build()

-- A wait expired with nothing from the server.
--
-- Silence is counted in wait_ms steps. Once it reaches idle_ms the server is
-- taken to be gone. Before that, if the session is authenticated and not
-- mid-rekey, a keepalive (a global request wanting a reply) gives a server
-- that is merely quiet - a command that prints nothing for a while - the
-- chance to prove it is there: its reply is traffic, and traffic resets the
-- count. keepalive_max unanswered in a row also ends it, which only matters
-- when idle_ms is 0. `may_ping` is false while a WRITE is what is waiting:
-- the send queue is full, so a keepalive could not go out anyway.
function Transport:quiet(may_ping)
    self.quiet_ms = self.quiet_ms + self.wait_ms
    if self.idle_ms > 0 and self.quiet_ms >= self.idle_ms then
        self.dead = { code = "timeout",
                      detail = "nothing from the server for "
                               .. tostring(self.quiet_ms) .. " ms" }
        fail(self.dead.code, self.dead.detail)
    end
    if not (may_ping and self.authenticated and not self.in_kex
            and self.keepalive_ms > 0) then
        return
    end
    if self.keepalive_max > 0 and self.unanswered >= self.keepalive_max then
        self.dead = { code = "timeout",
                      detail = tostring(self.unanswered) .. " keepalives unanswered" }
        fail(self.dead.code, self.dead.detail)
    end
    self.unanswered = self.unanswered + 1
    self:send_packet(KEEPALIVE)
end

-- One read from the stream, however long the server takes, within the rules
-- above. Returns 1..max bytes, or "" at EOF.
function Transport:read_some(max)
    for _ = 1, 1000000 do
        local chunk, err, code = self.stream:read(max)
        if chunk then
            if chunk ~= "" then self.quiet_ms, self.unanswered = 0, 0 end
            return chunk
        end
        if code == "timeout" then
            self:quiet(true)
        elseif code == "deadline" then
            fail("deadline", "deadline reached")
        else
            error("ssh: read failed: " .. tostring(err))
        end
    end
    error("ssh: read made no progress")
end

-- Reading -------------------------------------------------------------------

-- The most asked of the stream in one read. The binding sizes its buffer from
-- the request before a byte arrives, so the request must never be a number
-- the peer chose; a short read is normal anyway, so asking for less costs
-- nothing but another loop turn.
local READ_CHUNK = 32768

-- Pull at least `n` bytes into the buffer. A short read is the normal case.
--
-- Callers bound `n` before calling: read_packet has the parser vet a declared
-- length first. This loop only moves bytes.
function Transport:fill(n)
    if #self.inbuf >= n then return end
    -- Gather into a table and join once. The stream is free to return SHORT
    -- reads, and does; appending each one onto a growing string makes filling
    -- a packet quadratic in its size, which for a 32 KiB packet arriving in
    -- small pieces is hundreds of megabytes of copying for 32 KiB of data.
    local parts, have = { self.inbuf }, #self.inbuf
    while have < n do
        local chunk = self:read_some(math.min(n - have, READ_CHUNK))
        if chunk == "" then
            error("ssh: connection closed by peer")
        end
        parts[#parts + 1] = chunk
        have = have + #chunk
    end
    self.inbuf = table.concat(parts)
end

-- Write all of `bytes`. A write that waited too long admitted nothing (the
-- stream is all-or-none), so it is retried under the same liveness rules as
-- a read, without a keepalive: the queue a keepalive would join is full.
function Transport:send_raw(bytes)
    for _ = 1, 1000000 do
        local ok, err, code = self.stream:write(bytes)
        if ok then return end
        if code == "timeout" then
            self:quiet(false)
        elseif code == "deadline" then
            fail("deadline", "deadline reached")
        else
            error("ssh: write failed: " .. tostring(err))
        end
    end
    error("ssh: write made no progress")
end

-- One line of the identification exchange, without its CR LF.
function Transport:read_line()
    for _ = 1, 64 do
        local nl = self.inbuf:find("\n", 1, true)
        if nl then
            local line = self.inbuf:sub(1, nl - 1)
            self.inbuf = self.inbuf:sub(nl + 1)
            return (line:gsub("\r$", ""))
        end
        if #self.inbuf > packet.MAX_IDENT * 4 then
            -- A peer that never sends a newline must not make us buffer
            -- without limit while it does so.
            error("ssh: no identification line within a sensible bound")
        end
        local chunk = self:read_some(256)
        if chunk == "" then error("ssh: connection closed during identification") end
        self.inbuf = self.inbuf .. chunk
    end
    error("ssh: too many lines before the identification string")
end

-- Packets -------------------------------------------------------------------

-- Before NEWKEYS: plaintext framing. After: the AEAD.
function Transport:send_packet(payload)
    if self.c2s then
        self:send_raw(self.c2s:seal(self.aead, payload, self.crypto.random))
    else
        self:send_raw(packet.frame(payload, 8, self.crypto.random))
    end
    self.send_seq = (self.send_seq + 1) & 0xFFFFFFFF
end

-- One packet off the wire: plaintext framing before NEWKEYS, the AEAD after.
--
-- The parser is asked FIRST, with only the 4-byte length in hand. The length
-- is peer-controlled - and before NEWKEYS, or always under GCM, it is also
-- unauthenticated - so waiting for however many bytes it claims would let a
-- 0xFFFFFFFF make us try to buffer 4 GiB. packet.parse and Cipher:open bound
-- the claim (maximum, minimum, block alignment) and raise before answering
-- "need_more", so only a packet that can be valid is ever waited for, and the
-- rules live in one place per framing rather than being restated here.
function Transport:read_packet()
    local s2c, aead = self.s2c, self.aead
    local function parse(buf)
        if s2c then return s2c:open(aead, buf) end
        return packet.parse(buf, 8)
    end

    self:fill(4)
    local payload, used = parse(self.inbuf)
    if not payload then
        local n = string.unpack(">I4", self.inbuf)
        self:fill(s2c and cipher.frame_size(n) or n + 4)
        payload, used = parse(self.inbuf)
    end
    self.inbuf = self.inbuf:sub(used + 1)
    self.recv_seq = (self.recv_seq + 1) & 0xFFFFFFFF
    return payload
end

-- Read the next packet the layer above cares about.
--
-- DISCONNECT, IGNORE, DEBUG and UNIMPLEMENTED can arrive at any time and mean
-- nothing to a caller; a GLOBAL_REQUEST wanting a reply gets a refusal, since
-- Hull implements none of them and silence would stall a server that waits.
-- `strict` is the strict-KEX rule: during a key exchange the transport
-- chatter this normally skips is not permitted at all. Skipping it is the
-- primitive Terrapin uses - an inserted IGNORE shifts what the two ends
-- think they agreed, and a client that silently drops it never notices.
-- Hold a message that arrived at a moment we cannot deliver it, to be
-- returned by the next next_message.
--
-- Exactly one situation produces these: a rekey THIS side started. Between
-- our KEXINIT and the peer's, the peer has not yet seen ours and is still
-- entitled to send channel data (RFC 4253 section 9 constrains it only from
-- its own KEXINIT onward). Those bytes belong to whatever the caller was
-- doing; dropping them would silently lose output, and handing them to the
-- key exchange would make it fail on a message that is perfectly legal.
function Transport:defer_message(p)
    self.deferred[#self.deferred + 1] = p
end

function Transport:take_deferred()
    local q = self.deferred
    if self.deferred_head > #q then return nil end
    local p = q[self.deferred_head]
    q[self.deferred_head] = nil
    self.deferred_head = self.deferred_head + 1
    if self.deferred_head > #q then
        self.deferred, self.deferred_head = {}, 1
    end
    return p
end

-- Anything set aside during a rekey comes out first, in order: it arrived
-- first. The key exchange itself must NOT go through here - it calls
-- read_message directly - or it would re-read the messages it just deferred
-- and make no progress at all.
function Transport:next_message(strict)
    local held = self:take_deferred()
    if held then return held end
    return self:read_message(strict)
end

-- The next message off the WIRE, with transport chatter filtered.
--
-- No cap on how much chatter it skips. Every pass needs a packet from the
-- peer, so this cannot spin by itself; a peer that sends only chatter is no
-- different from a slow one, and the caller's deadline (exec timeout_ms, the
-- connect timeout) and the idle timeout bound that. A count did harm instead:
-- keepalives ARE chatter - the replies to ours, a server's own
-- keepalive@openssh.com requests, the IGNOREs some servers send - so a quiet
-- exec or SFTP wait died after 256 of them, about two hours.
function Transport:read_message(strict)
    while true do
        local p = self:read_packet()
        local m = p:byte(1)

        if strict and (m == SSH_MSG_IGNORE or m == SSH_MSG_DEBUG
                       or m == SSH_MSG_UNIMPLEMENTED
                       or m == SSH_MSG_GLOBAL_REQUEST) then
            error("ssh: message " .. tostring(m)
                  .. " is not permitted during key exchange (strict KEX)")
        end

        if m == SSH_MSG_KEXINIT and self.session_id and not self.in_kex then
            -- The server has asked to rekey (RFC 4253 section 9). Absorb it
            -- here rather than letting it reach the channel layer: whatever
            -- the caller is in the middle of - an exec streaming output, an
            -- sftp transfer - should not have to know that the connection
            -- re-keyed underneath it.
            self.in_kex = true
            local ok, why = self:run_kex(self.kex_opts or {}, p)
            self.in_kex = false
            if not ok then
                error("ssh: rekey failed: "
                      .. wire.safe_name((why and why.code) or "unknown"))
            end
        elseif m == SSH_MSG_DISCONNECT then
            local r = wire.reader(p); r:byte()
            local code = r:uint32()
            local desc = r:remaining() > 0 and r:string() or ""
            -- safe_name, not safe_text: this lands inside ONE error line, and
            -- a newline in it could forge a second one in the caller's log.
            error("ssh: server disconnected (" .. tostring(code) .. "): "
                  .. wire.safe_name(desc, 200))
        elseif m == SSH_MSG_GLOBAL_REQUEST then
            local r = wire.reader(p); r:byte(); r:string()
            if r:boolean() then self:send_packet(string.char(SSH_MSG_REQUEST_FAILURE)) end
        elseif m == channel.SSH_MSG_CHANNEL_OPEN then
            -- The server asking US to open a channel (forwarded connections,
            -- agent, X11). None is offered, so every one is refused - with a
            -- reply, because a server that asked is waiting for one.
            local r = wire.reader(p); r:byte(); r:string()
            local sender = r:uint32()
            self:send_packet(channel.build_open_failure(sender,
                channel.OPEN_ADMINISTRATIVELY_PROHIBITED,
                "hull/ssh accepts no server-initiated channels"))
        -- Skipped as well as the chatter: REQUEST_SUCCESS / FAILURE, the
        -- answer to a keepalive (the only global request this client sends).
        -- Its arrival already reset the silence count in read_some; the
        -- content says nothing more.
        elseif m ~= SSH_MSG_IGNORE and m ~= SSH_MSG_DEBUG
               and m ~= SSH_MSG_UNIMPLEMENTED
               and m ~= SSH_MSG_REQUEST_SUCCESS and m ~= SSH_MSG_REQUEST_FAILURE then
            return p
        end
    end
end

-- The next message OFF THE WIRE must be exactly `want`. Key exchange only.
--
-- Replaces the earlier "read up to eight and look for it" loops. Those would
-- silently discard whatever else arrived, which during a key exchange is the
-- window an injected message lives in - and one of them did not even check
-- that it had found what it was looking for before carrying on.
--
-- read_message, not next_message: during a rekey this side started, channel
-- data that arrived before the peer's KEXINIT is held in `deferred`, and
-- next_message hands that out first - so the exchange would find a data
-- message where its reply belongs and fail the connection.
function Transport:expect(want, what, strict)
    local p = self:read_message(strict)
    local got = p:byte(1)
    if got ~= want then
        error("ssh: expected " .. what .. " (" .. tostring(want)
              .. ") but the peer sent message " .. tostring(got))
    end
    return p
end

-- Handshake -------------------------------------------------------------------

-- Returns true, or nil plus a structured reason. The host-key cases carry the
-- fingerprint, because the caller has to be able to show it.
function Transport:handshake(opts)
    -- identification
    local v_s
    for _ = 1, 32 do
        v_s = self:read_line()
        if v_s:match("^SSH%-") then break end
        if opts.on_banner then opts.on_banner(userauth.sanitize_text(v_s)) end
        v_s = nil
    end
    if not v_s then return nil, { code = "no_identification" } end

    local id = packet.parse_ident(v_s)
    if not id then
        return nil, { code = "bad_identification",
                      detail = wire.safe_name(v_s, 255) }
    end
    self:send_raw(self.ident .. "\r\n")
    self.server_ident = v_s
    -- A rekey needs the same offer and the same host; keep them rather
    -- than making every later call thread them through.
    self.kex_opts = opts

    return self:run_kex(opts)
end

-- One key exchange: negotiate, exchange, verify the host key, install keys.
--
-- Split out of handshake because it happens more than once. RFC 4253 section
-- 9 lets EITHER side ask for a new key exchange at any point after the first,
-- and OpenSSH does so once a connection has moved about a gigabyte. Before
-- this existed that message reached the channel layer, which raised
-- "unexpected connection message 20" and killed the connection - which is to
-- say a long-running streamed command, the thing streaming exists for, would
-- die partway through for no reason the caller could act on.
--
-- `i_s` is the server KEXINIT payload when the server started the exchange
-- and next_message has already read it; nil when we are starting.
function Transport:run_kex(opts, i_s)
    local rekey = self.session_id ~= nil

    local i_c = kexinit.build(opts.offer, self.crypto.random(16))
    self:send_packet(i_c)
    if not i_s then
        if rekey then
            -- WE started this one, so the peer has not seen our KEXINIT yet
            -- and may still be sending channel data. Set that aside rather
            -- than failing on it (see defer_message); from the peer's own
            -- KEXINIT onward, RFC 4253 section 9 permits only key-exchange
            -- traffic, so nothing after this point needs deferring.
            -- Bounded by BYTES, not a count: packets can be 35000 bytes each,
            -- and only channel traffic (types 90-100) is legal here at all.
            local held = 0
            for _ = 1, 4096 do
                local p = self:read_message()
                local m = p:byte(1)
                if m == SSH_MSG_KEXINIT then i_s = p; break end
                if m < 90 or m > 100 then
                    return nil, { code = "unexpected_message",
                                  detail = "message " .. m .. " before the peer's KEXINIT" }
                end
                held = held + #p
                if held > DEFER_MAX_BYTES then
                    return nil, { code = "no_kexinit_response",
                                  detail = "more than " .. DEFER_MAX_BYTES
                                           .. " bytes before the peer's KEXINIT" }
                end
                self:defer_message(p)
            end
            if not i_s then
                return nil, { code = "no_kexinit_response" }
            end
        else
            i_s = self:read_message()
        end
    end

    local server = kexinit.parse(i_s)
    local neg, nerr = kexinit.negotiate(opts.offer, server)
    if not neg then return nil, { code = "no_common_algorithm", detail = nerr } end
    self.negotiated = neg
    -- Both sides have to advertise it for the stricter rules to apply; a
    -- server that does not gets RFC 4253 behaviour, where transport chatter
    -- during a key exchange is legal.
    --
    -- Decided by the FIRST exchange and kept: the markers are only meaningful
    -- in the initial KEXINIT (OpenSSH ignores them later), so recomputing it
    -- per rekey relaxed the rules for every rekey after the first.
    if not rekey then
        self.strict_kex = kexinit.server_is_strict(server)
        -- Strict KEX requires the peer's KEXINIT to be its very first packet.
        -- read_message skips IGNORE and DEBUG, and it was a skipped IGNORE
        -- before KEXINIT that let Terrapin shift the sequence numbers.
        if self.strict_kex and self.recv_seq ~= 1 then
            error("ssh: strict KEX: the server's KEXINIT was not its first packet")
        end
    end

    if kexinit.guess_was_wrong(server, neg) then
        self:read_message(self.strict_kex)   -- discard the guess (RFC 4253 7.1)
    end

    -- curve25519 exchange
    local q_c_hex, sk_hex = self.crypto.x25519_keypair()
    local q_c = unhex(q_c_hex)
    self:send_packet(kex.build_ecdh_init(q_c))

    local reply = kex.parse_ecdh_reply(
        self:expect(kex.SSH_MSG_KEX_ECDH_REPLY, "KEX_ECDH_REPLY",
                    self.strict_kex))

    local k_hex, kerr = self.crypto.x25519(sk_hex, hex.encode(reply.q_s))
    if not k_hex then return nil, { code = "bad_kex_point", detail = kerr } end
    local k_raw = unhex(k_hex)

    local h = self.raw_sha(kex.exchange_hash_input({
        v_c = self.ident, v_s = self.server_ident, i_c = i_c, i_s = i_s,
        k_s = reply.host_key, q_c = q_c, q_s = reply.q_s, k = k_raw,
    }))

    if rekey then
        -- A rekey re-presents the host key, and it must be the SAME one. The
        -- store would catch a substitution too, but only against what was
        -- stored; this pins against the key THIS connection was built on, so
        -- a server cannot swap identity halfway through a session it already
        -- holds. The signature is still checked, over the new exchange hash.
        if reply.host_key ~= self.host_key_blob then
            return nil, { code = "host_changed_midsession",
                          fingerprint = hostkey.fingerprint(self.raw_sha,
                                                            reply.host_key),
                          stored_fingerprint = self.host_fingerprint }
        end
        local ok, why = hostkey.verify_signature(self.crypto, hex.encode,
                                                 reply.host_key,
                                                 reply.signature, h)
        if not ok then
            return nil, { code = "host_key_invalid", detail = why }
        end
    else
        -- host key: verified, then trusted or not. This never decides for the
        -- caller; an unknown or changed host comes back as a reason carrying
        -- the fingerprint (see hull.ssh.hostkey).
        local d = hostkey.verify(self.crypto, hex.encode, self.raw_sha,
                                 opts.trust, hostkey.store_name(opts.host, opts.port),
                                 reply.host_key, reply.signature, h)
        if not d.ok then
            return nil, { code = "host_key_invalid", detail = d.reason }
        end
        if d.status ~= hostkey.TRUSTED then
            return nil, { code = "host_" .. d.status,
                          fingerprint = d.fingerprint,
                          stored_fingerprint = d.stored_fingerprint,
                          key_blob = d.key_blob }
        end
        self.host_fingerprint = d.fingerprint
        self.host_key_blob    = reply.host_key
        -- RFC 4253 section 7.2: the session identifier is the exchange hash
        -- from the FIRST exchange and never changes. A rekey derives new keys
        -- from a new H against that same identifier, which is what keeps the
        -- userauth signature bound to the connection rather than to a key set.
        self.session_id = h
    end

    local keys = kex.derive_keys(self.raw_sha, k_raw, h, self.session_id,
                                 kex.SIZES[neg.cipher_c2s])

    -- The counters live in the cipher objects, which are about to be
    -- replaced. Carry the totals up first, so `stats()` describes the
    -- CONNECTION rather than only the current key.
    if self.c2s then
        self.total_sent = (self.total_sent or 0) + self.c2s:bytes_processed()
    end
    if self.s2c then
        self.total_received = (self.total_received or 0) + self.s2c:bytes_processed()
    end

    -- Nothing may be sent between our NEWKEYS and the peer's, which is what
    -- lets both directions switch together here: NEWKEYS itself travels under
    -- the OLD keys, and the peer switches its send side the moment it sends
    -- its own.
    self:send_packet(string.char(kex.SSH_MSG_NEWKEYS))
    -- Strict KEX resets each direction's sequence number at its NEWKEYS, so
    -- nothing that crossed the wire before the keys changed can be counted
    -- after it.
    if self.strict_kex then self.send_seq = 0 end
    self:expect(kex.SSH_MSG_NEWKEYS, "NEWKEYS", self.strict_kex)
    if self.strict_kex then self.recv_seq = 0 end
    self.c2s = cipher.new(keys.key_c2s, keys.iv_c2s)
    self.s2c = cipher.new(keys.key_s2c, keys.iv_s2c)
    if rekey then self.rekeys = self.rekeys + 1 end
    return true
end

-- Rekeying from THIS side ------------------------------------------------
--
-- RFC 4253 section 9 lets either end ask, and says the one that reaches the
-- limit first should. Until now Hull only ever absorbed the server's: correct
-- against OpenSSH, which asks after about a gigabyte, and worth nothing
-- against a peer that never does. Embedded servers, appliance SSH stacks and
-- plenty of jump hosts never do - and a client that also never does runs one
-- key until hull.ssh.cipher's MAX_PACKETS backstop drops the connection.
--
-- So this side now asks too, and the two mechanisms cover different halves:
-- WE initiate between operations, where starting an exchange is unambiguous;
-- the SERVER's request is absorbed wherever it arrives, including halfway
-- through a streamed command. A single transfer larger than the limit is the
-- server's to rekey - which is exactly the case OpenSSH handles.

--- Start a key exchange now. Returns true, or nil plus a structured reason.
---
--- Safe to call between operations. It is NOT safe to call while a channel is
--- mid-stream from the caller's own code (inside an on_stdout callback, say):
--- the exchange reads packets, and those reads would consume the very output
--- the callback is being handed.
function Transport:rekey()
    if not self.session_id then return nil, { code = "not_handshaken" } end
    if self.dead then return nil, self.dead end
    if self.in_kex then return true end          -- one is already running
    self.in_kex = true
    -- No pcall: a raise here comes from the stream or from a failed
    -- authentication of the exchange, and in both cases the connection is
    -- already finished - the same reasoning as the absorb path in
    -- next_message, which is why in_kex is not restored on that path either.
    local ok, why = self:run_kex(self.kex_opts or {})
    self.in_kex = false
    if not ok then return nil, why end
    return true
end

--- Rekey if this connection has moved enough under the current keys.
---
--- Returns true if an exchange ran. Called from open_session, which is the
--- one point every operation passes through and the one point where nothing
--- is in flight.
function Transport:maybe_rekey()
    if self.in_kex or not self.session_id then return false end
    if not self.c2s or not self.s2c then return false end
    if not (self.c2s:rekey_due(self.rekey_limit)
            or self.s2c:rekey_due(self.rekey_limit)) then
        return false
    end
    local ok, why = self:rekey()
    if not ok then
        -- Same response as a failed absorbed rekey: continuing would mean
        -- encrypting past the limit the key was chosen for.
        error("ssh: rekey failed: "
              .. wire.safe_name((why and why.code) or "unknown"))
    end
    return true
end

--- What this connection has moved, and how many times it has re-keyed.
---
--- The byte counts are for the whole connection, not the current key: a
--- number that silently reset on every rekey would be the one number a
--- caller watching for a rekey must not be given.
function Transport:stats()
    local sent = (self.total_sent or 0)
        + (self.c2s and self.c2s:bytes_processed() or 0)
    local recv = (self.total_received or 0)
        + (self.s2c and self.s2c:bytes_processed() or 0)
    return {
        rekeys           = self.rekeys,
        bytes_sent       = sent,
        bytes_received   = recv,
        packets_sent     = self.c2s and self.c2s:packets_sent() or 0,
        packets_received = self.s2c and self.s2c:packets_sent() or 0,
        rekey_due        = (self.c2s ~= nil and self.c2s:rekey_due(self.rekey_limit))
                           or (self.s2c ~= nil and self.s2c:rekey_due(self.rekey_limit)),
    }
end

-- Authentication ------------------------------------------------------------------

function Transport:authenticate(user, key, on_banner)
    self:send_packet(userauth.build_service_request())
    local accepted = userauth.parse_service_accept(self:next_message())
    if accepted ~= userauth.SERVICE_USERAUTH then
        return nil, { code = "service_refused",
                      detail = wire.safe_name(accepted) }
    end

    -- hex on the way in, raw on the way out. crypto.ed25519_sign takes a
    -- 128-char hex secret and hands back a hex signature, while a private key
    -- carries 64 RAW bytes and signature_blob wants 64 raw bytes back - so
    -- both ends need converting, exactly as the VERIFY path already does via
    -- hostkey.verify_signature(crypto, hex.encode, ...). Without it userauth
    -- died on "secret key must be 128 hex chars" against a real server.
    local blob    = userauth.signed_blob(self.session_id, user, key.blob)
    local sig_hex = self.crypto.ed25519_sign(blob, hex.encode(key.secret))
    local sig     = unhex(sig_hex)
    self:send_packet(userauth.build_request(user, key.blob,
                                            userauth.signature_blob(sig)))

    for _ = 1, 16 do
        local r = userauth.parse_response(self:next_message())
        if r.type == "banner" then
            if on_banner then on_banner(r.message) end
        elseif r.type == "success" then
            self.user = user
            self.authenticated = true      -- keepalives may now be sent
            return true
        elseif r.type == "failure" then
            -- Partial success is NOT authentication: the server wants another
            -- factor, and reporting it as success would skip that.
            return nil, { code = r.partial and "partial_success" or "auth_failed",
                          methods = r.methods }
        end
    end
    return nil, { code = "no_auth_response" }
end

-- Channels ----------------------------------------------------------------------------

function Transport:open_session()
    -- The one gateway every operation passes through, and the one moment
    -- nothing is in flight: exec and sftp both start here, and neither has a
    -- channel open yet. Asking for keys anywhere else means asking in the
    -- middle of somebody's output.
    --
    -- Also where a connection already declared dead says so: once the
    -- server has stopped answering, every later call fails at once with the
    -- same reason rather than waiting out the idle bound again.
    if self.dead then return nil, self.dead end
    self:maybe_rekey()

    local ch = channel.new({ id = self.next_channel })
    self.next_channel = self.next_channel + 1
    self:send_packet(ch:open_message())

    for _ = 1, 16 do
        local m = channel.parse(self:next_message())
        if m.type == "open_confirmation" then
            ch:handle(m); return ch
        elseif m.type == "open_failure" then
            ch:handle(m)
            return nil, { code = "channel_refused", detail = m.description }
        end
    end
    return nil, { code = "no_channel_response" }
end

-- Read and discard what the peer still has to say about a channel we have
-- given up on, up to and including its CHANNEL_CLOSE.
--
-- RFC 4254 section 5.3 makes the exchange symmetric: sending CHANNEL_CLOSE
-- does not end it, the peer's CHANNEL_CLOSE does. Until then the peer may
-- still be sending data it produced before it saw ours. Those bytes are on
-- the one connection everything else shares, so not reading them here means
-- reading them THERE - inside the next command, which raises because they
-- name a channel it does not own.
--
-- Bounded, and tolerant of a read that fails: this runs on a path that is
-- already handling a failure, and must not turn it into a hang or a second
-- error that buries the first. If the bound is reached the channel is still
-- open and its messages still coming, so the CONNECTION is closed: the next
-- operation then fails as "closed" instead of receiving this channel's data.
function Transport:drain_channel(ch)
    if ch.closed then return end
    for _ = 1, 10000 do
        local ok, m = pcall(function()
            return channel.parse(self:next_message())
        end)
        if not ok then return end
        if m.recipient == ch.local_id then
            pcall(function() ch:handle(m) end)
            if m.type == "close" then return end
        end
    end
    self:close()
end

-- Close channel `ch` from our side and read it through to the peer's CLOSE.
--
-- The one way every path ends a channel - a command that finished, one that
-- was aborted or whose callback raised, an sftp session the caller closed.
-- Sending CLOSE alone is not enough: until the peer's CLOSE arrives, its
-- data, EOF and exit-status for this channel are still in flight, and
-- whatever reads next on the connection gets them - the NEXT channel, which
-- raises on a message addressed to an id it does not own. Once only: a
-- second CLOSE for one channel is a protocol error.
function Transport:close_channel(ch)
    if ch.close_sent then return end
    ch.close_sent = true
    pcall(function()
        self:send_packet(channel.build_close(ch.remote_id))
        self:drain_channel(ch)
    end)
end

-- The next connection message, applied to channel `ch`.
--
-- A channel request the server wants answered gets CHANNEL_FAILURE: this
-- client acts on exit-status and exit-signal (which never ask for a reply)
-- and nothing else. Staying silent is not neutral - OpenSSH's
-- ClientAliveInterval sends keepalive@openssh.com with want_reply set on an
-- open session channel, and disconnects a client that never answers, which
-- killed long-running exec and sftp sessions.
function Transport:channel_message(ch)
    local m = channel.parse(self:next_message())
    ch:handle(m)
    if m.type == "request" and m.want_reply and ch.remote_id then
        self:send_packet(channel.build_failure(ch.remote_id))
    end
    return m
end

-- Wait for the reply to a channel request.
--
-- A window adjust can arrive first: replies interleave, and assuming the next
-- message answers the last request is how a client desynchronises.
function Transport:await_channel_reply(ch)
    for _ = 1, 32 do
        local m = self:channel_message(ch)
        if m.type == "request_success" then return true end
        if m.type == "request_failure" then return false end
    end
    error("ssh: no reply to the channel request")
end

--- Run a command. Returns { status, signal, stdout, stderr }.
-- Run a command and return { status, signal, stdout, stderr }.
--
-- Each output stream is delivered one of two ways, chosen per stream:
--
--   opts.on_stdout / opts.on_stderr   a chunk at a time, as it arrives
--   (neither given)                   accumulated, returned at the end
--
-- Accumulating is convenient for a command that prints a line and exits, but
-- it is the wrong shape for anything else: a caller watching a deploy sees
-- nothing until the process ends, and the output has to fit in memory, which
-- is why that path needs `max_output` and the streaming one does not. A
-- callback stream is never accumulated, so `stdout` comes back empty for it.
--
-- opts.stdin is written to the command before its output is drained, then
-- EOF is sent. That is the path for feeding a command data without a shell
-- redirect, the same way sftp writes a file without shell quoting.
function Transport:exec(command, opts)
    opts = opts or {}
    local on_stdout, on_stderr = opts.on_stdout, opts.on_stderr

    local stdin = opts.stdin
    if stdin ~= nil and type(stdin) ~= "string" then
        return nil, { code = "bad_stdin", detail = type(stdin) }
    end
    -- The whole command - output, exit status, close - within this many ms.
    local timeout = opts.timeout_ms
    if timeout ~= nil and (math.type(timeout) ~= "integer" or timeout < 1
                           or timeout > 86400000) then
        return nil, { code = "bad_timeout", detail = tostring(timeout) }
    end

    local ch, cerr = self:open_session()
    if not ch then return nil, cerr end

    -- Any exit that does not run to the peer's CHANNEL_CLOSE goes through
    -- close_channel: returning early without draining is how one aborted
    -- command breaks every command after it.
    local function abort(reason)
        self:close_channel(ch)
        return nil, reason
    end

    self:send_packet(channel.build_exec(ch.remote_id, command))
    if not self:await_channel_reply(ch) then
        return abort({ code = "exec_refused", detail = command })
    end

    local out, errout = {}, {}
    local limit = opts.max_output or (8 * 1024 * 1024)
    local buffered = 0
    local overflow = false

    -- `buffered` counts only what is held in memory, so a caller streaming
    -- stdout and accumulating stderr has the cap applied to stderr alone.
    local function deliver(m)
        local cb, into
        if m.type == "data" then
            cb, into = on_stdout, out
        elseif m.type == "extended_data" and m.stderr then
            cb, into = on_stderr, errout
        else
            return
        end
        if cb then
            -- The caller's own error stays the caller's: wrapped so the
            -- facade rethrows it as raised, rather than reporting it as a
            -- connection failure.
            local cb_ok, cb_err = pcall(cb, m.data)
            if not cb_ok then error({ app_error = cb_err }, 0) end
            return
        end
        buffered = buffered + #m.data
        if buffered > limit then overflow = true return end
        into[#into + 1] = m.data
    end

    -- One message: account for it, hand off its payload, and top up the
    -- receive window so a streaming sender never stalls waiting on us.
    local function pump()
        local m = self:channel_message(ch)
        deliver(m)
        local adj = ch:window_adjustment()
        if adj then self:send_packet(adj) end
        return m
    end

    local function drive()
        if stdin then
            -- Refuse up front rather than part way through: a caller whose
            -- input is too large should learn that before half of it is on
            -- the wire and the command has started acting on it.
            if #stdin > STDIN_MAX then
                return { code = "stdin_too_large", limit = STDIN_MAX,
                         detail = "use sftp for bulk data" }
            end
            local sent = 1
            while sent <= #stdin and not ch.closed do
                local room = ch:sendable()
                -- Zero room means the peer has granted nothing more, and only
                -- the peer can change that, so blocking here is correct.
                while room <= 0 and not ch.closed do
                    pump()
                    if overflow then return { code = "output_too_large",
                                              limit = limit } end
                    room = ch:sendable()
                end
                if ch.closed then break end
                local chunk = stdin:sub(sent, sent + room - 1)
                self:send_packet(ch:data_message(chunk))
                sent = sent + #chunk
            end
            if not ch.closed then
                self:send_packet(channel.build_eof(ch.remote_id))
            end
        end

        -- Until the peer closes the channel, however long a command streams:
        -- every pass needs a message from the peer, and opts.timeout_ms bounds
        -- the time. (A count here ended a long `journalctl -f` as if it had
        -- finished.)
        while true do
            local m = pump()
            if overflow then
                return { code = "output_too_large", limit = limit }
            end
            if m.type == "close" then
                -- RFC 4254 section 5.3: a side that receives CHANNEL_CLOSE
                -- must send one back unless it already has. Breaking without
                -- replying leaves the channel half-open on the server for the
                -- life of the connection, which matters once a tool runs many
                -- commands.
                break
            end
        end
        return nil
    end

    if timeout then self:set_deadline(timeout) end
    local ok, failure = pcall(drive)
    if timeout then self:set_deadline(0) end
    if not ok then
        if type(failure) == "table" and failure.code == "deadline" then
            -- The COMMAND ran out of time; the connection may be fine. Close
            -- the channel under a short bound of its own, so the next command
            -- starts clean. If even that does not finish, the connection is
            -- not trusted with another one.
            self:set_deadline(CHANNEL_DRAIN_MS)
            self:close_channel(ch)
            self:set_deadline(0)
            if not ch.closed then
                self.dead = { code = "timeout",
                              detail = "a timed-out command's channel did not close" }
                self:close()
            end
            return nil, { code = "timeout",
                          detail = "the command did not finish within "
                                   .. tostring(timeout) .. " ms" }
        end
        -- A raising callback must not leave the channel half-open, nor its
        -- residue in the read path, just because the error came from above.
        self:close_channel(ch)
        error(failure, 0)
    end
    if failure then return abort(failure) end
    self:close_channel(ch)

    local res = ch:result()
    return { status = res.status, signal = res.signal,
             stdout = table.concat(out), stderr = table.concat(errout) }
end

-- SFTP --------------------------------------------------------------------------------

--- Open an SFTP session on its own channel. See hull.ssh.sftp_client.
function Transport:sftp()
    return require('hull.ssh.sftp_client').open(self)
end

function Transport:close()
    -- Swallowed deliberately, and this is the only place that does it
    -- without saying so: close() is the last act on a connection that is
    -- already finished, and a stream that has gone away cannot be closed any
    -- harder. Raising here would replace whatever the caller was actually
    -- dealing with.
    pcall(function() self.stream:close() end)
end

return M

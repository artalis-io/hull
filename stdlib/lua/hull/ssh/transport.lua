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

-- The most channel data held back while a rekey WE started waits for the
-- peer's KEXINIT (see defer_message). Far below the Lua heap limit.
local DEFER_MAX_BYTES = 8 * 1024 * 1024

local packet     = require('hull.ssh.packet')
local kexinit    = require('hull.ssh.kexinit')
local kex        = require('hull.ssh.kex')
local hostkey    = require('hull.ssh.hostkey')
local cipher     = require('hull.ssh.cipher')
local chacha     = require('hull.ssh.chacha')
local userauth   = require('hull.ssh.userauth')
local channel    = require('hull.ssh.channel')
local session    = require('hull.ssh.session')
local wire       = require('hull.ssh.wire')

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
--- @param crypto  sha256, x25519, x25519_keypair, ed25519_verify, verify
---                (ECDSA / RSA host keys), ed25519_sign, sign (RSA and
---                ECDSA user keys), random, gcm_seal, gcm_open
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
        channels = {},          -- our local id -> Channel, while it is open
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
        raw_sha = crypto.sha256,
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

-- One stream call, with the transport marked as having it in flight for its
-- duration. A stream call can park the coroutine (the binding yields to the
-- event loop), and a SECOND coroutine using the connection meanwhile is
-- refused by the binding as "busy" - but for a send, only after the packet
-- was sealed, which had already advanced the cipher's nonce: the next packet
-- then failed authentication at the server and the shared connection died.
-- send_packet consults this flag first, so that caller's mistake is refused
-- with the cipher state untouched.
function Transport:stream_call(method, arg)
    self.io_busy = true
    local ok, a, b, c = pcall(self.stream[method], self.stream, arg)
    self.io_busy = false
    if not ok then error(a, 0) end
    return a, b, c
end

-- One read from the stream, however long the server takes, within the rules
-- above. Returns 1..max bytes, or "" at EOF.
function Transport:read_some(max)
    for _ = 1, 1000000 do
        local chunk, err, code = self:stream_call("read", max)
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
    -- read_some already turns every failure into a raise (with keepalives
    -- between timeouts), so the only outcome left to handle is the end.
    local buf, ok = wire.gather(self.inbuf, n,
                                function(m) return self:read_some(m) end,
                                READ_CHUNK)
    self.inbuf = buf
    if not ok then error("ssh: connection closed by peer") end
end

-- Write all of `bytes`. A write that waited too long admitted nothing (the
-- stream is all-or-none), so it is retried under the same liveness rules as
-- a read, without a keepalive: the queue a keepalive would join is full.
function Transport:send_raw(bytes)
    for _ = 1, 1000000 do
        local ok, err, code = self:stream_call("write", bytes)
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
--
-- Sealing advances the cipher's state (GCM's invocation counter), so a packet
-- that was sealed must reach the wire or the connection is finished: the next
-- one would be sealed under a nonce the peer never saw used. Hence the busy
-- check BEFORE sealing (see stream_call), and a connection marked dead when a
-- sealed packet's write fails after all - rather than left looking usable,
-- with the next send failing authentication at the server.
function Transport:send_packet(payload)
    if self.dead then
        -- Coded like the failure that killed it (a timeout stays a timeout).
        fail(self.dead.code or "dead",
             "the connection is no longer usable"
             .. (self.dead.detail and (": " .. tostring(self.dead.detail)) or ""))
    end
    if self.io_busy then
        error("ssh: busy - another coroutine is using this connection")
    end
    if self.c2s then
        local sealed = self.c2s:seal(self.aead, payload, self.crypto.random,
                                     self.send_seq)
        local ok, err = pcall(self.send_raw, self, sealed)
        if not ok then
            if not self.dead then
                self.dead = { code = "send_failed",
                              detail = "a sealed packet could not be written" }
            end
            error(err, 0)
        end
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
    local s2c, aead, seq = self.s2c, self.aead, self.recv_seq
    local function parse(buf)
        if s2c then return s2c:open(aead, buf, seq) end
        return packet.parse(buf, 8)
    end

    self:fill(4)
    local payload, used = parse(self.inbuf)
    if not payload then
        -- The cipher says how much: not every cipher sends the length in the
        -- clear (chacha20-poly1305 encrypts it).
        self:fill(s2c and s2c:needed(self.inbuf, seq)
                      or wire.peek_uint32(self.inbuf) + 4)
        payload, used = parse(self.inbuf)
    end
    self.inbuf = self.inbuf:sub(used + 1)
    self.recv_seq = (self.recv_seq + 1) & 0xFFFFFFFF
    return payload
end

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
-- DISCONNECT, IGNORE, DEBUG and UNIMPLEMENTED can arrive at any time and mean
-- nothing to a caller; a GLOBAL_REQUEST wanting a reply gets a refusal, since
-- Hull implements none of them and silence would stall a server that waits.
-- `strict` is the strict-KEX rule: during a key exchange the transport
-- chatter this normally skips is not permitted at all. Skipping it is the
-- primitive Terrapin uses - an inserted IGNORE shifts what the two ends
-- think they agreed, and a client that silently drops it never notices.
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
        local p = self:handle_packet(self:read_packet(), strict)
        if p then return p end
    end
end

-- A reply the transport owes the peer (a refused channel or global request).
-- During a key exchange it waits for the new keys: once our KEXINIT is out,
-- only key-exchange messages may be sent (RFC 4253 section 7.1), and between
-- our NEWKEYS and the peer's the sequence number has already been reset for
-- strict KEX while the OLD keys are still installed - a reply sent then went
-- out under a key with a nonce it had already used.
function Transport:send_reply(payload)
    if self.in_kex then
        -- Its own queue: `deferred` holds INCOMING messages set aside
        -- during a rekey, and must survive the exchange.
        local q = self.held_replies or {}
        if #q >= 64 then
            error("ssh: too many requests from the peer during key exchange")
        end
        q[#q + 1] = payload
        self.held_replies = q
        return
    end
    self:send_packet(payload)
end

-- One packet off the wire, dealt with as read_message describes: returned
-- when it is for the caller, nil when it was chatter the transport handled.
function Transport:handle_packet(p, strict)
    do
        local m = p:byte(1)

        -- Strict KEX allows nothing but the exchange itself (KEXINIT,
        -- NEWKEYS, the method messages 30-49) until it completes - and a
        -- DISCONNECT, which is reported below. Only the chatter used to be
        -- refused here; a CHANNEL_OPEN or REQUEST_SUCCESS / FAILURE injected
        -- into the exchange was absorbed (and a refusal SENT mid-exchange).
        if strict and m ~= SSH_MSG_DISCONNECT
           and not (m == SSH_MSG_KEXINIT or m == kex.SSH_MSG_NEWKEYS
                    or (m >= 30 and m <= 49)) then
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
            local done, ok, why = pcall(self.run_kex, self, self.kex_opts or {}, p)
            self.in_kex = false
            if not done then
                -- The connection is finished: say so, rather than leave
                -- in_kex set and let a later rekey() report success.
                self.dead = { code = "kex_failed" }
                error(ok, 0)
            end
            if not ok then
                -- A reason (a changed host key, a bad signature) leaves the
                -- exchange half done: the keys are the old ones, the peer is
                -- mid-exchange. Nothing may be sent on it again.
                self.dead = { code = "kex_failed",
                              detail = (why and why.code) or "unknown" }
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
            if r:boolean() then self:send_reply(string.char(SSH_MSG_REQUEST_FAILURE)) end
        elseif m == channel.SSH_MSG_CHANNEL_OPEN then
            -- The server asking US to open a channel (forwarded connections,
            -- agent, X11). None is offered, so every one is refused - with a
            -- reply, because a server that asked is waiting for one.
            local r = wire.reader(p); r:byte(); r:string()
            local sender = r:uint32()
            self:send_reply(channel.build_open_failure(sender,
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
    return nil
end

-- Whether the next message can be had without waiting on the peer: one held
-- from a rekey, a whole packet already buffered, or bytes the stream has right
-- now that complete one.
--
-- For a sender that must keep reading while it writes (exec streaming stdin):
-- output the command produced meanwhile is taken in - and the receive window
-- topped up - between writes, so neither direction waits on the other. "Right
-- now" is a read allowed to wait 1 ms; a stream without a per-read bound
-- (a test double) is only checked for what is already buffered.
function Transport:message_ready()
    if self.deferred_head <= #self.deferred then return true end
    local function whole()
        local buf = self.inbuf
        if #buf < 4 then return false end
        -- The cipher sizes the frame: chacha20-poly1305's length is encrypted.
        return #buf >= (self.s2c and self.s2c:needed(buf, self.recv_seq)
                                 or wire.peek_uint32(buf) + 4)
    end
    if whole() then return true end
    if not self.stream.wait then return false end

    self.stream:wait(1)
    local chunk, err, code = self:stream_call("read", READ_CHUNK)
    self.stream:wait(self.wait_ms > 0 and self.wait_ms or 0)
    if chunk == nil then
        if code == "timeout" then return false end
        if code == "deadline" then fail("deadline", "deadline reached") end
        error("ssh: read failed: " .. tostring(err))
    end
    -- End of stream: say ready, so the read that follows reports it.
    if chunk == "" then return true end
    self.quiet_ms, self.unanswered = 0, 0
    self.inbuf = self.inbuf .. chunk
    return whole()
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

-- `opts` with the host key algorithms reordered so that the type the trust
-- store already holds for this host comes first. OpenSSH does the same, and
-- for the same reason: a server with several host keys presents the one the
-- client ranks highest, and a host we know by its RSA key that presented its
-- Ed25519 key instead would come back as a CHANGED host - a false alarm that
-- trains operators to accept changed keys. A store that cannot be read here
-- is left alone: the trust check proper reads it again and reports failure.
local function prefer_known_key(opts)
    local store = opts.trust
    if type(store) ~= "table" or type(store.get) ~= "function" then return opts end
    local ok, blob = pcall(store.get, hostkey.store_name(opts.host, opts.port))
    if not ok or type(blob) ~= "string" then return opts end
    local wanted = {}
    for _, a in ipairs(hostkey.algorithms_for(hostkey.key_type(blob))) do wanted[a] = true end

    local base = opts.offer or kexinit.DEFAULT_OFFER
    local first, rest = {}, {}
    for _, a in ipairs(base.host_key) do
        if wanted[a] then first[#first + 1] = a else rest[#rest + 1] = a end
    end
    if #first == 0 or first[1] == base.host_key[1] and #first == 1 then return opts end
    for _, a in ipairs(rest) do first[#first + 1] = a end

    local offer = {}
    for k, v in pairs(base) do offer[k] = v end
    offer.host_key = first
    local out = {}
    for k, v in pairs(opts) do out[k] = v end
    out.offer = offer
    return out
end

-- Returns true, or nil plus a structured reason. The host-key cases carry the
-- fingerprint, because the caller has to be able to show it.
function Transport:handshake(opts)
    -- identification
    local v_s
    for _ = 1, 32 do
        v_s = self:read_line()
        if v_s:match("^SSH%-") then break end
        if opts.on_banner then opts.on_banner(wire.safe_text(v_s)) end
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
    opts = prefer_known_key(opts)
    self.kex_opts = opts

    -- in_kex for the FIRST exchange too: send_reply holds what it owes the
    -- peer until the keys are in (it sent an OPEN_FAILURE mid-exchange), and
    -- every way out of a failed exchange leaves the connection dead, so
    -- nothing is ever sent on it after.
    self.in_kex = true
    local done, ok, why = pcall(self.run_kex, self, opts)
    self.in_kex = false
    if not done then
        self.dead = { code = "kex_failed" }
        error(ok, 0)
    end
    if not ok then
        self.dead = why or { code = "kex_failed" }
        return nil, why
    end
    return true
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
    -- Strict KEX gates which ciphers may be picked (chacha20-poly1305 only
    -- under it), so it is known before negotiating: from this KEXINIT on the
    -- first exchange, and as that exchange decided on every rekey.
    local strict = self.strict_kex
    if not rekey then strict = kexinit.server_is_strict(server) end
    local neg, nerr = kexinit.negotiate(opts.offer, server, strict)
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
    local q_c, sk = self.crypto.x25519_keypair()
    self:send_packet(kex.build_ecdh_init(q_c))

    local reply = kex.parse_ecdh_reply(
        self:expect(kex.SSH_MSG_KEX_ECDH_REPLY, "KEX_ECDH_REPLY",
                    self.strict_kex))

    local k_raw, kerr = self.crypto.x25519(sk, reply.q_s)
    if not k_raw then return nil, { code = "bad_kex_point", detail = kerr } end

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
        local ok, why = hostkey.verify_signature(self.crypto,
                                                 reply.host_key,
                                                 reply.signature, h,
                                                 neg.host_key)
        if not ok then
            return nil, { code = "host_key_invalid", detail = why }
        end
    else
        -- host key: verified, then trusted or not. This never decides for the
        -- caller; an unknown or changed host comes back as a reason carrying
        -- the fingerprint (see hull.ssh.hostkey).
        local d = hostkey.verify(self.crypto,
                                 opts.trust, hostkey.store_name(opts.host, opts.port),
                                 reply.host_key, reply.signature, h, neg.host_key)
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
                                 kex.sizes_for(neg.cipher_c2s, neg.cipher_s2c))

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
    self.c2s = self:new_cipher(neg.cipher_c2s, keys.key_c2s, keys.iv_c2s)
    self.s2c = self:new_cipher(neg.cipher_s2c, keys.key_s2c, keys.iv_s2c)
    if rekey then self.rekeys = self.rekeys + 1 end
    -- Replies held during the exchange (send_reply) go out under the new keys.
    local q = self.held_replies
    self.held_replies = nil
    if q then
        for _, payload in ipairs(q) do self:send_packet(payload) end
    end
    return true
end

-- One direction's packet layer, for the cipher negotiated for it.
function Transport:new_cipher(name, key, iv)
    if name == chacha.NAME then
        local c = self.crypto
        return chacha.new(key, {
            chacha20 = function(k, n, ctr, d) return c.chacha20(k, n, ctr, d) end,
            poly1305 = function(k, m) return c.poly1305(k, m) end,
            ct_eq    = function(a, b) return c.constant_time_eq(a, b) end,
        })
    end
    return cipher.new(key, iv)
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
    -- A raise here comes from the stream or from a failed authentication of
    -- the exchange; either way the connection is finished. It is marked dead
    -- and the error re-raised: in_kex left set made the next rekey() return
    -- true ("one is already running") on a connection that could not work.
    local done, ok, why = pcall(self.run_kex, self, self.kex_opts or {})
    self.in_kex = false
    if not done then
        self.dead = { code = "kex_failed" }
        error(ok, 0)
    end
    if not ok then
        -- Returned rather than raised (host_changed_midsession, a bad
        -- signature), but the exchange is just as unfinished: the peer is
        -- waiting for our NEWKEYS under keys we never installed. A later
        -- exec used to send CHANNEL_OPEN into that.
        self.dead = why or { code = "kex_failed" }
        return nil, why
    end
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
        auth_key         = self.auth_key,
        bytes_sent       = sent,
        bytes_received   = recv,
        packets_sent     = self.c2s and self.c2s:packets_sent() or 0,
        packets_received = self.s2c and self.s2c:packets_sent() or 0,
        rekey_due        = (self.c2s ~= nil and self.c2s:rekey_due(self.rekey_limit))
                           or (self.s2c ~= nil and self.s2c:rekey_due(self.rekey_limit)),
    }
end

-- Authentication ------------------------------------------------------------------

-- Offer one key, under each signature algorithm it has. Returns true, or nil
-- plus a failure; `next_key` says whether another key is worth offering.
local function offer_key(self, user, key, on_banner)
    -- An RSA key signs under SHA-512 first, then SHA-256 (RFC 8332): a
    -- server refusing the first may still take the second, and one that
    -- refuses both refuses the key. Hull does not read server-sig-algs
    -- (RFC 8308), so it asks rather than being told. SHA-1 ssh-rsa is never
    -- offered.
    local algorithms = key.algorithm == "ssh-rsa" and { "rsa-sha2-512", "rsa-sha2-256" }
                       or { key.algorithm or userauth.ALGORITHM }

    for i, alg in ipairs(algorithms) do
        local blob = userauth.signed_blob(self.session_id, user, key.blob, alg)
        -- RSA and ECDSA sign through crypto.sign under the algorithm's hash
        -- (the table host keys are verified with); Ed25519 has its own.
        local sig
        local a = hostkey.ALGORITHMS[alg]
        if a and a.jose then
            sig = self.crypto.sign(a.jose, key.pem, blob)
        else
            sig = self.crypto.ed25519_sign(blob, key.secret)
        end
        self:send_packet(userauth.build_request(user, key.blob,
                                                userauth.signature_blob(sig, alg), alg))

        local failure
        for _ = 1, 16 do
            local r = userauth.parse_response(self:next_message())
            if r.type == "banner" then
                if on_banner then on_banner(r.message) end
            elseif r.type == "success" then
                return true
            elseif r.type == "failure" then
                failure = r
                break
            end
        end
        if not failure then return nil, { code = "no_auth_response" }, false end
        -- Partial success is NOT authentication: the server wants another
        -- factor, and reporting it as success would skip that. It is also
        -- not a refusal of this signature, so neither another hash nor
        -- another key will help. Nor will either once publickey is no longer
        -- on offer at all.
        if failure.partial then
            return nil, { code = "partial_success", methods = failure.methods }, false
        end
        if not userauth.can_retry_publickey(failure) then
            return nil, { code = "auth_failed", methods = failure.methods }, false
        end
        if i == #algorithms then
            return nil, { code = "auth_failed", methods = failure.methods }, true
        end
    end
end

-- `keys` is one loaded key, or a list of them offered in order until one is
-- accepted - what ssh(1) does with several IdentityFile lines.
function Transport:authenticate(user, keys, on_banner)
    self:send_packet(userauth.build_service_request())
    local accepted = userauth.parse_service_accept(self:next_message())
    if accepted ~= userauth.SERVICE_USERAUTH then
        return nil, { code = "service_refused",
                      detail = wire.safe_name(accepted) }
    end

    if keys.blob then keys = { keys } end
    local err
    for n, key in ipairs(keys) do
        local ok, e, next_key = offer_key(self, user, key, on_banner)
        if ok then
            self.user = user
            self.auth_key = n                  -- which one; see Conn:stats()
            self.authenticated = true          -- keepalives may now be sent
            return true
        end
        err = e
        -- A server stops accepting attempts (OpenSSH's MaxAuthTries, 6 by
        -- default, counts each signature) by disconnecting, which surfaces
        -- here as a raise, not as a failure to move past.
        if not next_key then break end
    end
    if err and err.code == "auth_failed" and #keys > 1 then
        err.detail = "none of the " .. tostring(#keys) .. " keys was accepted"
    end
    return nil, err
end

-- SFTP --------------------------------------------------------------------------------

--- Open an SFTP session on its own channel. See hull.ssh.sftp_client.
function Transport:sftp(opts)
    return require('hull.ssh.sftp_client').open(self, opts)
end

function Transport:close()
    -- Swallowed deliberately, and this is the only place that does it
    -- without saying so: close() is the last act on a connection that is
    -- already finished, and a stream that has gone away cannot be closed any
    -- harder. Raising here would replace whatever the caller was actually
    -- dealing with.
    pcall(function() self.stream:close() end)
end

-- Channels: sessions, exec and the routing between channels live in
-- hull.ssh.session, installed onto this class so `t:exec` and the sftp
-- client's `t:open_session` read the same as before.
session.install(Transport)

return M

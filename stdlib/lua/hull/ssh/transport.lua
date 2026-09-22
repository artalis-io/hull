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
--   stream:readable() -> boolean                             (OPTIONAL)
--
-- readable() answers "would a read return without waiting". It is optional
-- because most of the protocol is strictly request/response, where the answer
-- is always eventually yes. The one place it matters is writing a command's
-- stdin while that command is writing output: without it the two directions
-- cannot be interleaved, and both sides eventually block on full buffers. A
-- stream that cannot answer gets a bounded stdin instead of a deadlock.
--
-- read() returning SHORT is normal, not an error: the binding hands back
-- whatever arrived. Under Hull the call parks the coroutine and resumes when
-- the loop has more, so this code reads as though it were blocking while
-- never blocking the loop it runs on.

-- How much stdin may be written to a command without ever reading, when the
-- stream cannot say whether a read would block. Measured against OpenSSH on
-- loopback, the two directions wedge at roughly 256 KiB of combined in-flight
-- bytes (four 64 KiB socket buffers), so this sits safely under that. Bulk
-- data belongs in sftp, which is one direction at a time and has no such
-- limit.
local HALF_DUPLEX_STDIN_MAX = 128 * 1024

local packet     = require('hull.ssh.packet')
local kexinit    = require('hull.ssh.kexinit')
local kex        = require('hull.ssh.kex')
local hostkey    = require('hull.ssh.hostkey')
local cipher     = require('hull.ssh.cipher')
local userauth   = require('hull.ssh.userauth')
local channel    = require('hull.ssh.channel')
local sftp       = require('hull.ssh.sftp')
local wire       = require('hull.ssh.wire')

local M = {}

-- Transport-layer messages that can arrive at any time and mean nothing to
-- the layer above (RFC 4253 section 11).
local SSH_MSG_DISCONNECT    = 1
local SSH_MSG_IGNORE        = 2
local SSH_MSG_UNIMPLEMENTED = 3
local SSH_MSG_DEBUG         = 4
local SSH_MSG_GLOBAL_REQUEST = 80
local SSH_MSG_REQUEST_FAILURE = 82

local Transport = {}
Transport.__index = Transport

--- @param stream  read/write/close, as above
--- @param crypto  sha256, x25519, x25519_keypair, ed25519_verify,
---                ed25519_sign, random, gcm_seal, gcm_open
function M.new(stream, crypto, opts)
    opts = opts or {}
    return setmetatable({
        stream = stream,
        crypto = crypto,
        ident = "SSH-2.0-" .. (opts.software or "Hull"),
        inbuf = "",
        next_channel = 0,
        aead = {
            seal = function(k, iv, aad, p) return crypto.gcm_seal(k, iv, aad, p) end,
            open = function(k, iv, aad, c, t) return crypto.gcm_open(k, iv, aad, c, t) end,
        },
        raw_sha = kex.raw_hash(crypto.sha256),
    }, Transport)
end

-- Reading -------------------------------------------------------------------

-- Pull at least `n` bytes into the buffer. A short read is the normal case.
function Transport:fill(n)
    if #self.inbuf >= n then return end
    -- Gather into a table and join once. The stream is free to return SHORT
    -- reads, and does; appending each one onto a growing string makes filling
    -- a packet quadratic in its size, which for a 32 KiB packet arriving in
    -- small pieces is hundreds of megabytes of copying for 32 KiB of data.
    local parts, have = { self.inbuf }, #self.inbuf
    while have < n do
        local chunk, err = self.stream:read(n - have)
        if chunk == nil then
            error("ssh: read failed: " .. tostring(err))
        end
        if chunk == "" then
            error("ssh: connection closed by peer")
        end
        parts[#parts + 1] = chunk
        have = have + #chunk
    end
    self.inbuf = table.concat(parts)
end

function Transport:take(n)
    self:fill(n)
    local s = self.inbuf:sub(1, n)
    self.inbuf = self.inbuf:sub(n + 1)
    return s
end

function Transport:send_raw(bytes)
    local ok, err = self.stream:write(bytes)
    if not ok then error("ssh: write failed: " .. tostring(err)) end
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
        local chunk, err = self.stream:read(256)
        if chunk == nil then error("ssh: read failed: " .. tostring(err)) end
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
end

function Transport:read_packet()
    if self.s2c then
        -- The length is plaintext, so read the header, then exactly the rest.
        self:fill(4)
        local n = string.unpack(">I4", self.inbuf)
        local total = cipher.frame_size(n)
        self:fill(total)
        local payload, used = self.s2c:open(self.aead, self.inbuf)
        self.inbuf = self.inbuf:sub(used + 1)
        return payload
    end
    self:fill(4)
    local n = string.unpack(">I4", self.inbuf)
    self:fill(n + 4)
    local payload, used = packet.parse(self.inbuf, 8)
    self.inbuf = self.inbuf:sub(used + 1)
    return payload
end

-- Read the next packet the layer above cares about.
--
-- DISCONNECT, IGNORE, DEBUG and UNIMPLEMENTED can arrive at any time and mean
-- nothing to a caller; a GLOBAL_REQUEST wanting a reply gets a refusal, since
-- Hull implements none of them and silence would stall a server that waits.
function Transport:next_message()
    for _ = 1, 256 do
        local p = self:read_packet()
        local m = p:byte(1)

        if m == SSH_MSG_DISCONNECT then
            local r = wire.reader(p); r:byte()
            local code = r:uint32()
            local desc = r:remaining() > 0 and r:string() or ""
            error("ssh: server disconnected (" .. tostring(code) .. "): "
                  .. userauth.sanitize_text(desc))
        elseif m == SSH_MSG_GLOBAL_REQUEST then
            local r = wire.reader(p); r:byte(); r:string()
            if r:boolean() then self:send_packet(string.char(SSH_MSG_REQUEST_FAILURE)) end
        elseif m ~= SSH_MSG_IGNORE and m ~= SSH_MSG_DEBUG
               and m ~= SSH_MSG_UNIMPLEMENTED then
            return p
        end
    end
    error("ssh: too many transport messages without progress")
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
        return nil, { code = "bad_identification", detail = v_s }
    end
    self:send_raw(self.ident .. "\r\n")
    self.server_ident = v_s

    -- algorithm negotiation
    local i_c = kexinit.build(opts.offer, self.crypto.random(16))
    self:send_packet(i_c)
    local i_s = self:next_message()
    local server = kexinit.parse(i_s)
    local neg, nerr = kexinit.negotiate(opts.offer, server)
    if not neg then return nil, { code = "no_common_algorithm", detail = nerr } end
    self.negotiated = neg

    if kexinit.guess_was_wrong(server, neg) then
        self:next_message()   -- discard the guessed packet (RFC 4253 7.1)
    end

    -- curve25519 exchange
    local q_c_hex, sk_hex = self.crypto.x25519_keypair()
    local q_c = kex.from_hex(q_c_hex)
    self:send_packet(kex.build_ecdh_init(q_c))

    local reply
    for _ = 1, 8 do
        local p = self:next_message()
        if p:byte(1) == kex.SSH_MSG_KEX_ECDH_REPLY then
            reply = kex.parse_ecdh_reply(p); break
        end
    end
    if not reply then return nil, { code = "no_kex_reply" } end

    local k_hex, kerr = self.crypto.x25519(sk_hex, kex.to_hex(reply.q_s))
    if not k_hex then return nil, { code = "bad_kex_point", detail = kerr } end
    local k_raw = kex.from_hex(k_hex)

    local h = self.raw_sha(kex.exchange_hash_input({
        v_c = self.ident, v_s = v_s, i_c = i_c, i_s = i_s,
        k_s = reply.host_key, q_c = q_c, q_s = reply.q_s, k = k_raw,
    }))

    -- host key: verified, then trusted or not. This never decides for the
    -- caller; an unknown or changed host comes back as a reason carrying the
    -- fingerprint (see hull.ssh.hostkey).
    local d = hostkey.verify(self.crypto, kex.to_hex, self.raw_sha,
                             opts.trust, opts.host, reply.host_key,
                             reply.signature, h)
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

    -- keys, and the switch to encrypted
    self.session_id = h
    local keys = kex.derive_keys(self.raw_sha, k_raw, h, h,
                                 kex.SIZES[neg.cipher_c2s])
    self:send_packet(string.char(kex.SSH_MSG_NEWKEYS))
    for _ = 1, 8 do
        if self:next_message():byte(1) == kex.SSH_MSG_NEWKEYS then break end
    end
    -- Only now, so the NEWKEYS exchange itself stays plaintext.
    self.c2s = cipher.new(keys.key_c2s, keys.iv_c2s)
    self.s2c = cipher.new(keys.key_s2c, keys.iv_s2c)

    return true
end

-- Authentication ------------------------------------------------------------------

function Transport:authenticate(user, key, on_banner)
    self:send_packet(userauth.build_service_request())
    local accepted = userauth.parse_service_accept(self:next_message())
    if accepted ~= userauth.SERVICE_USERAUTH then
        return nil, { code = "service_refused", detail = accepted }
    end

    local blob = userauth.signed_blob(self.session_id, user, key.blob)
    local sig  = self.crypto.ed25519_sign(blob, key.secret)
    self:send_packet(userauth.build_request(user, key.blob,
                                            userauth.signature_blob(sig)))

    for _ = 1, 16 do
        local r = userauth.parse_response(self:next_message())
        if r.type == "banner" then
            if on_banner then on_banner(r.message) end
        elseif r.type == "success" then
            self.user = user
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
-- error that buries the first.
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
end

-- Wait for the reply to a channel request.
--
-- A window adjust can arrive first: replies interleave, and assuming the next
-- message answers the last request is how a client desynchronises.
function Transport:await_channel_reply(ch)
    for _ = 1, 32 do
        local m = channel.parse(self:next_message())
        ch:handle(m)
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

    local ch, cerr = self:open_session()
    if not ch then return nil, cerr end

    -- Any exit that does not run to the peer's CHANNEL_CLOSE has to close AND
    -- drain, because the peer's messages for this channel are still in
    -- flight. Leaving them unread hands them to whatever reads next on this
    -- connection, which is the NEXT channel: it sees a message addressed to
    -- an id it does not own and raises. Returning early without draining is
    -- how one aborted command breaks every command after it.
    local function abort(reason)
        pcall(function()
            self:send_packet(channel.build_close(ch.remote_id))
            self:drain_channel(ch)
        end)
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
        if cb then cb(m.data) return end
        buffered = buffered + #m.data
        if buffered > limit then overflow = true return end
        into[#into + 1] = m.data
    end

    -- One message: account for it, hand off its payload, and top up the
    -- receive window so a streaming sender never stalls waiting on us.
    local function pump()
        local m = channel.parse(self:next_message())
        ch:handle(m)
        deliver(m)
        local adj = ch:window_adjustment()
        if adj then self:send_packet(adj) end
        return m
    end

    -- Consume whatever has ALREADY arrived, without waiting for more.
    --
    -- Writing a large stdin to a command that is writing a large stdout
    -- deadlocks if the two directions are never interleaved: both peers fill
    -- the operating system's buffers and then block, each waiting for the
    -- other to read. Measured against OpenSSH on loopback, the wall is around
    -- 256 KiB of combined in-flight bytes - four 64 KiB socket buffers.
    --
    -- Interleaving needs to know whether a read would BLOCK, which a plain
    -- read cannot tell us: a command like `find /c /v ""` produces nothing
    -- until its input is closed, so reading on a fixed schedule deadlocks
    -- the other way round. So this is capability-detected. A stream that can
    -- answer `readable()` gets true duplex; one that cannot is bounded below
    -- instead (see the guard after the write loop).
    local function drain_ready()
        local probe = self.stream.readable
        if not probe then return 0 end
        local n = 0
        while probe(self.stream) do
            local m = pump()
            n = n + 1
            if overflow or m.type == "close" then break end
        end
        return n
    end

    local function drive()
        if stdin then
            local sent = 1
            local unread = 0
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
                unread = unread + #chunk

                if drain_ready() > 0 then
                    if overflow then return { code = "output_too_large",
                                              limit = limit } end
                    unread = 0
                elseif not self.stream.readable
                       and unread > HALF_DUPLEX_STDIN_MAX then
                    -- Past this the next write can wedge against output we
                    -- are not reading, and the caller would see a stalled
                    -- connection with no explanation. Say so instead.
                    return { code = "stdin_too_large",
                             limit = HALF_DUPLEX_STDIN_MAX,
                             detail = "this stream cannot interleave reads "
                                      .. "with writes; use sftp for bulk data" }
                end
            end
            if not ch.closed then
                self:send_packet(channel.build_eof(ch.remote_id))
            end
        end

        for _ = 1, 1000000 do
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

    local ok, failure = pcall(drive)
    if not ok then
        -- A raising callback must not leave the channel half-open, nor its
        -- residue in the read path, just because the error came from above.
        pcall(function()
            self:send_packet(channel.build_close(ch.remote_id))
            self:drain_channel(ch)
        end)
        error(failure, 0)
    end
    if failure then return abort(failure) end
    self:send_packet(channel.build_close(ch.remote_id))

    local res = ch:result()
    return { status = res.status, signal = res.signal,
             stdout = table.concat(out), stderr = table.concat(errout) }
end

-- SFTP --------------------------------------------------------------------------------

local Sftp = {}
Sftp.__index = Sftp

--- Open an SFTP session on its own channel.
---
--- Paths travel as length-prefixed strings inside the subsystem, never as
--- words in a command line, which is the whole reason file transfer here
--- needs no shell quoting.
function Transport:sftp()
    local ch, cerr = self:open_session()
    if not ch then return nil, cerr end

    self:send_packet(channel.build_subsystem(ch.remote_id, "sftp"))
    if not self:await_channel_reply(ch) then
        return nil, { code = "sftp_unavailable" }
    end

    local s = setmetatable({ t = self, ch = ch, buf = "", id = 0 }, Sftp)
    s:send(sftp.build_init())
    local ver = s:recv()
    if ver.type ~= "version" then
        return nil, { code = "sftp_no_version" }
    end
    s.version = ver.version
    return s
end

function Sftp:send(payload)
    self.t:send_packet(self.ch:data_message(sftp.frame(payload)))
end

function Sftp:next_id()
    self.id = self.id + 1
    return self.id
end

-- Pull one SFTP message, feeding the channel as data arrives.
function Sftp:recv()
    for _ = 1, 100000 do
        local p, used = sftp.parse_frame(self.buf)
        if p then
            self.buf = self.buf:sub(used + 1)
            return sftp.parse(p)
        end
        local m = channel.parse(self.t:next_message())
        self.ch:handle(m)
        if m.type == "data" then
            self.buf = self.buf .. m.data
        elseif m.type == "close" then
            error("ssh.sftp: the channel closed mid-request")
        end
        local adj = self.ch:window_adjustment()
        if adj then self.t:send_packet(adj) end
    end
    error("ssh.sftp: no response")
end

--- Resolve a path on the server. Returns the canonical path.
function Sftp:realpath(path)
    self:send(sftp.build_realpath(self:next_id(), path))
    local r = self:recv()
    if r.type == "status" then return nil, r.text end
    return r.names[1] and r.names[1].filename
end

--- List a directory. Returns the entries SAFE to use as local names, plus the
--- ones refused and why - refused rather than dropped, because a name
--- rejected for traversal is something an operator should hear about.
function Sftp:list(path)
    self:send(sftp.build_opendir(self:next_id(), path))
    local h = self:recv()
    if h.type ~= "handle" then return nil, h.text or h.type end

    local all = {}
    for _ = 1, 4096 do
        self:send(sftp.build_readdir(self:next_id(), h.handle))
        local r = self:recv()
        if r.type == "status" then
            -- EOF ends the listing; anything else is a real failure.
            if not r.eof then
                self:send(sftp.build_close(self:next_id(), h.handle))
                return nil, r.text
            end
            break
        end
        for _, e in ipairs(r.names) do all[#all + 1] = e end
    end
    self:send(sftp.build_close(self:next_id(), h.handle))
    self:recv()
    return sftp.safe_names(all)
end

--- Read a whole file. `max` bounds it, because the server chooses the size.
function Sftp:read(path, max)
    max = max or (16 * 1024 * 1024)
    self:send(sftp.build_open(self:next_id(), path, sftp.FXF_READ))
    local h = self:recv()
    if h.type ~= "handle" then return nil, h.text or h.type end

    local parts, off, total = {}, 0, 0
    for _ = 1, 100000 do
        self:send(sftp.build_read(self:next_id(), h.handle, off, 32768))
        local r = self:recv()
        if r.type == "status" then
            if r.eof then break end
            self:send(sftp.build_close(self:next_id(), h.handle))
            return nil, r.text
        end
        total = total + #r.data
        if total > max then
            self:send(sftp.build_close(self:next_id(), h.handle))
            return nil, "file exceeds the " .. tostring(max) .. " byte limit"
        end
        parts[#parts + 1] = r.data
        off = off + #r.data
    end
    self:send(sftp.build_close(self:next_id(), h.handle))
    self:recv()
    return table.concat(parts)
end

--- Write a whole file, creating or truncating it.
function Sftp:write(path, data)
    self:send(sftp.build_open(self:next_id(), path,
        sftp.FXF_WRITE | sftp.FXF_CREAT | sftp.FXF_TRUNC))
    local h = self:recv()
    if h.type ~= "handle" then return nil, h.text or h.type end

    local off = 0
    while off < #data do
        -- Chunked to stay under the channel packet cap; the channel refuses
        -- an oversized message rather than truncating it.
        local chunk = data:sub(off + 1, off + 16384)
        self:send(sftp.build_write(self:next_id(), h.handle, off, chunk))
        local r = self:recv()
        if r.type ~= "status" or not r.ok then
            self:send(sftp.build_close(self:next_id(), h.handle))
            return nil, r.text or r.type
        end
        off = off + #chunk
    end
    self:send(sftp.build_close(self:next_id(), h.handle))
    local st = self:recv()
    if st.type == "status" and not st.ok then return nil, st.text end
    return true
end

function Sftp:close()
    self.t:send_packet(channel.build_close(self.ch.remote_id))
end

function Transport:close()
    pcall(function() self.stream:close() end)
end

return M

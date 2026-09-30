-- hull.ssh.session - channels on an established connection: open a session,
-- run a command on it, route what arrives to the channel it belongs to, and
-- close channels so the next operation starts clean.
--
-- Split from hull.ssh.transport, which keeps the connection itself: the
-- handshake, key exchange and rekey, packets, liveness. Everything here works
-- in messages (`send_packet`, `next_message`) and never sees a byte of the
-- stream or a key.
--
-- These are methods of the Transport class, installed onto it by
-- `install(Transport)`, so callers keep writing `t:exec(...)` and the sftp
-- client keeps calling `t:open_session()`. A separate object would have to
-- share the transport's channel table and dead-connection state anyway.

local channel = require('hull.ssh.channel')

local M = {}

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

local Session = {}

function Session:open_session()
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
    -- Registered before the open goes out, so what arrives for the channels
    -- already open while we wait reaches them (see read_for), instead of
    -- being skipped - which dropped an sftp session's data on the floor.
    self.channels[ch.local_id] = ch
    self:send_packet(ch:open_message())

    for _ = 1, 16 do
        local m = self:read_for(ch)
        if m.type == "open_confirmation" then
            ch:handle(m); return ch
        elseif m.type == "open_failure" then
            ch:handle(m)
            self.channels[ch.local_id] = nil
            return nil, { code = "channel_refused", detail = m.description }
        end
    end
    self.channels[ch.local_id] = nil
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
function Session:drain_channel(ch)
    if ch.closed then return end
    for _ = 1, 10000 do
        local ok, m = pcall(self.read_for, self, ch)
        if not ok then return end
        if m.recipient == ch.local_id then
            if not m.routed then pcall(function() ch:handle(m) end) end
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
function Session:close_channel(ch)
    if ch.close_sent then return end
    ch.close_sent = true
    pcall(function()
        self:send_packet(channel.build_close(ch.remote_id))
        self:drain_channel(ch)
    end)
    self.channels[ch.local_id] = nil
end

-- The next connection message, applied to channel `ch`.
--
-- A channel request the server wants answered gets CHANNEL_FAILURE: this
-- client acts on exit-status and exit-signal (which never ask for a reply)
-- and nothing else. Staying silent is not neutral - OpenSSH's
-- ClientAliveInterval sends keepalive@openssh.com with want_reply set on an
-- open session channel, and disconnects a client that never answers, which
-- killed long-running exec and sftp sessions.
function Session:channel_message(ch)
    local m = self:read_for(ch)
    if not m.routed then
        ch:handle(m)
        if m.type == "request" and m.want_reply and ch.remote_id then
            self:send_packet(channel.build_failure(ch.remote_id))
        end
    end
    return m
end

-- The next channel message for `ch`, with the others on the connection kept
-- where they belong.
--
-- Every channel shares one stream, so whoever reads next reads for all of
-- them. A message for another OPEN channel (an sftp session sitting idle
-- while a command runs, say) is applied to that channel at once - a window
-- adjust, an EOF, a close - and a request that wants a reply is refused at
-- once, since the server may be waiting on it (OpenSSH's keepalive). One its
-- owner needs to see is queued for it, and handed over the next time that
-- channel is read. The queue cannot outgrow the receive window we granted:
-- the window only reopens as the owner consumes. A message for a channel
-- that is not open is returned as it is, and fails where it lands - the
-- mix-up Channel:handle reports.
--
-- A queued message comes back with `routed` set: it has already been
-- applied, and must not be applied twice.
function Session:read_for(ch)
    local q = ch.inbox
    if ch.inbox_head <= #q then
        local m = q[ch.inbox_head]
        q[ch.inbox_head] = nil
        ch.inbox_head = ch.inbox_head + 1
        if ch.inbox_head > #q then ch.inbox, ch.inbox_head = {}, 1 end
        return m
    end
    while true do
        local m = self:route(ch, channel.parse(self:next_message()))
        if m then return m end
    end
end

-- `m` if it is for `ch`; otherwise applied to (and queued for) the open
-- channel it belongs to, and nil. See read_for.
function Session:route(ch, m)
    local other = m.recipient ~= ch.local_id and self.channels[m.recipient]
    if not other then return m end
    other:handle(m)
    if m.type == "request" and m.want_reply and other.remote_id then
        self:send_packet(channel.build_failure(other.remote_id))
    end
    if m.type ~= "window_adjust" then
        m.routed = true
        other.inbox[#other.inbox + 1] = m
    end
    return nil
end

-- The next channel message for `ch` IF one can be had without waiting on the
-- peer (Transport:message_ready), else nil. Applied as channel_message would.
function Session:poll_channel(ch)
    while self:message_ready() do
        local p = self:take_deferred()
        if not p then p = self:handle_packet(self:read_packet()) end
        if p then
            local m = self:route(ch, channel.parse(p))
            if m then
                ch:handle(m)
                if m.type == "request" and m.want_reply and ch.remote_id then
                    self:send_packet(channel.build_failure(ch.remote_id))
                end
                return m
            end
        end
    end
    return nil
end

-- Wait for the reply to a channel request.
--
-- A window adjust can arrive first: replies interleave, and assuming the next
-- message answers the last request is how a client desynchronises.
function Session:await_channel_reply(ch)
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
-- opts.stdin is written to the command, then EOF is sent. That is the path
-- for feeding a command data without a shell redirect, the same way sftp
-- writes a file without shell quoting. A string is capped (STDIN_MAX, above).
-- A function is a SOURCE, called for the next chunk until it returns nil or
-- "", with no cap: between writes, output the command has already produced
-- is taken in (poll_channel) so the two directions do not wait on each
-- other. Against the server that measurement was made on it may still wedge
-- - see STDIN_MAX - and timeout_ms is what bounds that.
function Session:exec(command, opts)
    opts = opts or {}
    local on_stdout, on_stderr = opts.on_stdout, opts.on_stderr

    local stdin = opts.stdin
    if stdin ~= nil and type(stdin) ~= "string" and type(stdin) ~= "function" then
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
    -- The peer's close, once seen by ANY read here - the stdin writer's reads
    -- included. Waiting for it again after one of those took it in would
    -- wait for a message that has already come.
    local peer_closed = false

    local function pump()
        local m = self:channel_message(ch)
        if m.type == "close" then peer_closed = true end
        deliver(m)
        local adj = ch:window_adjustment()
        if adj then self:send_packet(adj) end
        return m
    end

    -- Take in whatever the command has already sent, without waiting for
    -- more. Returns the peer's close, if that is what arrived.
    local function drain()
        -- Nothing follows the peer's close on this channel: reading on would
        -- wait on (or report the end of) a stream that has nothing for it.
        while not peer_closed do
            local m = self:poll_channel(ch)
            if not m then return nil end
            if m.type == "close" then peer_closed = true end
            deliver(m)
            local adj = ch:window_adjustment()
            if adj then self:send_packet(adj) end
            if overflow then return { code = "output_too_large", limit = limit } end
        end
        return nil
    end

    -- Write `data` within the peer's window, taking in output between
    -- packets. Returns a failure, or nil.
    local function send_all(data)
        local sent = 1
        while sent <= #data and not ch.closed do
            local f = drain()
            if f then return f end
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
            local chunk = data:sub(sent, sent + room - 1)
            self:send_packet(ch:data_message(chunk))
            sent = sent + #chunk
        end
        return nil
    end

    local function drive()
        if type(stdin) == "string" then
            -- Refuse up front rather than part way through: a caller whose
            -- input is too large should learn that before half of it is on
            -- the wire and the command has started acting on it.
            if #stdin > STDIN_MAX then
                return { code = "stdin_too_large", limit = STDIN_MAX,
                         detail = "use sftp for bulk data, or pass a function to stream it" }
            end
            local f = send_all(stdin)
            if f then return f end
        elseif stdin then
            while not ch.closed do
                -- The caller's source: its error stays the caller's, as the
                -- output callbacks' do.
                local s_ok, chunk = pcall(stdin)
                if not s_ok then error({ app_error = chunk }, 0) end
                if chunk == nil or chunk == "" then break end
                if type(chunk) ~= "string" then
                    return { code = "bad_stdin",
                             detail = "the stdin source returned a " .. type(chunk) }
                end
                local f = send_all(chunk)
                if f then return f end
            end
        end
        if stdin and not ch.closed then
            self:send_packet(channel.build_eof(ch.remote_id))
        end

        -- Until the peer closes the channel, however long a command streams:
        -- every pass needs a message from the peer, and opts.timeout_ms bounds
        -- the time. (A count here ended a long `journalctl -f` as if it had
        -- finished.)
        -- RFC 4254 section 5.3: a side that receives CHANNEL_CLOSE must send
        -- one back unless it already has; close_channel does, after this.
        -- Stopping without replying leaves the channel half-open on the
        -- server for the life of the connection, which matters once a tool
        -- runs many commands.
        while not peer_closed do
            pump()
            if overflow then
                return { code = "output_too_large", limit = limit }
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

-- Copy the session methods onto the Transport class.
function M.install(Transport)
    for name, fn in pairs(Session) do
        Transport[name] = fn
    end
end

return M

-- hull.ssh.channel - the connection protocol: channels, exec, flow control.
--
-- RFC 4254. Everything before this slice was a pure transform; a channel has
-- state, and the state that matters is the two windows.
--
-- Flow control is not a performance feature here, it is the bound on how much
-- an unauthenticated-to-us peer can make Hull hold. A channel advertises how
-- many bytes it is willing to receive, and a peer that sends more than that
-- has broken the protocol. Accepting the excess anyway - the tempting,
-- forgiving thing - turns a declared bound into no bound at all, so it is a
-- hard error here.
--
-- Still no I/O: this builds and interprets messages and tracks the counters.
-- The transport moves the bytes.

local wire = require('hull.ssh.wire')

local M = {}

M.SSH_MSG_CHANNEL_OPEN              = 90
M.SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91
M.SSH_MSG_CHANNEL_OPEN_FAILURE      = 92
M.SSH_MSG_CHANNEL_WINDOW_ADJUST     = 93
M.SSH_MSG_CHANNEL_DATA              = 94
M.SSH_MSG_CHANNEL_EXTENDED_DATA     = 95
M.SSH_MSG_CHANNEL_EOF               = 96
M.SSH_MSG_CHANNEL_CLOSE             = 97
M.SSH_MSG_CHANNEL_REQUEST           = 98
M.SSH_MSG_CHANNEL_SUCCESS           = 99
M.SSH_MSG_CHANNEL_FAILURE           = 100

-- RFC 4254 section 5.2: the only extended data type defined is stderr.
M.EXTENDED_DATA_STDERR = 1

-- Defaults matching what OpenSSH advertises. The window is large enough that
-- a normal command never stalls on it, and the packet cap keeps any single
-- message bounded regardless.
M.DEFAULT_WINDOW     = 2 * 1024 * 1024
M.DEFAULT_MAX_PACKET = 32 * 1024

-- Builders ---------------------------------------------------------------

function M.build_open(sender_id, window, max_packet)
    return wire.writer()
        :byte(M.SSH_MSG_CHANNEL_OPEN)
        :string("session")
        :uint32(sender_id)
        :uint32(window or M.DEFAULT_WINDOW)
        :uint32(max_packet or M.DEFAULT_MAX_PACKET)
        :build()
end

function M.build_exec(recipient_id, command, want_reply)
    if type(command) ~= "string" then
        error("ssh.channel: exec needs a command string", 2)
    end
    -- The command is a STRING, not a shell word list, and it is sent as one
    -- length-prefixed blob. No quoting happens anywhere in this path, which
    -- is the whole reason file transfer can avoid shell escaping.
    return wire.writer()
        :byte(M.SSH_MSG_CHANNEL_REQUEST)
        :uint32(recipient_id)
        :string("exec")
        :boolean(want_reply ~= false)
        :string(command)
        :build()
end

function M.build_subsystem(recipient_id, name, want_reply)
    return wire.writer()
        :byte(M.SSH_MSG_CHANNEL_REQUEST)
        :uint32(recipient_id)
        :string("subsystem")
        :boolean(want_reply ~= false)
        :string(name)
        :build()
end

function M.build_data(recipient_id, data)
    return wire.writer()
        :byte(M.SSH_MSG_CHANNEL_DATA)
        :uint32(recipient_id)
        :string(data)
        :build()
end

function M.build_eof(recipient_id)
    return wire.writer():byte(M.SSH_MSG_CHANNEL_EOF):uint32(recipient_id):build()
end

function M.build_close(recipient_id)
    return wire.writer():byte(M.SSH_MSG_CHANNEL_CLOSE):uint32(recipient_id):build()
end

function M.build_window_adjust(recipient_id, add)
    return wire.writer()
        :byte(M.SSH_MSG_CHANNEL_WINDOW_ADJUST)
        :uint32(recipient_id)
        :uint32(add)
        :build()
end

-- Parsing -----------------------------------------------------------------

-- Parse one connection-protocol message into a table with a `type`.
-- Raises on anything malformed; the caller pcalls at the packet boundary.
function M.parse(payload)
    local r = wire.reader(payload)
    local msg = r:byte()

    if msg == M.SSH_MSG_CHANNEL_OPEN_CONFIRMATION then
        return { type = "open_confirmation",
                 recipient = r:uint32(), sender = r:uint32(),
                 window = r:uint32(), max_packet = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_OPEN_FAILURE then
        return { type = "open_failure", recipient = r:uint32(),
                 reason = r:uint32(),
                 -- Server text that reaches a caller error report.
                 description = wire.safe_name(r:string(), 200),
                 language = r:string() }
    end

    if msg == M.SSH_MSG_CHANNEL_WINDOW_ADJUST then
        return { type = "window_adjust", recipient = r:uint32(), add = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_DATA then
        return { type = "data", recipient = r:uint32(), data = r:string() }
    end

    if msg == M.SSH_MSG_CHANNEL_EXTENDED_DATA then
        local recipient = r:uint32()
        local code = r:uint32()
        return { type = "extended_data", recipient = recipient,
                 code = code, data = r:string(),
                 stderr = code == M.EXTENDED_DATA_STDERR }
    end

    if msg == M.SSH_MSG_CHANNEL_EOF then
        return { type = "eof", recipient = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_CLOSE then
        return { type = "close", recipient = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_SUCCESS then
        return { type = "request_success", recipient = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_FAILURE then
        return { type = "request_failure", recipient = r:uint32() }
    end

    if msg == M.SSH_MSG_CHANNEL_REQUEST then
        local recipient = r:uint32()
        local what = r:string()
        local want_reply = r:boolean()
        local out = { type = "request", recipient = recipient,
                      request = what, want_reply = want_reply }
        if what == "exit-status" then
            out.exit_status = r:uint32()
        elseif what == "exit-signal" then
            out.signal = r:string()
            out.core_dumped = r:boolean()
            out.error_message = r:string()
            out.language = r:string()
        end
        -- Any other request type is returned unparsed rather than refused: a
        -- server may send things we do not act on, and hanging up on one
        -- would be worse than ignoring it.
        return out
    end

    error("ssh.channel: unexpected connection message " .. tostring(msg))
end

-- Channel state -------------------------------------------------------------

local Channel = {}
Channel.__index = Channel

function M.new(opts)
    opts = opts or {}
    local c = setmetatable({}, Channel)
    c.local_id    = opts.id or 0
    c.remote_id   = nil
    c.recv_window = opts.window or M.DEFAULT_WINDOW
    c.recv_initial = c.recv_window
    c.send_window = 0                 -- until the peer tells us
    c.max_packet  = opts.max_packet or M.DEFAULT_MAX_PACKET
    c.send_max_packet = nil
    c.open        = false
    c.eof_received = false
    c.closed      = false
    c.exit_status = nil
    c.exit_signal = nil
    return c
end

function Channel:open_message()
    return M.build_open(self.local_id, self.recv_window, self.max_packet)
end

-- Every inbound message for this channel goes through here.
--
-- Returns the parsed event. Raises on a protocol violation, which covers the
-- cases a peer controls: a message for a channel we do not have, data beyond
-- the window we advertised, or a packet larger than the cap we set.
function Channel:handle(msg)
    if msg.recipient ~= nil and msg.recipient ~= self.local_id then
        -- Messages carry OUR id as the recipient. A different one is either a
        -- channel we never opened or a mix-up we must not paper over.
        error("ssh.channel: message for channel " .. tostring(msg.recipient)
              .. ", expected " .. tostring(self.local_id))
    end

    if msg.type == "open_confirmation" then
        self.remote_id       = msg.sender
        self.send_window     = msg.window
        self.send_max_packet = msg.max_packet
        self.open            = true
        return msg
    end

    if msg.type == "open_failure" then
        self.closed = true
        return msg
    end

    if msg.type == "window_adjust" then
        -- The window is a uint32 on the wire; adding without a bound would
        -- let a peer roll it over and make can_send lie.
        local grown = self.send_window + msg.add
        if grown > 0xFFFFFFFF then
            error("ssh.channel: window adjust overflows the window")
        end
        self.send_window = grown
        return msg
    end

    if msg.type == "data" or msg.type == "extended_data" then
        local n = #msg.data
        if n > self.max_packet then
            error("ssh.channel: peer sent " .. tostring(n)
                  .. " bytes, over the " .. tostring(self.max_packet)
                  .. " byte packet cap")
        end
        if n > self.recv_window then
            -- The advertised window is a promise about how much we will hold.
            -- Accepting more would make it meaningless.
            error("ssh.channel: peer sent " .. tostring(n)
                  .. " bytes, over the remaining window of "
                  .. tostring(self.recv_window))
        end
        self.recv_window = self.recv_window - n
        return msg
    end

    if msg.type == "eof" then
        self.eof_received = true
        return msg
    end

    if msg.type == "close" then
        self.closed = true
        return msg
    end

    if msg.type == "request" then
        if msg.request == "exit-status" then
            self.exit_status = msg.exit_status
        elseif msg.request == "exit-signal" then
            -- A command killed by a signal has no exit status. Reporting one
            -- anyway (0, or 128+n) would make "did it succeed" unanswerable.
            self.exit_signal = {
                signal = msg.signal,
                core_dumped = msg.core_dumped,
                message = msg.error_message,
            }
        end
        return msg
    end

    return msg
end

-- Whether n bytes fit in what the peer has granted, and under its packet cap.
function Channel:can_send(n)
    if not self.open or self.closed then return false end
    if self.send_max_packet and n > self.send_max_packet then return false end
    return n <= self.send_window
end

-- The largest chunk that can be sent right now: the smaller of the remaining
-- window and the peer packet cap. Zero means wait for a window adjust.
function Channel:sendable()
    if not self.open or self.closed then return 0 end
    local n = self.send_window
    if self.send_max_packet and self.send_max_packet < n then
        n = self.send_max_packet
    end
    return n
end

-- Build a data message and account for it. Splitting to fit is the caller
-- job; this refuses rather than silently truncating, because a short write
-- that looks like a full one corrupts whatever is being transferred.
function Channel:data_message(data)
    if not self:can_send(#data) then
        error("ssh.channel: " .. tostring(#data)
              .. " bytes does not fit the current window/packet limit")
    end
    self.send_window = self.send_window - #data
    return M.build_data(self.remote_id, data)
end

-- Top up the receive window when it has dropped far enough to be worth a
-- round trip. Returns a message, or nil when no adjustment is due.
--
-- Half the initial window is the usual threshold: often enough that a sender
-- streaming at full rate never stalls, rare enough that a small command does
-- not generate adjust traffic of its own.
function Channel:window_adjustment()
    if not self.open or self.closed then return nil end
    if self.recv_window > self.recv_initial // 2 then return nil end
    local add = self.recv_initial - self.recv_window
    if add <= 0 then return nil end
    self.recv_window = self.recv_window + add
    return M.build_window_adjust(self.remote_id, add)
end

-- The result of a finished command.
--
-- `status` is nil when the command was killed by a signal; the caller has to
-- look at `signal` for that case rather than being handed a number that
-- implies a clean exit.
function Channel:result()
    return { status = self.exit_status, signal = self.exit_signal,
             eof = self.eof_received, closed = self.closed }
end

return M

-- test_ssh_channel.lua - Tests for hull.ssh.channel
--
-- RFC 4254. The window cases are the point: the advertised window is a
-- promise about how much Hull will hold for a peer, and a peer that exceeds
-- it has broken the protocol. Accepting the excess anyway - the forgiving
-- thing - would turn a declared bound into no bound at all.

local channel = require('hull.ssh.channel')
local wire = require('hull.ssh.wire')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

local function assert_raises(fn, msg)
    local ok = pcall(fn)
    if ok then error((msg or "should have raised") .. " but did not") end
end

-- a channel already opened against a peer
local function opened(opts)
    local c = channel.new(opts)
    c:handle(channel.parse(wire.writer()
        :byte(91):uint32(c.local_id):uint32(7)
        :uint32((opts and opts.peer_window) or 1000)
        :uint32((opts and opts.peer_max) or 100)
        :build()))
    return c
end

-- builders -----------------------------------------------------------------

test("open advertises our window and packet cap", function()
    local r = wire.reader(channel.build_open(3, 4096, 512))
    assert_eq(r:byte(), channel.SSH_MSG_CHANNEL_OPEN)
    assert_eq(r:string(), "session")
    assert_eq(r:uint32(), 3)
    assert_eq(r:uint32(), 4096)
    assert_eq(r:uint32(), 512)
end)

test("exec sends the command as one opaque string", function()
    -- No quoting happens anywhere in this path. That is what lets file
    -- transfer avoid shell escaping entirely.
    local nasty = [[echo 'a b'; rm -rf / & $(whoami) `id`]]
    local r = wire.reader(channel.build_exec(7, nasty))
    assert_eq(r:byte(), channel.SSH_MSG_CHANNEL_REQUEST)
    assert_eq(r:uint32(), 7)
    assert_eq(r:string(), "exec")
    assert_eq(r:boolean(), true)
    assert_eq(r:string(), nasty, "the command survives byte for byte")
end)

test("exec refuses a non-string command", function()
    assert_raises(function() channel.build_exec(7, nil) end, "nil command")
    assert_raises(function() channel.build_exec(7, { "ls" }) end, "table command")
end)

test("subsystem request is built for sftp", function()
    local r = wire.reader(channel.build_subsystem(7, "sftp"))
    r:byte(); r:uint32()
    assert_eq(r:string(), "subsystem")
    r:boolean()
    assert_eq(r:string(), "sftp")
end)

test("data, eof, close and adjust all name the recipient", function()
    for _, b in ipairs({
        channel.build_data(7, "x"), channel.build_eof(7),
        channel.build_close(7), channel.build_window_adjust(7, 10),
    }) do
        local r = wire.reader(b)
        r:byte()
        assert_eq(r:uint32(), 7)
    end
end)

-- parsing --------------------------------------------------------------------

test("open confirmation carries both ids and the peer limits", function()
    local p = wire.writer():byte(91):uint32(0):uint32(7):uint32(2048):uint32(256):build()
    local m = channel.parse(p)
    assert_eq(m.type, "open_confirmation")
    assert_eq(m.recipient, 0)
    assert_eq(m.sender, 7)
    assert_eq(m.window, 2048)
    assert_eq(m.max_packet, 256)
end)

test("open failure carries a reason a human can read", function()
    local p = wire.writer():byte(92):uint32(0):uint32(4)
        :string("administratively prohibited"):string("en"):build()
    local m = channel.parse(p)
    assert_eq(m.type, "open_failure")
    assert_eq(m.reason, 4)
    assert_eq(m.description, "administratively prohibited")
end)

test("stdout and stderr are distinguishable", function()
    -- Separate streams are a requirement, not a nicety: merging them makes
    -- command output unparseable.
    local out = channel.parse(wire.writer():byte(94):uint32(0):string("to stdout"):build())
    assert_eq(out.type, "data")
    assert_eq(out.data, "to stdout")

    local err = channel.parse(wire.writer():byte(95):uint32(0):uint32(1)
        :string("to stderr"):build())
    assert_eq(err.type, "extended_data")
    assert_eq(err.stderr, true)
    assert_eq(err.data, "to stderr")
end)

test("an unknown extended data code is not treated as stderr", function()
    local m = channel.parse(wire.writer():byte(95):uint32(0):uint32(99)
        :string("?"):build())
    assert_eq(m.stderr, false)
    assert_eq(m.code, 99)
end)

test("exit-status is parsed", function()
    local p = wire.writer():byte(98):uint32(0):string("exit-status")
        :boolean(false):uint32(42):build()
    local m = channel.parse(p)
    assert_eq(m.request, "exit-status")
    assert_eq(m.exit_status, 42)
end)

test("exit-signal is parsed", function()
    local p = wire.writer():byte(98):uint32(0):string("exit-signal")
        :boolean(false):string("TERM"):boolean(false)
        :string("terminated"):string("en"):build()
    local m = channel.parse(p)
    assert_eq(m.signal, "TERM")
    assert_eq(m.core_dumped, false)
end)

test("an unknown channel request is returned, not refused", function()
    -- A server may send requests we do not act on; hanging up would be worse
    -- than ignoring them.
    local p = wire.writer():byte(98):uint32(0):string("keepalive@openssh.com")
        :boolean(true):build()
    local m = channel.parse(p)
    assert_eq(m.request, "keepalive@openssh.com")
end)

test("an unexpected message number raises", function()
    assert_raises(function()
        channel.parse(wire.writer():byte(200):uint32(0):build())
    end, "message 200")
end)

-- channel identity --------------------------------------------------------------

test("a message for another channel is refused", function()
    -- Inbound messages carry OUR id. A different one is a channel we never
    -- opened, or a mix-up that must not be papered over.
    local c = opened({ id = 3 })
    assert_raises(function()
        c:handle({ type = "data", recipient = 9, data = "x" })
    end, "wrong channel")
end)

test("open confirmation records the peer id and limits", function()
    local c = opened({ id = 3, peer_window = 500, peer_max = 50 })
    assert_eq(c.open, true)
    assert_eq(c.remote_id, 7)
    assert_eq(c.send_window, 500)
    assert_eq(c.send_max_packet, 50)
end)

test("open failure leaves the channel closed", function()
    local c = channel.new({ id = 1 })
    c:handle({ type = "open_failure", recipient = 1, reason = 1,
               description = "no", language = "" })
    assert_eq(c.open, false)
    assert_eq(c.closed, true)
end)

-- receive window ------------------------------------------------------------------

test("received data draws down the advertised window", function()
    local c = opened({ id = 0, window = 100, max_packet = 100 })
    c:handle({ type = "data", recipient = 0, data = string.rep("x", 30) })
    assert_eq(c.recv_window, 70)
end)

test("data beyond the advertised window is refused", function()
    -- The window is a promise about how much we will hold. Accepting more
    -- would make it meaningless.
    local c = opened({ id = 0, window = 50, max_packet = 1000 })
    assert_raises(function()
        c:handle({ type = "data", recipient = 0, data = string.rep("x", 51) })
    end, "over window")
end)

test("data over the packet cap is refused", function()
    local c = opened({ id = 0, window = 10000, max_packet = 64 })
    assert_raises(function()
        c:handle({ type = "data", recipient = 0, data = string.rep("x", 65) })
    end, "over packet cap")
end)

test("stderr draws on the same window as stdout", function()
    -- One window covers the channel, not one per stream; counting them
    -- separately would let a peer send double what it promised.
    local c = opened({ id = 0, window = 100, max_packet = 100 })
    c:handle({ type = "data", recipient = 0, data = string.rep("x", 30) })
    c:handle({ type = "extended_data", recipient = 0, code = 1,
               data = string.rep("y", 30), stderr = true })
    assert_eq(c.recv_window, 40)
end)

test("the window is topped up only once it is half spent", function()
    local c = opened({ id = 0, window = 100, max_packet = 100 })
    c:handle({ type = "data", recipient = 0, data = string.rep("x", 40) })
    assert_eq(c:window_adjustment(), nil, "not due yet")

    c:handle({ type = "data", recipient = 0, data = string.rep("x", 20) })
    local adj = c:window_adjustment()
    assert_eq(adj ~= nil, true, "due after half is spent")
    local r = wire.reader(adj)
    assert_eq(r:byte(), channel.SSH_MSG_CHANNEL_WINDOW_ADJUST)
    assert_eq(r:uint32(), 7)
    assert_eq(r:uint32(), 60)
    assert_eq(c.recv_window, 100, "restored to the initial size")
end)

-- send window ------------------------------------------------------------------------

test("nothing can be sent before the channel opens", function()
    local c = channel.new({ id = 0 })
    assert_eq(c:can_send(1), false)
    assert_eq(c:sendable(), 0)
end)

test("sending draws down the granted window", function()
    local c = opened({ id = 0, peer_window = 100, peer_max = 100 })
    c:data_message(string.rep("x", 40))
    assert_eq(c.send_window, 60)
end)

test("sending more than the window is refused, not truncated", function()
    -- A short write that looks like a full one corrupts whatever is being
    -- transferred.
    local c = opened({ id = 0, peer_window = 10, peer_max = 100 })
    assert_raises(function()
        c:data_message(string.rep("x", 11))
    end, "over send window")
end)

test("sending more than the peer packet cap is refused", function()
    local c = opened({ id = 0, peer_window = 10000, peer_max = 64 })
    assert_raises(function()
        c:data_message(string.rep("x", 65))
    end, "over peer packet cap")
end)

test("sendable is the smaller of window and packet cap", function()
    assert_eq(opened({ peer_window = 500, peer_max = 100 }):sendable(), 100)
    assert_eq(opened({ peer_window = 50, peer_max = 100 }):sendable(), 50)
end)

test("a window adjust from the peer lets more through", function()
    local c = opened({ id = 0, peer_window = 10, peer_max = 100 })
    c:data_message(string.rep("x", 10))
    assert_eq(c:can_send(1), false)
    c:handle({ type = "window_adjust", recipient = 0, add = 90 })
    assert_eq(c.send_window, 90)
    assert_eq(c:can_send(90), true)
end)

test("a window adjust that would overflow is refused", function()
    -- The window is a uint32 on the wire; rolling it over would make
    -- can_send lie about what fits.
    local c = opened({ id = 0, peer_window = 10, peer_max = 100 })
    assert_raises(function()
        c:handle({ type = "window_adjust", recipient = 0, add = 0xFFFFFFFF })
    end, "overflow")
end)

-- results ------------------------------------------------------------------------------

test("exit status is reported", function()
    local c = opened({ id = 0 })
    c:handle(channel.parse(wire.writer():byte(98):uint32(0)
        :string("exit-status"):boolean(false):uint32(3):build()))
    assert_eq(c:result().status, 3)
end)

test("a signalled command has no exit status", function()
    -- Reporting 0, or 128+n, would make "did it succeed" unanswerable.
    local c = opened({ id = 0 })
    c:handle(channel.parse(wire.writer():byte(98):uint32(0)
        :string("exit-signal"):boolean(false):string("KILL")
        :boolean(true):string("killed"):string(""):build()))
    local res = c:result()
    assert_eq(res.status, nil)
    assert_eq(res.signal.signal, "KILL")
    assert_eq(res.signal.core_dumped, true)
end)

test("eof and close are tracked separately", function()
    -- EOF means no more data; close means the channel is gone. A command
    -- that sent output then exited produces both, in that order.
    local c = opened({ id = 0 })
    c:handle({ type = "eof", recipient = 0 })
    assert_eq(c:result().eof, true)
    assert_eq(c:result().closed, false)
    c:handle({ type = "close", recipient = 0 })
    assert_eq(c:result().closed, true)
end)

test("a closed channel sends nothing further", function()
    local c = opened({ id = 0, peer_window = 100, peer_max = 100 })
    c:handle({ type = "close", recipient = 0 })
    assert_eq(c:can_send(1), false)
    assert_eq(c:window_adjustment(), nil)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

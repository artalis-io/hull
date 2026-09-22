-- test_ssh_transport.lua - Tests for hull.ssh.transport
--
-- Drives the transport against a fake stream, so the SEQUENCE can be tested
-- without a server: that a banner before the identification line is skipped,
-- that transport chatter is filtered, that a global request gets refused
-- rather than ignored, that a disconnect surfaces as an error rather than a
-- hang, and that an unknown host stops the connection with a fingerprint
-- instead of proceeding.
--
-- The live behaviour of the crypto is covered by the interop work and by
-- test_crypto.c; what is exercised here is the state machine around it.

local transport = require('hull.ssh.transport')
local packet    = require('hull.ssh.packet')
local kexinit   = require('hull.ssh.kexinit')
local wire      = require('hull.ssh.wire')

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
    local ok, err = pcall(fn)
    if ok then error((msg or "should have raised") .. " but did not") end
    return tostring(err)
end

-- A stream over a fixed script of inbound bytes, recording what was written.
-- read() deliberately returns SHORT chunks, because the real binding does and
-- a transport that assumed otherwise would work here and fail in production.
local function fake_stream(inbound, chunk)
    return {
        _in = inbound, _pos = 1, written = {}, closed = false,
        read = function(self, n)
            if self._pos > #self._in then return "" end
            local take = math.min(n, chunk or 3, #self._in - self._pos + 1)
            local s = self._in:sub(self._pos, self._pos + take - 1)
            self._pos = self._pos + take
            return s
        end,
        write = function(self, s)
            self.written[#self.written + 1] = s
            return true
        end,
        close = function(self) self.closed = true end,
    }
end

local function stub_crypto()
    return {
        random = function(n) return string.rep("\0", n) end,
        sha256 = function() return string.rep("ab", 32) end,
    }
end

local function plain(payload)
    return packet.frame(payload, 8, function(n) return string.rep("\0", n) end)
end

-- reading -------------------------------------------------------------------

test("short reads are assembled, not treated as failure", function()
    -- The binding returns whatever arrived; a transport that expected exactly
    -- n bytes would work against a fake and fail against a socket.
    local t = transport.new(fake_stream("abcdefghij", 2), stub_crypto())
    assert_eq(t:take(10), "abcdefghij")
end)

test("a closed stream mid-packet is an error, not a hang", function()
    local t = transport.new(fake_stream("abc"), stub_crypto())
    local err = assert_raises(function() t:take(10) end)
    assert_eq(err:find("closed", 1, true) ~= nil, true, err)
end)

test("identification lines are read one at a time", function()
    local t = transport.new(fake_stream("first\r\nsecond\r\n"), stub_crypto())
    assert_eq(t:read_line(), "first")
    assert_eq(t:read_line(), "second")
end)

test("a peer that never sends a newline is bounded", function()
    -- Otherwise it could make us buffer without limit.
    local t = transport.new(fake_stream(string.rep("x", 4000)), stub_crypto())
    assert_raises(function() t:read_line() end, "unbounded line")
end)

-- message filtering ------------------------------------------------------------

test("IGNORE and DEBUG are skipped", function()
    local inbound = plain(string.char(2) .. "ignore me")
                 .. plain(string.char(4) .. "\0debug")
                 .. plain(string.char(20) .. "real")
    local t = transport.new(fake_stream(inbound), stub_crypto())
    assert_eq(t:next_message():byte(1), 20)
end)

test("a global request wanting a reply is refused, not ignored", function()
    -- Silence would stall a server that waits for the answer.
    local gr = wire.writer():byte(80):string("keepalive@openssh.com")
                            :boolean(true):build()
    local inbound = plain(gr) .. plain(string.char(20) .. "real")
    local s = fake_stream(inbound)
    local t = transport.new(s, stub_crypto())
    assert_eq(t:next_message():byte(1), 20)

    local sent = table.concat(s.written)
    local reply = packet.parse(sent, 8)
    assert_eq(reply:byte(1), 82, "REQUEST_FAILURE")
end)

test("a global request not wanting a reply gets none", function()
    local gr = wire.writer():byte(80):string("x"):boolean(false):build()
    local s = fake_stream(plain(gr) .. plain(string.char(20) .. "real"))
    local t = transport.new(s, stub_crypto())
    t:next_message()
    assert_eq(#s.written, 0, "nothing should have been sent")
end)

test("DISCONNECT surfaces the server reason", function()
    local d = wire.writer():byte(1):uint32(11)
                           :string("too many authentication failures")
                           :string("en"):build()
    local t = transport.new(fake_stream(plain(d)), stub_crypto())
    local err = assert_raises(function() t:next_message() end)
    assert_eq(err:find("too many authentication", 1, true) ~= nil, true, err)
    assert_eq(err:find("11", 1, true) ~= nil, true, err)
end)

test("a hostile disconnect message cannot write escapes to a terminal", function()
    -- The reason string is attacker-controlled and headed for an operator.
    local d = wire.writer():byte(1):uint32(2)
                           :string("bye\27[2Jcleared"):string(""):build()
    local t = transport.new(fake_stream(plain(d)), stub_crypto())
    local err = assert_raises(function() t:next_message() end)
    assert_eq(err:find("\27", 1, true), nil, "escape survived")
end)

test("endless chatter does not loop forever", function()
    local one = plain(string.char(2) .. "ignore")
    local t = transport.new(fake_stream(string.rep(one, 400)), stub_crypto())
    assert_raises(function() t:next_message() end, "chatter bound")
end)

-- handshake refusals ----------------------------------------------------------------

test("a server with no identification line does not connect", function()
    -- The transport RAISES on I/O failure (the wire readers raise by design)
    -- and RETURNS a reason for protocol refusals. Both are refusals; the
    -- public ssh.connect is where they become one shape, so a caller never
    -- has to pcall to find out whether it got a connection.
    local t = transport.new(fake_stream("hello\r\nthere\r\n"), stub_crypto())
    local ok, err = pcall(function()
        return t:handshake({ host = "h", trust = {} })
    end)
    assert_eq(ok == false or err == nil, true, "must not report success")
end)

test("banners before the identification line are reported", function()
    local seen = {}
    local inbound = "### maintenance window ###\r\nSSH-2.0-Test\r\n"
    local t = transport.new(fake_stream(inbound), stub_crypto())
    pcall(function()
        t:handshake({ host = "h", trust = {},
                      on_banner = function(b) seen[#seen + 1] = b end })
    end)
    assert_eq(#seen, 1)
    assert_eq(seen[1], "### maintenance window ###")
end)

test("a banner cannot smuggle terminal escapes", function()
    local seen = {}
    local inbound = "evil\27[2J\r\nSSH-2.0-Test\r\n"
    local t = transport.new(fake_stream(inbound), stub_crypto())
    pcall(function()
        t:handshake({ host = "h", trust = {},
                      on_banner = function(b) seen[#seen + 1] = b end })
    end)
    assert_eq(seen[1]:find("\27", 1, true), nil, seen[1])
end)

test("no common algorithm is a structured refusal", function()
    local server = kexinit.build({
        kex = { "diffie-hellman-group1-sha1" },
        host_key = { "ssh-rsa" },
        cipher = { "3des-cbc" },
        mac = { "hmac-md5" },
        compression = { "none" },
        languages = {},
    }, string.rep("\0", 16))

    local inbound = "SSH-2.0-Test\r\n" .. plain(server)
    local t = transport.new(fake_stream(inbound), stub_crypto())
    local ok, err = t:handshake({ host = "h", trust = {} })
    assert_eq(ok, nil)
    assert_eq(err.code, "no_common_algorithm")
    -- the detail names the category, so an operator knows which knob to turn
    assert_eq(err.detail:find("key exchange", 1, true) ~= nil, true, err.detail)
end)

test("the client identification string is sent after the server one", function()
    -- RFC 4253 allows either order, but ours must go out before the first
    -- packet or the exchange hash will not match.
    local s = fake_stream("SSH-2.0-Test\r\n")
    local t = transport.new(s, stub_crypto(), { software = "Hull_x" })
    pcall(function() t:handshake({ host = "h", trust = {} }) end)
    assert_eq(s.written[1], "SSH-2.0-Hull_x\r\n")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

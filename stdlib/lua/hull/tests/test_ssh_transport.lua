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

-- exec ----------------------------------------------------------------------
--
-- exec is driven here over a PLAINTEXT fake, which is legal before NEWKEYS
-- and lets the channel sequencing be tested without a key exchange. What is
-- under test is the loop: whether output is streamed or accumulated, whether
-- the cap applies to the right stream, and whether stdin respects the window.

local function conf(window, maxp)
    return plain(wire.writer():byte(91):uint32(0):uint32(7)
                 :uint32(window or 65536):uint32(maxp or 32768):build())
end
local function ok_reply() return plain(wire.writer():byte(99):uint32(0):build()) end
local function data(s)   return plain(wire.writer():byte(94):uint32(0):string(s):build()) end
local function errdata(s)
    return plain(wire.writer():byte(95):uint32(0):uint32(1):string(s):build())
end
local function status(code)
    return plain(wire.writer():byte(98):uint32(0):string("exit-status")
                 :boolean(false):uint32(code):build())
end
local function grant(n) return plain(wire.writer():byte(93):uint32(0):uint32(n):build()) end
local function eof()    return plain(wire.writer():byte(96):uint32(0):build()) end
local function fin()    return plain(wire.writer():byte(97):uint32(0):build()) end

-- The message type of a packet we wrote: 4 length, 1 padding length, then the
-- payload, whose first byte is the type.
local function written_types(s)
    local types = {}
    for _, p in ipairs(s.written) do
        if #p >= 6 then types[#types + 1] = p:byte(6) end
    end
    return types
end

local function has_type(s, want)
    for _, ty in ipairs(written_types(s)) do
        if ty == want then return true end
    end
    return false
end

test("exec streams stdout to a callback as it arrives", function()
    local s = fake_stream(conf() .. ok_reply() .. data("one\n") .. data("two\n")
                          .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local chunks = {}
    local r = t:exec("cmd", { on_stdout = function(c) chunks[#chunks + 1] = c end })
    assert_eq(#chunks, 2, "one callback per inbound chunk:")
    assert_eq(chunks[1], "one\n")
    assert_eq(chunks[2], "two\n")
    -- A streamed stream is never also accumulated; that is the whole point.
    assert_eq(r.stdout, "", "streamed stdout must not be buffered too:")
    assert_eq(r.status, 0)
end)

test("a streamed command is not bounded by max_output", function()
    -- The cap exists because accumulating is unbounded. Streaming is not, so
    -- a caller watching a long-running command must not trip it.
    local big = string.rep("x", 4096)
    local s = fake_stream(conf() .. ok_reply() .. data(big) .. data(big) .. data(big)
                          .. status(0) .. eof() .. fin(), 64)
    local t = transport.new(s, stub_crypto())
    local seen = 0
    local r = t:exec("cmd", { max_output = 16,
                              on_stdout = function(c) seen = seen + #c end })
    assert_eq(seen, 3 * 4096)
    assert_eq(r.status, 0)
end)

test("without a callback the output cap still applies", function()
    local big = string.rep("x", 4096)
    local s = fake_stream(conf() .. ok_reply() .. data(big) .. data(big)
                          .. status(0) .. eof() .. fin(), 64)
    local t = transport.new(s, stub_crypto())
    local r, err = t:exec("cmd", { max_output = 16 })
    assert_eq(r, nil)
    assert_eq(err.code, "output_too_large")
    assert_eq(has_type(s, 97), true, "an aborted exec must close its channel:")
end)

test("streaming one stream still accumulates the other", function()
    local s = fake_stream(conf() .. ok_reply() .. data("out") .. errdata("err")
                          .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local got = {}
    local r = t:exec("cmd", { on_stdout = function(c) got[#got + 1] = c end })
    assert_eq(got[1], "out")
    assert_eq(r.stdout, "")
    assert_eq(r.stderr, "err", "stderr has no callback, so it is buffered:")
end)

test("stdin is written to the command and followed by EOF", function()
    local s = fake_stream(conf() .. ok_reply() .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cat", { stdin = "hello" })
    assert_eq(r.status, 0)
    assert_eq(has_type(s, 94), true, "stdin must be sent as CHANNEL_DATA:")
    assert_eq(has_type(s, 96), true, "stdin must be terminated with CHANNEL_EOF:")
    local joined = table.concat(s.written)
    assert_eq(joined:find("hello", 1, true) ~= nil, true)
end)

test("stdin larger than the window waits for the peer to grant more", function()
    -- A client that wrote past the advertised window would be killed by the
    -- server. Splitting is the transport's job, not the caller's.
    local s = fake_stream(conf(4, 4) .. ok_reply() .. grant(4)
                          .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cat", { stdin = "abcdefgh" })
    assert_eq(r.status, 0)
    local sent = {}
    for _, p in ipairs(s.written) do
        if #p >= 6 and p:byte(6) == 94 then sent[#sent + 1] = p:sub(11) end
    end
    assert_eq(#sent, 2, "eight bytes through a four byte window is two writes:")
end)

test("a stdout callback that raises closes the channel", function()
    local s = fake_stream(conf() .. ok_reply() .. data("boom")
                          .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    assert_raises(function()
        t:exec("cmd", { on_stdout = function() error("caller blew up") end })
    end, "a raising callback should propagate")
    assert_eq(has_type(s, 97), true,
              "the channel must not be left half open behind it:")
end)

test("an oversized stdin is refused up front, not part way through", function()
    -- Writing megabytes to a command that is writing megabytes back wedges
    -- both sides until a socket timeout. Refusing before anything is on the
    -- wire beats stalling, and beats stopping half way with the command
    -- already acting on the first half.
    local s = fake_stream(conf(4 * 1024 * 1024, 32768) .. ok_reply()
                          .. status(0) .. eof() .. fin(), 4096)
    local t = transport.new(s, stub_crypto())
    local r, err = t:exec("cat", { stdin = string.rep("y", 512 * 1024) })
    assert_eq(r, nil)
    assert_eq(err.code, "stdin_too_large")
    assert_eq(err.limit, 128 * 1024)
    assert_eq(has_type(s, 94), false, "and nothing was written to the wire:")
    assert_eq(has_type(s, 97), true, "but the channel is still closed:")
end)

test("a stdin at the limit is still accepted", function()
    local s = fake_stream(conf(4 * 1024 * 1024, 32768) .. ok_reply()
                          .. status(0) .. eof() .. fin(), 4096)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cat", { stdin = string.rep("y", 128 * 1024) })
    assert_eq(r ~= nil and r.status, 0)
    assert_eq(has_type(s, 94), true, "it reached the wire:")
    assert_eq(has_type(s, 96), true, "and was terminated with EOF:")
end)

test("a non-string stdin is refused rather than coerced", function()
    local s = fake_stream(conf() .. ok_reply() .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r, err = t:exec("cat", { stdin = 42 })
    assert_eq(r, nil)
    assert_eq(err.code, "bad_stdin")
end)

-- KEX message discipline ------------------------------------------------------
--
-- During a key exchange the transport is at its most exposed: the packets are
-- plaintext and unauthenticated, so anything accepted there is accepted from
-- whoever is on the wire, not from the server.

test("the expected message is required, not merely looked for", function()
    -- The earlier code read up to eight messages hunting for the one it
    -- wanted and discarded the rest; where NEWKEYS was concerned it did not
    -- even check it had found it.
    local s = fake_stream(plain(string.char(30) .. "wrong"), 4)
    local t = transport.new(s, stub_crypto())
    local err = assert_raises(function() t:expect(21, "NEWKEYS") end)
    assert_eq(err:find("expected NEWKEYS", 1, true) ~= nil, true, err)
    assert_eq(err:find("message 30", 1, true) ~= nil, true, err)
end)

test("the expected message passes through", function()
    local s = fake_stream(plain(string.char(21)), 4)
    local t = transport.new(s, stub_crypto())
    assert_eq(t:expect(21, "NEWKEYS"):byte(1), 21)
end)

test("strict KEX refuses the chatter that is normally skipped", function()
    -- An inserted IGNORE is the Terrapin primitive: it shifts what the two
    -- ends think they agreed, and a client that silently drops it never
    -- notices it happened.
    local s = fake_stream(plain(string.char(2) .. "inserted")
                          .. plain(string.char(21)), 4)
    local t = transport.new(s, stub_crypto())
    local err = assert_raises(function() t:expect(21, "NEWKEYS", true) end)
    assert_eq(err:find("strict KEX", 1, true) ~= nil, true, err)
end)

test("without strict KEX the same chatter is still tolerated", function()
    -- RFC 4253 permits DEBUG and IGNORE at any time, so a server that does
    -- not advertise strict KEX must not be hung up on for sending one.
    local s = fake_stream(plain(string.char(2) .. "chatter")
                          .. plain(string.char(21)), 4)
    local t = transport.new(s, stub_crypto())
    assert_eq(t:expect(21, "NEWKEYS", false):byte(1), 21)
end)

test("strict KEX refuses a global request rather than answering it", function()
    local gr = wire.writer():byte(80):string("x"):boolean(true):build()
    local s = fake_stream(plain(gr) .. plain(string.char(21)), 4)
    local t = transport.new(s, stub_crypto())
    assert_raises(function() t:expect(21, "NEWKEYS", true) end,
                  "a global request during KEX should be refused")
end)

-- rekeying (RFC 4253 section 9) -----------------------------------------------
--
-- The exchange itself needs real crypto, so what is asserted here is the
-- DISPATCH: that a KEXINIT arriving mid-session goes to the key exchange and
-- not to the channel layer, which is what used to kill the connection.

test("a KEXINIT after the handshake is absorbed, not handed to the caller", function()
    local s = fake_stream(plain(string.char(20) .. "rekey")
                          .. plain(string.char(94) .. "data"), 4)
    local t = transport.new(s, stub_crypto())
    t.session_id = "already-handshaken"
    local seen
    t.run_kex = function(_, _, i_s) seen = i_s; return true end

    local m = t:next_message()
    assert_eq(seen ~= nil and seen:byte(1), 20, "the KEXINIT reaches run_kex:")
    assert_eq(m:byte(1), 94, "and the caller gets the next real message:")
end)

test("a KEXINIT before the handshake is left for the exchange to read", function()
    -- run_kex reads the server KEXINIT itself during the first exchange;
    -- absorbing it here would consume the message it is waiting for.
    local s = fake_stream(plain(string.char(20) .. "first"), 4)
    local t = transport.new(s, stub_crypto())
    assert_eq(t:next_message():byte(1), 20)
end)

test("a rekey is not re-entered while one is running", function()
    local s = fake_stream(plain(string.char(20) .. "a") .. plain(string.char(20) .. "b"), 4)
    local t = transport.new(s, stub_crypto())
    t.session_id = "sid"
    local calls = 0
    t.run_kex = function(self, _, _)
        calls = calls + 1
        -- run_kex reads more messages itself; those must not recurse.
        local inner = self:next_message()
        assert_eq(inner:byte(1), 20, "the inner read is NOT absorbed:")
        return true
    end
    assert_raises(function() t:next_message() end)   -- stream runs out after
    assert_eq(calls, 1, "exactly one exchange:")
end)

test("a failed rekey stops the connection rather than carrying on", function()
    -- Carrying on would mean continuing to encrypt under keys the peer has
    -- already moved away from, or worse, under a swapped host identity.
    local s = fake_stream(plain(string.char(20) .. "rekey"), 4)
    local t = transport.new(s, stub_crypto())
    t.session_id = "sid"
    t.run_kex = function() return nil, { code = "host_changed_midsession" } end
    local err = assert_raises(function() t:next_message() end)
    assert_eq(err:find("host_changed_midsession", 1, true) ~= nil, true, err)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

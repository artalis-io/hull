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
        sha256 = function() return string.rep("\171", 32) end,
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
    t:fill(10)
    assert_eq(t.inbuf, "abcdefghij")
end)

test("a closed stream mid-packet is an error, not a hang", function()
    local t = transport.new(fake_stream("abc"), stub_crypto())
    local err = assert_raises(function() t:fill(10) end)
    assert_eq(err:find("closed", 1, true) ~= nil, true, err)
end)

-- A stream that records the largest read it was asked for, and has nothing
-- more to give once its script is spent (so a transport that waits is caught
-- by the "closed" error rather than by a hang).
local function asking_stream(inbound)
    local s = fake_stream(inbound, 4096)
    s.max_asked = 0
    local read = s.read
    s.read = function(self, n)
        if n > self.max_asked then self.max_asked = n end
        return read(self, n)
    end
    return s
end

test("a huge declared length is refused before anything waits for it", function()
    -- Before NEWKEYS the length is unauthenticated: a hostile or injected
    -- 0xFFFFFFFF used to be waited for, and the read asked for 4 GiB.
    local s = asking_stream(string.pack(">I4", 0xFFFFFFFF) .. "rest")
    local t = transport.new(s, stub_crypto())
    local err = assert_raises(function() t:read_packet() end)
    assert_eq(err:find("exceeds the maximum", 1, true) ~= nil, true, err)
    assert_eq(s.max_asked <= 32768, true,
              "asked the stream for " .. tostring(s.max_asked) .. " bytes")
end)

test("a length below the minimum is refused, not waited for", function()
    local s = asking_stream(string.pack(">I4", 2) .. "xx")
    local t = transport.new(s, stub_crypto())
    local err = assert_raises(function() t:read_packet() end)
    assert_eq(err:find("below the minimum", 1, true) ~= nil, true, err)
end)

test("a misaligned length is refused, not waited for", function()
    local s = asking_stream(string.pack(">I4", 13))
    local t = transport.new(s, stub_crypto())
    local err = assert_raises(function() t:read_packet() end)
    assert_eq(err:find("block size", 1, true) ~= nil, true, err)
end)

test("after NEWKEYS a huge declared length is refused the same way", function()
    -- Under GCM the length is plaintext and only authenticated with the tag,
    -- so an on-path attacker can set it at any time, not only before auth.
    local s = asking_stream(string.pack(">I4", 0xFFFFFFF0) .. "rest")
    local t = transport.new(s, stub_crypto())
    t.s2c = require('hull.ssh.cipher').new(string.rep("k", 32), string.rep("i", 12))
    local err = assert_raises(function() t:read_packet() end)
    assert_eq(err:find("exceeds the maximum", 1, true) ~= nil, true, err)
    assert_eq(s.max_asked <= 32768, true,
              "asked the stream for " .. tostring(s.max_asked) .. " bytes")
end)

test("a valid packet still arrives whole through short reads", function()
    local t = transport.new(fake_stream(plain(string.char(20) .. "real"), 3),
                            stub_crypto())
    local p = t:read_packet()
    assert_eq(p, string.char(20) .. "real")
    assert_eq(t.inbuf, "")
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

test("a DISCONNECT reason cannot add a line to the caller's log", function()
    -- The reason lands inside one error line. A newline in it would let a
    -- server write a second, convincing-looking line of its own.
    local d = wire.writer():byte(1):uint32(11)
                           :string("bye" .. string.char(10) .. "host-b: OK")
                           :string("en"):build()
    local t = transport.new(fake_stream(plain(d)), stub_crypto())
    local err = assert_raises(function() t:next_message() end)
    assert_eq(err:find(string.char(10), 1, true), nil, err)
    assert_eq(err:find("byehost-b: OK", 1, true) ~= nil, true, err)
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

-- A whole key exchange against a scripted server, with every primitive
-- stubbed and each verify recorded, so what is under test is which host key
-- path the transport takes, not the arithmetic.
local function full_crypto(calls)
    local c = stub_crypto()
    c.x25519_keypair = function() return string.rep("\9", 32), string.rep("\1", 32) end
    c.x25519 = function() return string.rep("\2", 32) end
    c.gcm_seal = function(_, _, _, p) return p, string.rep("\0", 16) end
    c.gcm_open = function(_, _, _, x) return x end
    c.ed25519_verify = function() calls[#calls + 1] = "ed25519"; return true end
    c.verify = function(jose) calls[#calls + 1] = jose; return true end
    return c
end

local P256 = wire.writer():string("ecdsa-sha2-nistp256"):string("nistp256")
                 :string("\4" .. string.rep("\5", 64)):build()
local ED = wire.writer():string("ssh-ed25519"):string(string.rep("\6", 32)):build()

local function ecdsa_sig()
    local rs = wire.writer():mpint(string.rep("\1", 32)):mpint(string.rep("\2", 32)):build()
    return wire.writer():string("ecdsa-sha2-nistp256"):string(rs):build()
end

local function server_offering(host_keys, key, sig)
    local offer = {}
    for k, v in pairs(kexinit.DEFAULT_OFFER) do offer[k] = v end
    offer.host_key = host_keys
    return "SSH-2.0-Test\r\n" .. plain(kexinit.build(offer, string.rep("\0", 16)))
        .. plain(wire.writer():byte(31):string(key):string(string.rep("Q", 32))
                     :string(sig):build())
        .. plain("\21")
end

test("a server with only an ECDSA host key is verified with ES256", function()
    local calls = {}
    local s = fake_stream(server_offering({ "ecdsa-sha2-nistp256" }, P256, ecdsa_sig()), 64)
    local t = transport.new(s, full_crypto(calls))
    local ok, err = t:handshake({ host = "h", trust = { get = function() return P256 end } })
    assert_eq(ok, true, err and err.code)
    assert_eq(#calls, 1)
    assert_eq(calls[1], "ES256")
    assert_eq(t.negotiated.host_key, "ecdsa-sha2-nistp256")
end)

test("a key of another type than the negotiated algorithm is refused", function()
    -- ECDSA was negotiated; the server then presents an Ed25519 key.
    local calls = {}
    local s = fake_stream(server_offering({ "ecdsa-sha2-nistp256" }, ED, ecdsa_sig()), 64)
    local t = transport.new(s, full_crypto(calls))
    local ok, err = t:handshake({ host = "h", trust = { get = function() return ED end } })
    assert_eq(ok, nil)
    assert_eq(err.code, "host_key_invalid")
    assert_eq(#calls, 0, "nothing reached a verifier:")
end)

-- A rekey that presents a DIFFERENT host key: the case the stubbed
-- run_kex test above only asserts the handling of. Here the whole exchange
-- runs - a real handshake under key A, then the server answers a rekey with
-- key B - and the transport itself must notice. The store still trusts A,
-- and B's signature would verify (every stub signature does), so nothing
-- but the pin against the session's own key can catch it.
local cipher = require('hull.ssh.cipher')

local function ed_key(byte)
    return wire.writer():string("ssh-ed25519"):string(string.rep(byte, 32)):build()
end
local function ed_sig()
    return wire.writer():string("ssh-ed25519"):string(string.rep("\8", 64)):build()
end

-- Packets after NEWKEYS, under the stub's identity AEAD.
local function sealed(payloads)
    local c = cipher.new(string.rep("\0", 32), string.rep("\0", 12))
    local aead = { seal = function(_, _, _, p) return p, string.rep("\0", 16) end }
    local out = {}
    for i, p in ipairs(payloads) do out[i] = c:seal(aead, p, function(n) return string.rep("\0", n) end) end
    return table.concat(out)
end

test("a rekey presenting another host key is refused as host_changed_midsession", function()
    local KEY_A, KEY_B = ed_key("A"), ed_key("B")
    local offer = {}
    for k, v in pairs(kexinit.DEFAULT_OFFER) do offer[k] = v end
    offer.host_key = { "ssh-ed25519" }
    local server_kexinit = kexinit.build(offer, string.rep("\0", 16))
    local function reply(key)
        return wire.writer():byte(31):string(key):string(string.rep("Q", 32))
                  :string(ed_sig()):build()
    end
    local inbound = "SSH-2.0-Test\r\n" .. plain(server_kexinit) .. plain(reply(KEY_A))
                 .. plain("\21")
                 .. sealed({ server_kexinit, reply(KEY_B) })
    local calls = {}
    local c = full_crypto(calls)
    -- A digest that depends on its input, so the two keys' fingerprints differ.
    c.sha256 = function(d) return (string.rep("\0", 32) .. d):sub(-32) end
    local t = transport.new(fake_stream(inbound, 64), c)
    local ok, err = t:handshake({ host = "h", trust = { get = function() return KEY_A end } })
    assert_eq(ok, true, err and err.code)

    local rok, rerr = t:rekey()
    assert_eq(rok, nil)
    assert_eq(rerr.code, "host_changed_midsession")
    -- Both fingerprints, so an operator sees which key was expected.
    assert_eq(type(rerr.fingerprint) == "string" and rerr.fingerprint ~= rerr.stored_fingerprint,
              true, "fingerprints:")
    -- Refused before B's signature was looked at.
    assert_eq(#calls, 1, "only the first exchange verified a signature:")
end)

-- The host key algorithms in the first KEXINIT this client sent.
local function offered_host_keys(s)
    for _, w in ipairs(s.written) do
        -- The identification line goes out before any packet.
        local payload = w:sub(1, 4) ~= "SSH-" and packet.parse(w, 8)
        if payload and payload:byte(1) == 20 then
            return table.concat(kexinit.parse(payload).host_key, ",")
        end
    end
end

test("the key type the trust store holds for a host is asked for first", function()
    -- Known by its RSA key: ask for RSA first, or a server that also has an
    -- Ed25519 key presents that one and the host reads as CHANGED.
    local rsa = wire.writer():string("ssh-rsa"):string("e"):string("n"):build()
    local s = fake_stream("SSH-2.0-Test\r\n")
    local t = transport.new(s, stub_crypto())
    pcall(function()
        t:handshake({ host = "h", trust = { get = function() return rsa end } })
    end)
    assert_eq(offered_host_keys(s),
              "rsa-sha2-512,rsa-sha2-256,ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384")
end)

test("with nothing stored the default host key order is offered", function()
    local s = fake_stream("SSH-2.0-Test\r\n")
    local t = transport.new(s, stub_crypto())
    pcall(function() t:handshake({ host = "h", trust = { get = function() end } }) end)
    assert_eq(offered_host_keys(s), table.concat(kexinit.DEFAULT_OFFER.host_key, ","))
end)

test("a trust store that fails to read leaves the offer alone", function()
    local s = fake_stream("SSH-2.0-Test\r\n")
    local t = transport.new(s, stub_crypto())
    pcall(function()
        t:handshake({ host = "h", trust = { get = function() error("disk gone") end } })
    end)
    assert_eq(offered_host_keys(s), table.concat(kexinit.DEFAULT_OFFER.host_key, ","))
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

test("a channel request wanting a reply is answered, not ignored", function()
    -- OpenSSH's ClientAliveInterval sends exactly this on an open session and
    -- disconnects a client that never answers - which killed long commands.
    local keepalive = plain(wire.writer():byte(98):uint32(0)
                            :string("keepalive@openssh.com"):boolean(true):build())
    local s = fake_stream(conf() .. ok_reply() .. data("a") .. keepalive
                          .. data("b") .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cmd")
    assert_eq(r.stdout, "ab", "the command carried on:")
    assert_eq(r.status, 0)
    assert_eq(has_type(s, 100), true, "CHANNEL_FAILURE sent:")
end)

test("a channel request not wanting a reply gets none", function()
    -- exit-status is the everyday case; answering it would be a protocol error.
    local s = fake_stream(conf() .. ok_reply() .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    t:exec("cmd")
    assert_eq(has_type(s, 100), false, "no CHANNEL_FAILURE:")
end)

test("a channel the server asks to open is refused with a reply", function()
    -- A forwarded connection or agent request. The server waits for an
    -- answer; this used to raise "unexpected connection message 90" and take
    -- the whole connection down instead.
    local open = plain(wire.writer():byte(90):string("auth-agent@openssh.com")
                       :uint32(5):uint32(65536):uint32(32768):build())
    local s = fake_stream(conf() .. ok_reply() .. open .. data("x")
                          .. status(0) .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cmd")
    assert_eq(r.stdout, "x")
    assert_eq(has_type(s, 92), true, "CHANNEL_OPEN_FAILURE sent:")
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

-- rekeying from THIS side ------------------------------------------------
--
-- Absorbing the server's request is half of RFC 4253 section 9. The other
-- half is asking, which matters against every peer that never asks: without
-- it one key runs to hull.ssh.cipher's MAX_PACKETS backstop and the
-- connection dies for no reason the caller can act on.

-- A stand-in for a cipher direction: the transport only ever asks it three
-- questions, and driving the real one to a gigabyte is not a unit test.
local function fake_cipher(due, bytes, packets)
    return {
        rekey_due = function() return due end,
        bytes_processed = function() return bytes or 0 end,
        packets_sent = function() return packets or 0 end,
    }
end

test("a connection under its limits does not rekey", function()
    local t = transport.new(fake_stream("", 4), stub_crypto())
    t.session_id = "sid"
    t.c2s, t.s2c = fake_cipher(false), fake_cipher(false)
    local ran = false
    t.rekey = function() ran = true; return true end
    assert_eq(t:maybe_rekey(), false, "no exchange:")
    assert_eq(ran, false, "and rekey was not called:")
end)

test("either direction reaching its limit asks for new keys", function()
    for _, which in ipairs({ "c2s", "s2c" }) do
        local t = transport.new(fake_stream("", 4), stub_crypto())
        t.session_id = "sid"
        t.c2s, t.s2c = fake_cipher(false), fake_cipher(false)
        t[which] = fake_cipher(true)
        local ran = false
        t.rekey = function() ran = true; return true end
        assert_eq(t:maybe_rekey(), true, which .. " exchanged:")
        assert_eq(ran, true, which .. " called rekey:")
    end
end)

test("a rekey that fails stops the connection rather than continuing", function()
    -- Carrying on would mean encrypting past the limit the key was chosen
    -- for, which is the one thing the limit exists to prevent.
    local t = transport.new(fake_stream("", 4), stub_crypto())
    t.session_id = "sid"
    t.c2s, t.s2c = fake_cipher(true), fake_cipher(false)
    t.rekey = function() return nil, { code = "no_common_algorithm" } end
    local err = assert_raises(function() t:maybe_rekey() end)
    assert_eq(err:find("no_common_algorithm", 1, true) ~= nil, true, err)
end)

test("opening a session is where the limit is checked", function()
    -- Anywhere else would mean starting a key exchange in the middle of
    -- somebody's output. exec and sftp both come through here.
    local t = transport.new(fake_stream("", 4), stub_crypto())
    local asked = false
    t.maybe_rekey = function() asked = true; return false end
    pcall(function() t:open_session() end)   -- the stream ends; that is fine
    assert_eq(asked, true, "open_session asked:")
end)

test("rekey before the handshake is refused, not attempted", function()
    local t = transport.new(fake_stream("", 4), stub_crypto())
    local ok, why = t:rekey()
    assert_eq(ok, nil)
    assert_eq(why.code, "not_handshaken")
end)

test("a rekey already running is not started a second time", function()
    local t = transport.new(fake_stream("", 4), stub_crypto())
    t.session_id = "sid"
    t.in_kex = true
    t.run_kex = function() error("run_kex must not be reached") end
    assert_eq(t:rekey(), true)
end)

test("messages that arrive before the peer's KEXINIT are kept, in order", function()
    -- Between our KEXINIT and the peer's, the peer has not seen ours yet and
    -- is still entitled to send channel data. Dropping it would silently
    -- lose a command's output; handing it to the key exchange would fail on
    -- a message that is perfectly legal.
    local t = transport.new(fake_stream("", 4), stub_crypto())
    t:defer_message("first")
    t:defer_message("second")
    assert_eq(t:next_message(), "first")
    assert_eq(t:next_message(), "second")
    -- and then it reads from the stream again
    local u = transport.new(fake_stream(plain(string.char(94) .. "live"), 4),
                            stub_crypto())
    u:defer_message(string.char(93) .. "held")
    assert_eq(u:next_message():byte(1), 93, "held first:")
    assert_eq(u:next_message():byte(1), 94, "then the wire:")
end)

test("a rekey we start defers channel data until the exchange is over", function()
    -- End to end through run_kex rather than through the queue alone: the
    -- deferral only helps if the exchange actually routes through it.
    local channel_data = string.char(94) .. "output"
    local theirs = kexinit.build({ kex = { "nope" }, host_key = { "nope" },
                                   cipher = { "nope" }, mac = { "none" },
                                   compression = { "none" } },
                                 string.rep("c", 16))
    local s = fake_stream(plain(channel_data) .. plain(theirs), 4)
    local t = transport.new(s, stub_crypto())
    t.session_id = "sid"          -- makes this a rekey, not a first exchange

    -- Through rekey(), not run_kex directly: rekey() is what marks the
    -- exchange as running, and without that mark next_message absorbs the
    -- server's KEXINIT into a NESTED exchange instead of handing it to the
    -- one already waiting for it.
    --
    -- Negotiation then fails on purpose: what is being asserted is what
    -- happened to the data packet on the way there, not the exchange itself.
    local ok, why = t:rekey()
    assert_eq(ok, nil, "the exchange failed as arranged:")
    assert_eq(why.code, "no_common_algorithm")

    local held = t:next_message()
    assert_eq(held:byte(1), 94, "the channel data survived the rekey:")
    assert_eq(held:sub(2), "output")
end)

test("a rekey we start reads its reply past the data it set aside", function()
    -- Same setup, but negotiation SUCCEEDS, so the exchange goes on to wait
    -- for KEX_ECDH_REPLY. It used to read that through next_message, which
    -- hands out the deferred channel data first, and failed the connection
    -- with "expected KEX_ECDH_REPLY but the peer sent message 94" - the
    -- earlier test never got this far, because it fails negotiation first.
    local channel_data = string.char(94) .. "output"
    local theirs = kexinit.build(nil, string.rep("c", 16))
    local reply = wire.writer():byte(31):string("K_S")
                               :string(string.rep("q", 32)):string("sig"):build()
    local s = fake_stream(plain(channel_data) .. plain(theirs) .. plain(reply), 4)
    local crypto = stub_crypto()
    crypto.x25519_keypair = function()
        return string.rep("\0", 32), string.rep("\17", 32)
    end
    -- Stops the exchange just after the reply was accepted: reaching here at
    -- all is what is being asserted.
    crypto.x25519 = function() return nil, "stub stops here" end
    local t = transport.new(s, crypto)
    t.session_id = "sid"

    local ok, res, why = pcall(t.rekey, t)
    local detail
    if not ok then detail = tostring(res)            -- raised
    elseif res then detail = "succeeded"
    else detail = why and why.code end
    assert_eq(detail, "bad_kex_point", "the exchange read its own reply:")

    local held = t:next_message()
    assert_eq(held:byte(1), 94, "the channel data is still there for the caller:")
end)

-- strict KEX -------------------------------------------------------------------

-- A server KEXINIT built from our own offer, with the strict marker swapped
-- for the server's (or dropped).
local function server_kexinit(strict)
    local offer = {}
    for k, v in pairs(kexinit.DEFAULT_OFFER) do offer[k] = v end
    offer.kex = { "curve25519-sha256" }
    if strict then offer.kex[2] = kexinit.STRICT_S end
    return plain(kexinit.build(offer, string.rep("s", 16)))
end

local function kex_crypto()
    local c = stub_crypto()
    c.x25519_keypair = function() return string.rep("\0", 32), string.rep("\17", 32) end
    c.x25519 = function() return nil, "stub stops here" end
    return c
end

local function ecdh_reply()
    return plain(wire.writer():byte(31):string("K_S")
                 :string(string.rep("q", 32)):string("sig"):build())
end

local IGNORE = plain(string.char(2) .. "x")

-- Run the handshake; return its reason code, or the error it raised.
local function handshake_outcome(inbound)
    local t = transport.new(fake_stream("SSH-2.0-test\r\n" .. inbound, 16), kex_crypto())
    local ok, res, why = pcall(t.handshake, t, { host = "h", trust = {} })
    if not ok then return tostring(res) end
    return res and "succeeded" or (why and why.code)
end

test("strict KEX refuses a packet before the server's KEXINIT", function()
    -- Terrapin's primitive: an IGNORE the client skips shifts the sequence
    -- numbers both sides think they share.
    local out = handshake_outcome(IGNORE .. server_kexinit(true) .. ecdh_reply())
    assert_eq(out:find("was not its first packet", 1, true) ~= nil, true, out)
end)

test("without strict KEX a packet before KEXINIT is still tolerated", function()
    local out = handshake_outcome(IGNORE .. server_kexinit(false) .. ecdh_reply())
    assert_eq(out, "bad_kex_point", "reached the exchange:")
end)

test("strict KEX, decided by the first exchange, holds for a rekey", function()
    -- The markers only mean anything in the initial KEXINIT. Recomputing
    -- strictness per exchange made every rekey lenient again, so an IGNORE
    -- injected mid-rekey was skipped.
    local t = transport.new(fake_stream(server_kexinit(false) .. IGNORE .. ecdh_reply(), 16),
                            kex_crypto())
    t.session_id = "sid"
    t.strict_kex = true              -- as the first exchange left it
    local ok, err = pcall(t.next_message, t)
    assert_eq(ok, false)
    assert_eq(tostring(err):find("strict KEX", 1, true) ~= nil, true, tostring(err))
end)

test("sequence numbers count every packet, both ways", function()
    local t = transport.new(fake_stream(IGNORE .. plain(string.char(20) .. "x"), 16),
                            stub_crypto())
    t:next_message()
    assert_eq(t.recv_seq, 2, "the skipped IGNORE counts too:")
    t:send_packet(string.char(2))
    assert_eq(t.send_seq, 1)
end)

test("stats span the connection, not just the current key", function()
    local t = transport.new(fake_stream("", 4), stub_crypto())
    t.rekeys = 2
    t.total_sent, t.total_received = 100, 200
    t.c2s = fake_cipher(false, 7, 3)
    t.s2c = fake_cipher(false, 9, 4)
    local st = t:stats()
    assert_eq(st.rekeys, 2)
    assert_eq(st.bytes_sent, 107, "carried across rekeys:")
    assert_eq(st.bytes_received, 209)
    assert_eq(st.packets_sent, 3, "packets are per-key:")
    assert_eq(st.packets_received, 4)
    assert_eq(st.rekey_due, false)
end)

-- Return results for C test harness
-- liveness ------------------------------------------------------------------
--
-- A stream that follows a script, one step per read: a string is data, and
-- "TIMEOUT" / "DEADLINE" are the stream's bounded-wait expiries. It records
-- the bounds it was given, so a test can see what the transport asked for.

local function scripted_stream(steps)
    local s = fake_stream("", 1)
    s.steps, s.i, s.deadlines, s.waits = steps, 0, {}, {}
    s.read = function(self, n)
        self.i = self.i + 1
        local step = self.steps[self.i]
        if step == nil then return "" end
        if step == "TIMEOUT" then return nil, "timed out", "timeout" end
        if step == "DEADLINE" then return nil, "deadline reached", "deadline" end
        -- Serve a data step whole even if it is longer than n: the transport
        -- asks for what it needs, and never less than one byte.
        if #step > n then
            table.insert(self.steps, self.i + 1, step:sub(n + 1))
            step = step:sub(1, n)
        end
        return step
    end
    s.deadline = function(self, ms) self.deadlines[#self.deadlines + 1] = ms end
    s.wait = function(self, ms) self.waits[#self.waits + 1] = ms end
    return s
end

local function keepalives_sent(s)
    local n = 0
    for _, p in ipairs(s.written) do
        local payload = packet.parse(p, 8)
        if payload and payload:byte(1) == 80 then n = n + 1 end
    end
    return n
end

local function raised_code(fn)
    local ok, err = pcall(fn)
    if ok then return "no error" end
    return type(err) == "table" and err.code or tostring(err)
end

test("the stream is given the keepalive interval as its wait bound", function()
    local s = scripted_stream({})
    transport.new(s, stub_crypto(), { keepalive_ms = 30000 })
    assert_eq(s.waits[1], 30000)
end)

test("a silent server is given up on at the idle bound, keepalive in between", function()
    -- 30 s of nothing: a keepalive asks. 30 s more, still nothing: 60 s idle.
    local s = scripted_stream({ "TIMEOUT", "TIMEOUT" })
    local t = transport.new(s, stub_crypto(),
                            { keepalive_ms = 30000, idle_ms = 60000 })
    t.authenticated = true
    assert_eq(raised_code(function() t:fill(1) end), "timeout")
    assert_eq(keepalives_sent(s), 1, "one keepalive before giving up:")
    -- And the connection says so from then on, without waiting again.
    local ch, err = t:open_session()
    assert_eq(ch, nil)
    assert_eq(err.code, "timeout")
end)

test("a server that answers keepalives is not idle, however quiet", function()
    -- A command printing nothing for minutes: each keepalive's reply is
    -- traffic, which resets the silence, so the idle bound never trips.
    local reply = plain(string.char(82))
    local steps = {}
    for _ = 1, 10 do steps[#steps + 1] = "TIMEOUT"; steps[#steps + 1] = reply end
    steps[#steps + 1] = plain(string.char(20) .. "real")
    local s = scripted_stream(steps)
    local t = transport.new(s, stub_crypto(),
                            { keepalive_ms = 30000, idle_ms = 60000 })
    t.authenticated = true
    assert_eq(t:next_message():byte(1), 20, "the reply is absorbed:")
    assert_eq(keepalives_sent(s), 10)
end)

test("no keepalive before authentication, and the idle bound still holds", function()
    local s = scripted_stream({ "TIMEOUT", "TIMEOUT" })
    local t = transport.new(s, stub_crypto(),
                            { keepalive_ms = 30000, idle_ms = 60000 })
    assert_eq(raised_code(function() t:fill(1) end), "timeout")
    assert_eq(keepalives_sent(s), 0)
end)

test("with no idle bound, unanswered keepalives end it", function()
    local s = scripted_stream({ "TIMEOUT", "TIMEOUT", "TIMEOUT", "TIMEOUT" })
    local t = transport.new(s, stub_crypto(),
                            { keepalive_ms = 30000, keepalive_max = 3, idle_ms = 0 })
    t.authenticated = true
    assert_eq(raised_code(function() t:fill(1) end), "timeout")
    assert_eq(keepalives_sent(s), 3)
end)

test("a deadline is its own code, and does not kill the connection", function()
    local s = scripted_stream({ "DEADLINE" })
    local t = transport.new(s, stub_crypto())
    assert_eq(raised_code(function() t:fill(1) end), "deadline")
    assert_eq(t.dead, nil)
end)

test("a write that waited too long is retried, not failed", function()
    local s = scripted_stream({})
    local tries = 0
    s.write = function(self, b)
        tries = tries + 1
        if tries == 1 then return nil, "timed out", "timeout" end
        self.written[#self.written + 1] = b
        return true
    end
    local t = transport.new(s, stub_crypto(), { idle_ms = 60000 })
    t:send_raw("abc")
    assert_eq(tries, 2)
    assert_eq(s.written[1], "abc")
end)

test("exec with timeout_ms returns a timeout and keeps the connection", function()
    -- The command's deadline expires mid-output. exec closes the channel
    -- under a short bound of its own and reports; the connection lives on.
    local s = scripted_stream({ conf(), ok_reply(), data("partial"), "DEADLINE",
                                eof(), fin() })
    local t = transport.new(s, stub_crypto())
    local r, err = t:exec("sleep 600", { timeout_ms = 1500 })
    assert_eq(r, nil)
    assert_eq(err.code, "timeout")
    assert_eq(t.dead, nil, "the connection is still usable:")
    assert_eq(has_type(s, 97), true, "the channel was closed:")
    -- The command's bound, cleared, then the drain's own bound, cleared.
    assert_eq(table.concat(s.deadlines, ","), "1500,0,5000,0")
end)

test("exec refuses a timeout_ms that is not a positive integer", function()
    local t = transport.new(scripted_stream({}), stub_crypto())
    for _, bad in ipairs({ 0, -1, 1.5, "10" }) do
        local r, err = t:exec("x", { timeout_ms = bad })
        assert_eq(r, nil)
        assert_eq(err.code, "bad_timeout", tostring(bad))
    end
end)

-- sftp ----------------------------------------------------------------------
--
-- The SFTP client over the same plaintext fake: a scripted server that
-- answers each request in order. What is under test is the bookkeeping - that
-- every request's reply is the one it gets, on the error paths too.

local sftp_codec = require('hull.ssh.sftp')

-- One SFTP message from the server, inside channel data.
-- One SFTP message from the server, inside channel data - split across
-- messages the way a real server splits one larger than our packet size.
local function sreply(payload)
    local bytes, out = sftp_codec.frame(payload), {}
    for i = 1, #bytes, 16384 do out[#out + 1] = data(bytes:sub(i, i + 16383)) end
    return table.concat(out)
end
local function s_version() return sreply(wire.writer():byte(2):uint32(3):build()) end
local function s_handle(id, h)
    return sreply(wire.writer():byte(102):uint32(id):string(h):build())
end
local function s_data(id, d)
    return sreply(wire.writer():byte(103):uint32(id):string(d):build())
end
local function s_status(id, code)
    return sreply(wire.writer():byte(101):uint32(id):uint32(code)
                  :string(""):string("en"):build())
end

-- The SFTP requests we sent, decoded: { type, id, handle? } in order.
local function sent_sftp(s)
    -- SFTP is a byte stream inside the channel, and a message may be split
    -- across CHANNEL_DATA messages: join them all, then cut frames.
    local body = {}
    for _, p in ipairs(s.written) do
        local payload = packet.parse(p, 8)
        if payload and payload:byte(1) == 94 then
            local r = wire.reader(payload)
            r:byte(); r:uint32()
            body[#body + 1] = r:string()
        end
    end
    body = table.concat(body)
    local out, pos = {}, 1
    while pos + 4 <= #body do
        local n = string.unpack(">I4", body, pos)
        local req = body:sub(pos + 4, pos + 3 + n)
        pos = pos + 4 + n
        local rr = wire.reader(req)
        local ty = rr:byte()
        local e = { type = ty }
        if ty ~= 1 then                            -- INIT carries no id
            e.id = rr:uint32()
            if ty == 4 or ty == 8 then             -- CLOSE, FSTAT
                e.handle = rr:string()
            elseif ty == 5 then                    -- READ
                e.handle = rr:string(); e.offset = rr:uint64(); e.len = rr:uint32()
            elseif ty == 6 then                    -- WRITE
                e.handle = rr:string(); e.offset = rr:uint64(); e.data = rr:string()
            elseif ty == 3 then                    -- OPEN
                e.path = rr:string(); e.pflags = rr:uint32()
                e.attrs = sftp_codec.decode_attrs(rr)
            elseif ty == 14 or ty == 9 then        -- MKDIR, SETSTAT
                e.path = rr:string(); e.attrs = sftp_codec.decode_attrs(rr)
            end
        end
        out[#out + 1] = e
    end
    return out
end

local function sent_of_type(s, ty)
    local out = {}
    for _, e in ipairs(sent_sftp(s)) do
        if e.type == ty then out[#out + 1] = e end
    end
    return out
end

local function s_attrs(id, a)
    return sreply(wire.writer():byte(105):uint32(id)
                  :raw(sftp_codec.encode_attrs(a)):build())
end

-- A session to script: conf, subsystem reply and VERSION, then `rest`.
local function sftp_over(rest, chunk, window)
    local s = fake_stream(conf(window) .. ok_reply() .. s_version() .. rest, chunk or 11)
    local t = transport.new(s, stub_crypto())
    return assert(t:sftp()), s
end

test("stat returns the attributes the server sent", function()
    local f = sftp_over(s_attrs(1, { size = 1234, permissions = 0x41ed,
                                     uid = 1, gid = 2, atime = 5, mtime = 6 }))
    local a = f:stat("/srv")
    assert_eq(a.size, 1234)
    assert_eq(a.permissions, 0x41ed)
    assert_eq(a.is_dir, true)
    assert_eq(a.mtime, 6)
end)

test("a status by name: a missing file is no_such_file", function()
    local f = sftp_over(s_status(1, 2))
    local a, err = f:stat("/nope")
    assert_eq(a, nil)
    assert_eq(err.code, "no_such_file")
end)

test("mkdir sends an octal-string mode as its permission bits", function()
    -- Lua has no octal literal: "755" is what a caller means, not 755.
    local f, s = sftp_over(s_status(1, 0))
    assert_eq(f:mkdir("/srv/app", "755"), true)
    assert_eq(sent_of_type(s, 14)[1].attrs.permissions, 493)
end)

test("rename, remove, rmdir and chmod report success and failure", function()
    local f, s = sftp_over(s_status(1, 0) .. s_status(2, 0) .. s_status(3, 4)
                           .. s_status(4, 0))
    assert_eq(f:rename("/a", "/b"), true)
    assert_eq(f:remove("/b"), true)
    local ok, err = f:rmdir("/full")
    assert_eq(ok, nil)
    assert_eq(err.code, "failure")
    assert_eq(f:chmod("/x", "0644"), true)
    assert_eq(sent_of_type(s, 9)[1].attrs.permissions, 420)
end)

test("a mode that is not octal is a caller error, with a code", function()
    local f = sftp_over("")
    local ok, err = pcall(f.chmod, f, "/x", "rwx")
    assert_eq(ok, false)
    assert_eq(err.code, "bad_argument")
end)

test("open modes map to their flags", function()
    local cases = { r = 0x01, ["r+"] = 0x03, w = 0x1a, a = 0x0e, wx = 0x2a }
    for mode, flags in pairs(cases) do
        local f, s = sftp_over(s_handle(1, "h"))
        assert(f:open("/f", mode))
        assert_eq(sent_of_type(s, 3)[1].pflags, flags, mode)
    end
end)

test("open with opts.mode creates the file with those permissions", function()
    local f, s = sftp_over(s_handle(1, "h"))
    assert(f:open("/f", "w", { mode = "600" }))
    assert_eq(sent_of_type(s, 3)[1].attrs.permissions, 384)
end)

test("a large read ramps up to several requests in flight", function()
    -- The window starts at one and grows by one per full reply, topped up as
    -- each reply arrives: a big file reaches full pipelining in a few round
    -- trips, where one-at-a-time would be a round trip per 32 KiB. The
    -- replies below answer requests 2, 3+4, 5-8 in that order.
    local C = 32768
    local full = string.rep("a", C)
    local f = sftp_over(s_handle(1, "h")
        .. s_data(2, full)                           -- batch of 1
        .. s_data(4, full) .. s_data(3, full)        -- batch of 2, out of order
        .. s_data(5, "tail") .. s_status(6, 1) .. s_status(7, 1) .. s_status(8, 1))
    local file = assert(f:open("/big", "r"))
    local got = file:read(10 * C)
    assert_eq(#got, 3 * C + 4, "three full chunks and the tail:")
    assert_eq(got:sub(-4), "tail")
    assert_eq(file:tell(), 3 * C + 4)
end)

test("a read keeps the window full instead of draining it between batches", function()
    -- Record, at each request we send, how many replies had been consumed.
    -- In flight = requests sent - replies taken. Lock-step batches let that
    -- fall to zero before each new batch; a sliding window keeps it at the
    -- top once it gets there, sending one request per reply.
    local C, N = 32768, 200     -- enough to reach 64 and stay there
    local full = string.rep("r", C)
    local script = { s_handle(1, "h") }
    local ends = {}                          -- where each data reply ends
    local head = conf() .. ok_reply() .. s_version() .. s_handle(1, "h")
    local at = #head
    for i = 1, N do
        local rpl = s_data(i + 1, full)
        script[#script + 1] = rpl
        at = at + #rpl
        ends[i] = at
    end
    local s = fake_stream(conf() .. ok_reply() .. s_version() .. table.concat(script), 4096)
    local reads, inflight = 0, {}
    local write = s.write
    s.write = function(self, bytes)
        local p = packet.parse(bytes, 8)
        if p and p:byte(1) == 94 then
            local r = wire.reader(p); r:byte(); r:uint32()
            local body = r:string()
            -- one SFTP READ per CHANNEL_DATA here (they are small)
            if #body > 5 and body:byte(5) == 5 then
                reads = reads + 1
                local taken = 0
                for _, e in ipairs(ends) do if e < self._pos then taken = taken + 1 end end
                inflight[#inflight + 1] = reads - taken
            end
        end
        return write(self, bytes)
    end
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    local file = assert(f:open("/big", "r"))
    assert_eq(#file:read(N * C), N * C)
    assert_eq(reads, N)
    local peak, at_peak = 0, nil
    for i, v in ipairs(inflight) do
        if v > peak then peak, at_peak = v, i end
    end
    assert_eq(peak, 64, "the window reaches 64 requests:")
    for i = at_peak, #inflight do
        assert_eq(inflight[i] >= 63, true, "request " .. i .. " sent with "
                  .. inflight[i] .. " in flight")
    end
end)

test("a READ reply longer than the request is refused", function()
    local f = sftp_over(s_handle(1, "h") .. s_data(2, "toolong"))
    local file = assert(f:open("/f", "r"))
    local d, err = file:read(3)
    assert_eq(d, nil)
    assert_eq(err.code, "bad_reply")
end)

test("a failed write stops sending and still collects what is in flight", function()
    local C = 32768
    local f, s = sftp_over(s_handle(1, "h") .. s_status(2, 0) .. s_status(3, 4)
                           .. s_status(4, 0) .. s_status(5, 0) .. s_status(6, 0),
                           4096, 1048576)
    local file = assert(f:open("/f", "w"))
    local ok, err = file:write(string.rep("w", 4 * C))
    assert_eq(ok, nil)
    assert_eq(err.code, "failure")
    -- the CLOSE after it gets its own reply, not a leftover write status
    assert_eq(file:close(), true)
    assert_eq(#sent_of_type(s, 6), 4)
end)

test("replies are matched by id even when the server reorders them", function()
    local C = 32768
    local a, b = string.rep("1", C), string.rep("2", C)
    local f = sftp_over(s_handle(1, "h") .. s_data(2, a)
                        .. s_data(4, "end") .. s_data(3, b))
    local file = assert(f:open("/f", "r"))
    local got = file:read(3 * C)
    assert_eq(got, a .. b .. "end", "in offset order, not arrival order:")
end)

test("a large write goes out as pipelined chunks at the right offsets", function()
    local C = 32768
    local payload = string.rep("w", 3 * C + 10)
    local f, s = sftp_over(s_handle(1, "h") .. s_status(2, 0) .. s_status(3, 0)
                           .. s_status(4, 0) .. s_status(5, 0) .. s_status(6, 0), 4096, 1048576)
    local file = assert(f:open("/f", "w"))
    assert_eq(file:write(payload), true)
    assert_eq(file:close(), true)
    local writes = sent_of_type(s, 6)
    assert_eq(#writes, 4)
    for i, w in ipairs(writes) do assert_eq(w.offset, (i - 1) * C, "chunk " .. i) end
    assert_eq(#writes[4].data, 10)
end)

test("seek moves where the next read starts; a closed file says so", function()
    local f, s = sftp_over(s_handle(1, "h") .. s_data(2, "xyz") .. s_status(3, 0))
    local file = assert(f:open("/f", "r"))
    file:seek(100)
    assert_eq(file:read(3), "xyz")
    assert_eq(sent_of_type(s, 5)[1].offset, 100)
    assert_eq(file:close(), true)
    assert_eq(file:close(), true, "closing twice is harmless:")
    local d, err = file:read(1)
    assert_eq(d, nil)
    assert_eq(err.code, "closed")
end)

test("a failed sftp read does not shift the next request's replies", function()
    -- read() gives up when the file exceeds its limit and closes the handle.
    -- That CLOSE's status used to be left unread, so the next OPEN received
    -- it instead of its handle, and every reply after was off by one - a
    -- write could land in the previous file.
    local s = fake_stream(conf() .. ok_reply() .. s_version()
        .. s_handle(1, "hA") .. s_data(2, "xxxxx") .. s_status(3, 0)
        .. s_handle(4, "hB") .. s_status(5, 0) .. s_status(6, 0), 11)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())

    local d, err = f:read("/a", 4)
    assert_eq(d, nil)
    assert_eq(err.code, "too_large")
    assert_eq(err.detail:find("byte limit", 1, true) ~= nil, true, err.detail)

    assert_eq(f:write("/b", "hello"), true, "the write should succeed:")

    local writes = {}
    for _, e in ipairs(sent_sftp(s)) do
        if e.type == 6 then writes[#writes + 1] = e.handle end
    end
    assert_eq(#writes, 1)
    assert_eq(writes[1], "hB", "the write must go to the file it opened:")
end)

test("an sftp reply for another request is refused", function()
    -- A reply is only ever the answer to the request with its id. Taking the
    -- next one regardless is how a single stray reply becomes a write into
    -- the wrong file.
    local s = fake_stream(conf() .. ok_reply() .. s_version()
        .. s_status(7, 0))
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    local err = assert_raises(function() f:realpath("/x") end)
    assert_eq(err:find("reply for request 7 while waiting for 1", 1, true) ~= nil,
              true, err)
end)

test("closing sftp drains its channel, so the next reader is not handed its tail", function()
    -- The peer's EOF and CLOSE for the sftp channel are still in flight after
    -- ours. Sftp:close used to send CLOSE and return, leaving them for
    -- whatever read next. open_session happens to skip up to 16 stray
    -- messages while it waits for its confirmation, which is why the exec
    -- below survived even then; a longer tail, or any reader that is not
    -- open_session, did not. So the property asserted first is close's own:
    -- the channel is read through to its CLOSE before close returns.
    local function on(id, w) return plain(w:build()) end
    local ch1_conf = on(1, wire.writer():byte(91):uint32(1):uint32(8)
                               :uint32(65536):uint32(32768))
    local ch1_ok   = on(1, wire.writer():byte(99):uint32(1))
    local ch1_data = on(1, wire.writer():byte(94):uint32(1):string("hi"))
    local ch1_st   = on(1, wire.writer():byte(98):uint32(1):string("exit-status")
                               :boolean(false):uint32(0))
    local ch1_eof  = on(1, wire.writer():byte(96):uint32(1))
    local ch1_fin  = on(1, wire.writer():byte(97):uint32(1))

    local s = fake_stream(conf() .. ok_reply() .. s_version()
        .. eof() .. fin()                                   -- sftp channel's tail
        .. ch1_conf .. ch1_ok .. ch1_data .. ch1_st .. ch1_eof .. ch1_fin, 9)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    f:close()
    assert_eq(f.ch.closed, true, "the peer's CLOSE was read:")
    local r = t:exec("echo hi")
    assert_eq(r.stdout, "hi")
    assert_eq(r.status, 0)
end)

-- Two channels on one connection. Whoever reads next reads for both, so a
-- message for the channel NOT being waited on has to land on that channel.
-- These run a command while an sftp session is open (channel 0, the server's
-- id 7), with messages for the sftp channel arriving in the middle of it.

local function on(w) return plain(w:build()) end
local function exec_on_ch1(extra_mid)
    return on(wire.writer():byte(91):uint32(1):uint32(8):uint32(65536):uint32(32768))
        .. on(wire.writer():byte(99):uint32(1))
        .. extra_mid
        .. on(wire.writer():byte(94):uint32(1):string("hi"))
        .. on(wire.writer():byte(98):uint32(1):string("exit-status")
                  :boolean(false):uint32(0))
        .. on(wire.writer():byte(96):uint32(1))
        .. on(wire.writer():byte(97):uint32(1))
end

test("a window adjust for an open sftp channel does not break an exec", function()
    -- It used to raise "message for channel 0, expected 1" out of the exec.
    local adj = on(wire.writer():byte(93):uint32(0):uint32(4096))
    local s = fake_stream(conf() .. ok_reply() .. s_version() .. exec_on_ch1(adj), 9)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    local before = f.ch.send_window
    local r = assert(t:exec("echo hi"))
    assert_eq(r.stdout, "hi")
    assert_eq(f.ch.send_window, before + 4096, "the adjust reached the sftp channel:")
end)

test("sftp data that arrives during an exec is kept for the sftp session", function()
    -- The reply to the sftp request below arrives in the middle of the exec.
    -- It must wait for the session, not be dropped or handed to the command.
    local s = fake_stream(conf() .. ok_reply() .. s_version()
                          .. exec_on_ch1(s_status(1, 0)), 9)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    local r = assert(t:exec("echo hi"))
    assert_eq(r.stdout, "hi")
    assert_eq(f:remove("/x"), true, "the queued reply answers the request:")
end)

test("sftp data that arrives while an exec channel opens is not dropped", function()
    -- open_session used to skip whatever came before its confirmation.
    local mid = s_status(1, 0)
    local s = fake_stream(conf() .. ok_reply() .. s_version()
        .. mid .. exec_on_ch1(""), 9)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    assert(t:exec("echo hi"))
    assert_eq(f:remove("/x"), true)
end)

test("a keepalive on the sftp channel during an exec is answered at once", function()
    -- OpenSSH disconnects a client that leaves keepalive@openssh.com
    -- unanswered; the command's reader must answer for the sftp channel too.
    local ka = on(wire.writer():byte(98):uint32(0):string("keepalive@openssh.com")
                      :boolean(true))
    local s = fake_stream(conf() .. ok_reply() .. s_version() .. exec_on_ch1(ka), 9)
    local t = transport.new(s, stub_crypto())
    assert(t:sftp())
    assert(t:exec("echo hi"))
    local answered = false
    for _, pkt in ipairs(s.written) do
        local payload = packet.parse(pkt, 8)
        if payload and payload:byte(1) == 100
           and string.unpack(">I4", payload, 2) == 7 then
            answered = true
        end
    end
    assert_eq(answered, true, "CHANNEL_FAILURE to the sftp channel (server id 7):")
end)

-- An sftp session that stops being answered. The connection stays alive (a
-- real server keeps answering keepalives, so its idle bound never fires);
-- what ends the wait is the session's reply deadline. This stream reports a
-- deadline once its script is spent and one is armed, as the real stream does
-- when the time runs out.
local function stalling_stream(inbound)
    local s = fake_stream(inbound, 9)
    s.deadlines = {}
    s.armed = 0
    s.deadline = function(self, ms)
        self.deadlines[#self.deadlines + 1] = ms
        self.armed = ms
    end
    local read = s.read
    s.read = function(self, n)
        if self._pos > #self._in and self.armed > 0 then
            return nil, "deadline reached", "deadline"
        end
        return read(self, n)
    end
    return s
end

test("an sftp request nobody answers times out and closes the session", function()
    local s = stalling_stream(conf() .. ok_reply() .. s_version())
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp({ reply_timeout_ms = 1000 }))
    local ok, err = pcall(f.stat, f, "/x")
    assert_eq(ok, false)
    assert_eq(type(err) == "table" and err.code, "timeout")
    -- The session is not used again: its replies could still arrive.
    local ok2, err2 = pcall(f.stat, f, "/y")
    assert_eq(ok2, false)
    assert_eq(err2.code, "timeout")
    -- Its channel was closed (CHANNEL_CLOSE to the server's id 7)...
    local closed = false
    for _, pkt in ipairs(s.written) do
        local payload = packet.parse(pkt, 8)
        if payload and payload:byte(1) == 97 and string.unpack(">I4", payload, 2) == 7 then
            closed = true
        end
    end
    assert_eq(closed, true, "CHANNEL_CLOSE sent:")
    -- ...and since the server did not close it back, the connection is done.
    assert_eq(t.dead and t.dead.code, "timeout")
end)

test("the sftp reply deadline is armed per message and cleared after", function()
    local s = stalling_stream(conf() .. ok_reply() .. s_version() .. s_status(1, 0))
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp({ reply_timeout_ms = 1000 }))
    assert_eq(f:remove("/x"), true)
    assert_eq(s.armed, 0, "no deadline left armed:")
    local armed = 0
    for _, ms in ipairs(s.deadlines) do if ms == 1000 then armed = armed + 1 end end
    assert_eq(armed >= 2, true, "armed for the version and for the reply:")
end)

test("reply_timeout_ms is checked", function()
    for _, bad in ipairs({ -1, 1.5, "60", 86400001 }) do
        local s = fake_stream(conf() .. ok_reply() .. s_version(), 9)
        local t = transport.new(s, stub_crypto())
        local ok, err = pcall(t.sftp, t, { reply_timeout_ms = bad })
        assert_eq(ok, false, tostring(bad))
        assert_eq(err.code, "bad_argument")
    end
end)

test("closing sftp twice sends one CLOSE", function()
    local s = fake_stream(conf() .. ok_reply() .. s_version() .. fin(), 9)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    f:close()
    f:close()
    local closes = 0
    for _, ty in ipairs(written_types(s)) do
        if ty == 97 then closes = closes + 1 end
    end
    assert_eq(closes, 1)
end)

-- The CHANNEL_DATA sizes we sent, in order.
local function sent_data_sizes(s)
    local out = {}
    for _, p in ipairs(s.written) do
        local payload = packet.parse(p, 8)
        if payload and payload:byte(1) == 94 then
            local r = wire.reader(payload)
            r:byte(); r:uint32()
            out[#out + 1] = #r:string()
        end
    end
    return out
end

test("an sftp write fits the peer's packet size", function()
    -- A 16 KiB chunk plus its header went out as ONE message, and the channel
    -- refuses (rather than truncates) one over the peer's packet size.
    local s = fake_stream(conf(1048576, 8192) .. ok_reply() .. s_version()
        .. s_handle(1, "h") .. s_status(2, 0) .. s_status(3, 0), 11)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    assert_eq(f:write("/big", string.rep("z", 16384)), true)
    for _, n in ipairs(sent_data_sizes(s)) do
        assert_eq(n <= 8192, true, "sent " .. n .. " bytes in one message")
    end
end)

test("an sftp write waits for the window instead of raising", function()
    -- The window runs out part way through the write; the peer's adjust
    -- arrives afterwards, and the rest of the message follows it.
    local s = fake_stream(conf(2000, 32768) .. ok_reply() .. s_version()
        .. s_handle(1, "h") .. grant(1048576) .. s_status(2, 0) .. s_status(3, 0), 11)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    assert_eq(f:write("/w", string.rep("z", 5000)), true)
end)

test("exec stdin is chunked below our own limit, whatever the peer allows", function()
    -- A peer may advertise a packet size far above what our framing will
    -- send; a chunk sized to its number was refused on the way out.
    local s = fake_stream(conf(1048576, 262144) .. ok_reply() .. status(0)
                          .. eof() .. fin(), 7)
    local t = transport.new(s, stub_crypto())
    local r = t:exec("cat", { stdin = string.rep("y", 100 * 1024) })
    assert_eq(r.status, 0)
    local total = 0
    for _, n in ipairs(sent_data_sizes(s)) do
        assert_eq(n <= 32768, true, "sent " .. n .. " bytes in one message")
        total = total + n
    end
    assert_eq(total, 100 * 1024)
end)

test("an sftp write closes its handle and reads the answer on failure", function()
    local s = fake_stream(conf() .. ok_reply() .. s_version()
        .. s_handle(1, "hW") .. s_status(2, 4) .. s_status(3, 0)
        .. s_handle(4, "hR") .. s_data(5, "ok") .. s_status(6, 1)
        .. s_status(7, 0), 5)
    local t = transport.new(s, stub_crypto())
    local f = assert(t:sftp())
    local ok, err = f:write("/w", "data")
    assert_eq(ok, nil)
    -- Status 4 by name, not by text a caller would have to match.
    assert_eq(err.code, "failure")
    assert_eq(err.status, 4)
    -- The session is still in step: the next read gets its own replies.
    assert_eq(f:read("/r"), "ok")
end)

-- userauth --------------------------------------------------------------------
--
-- Driven over a plaintext stream with a session id set, as exec is above: what
-- is under test is which requests go out and which signatures are asked for.

local function accept_service() return plain(wire.writer():byte(6):string("ssh-userauth"):build()) end
local function auth_failure(methods, partial)
    return plain(wire.writer():byte(51):namelist(methods):boolean(partial or false):build())
end
local function auth_success() return plain("\52") end

-- The public key algorithm named in each USERAUTH_REQUEST written.
local function requested_algorithms(s)
    local out = {}
    for _, w in ipairs(s.written) do
        local payload = w:sub(1, 4) ~= "SSH-" and packet.parse(w, 8)
        if payload and payload:byte(1) == 50 then
            local r = wire.reader(payload)
            r:byte(); r:string(); r:string()
            if r:string() == "publickey" then
                r:boolean()
                out[#out + 1] = r:string()
            end
        end
    end
    return table.concat(out, ",")
end

local function auth_crypto(signed)
    local c = stub_crypto()
    c.sign = function(alg) signed[#signed + 1] = alg; return string.rep("\7", 256) end
    c.ed25519_sign = function() signed[#signed + 1] = "ed25519"; return string.rep("\8", 64) end
    return c
end

local RSA_KEY = { algorithm = "ssh-rsa", pem = "PEM",
                  blob = wire.writer():string("ssh-rsa"):mpint("\1\0\1"):mpint("\255"):build() }
local ED_KEY = { algorithm = "ssh-ed25519", secret = string.rep("\3", 64),
                 blob = wire.writer():string("ssh-ed25519"):string(string.rep("\4", 32)):build() }

local function authenticating(inbound, signed)
    local s = fake_stream(inbound, 64)
    local t = transport.new(s, auth_crypto(signed))
    t.session_id = "SESSION"
    return t, s
end

test("an RSA key refused under SHA-512 is offered again under SHA-256", function()
    local signed = {}
    local t, s = authenticating(accept_service() .. auth_failure({ "publickey" })
                                .. auth_success(), signed)
    assert_eq(t:authenticate("u", RSA_KEY), true)
    assert_eq(table.concat(signed, ","), "RS512,RS256")
    assert_eq(requested_algorithms(s), "rsa-sha2-512,rsa-sha2-256")
end)

test("an RSA key accepted under SHA-512 is signed once", function()
    local signed = {}
    local t, s = authenticating(accept_service() .. auth_success(), signed)
    assert_eq(t:authenticate("u", RSA_KEY), true)
    assert_eq(table.concat(signed, ","), "RS512")
    assert_eq(requested_algorithms(s), "rsa-sha2-512")
end)

test("no second hash once the server stops offering publickey", function()
    local signed = {}
    local t, s = authenticating(accept_service() .. auth_failure({ "password" }), signed)
    local ok, err = t:authenticate("u", RSA_KEY)
    assert_eq(ok, nil)
    assert_eq(err.code, "auth_failed")
    assert_eq(requested_algorithms(s), "rsa-sha2-512")
end)

test("partial success is not retried under another hash", function()
    -- The signature was accepted; the server wants another factor.
    local signed = {}
    local t, s = authenticating(accept_service() .. auth_failure({ "publickey", "password" }, true),
                                signed)
    local ok, err = t:authenticate("u", RSA_KEY)
    assert_eq(ok, nil)
    assert_eq(err.code, "partial_success")
    assert_eq(requested_algorithms(s), "rsa-sha2-512")
end)

test("both RSA hashes refused is an ordinary auth failure", function()
    local signed = {}
    local t = authenticating(accept_service() .. auth_failure({ "publickey" })
                             .. auth_failure({ "publickey" }), signed)
    local ok, err = t:authenticate("u", RSA_KEY)
    assert_eq(ok, nil)
    assert_eq(err.code, "auth_failed")
    assert_eq(table.concat(signed, ","), "RS512,RS256")
end)

local EC_KEY = { algorithm = "ecdsa-sha2-nistp384", pem = "PEM",
                 blob = wire.writer():string("ecdsa-sha2-nistp384"):string("nistp384")
                                    :string("\4" .. string.rep("\5", 96)):build() }

test("an ECDSA key signs under its own curve's hash, once", function()
    local signed = {}
    local c = auth_crypto(signed)
    c.sign = function(alg) signed[#signed + 1] = alg; return string.rep("\9", 96) end
    local s = fake_stream(accept_service() .. auth_success(), 64)
    local t = transport.new(s, c)
    t.session_id = "SESSION"
    assert_eq(t:authenticate("u", EC_KEY), true)
    assert_eq(table.concat(signed, ","), "ES384")
    assert_eq(requested_algorithms(s), "ecdsa-sha2-nistp384")
end)

test("a refused ECDSA key is not retried", function()
    local signed = {}
    local c = auth_crypto(signed)
    c.sign = function(alg) signed[#signed + 1] = alg; return string.rep("\9", 96) end
    local s = fake_stream(accept_service() .. auth_failure({ "publickey" }), 64)
    local t = transport.new(s, c)
    t.session_id = "SESSION"
    local ok, err = t:authenticate("u", EC_KEY)
    assert_eq(ok, nil)
    assert_eq(err.code, "auth_failed")
    assert_eq(requested_algorithms(s), "ecdsa-sha2-nistp384")
end)

test("an Ed25519 key is offered once, as before", function()
    local signed = {}
    local t, s = authenticating(accept_service() .. auth_failure({ "publickey" }), signed)
    local ok, err = t:authenticate("u", ED_KEY)
    assert_eq(ok, nil)
    assert_eq(err.code, "auth_failed")
    assert_eq(table.concat(signed, ","), "ed25519")
    assert_eq(requested_algorithms(s), "ssh-ed25519")
end)

return {pass = pass, fail = fail}

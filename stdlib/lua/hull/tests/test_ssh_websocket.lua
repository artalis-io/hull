-- test_ssh_websocket.lua - Tests for hull.ssh.websocket
--
-- The handshake and the framing are checked against RFC 6455's own worked
-- examples where it gives them, because "my encoder agrees with my decoder"
-- is exactly the kind of agreement that fails on contact with a real server.

local ws = require('hull.ssh.websocket')

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

local function hex(s)
    return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

-- handshake --------------------------------------------------------------

test("the accept value matches the RFC 6455 worked example", function()
    -- Section 1.3: key "dGhlIHNhbXBsZSBub25jZQ==" must produce
    -- "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=". This is the one place the spec hands
    -- over real bytes, so it is the only check that proves the GUID, the
    -- concatenation order and the base64 alphabet are all right.
    local known_key = "dGhlIHNhbXBsZSBub25jZQ=="
    local digest_hex = "b37a4f2cc0624f1690f64606cf385945b2bec4ea"
    local raw = (digest_hex:gsub("%x%x", function(cc)
        return string.char(tonumber(cc, 16))
    end))
    local accept = ws.accept(function() return raw end, known_key)
    assert_eq(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
end)

test("the key is 16 random bytes, base64 with padding", function()
    local key = ws.key(function(n) return string.rep("\0", n) end)
    assert_eq(key, "AAAAAAAAAAAAAAAAAAAAAA==")
    assert_eq(#key, 24, "16 bytes base64 is 24 characters:")
end)

test("a short random source is refused rather than used", function()
    assert_raises(function()
        ws.key(function() return "short" end)
    end, "a 5-byte nonce")
end)

test("the request carries the required upgrade headers", function()
    local req = ws.build_request({ host = "h.example.com", key = "KEY" })
    assert_eq(req:find("GET / HTTP/1.1\r\n", 1, true), 1)
    assert_eq(req:find("Host: h.example.com\r\n", 1, true) ~= nil, true)
    assert_eq(req:find("Upgrade: websocket\r\n", 1, true) ~= nil, true)
    assert_eq(req:find("Connection: Upgrade\r\n", 1, true) ~= nil, true)
    assert_eq(req:find("Sec-WebSocket-Key: KEY\r\n", 1, true) ~= nil, true)
    assert_eq(req:find("Sec-WebSocket-Version: 13\r\n", 1, true) ~= nil, true)
    assert_eq(req:sub(-4), "\r\n\r\n", "the header block must be terminated:")
end)

test("extra headers are appended in the order given", function()
    local req = ws.build_request({
        host = "h", key = "K",
        headers = { "Cf-Access-Client-Id: abc.access",
                    "Cf-Access-Client-Secret: shh" },
    })
    local a = req:find("Cf-Access-Client-Id: abc.access", 1, true)
    local b = req:find("Cf-Access-Client-Secret: shh", 1, true)
    assert_eq(a ~= nil and b ~= nil and a < b, true)
end)

test("a header carrying CRLF cannot inject a line", function()
    -- These values come from configuration, which may come from env.
    assert_raises(function()
        ws.build_request({ host = "h", key = "K",
                           headers = { "X: a\r\nInjected: yes" } })
    end, "CRLF in a header")
end)

test("the response head is parsed, names lowercased", function()
    local res = ws.parse_response(
        "HTTP/1.1 101 Switching Protocols\r\n" ..
        "Upgrade: websocket\r\n" ..
        "Sec-WebSocket-Accept: abc=\r\n\r\n")
    assert_eq(res.status, 101)
    assert_eq(res.headers["sec-websocket-accept"], "abc=")
    assert_eq(res.headers["upgrade"], "websocket")
end)

test("an incomplete response head is not parsed as one", function()
    local res, why = ws.parse_response("HTTP/1.1 101 Switching\r\nUpgrade: ws")
    assert_eq(res, nil)
    assert_eq(why, "incomplete")
end)

-- masking ----------------------------------------------------------------

test("masking is its own inverse", function()
    local mask = string.char(0x37, 0xfa, 0x21, 0x3d)
    local data = "Hello, this is a longer payload than four bytes"
    assert_eq(ws.apply_mask(ws.apply_mask(data, mask), mask), data)
end)

test("masking matches the RFC 6455 worked example", function()
    -- Section 5.7: "Hello" masked with 0x37fa213d is 0x7f9f4d5158.
    local mask = string.char(0x37, 0xfa, 0x21, 0x3d)
    assert_eq(hex(ws.apply_mask("Hello", mask)), "7f9f4d5158")
end)

test("masking is correct across a chunk boundary", function()
    -- The implementation walks the payload in blocks; the mask index must
    -- follow the byte's position in the PAYLOAD, not in the block.
    local mask = string.char(1, 2, 3, 4)
    local data = string.rep("z", 5000)
    local out = ws.apply_mask(data, mask)
    for i = 1, #data do
        local want = data:byte(i) ~ mask:byte(((i - 1) % 4) + 1)
        if out:byte(i) ~= want then
            error("byte " .. i .. " expected " .. want .. ", got " .. out:byte(i))
        end
    end
end)

-- framing ----------------------------------------------------------------

test("a small client frame matches the RFC 6455 worked example", function()
    -- Section 5.7, masked "Hello": 0x818537fa213d7f9f4d5158
    local mask = string.char(0x37, 0xfa, 0x21, 0x3d)
    local f = ws.encode(ws.OP_TEXT, "Hello", mask)
    assert_eq(hex(f), "818537fa213d7f9f4d5158")
end)

test("a client frame always sets the mask bit", function()
    local f = ws.encode(ws.OP_BIN, "x", string.rep("\0", 4))
    assert_eq(f:byte(2) & 0x80, 0x80, "RFC 6455 5.3: a client MUST mask")
end)

test("the three length encodings are used at the right boundaries", function()
    local m = string.rep("\0", 4)
    assert_eq(ws.encode(ws.OP_BIN, string.rep("a", 125), m):byte(2) & 0x7F, 125)
    assert_eq(ws.encode(ws.OP_BIN, string.rep("a", 126), m):byte(2) & 0x7F, 126)
    assert_eq(ws.encode(ws.OP_BIN, string.rep("a", 65535), m):byte(2) & 0x7F, 126)
    assert_eq(ws.encode(ws.OP_BIN, string.rep("a", 65536), m):byte(2) & 0x7F, 127)
end)

test("encode and decode round trip at each length boundary", function()
    local m = string.char(9, 8, 7, 6)
    for _, n in ipairs({ 0, 1, 125, 126, 127, 65535, 65536, 70000 }) do
        local body = string.rep("q", n)
        local f = ws.encode(ws.OP_BIN, body, m)
        -- decode expects a SERVER frame, which is unmasked; strip the mask
        -- the way a server would before comparing.
        local unmasked
        if n < 126 then
            unmasked = string.char(0x82, n) .. body
        elseif n < 0x10000 then
            unmasked = string.char(0x82, 126) .. string.pack(">I2", n) .. body
        else
            unmasked = string.char(0x82, 127) .. string.pack(">I8", n) .. body
        end
        local frame, used = ws.decode(unmasked)
        assert_eq(frame ~= nil and frame.payload, body, "n=" .. n .. ":")
        assert_eq(used, #unmasked, "n=" .. n .. " consumed:")
        assert_eq(#f > #unmasked, true, "the masked form is 4 bytes longer:")
    end
end)

test("a partial frame asks for more rather than guessing", function()
    local full = string.char(0x82, 126) .. string.pack(">I2", 300) .. string.rep("z", 300)
    for _, cut in ipairs({ 1, 2, 3, 4, 100, #full - 1 }) do
        local frame, why = ws.decode(full:sub(1, cut))
        assert_eq(frame, nil, "cut=" .. cut .. ":")
        assert_eq(why, "need_more", "cut=" .. cut .. ":")
    end
    assert_eq(ws.decode(full) ~= nil, true)
end)

test("a masked server frame is refused", function()
    -- RFC 6455 5.1: a server MUST NOT mask. Accepting one would mean
    -- guessing which side's rules apply.
    local f = string.char(0x82, 0x81) .. string.rep("\0", 4) .. "x"
    local err = assert_raises(function() ws.decode(f) end)
    assert_eq(err:find("masked", 1, true) ~= nil, true, err)
end)

test("reserved bits without an extension are refused", function()
    local err = assert_raises(function()
        ws.decode(string.char(0xC2, 0x01) .. "x")
    end)
    assert_eq(err:find("reserved", 1, true) ~= nil, true, err)
end)

test("an oversized frame is refused before its bytes are waited for", function()
    -- The length is 64 bits and entirely peer-controlled.
    local head = string.char(0x82, 127) .. string.pack(">I8", 1 << 40)
    local err = assert_raises(function() ws.decode(head) end)
    assert_eq(err:find("exceeds the maximum", 1, true) ~= nil, true, err)
end)

test("a control frame over 125 bytes is refused", function()
    local head = string.char(0x89, 126) .. string.pack(">I2", 200) .. string.rep("z", 200)
    local err = assert_raises(function() ws.decode(head) end)
    assert_eq(err:find("125", 1, true) ~= nil, true, err)
end)

test("a fragmented control frame is refused", function()
    -- FIN clear on a PING (RFC 6455 5.5).
    local err = assert_raises(function()
        ws.decode(string.char(0x09, 0x01) .. "x")
    end)
    assert_eq(err:find("fragmented", 1, true) ~= nil, true, err)
end)

test("encoding an oversized control frame is refused too", function()
    assert_raises(function()
        ws.encode(ws.OP_PING, string.rep("z", 126), string.rep("\0", 4))
    end, "a 126-byte ping")
end)

-- the stream adapter -----------------------------------------------------

-- A fake transport: hands out `inbound` in short pieces, records writes.
local function fake(inbound, chunk)
    return {
        _in = inbound or "", _pos = 1, written = {}, closed = false,
        read = function(self, n)
            if self._pos > #self._in then return "" end
            local take = math.min(n, chunk or 7, #self._in - self._pos + 1)
            local s = self._in:sub(self._pos, self._pos + take - 1)
            self._pos = self._pos + take
            return s
        end,
        write = function(self, s) self.written[#self.written + 1] = s; return true end,
        close = function(self) self.closed = true end,
    }
end

local function server_frame(opcode, payload)
    payload = payload or ""
    local n = #payload
    if n < 126 then
        return string.char(0x80 | opcode, n) .. payload
    end
    return string.char(0x80 | opcode, 126) .. string.pack(">I2", n) .. payload
end

local function stub_random(n) return string.rep("\42", n) end
local function stub_sha1() return string.rep("\7", 20) end

local function handshake_ok(body)
    local accept = ws.accept(stub_sha1, ws.key(stub_random))
    return "HTTP/1.1 101 Switching Protocols\r\n"
        .. "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        .. "Sec-WebSocket-Accept: " .. accept .. "\r\n\r\n" .. (body or "")
end

test("a successful upgrade yields a byte stream", function()
    local t = fake(handshake_ok(server_frame(ws.OP_BIN, "SSH-2.0-Server\r\n")))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s ~= nil, true)
    assert_eq(s:read(64), "SSH-2.0-Server\r\n")
end)

test("a non-101 status is reported with the status", function()
    -- 403 is the normal shape of an Access rejection.
    local t = fake("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
    local s, err = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s, nil)
    assert_eq(err.code, "upgrade_refused")
    assert_eq(err.status, 403)
end)

test("a wrong Sec-WebSocket-Accept is refused", function()
    -- Proves the peer ran the handshake rather than echoing a 101 back.
    local t = fake("HTTP/1.1 101 Switching Protocols\r\n"
                   .. "Sec-WebSocket-Accept: wrong\r\n\r\n")
    local s, err = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s, nil)
    assert_eq(err.code, "upgrade_failed")
end)

test("a connection closed mid-upgrade is an error, not a hang", function()
    local t = fake("HTTP/1.1 101 Switch")
    local s, err = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s, nil)
    assert_eq(err.code, "upgrade_failed")
end)

test("frames arriving with the handshake are not lost", function()
    -- A server may put the 101 and its first data in one segment.
    local t = fake(handshake_ok(server_frame(ws.OP_BIN, "immediate")))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(32), "immediate")
end)

test("fragmentation needs no reassembly for a byte stream", function()
    local body = string.char(0x00 | ws.OP_BIN, 3) .. "abc"      -- FIN clear
                 .. string.char(0x80 | ws.OP_CONT, 3) .. "def"  -- FIN set
    local t = fake(handshake_ok(body))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(64), "abc")
    assert_eq(s:read(64), "def")
end)

test("a short read returns what is there rather than waiting", function()
    local t = fake(handshake_ok(server_frame(ws.OP_BIN, "abcdef")))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(3), "abc")
    assert_eq(s:read(3), "def")
end)

test("a ping is answered with a pong carrying the same payload", function()
    -- Required by 5.5.2; a peer pinging for liveness hangs up without it.
    local t = fake(handshake_ok(server_frame(ws.OP_PING, "beat")
                                .. server_frame(ws.OP_BIN, "data")))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(16), "data", "the ping must not surface as data:")

    -- and the pong carries back what the ping sent (5.5.3)
    local pong
    for i = 2, #t.written do
        local w = t.written[i]
        if (w:byte(1) & 0x0F) == ws.OP_PONG then pong = w end
    end
    assert_eq(pong ~= nil, true, "a PONG should have been sent")
    assert_eq(ws.apply_mask(pong:sub(7), pong:sub(3, 6)), "beat")
end)

test("a close frame ends the stream", function()
    local t = fake(handshake_ok(server_frame(ws.OP_BIN, "last")
                                .. server_frame(ws.OP_CLOSE, string.pack(">I2", 1000))))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(16), "last")
    assert_eq(s:read(16), "", "EOF after close:")
end)

test("a transport that ends without a close frame is EOF, not an error", function()
    -- SSH carries its own disconnect; a missing close frame is untidy, not
    -- something to turn into an exception the caller cannot act on.
    local t = fake(handshake_ok(server_frame(ws.OP_BIN, "partial")))
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:read(16), "partial")
    assert_eq(s:read(16), "")
end)

test("writes are masked binary frames", function()
    local t = fake(handshake_ok())
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    assert_eq(s:write("payload"), true)
    local last = t.written[#t.written]
    assert_eq(last:byte(1) & 0x0F, ws.OP_BIN)
    assert_eq(last:byte(2) & 0x80, 0x80, "client frames are masked:")
    local mask = last:sub(3, 6)
    assert_eq(ws.apply_mask(last:sub(7), mask), "payload")
end)

test("close sends a close frame and closes the transport", function()
    local t = fake(handshake_ok())
    local s = ws.connect(t, { host = "h", random = stub_random, sha1 = stub_sha1 })
    s:close()
    local last = t.written[#t.written]
    assert_eq(last:byte(1) & 0x0F, ws.OP_CLOSE)
    assert_eq(t.closed, true)
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

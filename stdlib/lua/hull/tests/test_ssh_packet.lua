-- test_ssh_packet.lua - Tests for hull.ssh.packet
--
-- Version exchange (RFC 4253 section 4.2) and the binary packet protocol
-- (section 6). The length-field cases are the point: that is where a peer
-- first gets to choose a number Hull would otherwise allocate against.

local packet = require('hull.ssh.packet')

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

-- deterministic padding so a framed packet is reproducible
local function zeros(n) return string.rep("\0", n) end

-- identification string ------------------------------------------------

test("build_ident produces an RFC 4253 line", function()
    assert_eq(packet.build_ident("Hull_0.15"), "SSH-2.0-Hull_0.15\r\n")
end)

test("build_ident refuses characters that break the framing", function()
    -- SP separates software from comments and minus separates the fields, so
    -- either inside the software version would re-split the line.
    assert_raises(function() packet.build_ident("Hull 0.15") end, "space")
    assert_raises(function() packet.build_ident("Hull-0.15") end, "minus")
    assert_raises(function() packet.build_ident("Hull\r\n") end, "crlf")
    assert_raises(function() packet.build_ident("") end, "empty")
end)

test("parse_ident accepts a plain line", function()
    local id = packet.parse_ident("SSH-2.0-OpenSSH_9.6")
    assert_eq(id.protoversion, "2.0")
    assert_eq(id.software, "OpenSSH_9.6")
    assert_eq(id.comments, nil)
end)

test("parse_ident splits comments on the first space", function()
    local id = packet.parse_ident("SSH-2.0-OpenSSH_9.6 Ubuntu-3")
    assert_eq(id.software, "OpenSSH_9.6")
    assert_eq(id.comments, "Ubuntu-3")
end)

test("parse_ident rejects a non-identification line", function()
    -- A server may send banner lines first; the caller has to be able to tell
    -- them apart rather than have this guess.
    local id, err = packet.parse_ident("hello there")
    assert_eq(id, nil)
    assert_eq(err, "not an identification string")
end)

test("parse_ident rejects protocol versions we do not speak", function()
    local id = packet.parse_ident("SSH-1.99-OldServer")
    assert_eq(id, nil)
    local id2 = packet.parse_ident("SSH-1.5-Ancient")
    assert_eq(id2, nil)
end)

test("parse_ident rejects an over-long line", function()
    local id = packet.parse_ident("SSH-2.0-" .. string.rep("x", 300))
    assert_eq(id, nil)
end)

-- padding ---------------------------------------------------------------

test("padding aligns the whole packet to the block size", function()
    for _, block in ipairs({ 8, 16 }) do
        for payload_len = 0, 40 do
            local pad = packet.padding_for(payload_len, block, true)
            assert_eq(pad >= 4, true, "padding at least 4 for " .. payload_len)
            assert_eq((4 + 1 + payload_len + pad) % block, 0,
                      "aligned for len " .. payload_len .. " block " .. block)
        end
    end
end)

test("padding excludes the length field for an AEAD", function()
    -- aes256-gcm carries the length as associated data, so it is not part of
    -- the encrypted region that has to align.
    for payload_len = 0, 40 do
        local pad = packet.padding_for(payload_len, 16, false)
        assert_eq(pad >= 4, true)
        assert_eq((1 + payload_len + pad) % 16, 0)
    end
end)

test("a block smaller than 8 is raised to 8", function()
    -- RFC 4253: alignment is to max(8, cipher block size).
    assert_eq(packet.padding_for(0, 1, true), packet.padding_for(0, 8, true))
end)

-- framing ----------------------------------------------------------------

test("frame and parse round trip", function()
    for _, payload in ipairs({ "", "x", string.rep("abc", 100) }) do
        local b = packet.frame(payload, 8, zeros)
        local got, used = packet.parse(b, 8)
        assert_eq(got, payload)
        assert_eq(used, #b)
    end
end)

test("a framed packet has the declared shape", function()
    local b = packet.frame("hi", 8, zeros)
    local packet_length = string.unpack(">I4", b, 1)
    local padding_length = string.byte(b, 5)
    assert_eq(#b, packet_length + 4)
    assert_eq(packet_length, 2 + padding_length + 1)
    assert_eq(#b % 8, 0)
end)

test("frame demands real randomness from its caller", function()
    assert_raises(function() packet.frame("x", 8, nil) end, "no rng")
    assert_raises(function()
        packet.frame("x", 8, function() return "short" end)
    end, "wrong length from rng")
end)

test("parse asks for more when the packet is incomplete", function()
    local b = packet.frame("hello world", 8, zeros)
    for n = 0, #b - 1 do
        local got, err = packet.parse(b:sub(1, n), 8)
        assert_eq(got, nil, "partial of " .. n .. " should not parse")
        assert_eq(err, "need_more")
    end
    assert_eq(packet.parse(b, 8), "hello world")
end)

test("parse leaves trailing bytes alone", function()
    local b = packet.frame("one", 8, zeros) .. packet.frame("two", 8, zeros)
    local first, used = packet.parse(b, 8)
    assert_eq(first, "one")
    local second = packet.parse(b:sub(used + 1), 8)
    assert_eq(second, "two")
end)

-- hostile length fields ---------------------------------------------------

test("an enormous declared length is refused immediately", function()
    -- Not "wait for 4 GB and then fail": the claim is impossible, so it is
    -- rejected before a single byte is buffered for it.
    local b = "\255\255\255\255" .. string.rep("\0", 32)
    assert_raises(function() packet.parse(b, 8) end, "huge length")
end)

test("a length below the minimum is refused", function()
    local b = string.pack(">I4", 3) .. string.rep("\0", 32)
    assert_raises(function() packet.parse(b, 8) end, "tiny length")
end)

test("a misaligned length is refused", function()
    -- 4 + packet_length must be a multiple of the block size; a peer that
    -- sends otherwise is not speaking the protocol.
    local b = string.pack(">I4", 13) .. string.rep("\0", 32)
    assert_raises(function() packet.parse(b, 8) end, "misaligned")
end)

test("padding below the minimum is refused", function()
    local body = string.char(3) .. string.rep("\0", 11)   -- padding_length = 3
    local b = string.pack(">I4", #body) .. body
    assert_raises(function() packet.parse(b, 8) end, "short padding")
end)

test("padding larger than the packet is refused", function()
    -- padding_length 200 inside a 12-byte packet: payload length would be
    -- negative, which must not become a negative substring index.
    local body = string.char(200) .. string.rep("\0", 11)
    local b = string.pack(">I4", #body) .. body
    assert_raises(function() packet.parse(b, 8) end, "padding exceeds packet")
end)

test("the maximum packet size is enforced on frame too", function()
    assert_raises(function()
        packet.frame(string.rep("x", packet.MAX_PACKET), 8, zeros)
    end, "oversize frame")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}

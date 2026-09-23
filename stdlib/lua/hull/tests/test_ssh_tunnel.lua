-- test_ssh_tunnel.lua - reaching a host through a WebSocket relay.
--
-- The composition, not the protocol: ssh.connect must dial the RELAY, speak
-- the upgrade over it with the caller's headers, and hand what comes back to
-- the SSH transport unchanged. The SSH handshake itself is tested elsewhere,
-- so these drive it against a fake relay and assert on the bytes that relay
-- was sent - which is the part a tunnel gets wrong.

local ssh = require('hull.ssh')
local ws  = require('hull.web.ws-stream')

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

local function assert_match(s, pat, msg)
    if not tostring(s):find(pat, 1, true) then
        error((msg or "no match") .. ": " .. tostring(pat) .. " not in " .. tostring(s))
    end
end

-- Deterministic stand-ins: the nonce and the digest are the server's problem,
-- and fixing them lets a test state the exact accept value.
local function stub_random(n) return string.rep("\42", n) end
local function stub_sha1() return string.rep("\7", 20) end
local crypto_stub = { random = stub_random, sha1 = stub_sha1 }

local function relay(inbound)
    return {
        _in = inbound or "", _pos = 1, written = {}, closed = false, dialled = nil,
        read = function(self, n)
            if self._pos > #self._in then return "" end
            local take = math.min(n, #self._in - self._pos + 1)
            local s = self._in:sub(self._pos, self._pos + take - 1)
            self._pos = self._pos + take
            return s
        end,
        write = function(self, s) self.written[#self.written + 1] = s; return true end,
        close = function(self) self.closed = true end,
    }
end

local function upgraded(body)
    local accept = ws.accept(stub_sha1, ws.key(stub_random))
    return "HTTP/1.1 101 Switching Protocols\r\n"
        .. "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        .. "Sec-WebSocket-Accept: " .. accept .. "\r\n\r\n" .. (body or "")
end

-- Connect through a fake relay, returning (result, reason, relay).
local function connect_via(r, tunnel)
    local seen
    local _, err = ssh.connect{
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = tunnel or { host = "ssh.example.com" },
        open_stream = function(o) seen = o; return r end,
    }
    return err, seen
end

test("the relay is dialled, not the SSH host", function()
    local r = relay(upgraded())
    local _, seen = connect_via(r)
    -- host/port stay the SSH destination: they are what the grant, the host
    -- key and the login are about. The relay travels in `via`.
    assert_eq(seen.host, "spark-7468")
    assert_eq(seen.port, 22)
    assert_eq(seen.user, "operator")
    assert_eq(seen.via.host, "ssh.example.com")
    assert_eq(seen.via.port, 443)
    assert_eq(seen.via.tls, true, "a relay carries credentials; TLS is the default")
end)

test("tls = false is honoured, for a local shim", function()
    local r = relay(upgraded())
    local _, seen = connect_via(r, { host = "127.0.0.1", port = 8080, tls = false })
    assert_eq(seen.via.tls, false)
    assert_eq(seen.via.port, 8080)
end)

test("the upgrade request carries the caller's headers verbatim", function()
    local r = relay(upgraded())
    connect_via(r, {
        host = "ssh.example.com",
        headers = {
            "Cf-Access-Client-Id: abc.access",
            "Cf-Access-Client-Secret: shh",
            "Cf-Access-Jump-Destination: spark-7468:22",
        },
    })
    local req = r.written[1] or ""
    assert_match(req, "GET / HTTP/1.1\r\n")
    assert_match(req, "Host: ssh.example.com\r\n")
    assert_match(req, "Upgrade: websocket\r\n")
    assert_match(req, "Sec-WebSocket-Version: 13\r\n")
    assert_match(req, "Cf-Access-Client-Id: abc.access\r\n")
    assert_match(req, "Cf-Access-Client-Secret: shh\r\n")
    assert_match(req, "Cf-Access-Jump-Destination: spark-7468:22\r\n")
end)

test("a refused upgrade reports the status, not a manifest denial", function()
    -- 403 is the normal shape of an Access rejection, and it is not the
    -- manifest refusing. Flattening both to "denied" sends an operator to
    -- the wrong file.
    local r = relay("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
    local err = connect_via(r)
    assert_eq(err.code, "upgrade_refused")
    assert_eq(err.status, 403)
    assert_eq(r.closed, true, "the socket is ours to drop when the upgrade fails")
end)

test("a relay that is not a WebSocket server is not mistaken for one", function()
    local r = relay("HTTP/1.1 101 Switching Protocols\r\n"
                    .. "Sec-WebSocket-Accept: wrong\r\n\r\n")
    local err = connect_via(r)
    assert_eq(err.code, "upgrade_failed")
    assert_eq(r.closed, true)
end)

test("a dial that is refused keeps the refusal", function()
    local err
    do
        local _, e = ssh.connect{
            host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
            tunnel = { host = "ssh.example.com" },
            open_stream = function() return nil, "tunnel host is not in ssh.tunnel.hosts" end,
        }
        err = e
    end
    assert_eq(err.code, "denied")
    assert_match(err.detail, "ssh.tunnel.hosts")
end)

test("a tunnel without a host is refused before anything is dialled", function()
    local dialled = false
    local ok = pcall(function()
        ssh.connect{
            host = "h", user = "u", key = {}, crypto = crypto_stub,
            tunnel = { port = 443 },
            open_stream = function() dialled = true; return relay(upgraded()) end,
        }
    end)
    assert_eq(ok, false, "a tunnel with no host should raise")
    assert_eq(dialled, false)
end)

test("bytes from inside the tunnel reach the SSH transport", function()
    -- The relay answers the upgrade and then frames an SSH identification
    -- string. Reaching the SSH layer at all proves ws-stream unwrapped it;
    -- what the transport then makes of a truncated conversation is its own
    -- suite's business, so this only asserts we got past the tunnel.
    local frame = string.char(0x80 | ws.OP_BIN, 16) .. "SSH-2.0-Fake\r\n\0\0"
    local r = relay(upgraded(frame))
    local err = connect_via(r)
    assert_eq(err ~= nil, true, "a truncated conversation still fails")
    assert_eq(err.code ~= "upgrade_failed", true, "but not at the upgrade")
    assert_eq(err.code ~= "upgrade_refused", true)
    assert_eq(err.code ~= "denied", true)
end)

return { pass = pass, fail = fail }

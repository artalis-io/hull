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

-- The destination header ------------------------------------------------------
--
-- The relay connects wherever its destination header says, while the manifest
-- grant is checked against `host`. So the header is written from the granted
-- host, and one the caller supplies must name exactly that.

local function count(s, pat)
    local n = 0
    for _ in s:gmatch(pat) do n = n + 1 end
    return n
end

test("the destination header is written from the granted host", function()
    local r = relay(upgraded())
    connect_via(r, { host = "ssh.example.com",
                     headers = { "Cf-Access-Client-Id: abc.access" } })
    assert_match(r.written[1] or "", "Cf-Access-Jump-Destination: spark-7468:22\r\n")
end)

test("a destination header naming another host is refused before dialling", function()
    -- The hole this closes: granted spark-7468, relayed to anywhere.
    local r = relay(upgraded())
    local err, seen = connect_via(r, { host = "ssh.example.com",
        headers = { "cf-access-jump-destination: db-prod:22" } })
    assert_eq(err.code, "denied")
    assert_match(err.detail, "granted for spark-7468:22")
    assert_eq(seen, nil, "nothing may be dialled")
end)

test("a destination header on another port is refused too", function()
    local r = relay(upgraded())
    local err = connect_via(r, { host = "ssh.example.com",
        headers = { "Cf-Access-Jump-Destination: spark-7468:2222" } })
    assert_eq(err.code, "denied")
end)

test("a matching destination header is kept once, not doubled", function()
    local r = relay(upgraded())
    connect_via(r, { host = "ssh.example.com",
        headers = { "CF-ACCESS-JUMP-DESTINATION:  spark-7468:22 " } })
    local req = r.written[1] or ""
    assert_eq(count(req:lower(), "cf%-access%-jump%-destination:"), 1)
end)

test("a relay that routes by another header names it", function()
    local r = relay(upgraded())
    connect_via(r, { host = "ssh.example.com", destination_header = "X-Target" })
    local req = r.written[1] or ""
    assert_match(req, "X-Target: spark-7468:22\r\n")
    assert_eq(req:find("Cf-Access-Jump-Destination", 1, true), nil)
end)

test("destination_header = false sends none, as the caller said", function()
    local r = relay(upgraded())
    connect_via(r, { host = "ssh.example.com", destination_header = false })
    assert_eq((r.written[1] or ""):find("Jump-Destination", 1, true), nil)
end)

test("an IPv6 destination is bracketed", function()
    local r = relay(upgraded())
    ssh.connect{
        host = "fd00::7", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = { host = "ssh.example.com" },
        open_stream = function() return r end,
    }
    assert_match(r.written[1] or "", "Cf-Access-Jump-Destination: [fd00::7]:22\r\n")
end)

test("a destination_header that is not a name is a caller error", function()
    local ok = pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = { host = "ssh.example.com", destination_header = 7 },
    })
    assert_eq(ok, false)
end)

-- The connection handle ------------------------------------------------------
--
-- ssh.connect over a substitute transport, so the handle can be inspected
-- without a server. What is asserted is what an application can REACH.

local function connect_with_fake_transport()
    local transport = require('hull.ssh.transport')
    local fake = {
        host_fingerprint = "SHA256:fake",
        handshake    = function() return true end,
        set_deadline = function() end,
        authenticate = function() return true end,
        close        = function() end,
        send_packet  = function() error("must not be reachable") end,
        exec  = function(_, c) return { status = 0, stdout = c, stderr = "" } end,
        sftp  = function(self)
            return { t = self, read = function(_, p) return "contents of " .. p end }
        end,
    }
    local real_new = transport.new
    transport.new = function() return fake end
    local ok, conn, err = pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        open_stream = function() return {} end,
    })
    transport.new = real_new
    assert(ok, conn)
    return assert(conn, err and err.code)
end

test("a connection exposes its methods, not its transport", function()
    -- conn.t used to BE the transport: conn.t:send_packet could open a
    -- direct-tcpip channel through the server, and conn.t.stream was the raw
    -- socket the binding keeps from applications.
    local conn = connect_with_fake_transport()
    assert_eq(conn.t, nil, "conn.t")
    assert_eq(next(conn), nil, "the handle has no fields at all")
    assert_eq(getmetatable(conn), false, "and its metatable is not handed out")
    assert_eq(conn:exec("uptime").stdout, "uptime", "the methods still work:")
    assert_eq(conn:fingerprint(), "SHA256:fake")
end)

test("an sftp session exposes its methods, not the transport under it", function()
    local conn = connect_with_fake_transport()
    local f = conn:sftp()
    assert_eq(f.t, nil, "sftp.t")
    assert_eq(next(f), nil, "the handle has no fields at all")
    assert_eq(f:read("/etc/motd"), "contents of /etc/motd")
end)

test("connect hands the port to the host-key check", function()
    local transport = require('hull.ssh.transport')
    local seen
    local real_new = transport.new
    transport.new = function()
        return { handshake = function(_, o) seen = o; return nil, { code = "stop" } end,
                 set_deadline = function() end,
                 close = function() end }
    end
    pcall(ssh.connect, { host = "spark-7468", port = 2222, user = "operator",
                         key = {}, crypto = crypto_stub,
                         open_stream = function() return {} end })
    transport.new = real_new
    assert_eq(seen and seen.port, 2222)
end)

test("accept_host and forget_host name the entry by host and port", function()
    local trust = ssh.memory_store()
    ssh.accept_host(trust, "Spark-7468", "BLOB22")
    ssh.accept_host(trust, "spark-7468", "BLOB2222", 2222)
    local e = trust.entries()
    assert_eq(e["spark-7468"], "BLOB22")
    assert_eq(e["[spark-7468]:2222"], "BLOB2222")
    ssh.forget_host(trust, "spark-7468", 2222)
    assert_eq(trust.entries()["[spark-7468]:2222"], nil)
    assert_eq(trust.entries()["spark-7468"], "BLOB22", "port 22 untouched:")
end)

-- The error model --------------------------------------------------------------
--
-- Every method returns (nil, { code, detail }) on failure; nothing raises at
-- the caller except a raise from the caller's own callback.

local function conn_over(fake_methods)
    local transport = require('hull.ssh.transport')
    local fake = {
        handshake    = function() return true end,
        authenticate = function() return true end,
        set_deadline = function() end,
        close        = function() end,
    }
    for k, v in pairs(fake_methods) do fake[k] = v end
    local real_new = transport.new
    transport.new = function() return fake end
    local conn = ssh.connect{ host = "spark-7468", user = "operator", key = {},
                              crypto = crypto_stub,
                              open_stream = function() return {} end }
    transport.new = real_new
    return conn
end

test("a transport failure inside exec comes back as io_error, not a raise", function()
    local conn = conn_over({ exec = function() error("ssh: read failed: reset") end })
    local r, err = conn:exec("uptime")
    assert_eq(r, nil)
    assert_eq(err.code, "io_error")
    assert_match(err.detail, "read failed")
end)

test("a coded failure keeps its code", function()
    local conn = conn_over({ exec = function()
        error({ code = "timeout", detail = "nothing from the server" }, 0) end })
    local _, err = conn:exec("uptime")
    assert_eq(err.code, "timeout")
end)

test("the caller's own callback error is raised, unchanged", function()
    -- A bug in on_stdout is the caller's to see, not a connection failure.
    local conn = conn_over({ exec = function(_, _, o)
        local ok, e = pcall(o.on_stdout, "x")
        if not ok then error({ app_error = e }, 0) end
    end })
    local ok, e = pcall(conn.exec, conn, "x", {
        on_stdout = function() error("my bug", 0) end })
    assert_eq(ok, false)
    assert_eq(e, "my bug")
end)

test("an sftp file is a handle too, and its errors come back coded", function()
    local file = {
        s = "the session", handle = "h",
        read  = function() error("ssh: read failed: reset") end,
        write = function() return true end,
        close = function() return true end,
    }
    local conn = conn_over({ sftp = function()
        return { t = "the transport", open = function() return file end }
    end })
    local f = assert(conn:sftp():open("/x", "r"))
    assert_eq(next(f), nil, "no fields on the handle:")
    assert_eq(getmetatable(f), false)
    assert_eq(f:write("data"), true)
    local d, err = f:read(10)
    assert_eq(d, nil)
    assert_eq(err.code, "io_error")
end)

test("an unreadable key comes back as bad_key, not a raise", function()
    local _, err = ssh.connect{ host = "h", user = "u", key = "not a key",
                                crypto = crypto_stub,
                                open_stream = function() error("must not dial") end }
    assert_eq(err.code, "bad_key")
end)

test("a key already trusted is an answer, not a raise", function()
    local trust = ssh.memory_store()
    assert_eq(ssh.accept_host(trust, "h", "B1"), true)
    local ok, err = ssh.accept_host(trust, "h", "B2")
    assert_eq(ok, nil)
    assert_eq(err.code, "already_trusted")
end)

test("a host that cannot be reached is connect_failed, not denied", function()
    -- `denied` means the manifest said no. A health check exists to tell
    -- that apart from a machine that is down.
    local _, err = ssh.connect{ host = "h", user = "u", key = {}, crypto = crypto_stub,
        open_stream = function() return nil, "connection refused", "connect_failed" end }
    assert_eq(err.code, "connect_failed")
    local _, err2 = ssh.connect{ host = "h", user = "u", key = {}, crypto = crypto_stub,
        open_stream = function() return nil, "timed out", "timeout" end }
    assert_eq(err2.code, "timeout")
end)

test("the handshake runs under the connect budget, reported as timeout", function()
    local budget
    local conn, err = (function()
        local transport = require('hull.ssh.transport')
        local real_new = transport.new
        transport.new = function()
            return { set_deadline = function(_, ms) budget = budget or ms end,
                     handshake = function() error({ code = "deadline" }, 0) end,
                     close = function() end }
        end
        local c, e = ssh.connect{ host = "h", user = "u", key = {}, crypto = crypto_stub,
                                  timeout_ms = 4000,
                                  open_stream = function() return {} end }
        transport.new = real_new
        return c, e
    end)()
    assert_eq(conn, nil)
    assert_eq(budget, 4000)
    assert_eq(err.code, "timeout")
    assert_match(err.detail, "4000 ms")
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
            open_stream = function() return nil, "tunnel host is not in ssh.tunnel.hosts", "denied" end,
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

-- test_ssh_tunnel.lua - reaching a host through a WebSocket relay.
--
-- The composition, not the protocol: ssh.connect must dial the RELAY, speak
-- the upgrade over it with the caller's headers, and hand what comes back to
-- the SSH transport unchanged. The SSH handshake itself is tested elsewhere,
-- so these drive it against a fake relay and assert on the bytes that relay
-- was sent - which is the part a tunnel gets wrong.

-- The transport measures nested deadlines on hull.time's clock, a C module
-- this vanilla Lua state (no capability layer) does not have.
package.preload['hull.time'] = package.preload['hull.time']
    or function() return { clock = function() return 0 end } end

local ssh = require('hull.ssh')
local ws  = require('hull.ssh.ws_stream')

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

-- ssh-ed25519 public key blobs, which accept_host now checks for.
local function ed_blob(byte)
    return require('hull.ssh.wire').writer():string("ssh-ed25519")
        :string(string.rep(byte, 32)):build()
end
local BLOB22, BLOB2222 = ed_blob("\1"), ed_blob("\2")
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

test("the well-known destination header is checked even when another is configured", function()
    -- Renaming the header, or turning it off, must not let the caller route
    -- the relay through the default one to a host the grant never named.
    for _, dh in ipairs({ false, "X-Target" }) do
        local r = relay(upgraded())
        local err, seen = connect_via(r, { host = "ssh.example.com",
            destination_header = dh,
            headers = { "Cf-Access-Jump-Destination: db-prod:22" } })
        assert_eq(err and err.code, "denied")
        assert_eq(seen, nil, "nothing may be dialled")
    end
end)

test("tunnel.headers may not set the upgrade's own headers", function()
    -- A caller Host routes a CDN-fronted relay to another origin (audit 21).
    for _, h in ipairs({ "Host: other.zone.example", "upgrade: x",
                         "Connection: close", "Sec-WebSocket-Key: AAAA" }) do
        local r = relay(upgraded())
        local err, seen = connect_via(r, { host = "ssh.example.com", headers = { h } })
        assert_eq(err and err.code, "denied", h)
        assert_eq(seen, nil, "nothing may be dialled: " .. h)
    end
end)

test("tunnel.path is origin-form only", function()
    -- An absolute-form target is routed by its own authority, not Host
    -- (RFC 9112 section 3.2.2): the relay was reached as another host
    -- (audit 22).
    for _, p in ipairs({ "http://internal.example/", "//internal.example/",
                         "relay", "/a b", "/a\tb" }) do
        local ok, e = pcall(connect_via, relay(upgraded()),
                            { host = "ssh.example.com", path = p })
        assert_eq(ok, false, p)
        assert_match(tostring(e), "tunnel.path", p)
    end
    local r = relay(upgraded())
    connect_via(r, { host = "ssh.example.com", path = "/ssh?x=1" })
    assert_match(r.written[1] or "", "GET /ssh?x=1 HTTP/1.1\r\n")
end)

test("ssh_tunnel_audit44.lua_tunnel_options_read_once", function()
    -- A tunnel table whose fields answer differently on a later read: the
    -- first read of host / path passes the checks, later reads would name
    -- another relay and an absolute-form target. Whatever the options are
    -- read as, the dial and the upgrade must use exactly what was checked.
    local reads = {}
    local first = { host = "ssh.example.com", path = "/ok",
                    tls_hostname = "ssh.example.com" }
    local later = { host = "other.internal.example", path = "/other",
                    tls_hostname = "other.internal.example" }
    local proxy = setmetatable({}, { __index = function(_, k)
        reads[k] = (reads[k] or 0) + 1
        if reads[k] == 1 then return first[k] end
        return later[k]
    end })
    local r = relay(upgraded())
    local seen
    pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = proxy,
        open_stream = function(o) seen = o; return r end,
    })
    if not seen then
        -- Refused before anything was dialled is a correct answer too.
        assert_eq(#r.written, 0, "nothing may be sent when refused:")
    else
        assert_eq(seen.via.host, "ssh.example.com", "the relay dialled:")
        local req = r.written[1] or ""
        assert_match(req, "Host: ssh.example.com\r\n", "the Host sent")
        assert_match(req, "GET /ok HTTP/1.1\r\n", "the path sent")
        assert_eq(req:find("other", 1, true), nil, "no later answer is used")
    end

    -- Plain host and path, and a tls_hostname that is absent when first
    -- read and names another host after: the upgrade's Host is what the
    -- dial was granted for.
    local n = 0
    local half = setmetatable({ host = "ssh.example.com", path = "/ok" },
        { __index = function(_, k)
            if k ~= "tls_hostname" then return nil end
            n = n + 1
            if n > 1 then return "other.internal.example" end
        end })
    local r2 = relay(upgraded())
    local seen2
    ssh.connect{
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = half,
        open_stream = function(o) seen2 = o; return r2 end,
    }
    assert_eq(seen2 and seen2.via.tls_hostname, nil, "the TLS name dialled:")
    assert_match(r2.written[1] or "", "Host: ssh.example.com\r\n", "the Host sent")
end)

test("ssh_tunnel_audit44.write_refusal_unchanged", function()
    -- The relay's stream refuses every write after the upgrade the way the
    -- runtime refuses a wait inside a transaction. The refusal says nothing
    -- about the connection: it must reach the caller as it was raised, not
    -- as io_error text.
    local REFUSAL = "[hull:wait_refused] hull.ssh: cannot wait inside a transaction"
    local ident = string.char(0x80 | ws.OP_BIN, 14) .. "SSH-2.0-Fake\r\n"
    local r = relay(upgraded(ident))
    local writes = 0
    r.write = function(self, s)
        writes = writes + 1
        if writes > 1 then error(REFUSAL, 0) end
        self.written[#self.written + 1] = s
        return true
    end
    local ok, e = pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = { host = "ssh.example.com" },
        open_stream = function() return r end,
    })
    assert_eq(ok, false, "connect raises the refusal:")
    assert_eq(e, REFUSAL, "unchanged:")

    -- And the stream itself, under any caller.
    local r2 = relay(upgraded())
    local s = assert(ws.connect(r2, { host = "ssh.example.com", random = stub_random,
                                      sha1 = stub_sha1 }))
    r2.write = function() error(REFUSAL, 0) end
    local ok2, e2 = pcall(s.write, s, "abc")
    assert_eq(ok2, false, "write raises the refusal:")
    assert_eq(e2, REFUSAL, "unchanged:")
end)

test("ssh_audit44.banner_callback_error_reraised", function()
    -- The caller's own on_banner raising is the caller's bug, not the
    -- connection's: it comes back as raised, like on_stdout's, and the
    -- stream is closed.
    local r = relay("a banner line\r\nSSH-2.0-Fake\r\n")
    local ok, e = pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        open_stream = function() return r end,
        on_banner = function() error("my bug", 0) end,
    })
    assert_eq(ok, false, "connect raises:")
    assert_eq(e, "my bug", "the callback's own error:")
    assert_eq(r.closed, true, "the stream is closed:")
end)

test("ssh_audit44.pending_trust_not_spent_by_bad_args", function()
    -- A connect refused for its own arguments dials nothing, so it must not
    -- use up the one connection a pending trust is for.
    local pt = ssh.pending_trust()
    local function try(o)
        o.host = "spark-7468"; o.user = "operator"; o.key = {}
        o.crypto = crypto_stub; o.trust = pt
        o.open_stream = o.open_stream or function() error("nothing may be dialled") end
        return pcall(ssh.connect, o)
    end
    for _, o in ipairs({ { port = "22" }, { timeout_ms = 1.5 },
                         { tunnel = { host = "ssh.example.com", port = "443" } },
                         { tunnel = { host = "ssh.example.com", path = "relay" } } }) do
        local ok = try(o)
        assert_eq(ok, false, "a bad argument raises")
    end
    local ok, r, e = try({ software = "Hull\r\nX" })
    assert_eq(ok and r == nil and e and e.code, "bad_software")
    local ok2, _, e2 = try({ open_stream = function() return relay("") end })
    assert_eq(ok2, true)
    assert_eq(e2 and e2.code ~= "already_used", true,
              "the trust is still unused: " .. tostring(e2 and e2.code))
    local _, _, e3 = try({ open_stream = function() return relay("") end })
    assert_eq(e3 and e3.code, "already_used", "and the dialled connect spent it:")
end)

test("ssh_tunnel_audit44.upgrade_refusal_closes_raw", function()
    -- The runtime refuses the upgrade's own write (a wait inside a
    -- transaction): the refusal reaches the caller unchanged, and the relay
    -- stream is closed then, not left open until it is collected.
    local REFUSAL = "[hull:wait_refused] hull.ssh: cannot wait inside a transaction"
    local r = relay(upgraded())
    r.write = function() error(REFUSAL, 0) end
    local ok, e = pcall(ssh.connect, {
        host = "spark-7468", user = "operator", key = {}, crypto = crypto_stub,
        tunnel = { host = "ssh.example.com" },
        open_stream = function() return r end,
    })
    assert_eq(ok, false, "connect raises:")
    assert_eq(e, REFUSAL, "unchanged:")
    assert_eq(r.closed, true, "the relay stream is closed:")
end)

test("tunnel.headers entries are header lines", function()
    -- No name, a name that is not a token, or a continuation line a relay
    -- could unfold onto the destination header (audit 22).
    for _, h in ipairs({ "no colon here", " evil:22", ": empty",
                         "Bad Name: x", "\tX-A: b" }) do
        local r = relay(upgraded())
        local err, seen = connect_via(r, { host = "ssh.example.com",
            headers = { "Cf-Access-Jump-Destination: spark-7468:22", h } })
        assert_eq(err and err.code, "denied", h)
        assert_eq(seen, nil, "nothing may be dialled: " .. h)
    end
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

test("ports and timeouts are integers, not numeric strings (audit 39)", function()
    -- "22e2" passed the binding's grant check as 2200 while tostring put
    -- "spark-7468:22e2" in the header, which a relay reading the port with
    -- atoi dialled as 22.
    local function try(o)
        o.host = o.host or "spark-7468"; o.user = "operator"; o.key = {}
        o.crypto = crypto_stub
        o.tunnel = o.tunnel or { host = "ssh.example.com" }
        o.open_stream = function() error("nothing may be dialled") end
        return pcall(ssh.connect, o)
    end
    for _, o in ipairs({ { port = "22e2" }, { port = "22" }, { port = 22.0 },
                         { timeout_ms = "1000" },
                         { tunnel = { host = "ssh.example.com", port = "443" } } }) do
        local ok, e = try(o)
        assert_eq(ok, false)
        assert_match(tostring(e), "must be an integer")
    end
    local r = relay(upgraded())
    local _, seen = connect_via(r, { host = "ssh.example.com" })
    assert_eq(math.type(seen.port), "integer")
    local r2 = relay(upgraded())
    ssh.connect{
        host = "spark-7468", port = 2200, user = "operator", key = {},
        crypto = crypto_stub, tunnel = { host = "ssh.example.com" },
        open_stream = function() return r2 end,
    }
    assert_match(r2.written[1] or "", "Cf-Access-Jump-Destination: spark-7468:2200\r\n")
end)

test("a destination that is not one host name is refused before dialling (audit 39)", function()
    -- A glob grant ("*.example.com") admitted "evil,a.example.com", and a
    -- relay splitting the header on ',' read "evil".
    for _, h in ipairs({ "evil,a.example.com", "a.example.com x", "a\r\nX: y" }) do
        local seen
        local _, err = ssh.connect{
            host = h, user = "operator", key = {}, crypto = crypto_stub,
            tunnel = { host = "ssh.example.com" },
            open_stream = function(o) seen = o; return relay(upgraded()) end,
        }
        assert_eq(err and err.code, "denied", h)
        assert_eq(seen, nil, "nothing may be dialled: " .. h)
    end
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
    ssh.accept_host(trust, "Spark-7468", BLOB22)
    ssh.accept_host(trust, "spark-7468", BLOB2222, 2222)
    local e = trust.entries()
    assert_eq(e["spark-7468"], BLOB22)
    assert_eq(e["[spark-7468]:2222"], BLOB2222)
    ssh.forget_host(trust, "spark-7468", 2222)
    assert_eq(trust.entries()["[spark-7468]:2222"], nil)
    assert_eq(trust.entries()["spark-7468"], BLOB22, "port 22 untouched:")
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

test("exactly one of key and keys, and keys is a non-empty list", function()
    local base = { host = "h", user = "u", crypto = crypto_stub,
                   open_stream = function() error("must not dial") end }
    local function try(extra)
        local o = {}
        for k, v in pairs(base) do o[k] = v end
        for k, v in pairs(extra) do o[k] = v end
        return pcall(ssh.connect, o)
    end
    assert_eq((try({})), false)
    assert_eq((try({ key = {}, keys = { {} } })), false)
    assert_eq((try({ keys = {} })), false)
    assert_eq((try({ keys = "not a list" })), false)
end)

test("a bad key anywhere in keys fails before the dial, and says which", function()
    local _, err = ssh.connect{ host = "h", user = "u", keys = { {}, "not a key" },
                                crypto = crypto_stub,
                                open_stream = function() error("must not dial") end }
    assert_eq(err.code, "bad_key")
    assert_eq(err.key, 2)
end)

test("a key in another tool's format is unsupported_key_type", function()
    local _, err = ssh.connect{ host = "h", user = "u",
        key = "-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n",
        crypto = crypto_stub,
        open_stream = function() error("must not dial") end }
    assert_eq(err.code, "unsupported_key_type")
end)

test("an unusable software string is bad_software, before any dial", function()
    for _, sw in ipairs({ "", "Hull 1.0", "Hull\r\nX", 7 }) do
        local _, err = ssh.connect{ host = "h", user = "u", key = {}, software = sw,
                                    crypto = crypto_stub,
                                    open_stream = function() error("must not dial") end }
        assert_eq(err.code, "bad_software", tostring(sw))
    end
end)

test("a key already trusted is an answer, not a raise", function()
    local trust = ssh.memory_store()
    assert_eq(ssh.accept_host(trust, "h", BLOB22), true)
    local ok, err = ssh.accept_host(trust, "h", BLOB2222)
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

test("the upgrade runs under the connect deadline", function()
    local r = relay(upgraded())
    local log = {}
    r.deadline = function(_, ms) log[#log + 1] = "deadline:" .. tostring(ms) end
    local write = r.write
    r.write = function(self, x) log[#log + 1] = "write"; return write(self, x) end
    connect_via(r)
    assert_eq(log[1], "deadline:30000", "bounded before the request goes out: "
              .. table.concat(log, " "))
    assert_eq(log[2], "write")
end)

-- The dial's errno ---------------------------------------------------------------
--
-- The binding's fourth value is the socket error's symbolic name; the stdlib
-- hands it to the app as `err.errno`, so a health check branches on a value
-- and never on `detail`. The stub stands in for the binding: EHOSTUNREACH is
-- not reproducible portably, and the hint is stdlib text in any case.

local LOCAL_NET = "Privacy & Security > Local Network"
local function dial_fails(errno_name, extra)
    local o = { host = "h", user = "u", key = {}, crypto = crypto_stub,
        open_stream = function()
            return nil, "connection refused or unreachable: " .. tostring(errno_name)
                        .. " (No route to host)", "connect_failed", errno_name
        end }
    for k, v in pairs(extra or {}) do o[k] = v end
    local _, err = ssh.connect(o)
    return err
end

test("connect_failed carries the errno name, and the hint for a link-local literal", function()
    local err = dial_fails("EHOSTUNREACH", { host = "169.254.1.2" })
    assert_eq(err.code, "connect_failed")
    assert_eq(err.errno, "EHOSTUNREACH")
    assert_match(err.detail, "EHOSTUNREACH (No route to host)", "the C text is kept")
    assert_match(err.detail, LOCAL_NET, "the macOS hint follows it")
    assert_match(err.detail, "after every hull build")
    assert_eq(err.detail:find("169.254", 1, true), nil, "no host is added to detail")
end)

test("the hint is for EHOSTUNREACH to a private or link-local literal only", function()
    for _, h in ipairs({ "10.0.0.7", "172.16.5.5", "172.31.255.1", "192.168.1.1",
                         "[fe80::1%en0]", "fd12:3456::1", "FC00::1" }) do
        local err = dial_fails("EHOSTUNREACH", { host = h })
        assert_eq(err.errno, "EHOSTUNREACH")
        assert_match(err.detail, LOCAL_NET, h)
    end
    for _, h in ipairs({ "203.0.113.5", "172.32.0.1", "172.15.0.1", "8.8.8.8",
                         "2001:db8::1", "h", "router.local", "999.168.1.1" }) do
        local err = dial_fails("EHOSTUNREACH", { host = h })
        assert_eq(err.errno, "EHOSTUNREACH")
        assert_eq(err.detail:find(LOCAL_NET, 1, true), nil, h .. " gets no hint")
    end
    -- A refusal on a private address is a closed port, not privacy.
    local err = dial_fails("ECONNREFUSED", { host = "192.168.1.1" })
    assert_eq(err.errno, "ECONNREFUSED")
    assert_eq(err.detail:find(LOCAL_NET, 1, true), nil)
end)

test("a dial that reports no errno (DNS, TLS) sets none", function()
    local _, err = ssh.connect{ host = "h", user = "u", key = {}, crypto = crypto_stub,
        open_stream = function() return nil, "host did not resolve", "connect_failed" end }
    assert_eq(err.code, "connect_failed")
    assert_eq(err.errno, nil)
    assert_eq(err.detail, "host did not resolve")
end)

test("through a tunnel the errno is the relay's, and the hint reads the relay host", function()
    local _, err = ssh.connect{ host = "h", user = "u", key = {}, crypto = crypto_stub,
        tunnel = { host = "fe80::1" },
        open_stream = function() return nil, "no route", "connect_failed", "EHOSTUNREACH" end }
    assert_eq(err.code, "connect_failed")
    assert_eq(err.errno, "EHOSTUNREACH")
    assert_match(err.detail, LOCAL_NET)
    -- The SSH destination is a name here; the relay literal decides.
    local _, err2 = ssh.connect{ host = "169.254.1.2", user = "u", key = {}, crypto = crypto_stub,
        tunnel = { host = "relay.example" },
        open_stream = function() return nil, "no route", "connect_failed", "EHOSTUNREACH" end }
    assert_eq(err2.errno, "EHOSTUNREACH")
    assert_eq(err2.detail:find(LOCAL_NET, 1, true), nil, "a relay NAME gets no hint")
end)

return { pass = pass, fail = fail }

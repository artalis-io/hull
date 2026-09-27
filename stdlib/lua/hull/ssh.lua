-- hull.ssh - the public SSH client.
--
-- The only module an application requires. Everything under hull.ssh.* is
-- internal: the byte stream itself is not reachable from application code at
-- all (see src/hull/runtime/lua/mod_ssh.c), so an app asks for an SSH
-- connection and never holds a socket.
--
--   local ssh = require("hull.ssh")
--
--   local conn, err = ssh.connect{
--       host = "spark-7468.example.com",
--       user = "operator",
--       key  = key_file_contents,
--       trust = my_store,              -- get/put/forget, app-owned
--   }
--   local r = conn:exec("uname -a")    -- { status, signal, stdout, stderr }
--   conn:exec("journalctl -fu app",    -- or stream it, chunk by chunk
--             { on_stdout = print })
--   conn:close()
--
-- Reaching the host at all requires the manifest to say so:
--
--   ssh = { connect = { hosts = {"*.example.com"}, ports = {22},
--                       users = {"operator"} } }
--
-- A host behind a WebSocket tunnel is reached with `tunnel`, and needs a
-- second grant for the relay, because the relay is a different machine:
--
--   ssh = { connect = { hosts = {"spark-7468"}, ports = {22},
--                       users = {"operator"} },
--           tunnel  = { hosts = {"ssh.example.com"}, ports = {443} } }
--
--   local conn = ssh.connect{
--       host = "spark-7468", user = "operator", key = key,
--       tunnel = {
--           host = "ssh.example.com",
--           headers = {
--               "Cf-Access-Client-Id: " .. env.get("CF_ID"),
--               "Cf-Access-Client-Secret: " .. env.get("CF_SECRET"),
--           },   -- Cf-Access-Jump-Destination: spark-7468:22 is added
--       },
--   }
--
-- `connect` still names the host being reached and the login used on it, so
-- a tunnel never widens either; `tunnel` names only the relay dialled to get
-- there. Both are checked. The tunnel is TLS by default and the certificate
-- is verified against the same CA bundle http.fetch uses.
--
-- An unknown or changed host key is NOT a callback. connect() fails with a
-- reason carrying the fingerprint, and the caller decides and retries. A hook
-- there is a thing applications wire to "return true" once and forget, which
-- is the same as having no trust store.

local transport  = require('hull.ssh.transport')
local privatekey = require('hull.ssh.privatekey')
local hostkey    = require('hull.ssh.hostkey')
local kex        = require('hull.ssh.kex')

local M = {}

M.hostkey    = hostkey
M.privatekey = privatekey

--- Load a key from the contents of a key file.
---
--- `opts.passphrase_env` names an environment variable holding the passphrase
--- (read and scrubbed in C, never a Lua string - prefer this);
--- `opts.passphrase` passes the bytes directly.
function M.load_key(text, opts)
    return privatekey.load(text, opts)
end

--- An in-memory trust store, for callers that persist it themselves.
--- Hull ships no on-disk store on purpose: where trust lives is the
--- application's decision, not the library's.
function M.memory_store(seed)
    local t = {}
    for k, v in pairs(seed or {}) do t[k] = v end
    return {
        get = function(host) return t[host] end,
        put = function(host, blob) t[host] = blob end,
        forget = function(host) t[host] = nil end,
        entries = function() return t end,
    }
end

-- What an application holds is a HANDLE, never the machinery behind it.
--
-- A connection used to carry its transport in a plain field, `conn.t`. That
-- one field was the whole transport: `conn.t:send_packet(...)` could open a
-- direct-tcpip channel and forward through the server to anything IT can
-- reach, and `conn.t.stream` was the raw byte stream the binding keeps from
-- apps. The manifest grants SSH to a host as a login; it does not grant
-- arbitrary packets over that session. So the transport (and an sftp
-- session's) live in this table, keyed weakly by the handle, and only the
-- methods below can reach them. The sandbox has no `debug` library, so an
-- upvalue is as far as an application can see.
local inner = setmetatable({}, { __mode = "k" })

local function handle(class, obj)
    local h = setmetatable({}, class)
    inner[h] = obj
    return h
end

--- The budget for reaching an authenticated connection, unless
--- `opts.timeout_ms` says otherwise.
M.CONNECT_TIMEOUT_MS = 30000

-- The one error shape. The transport raises on I/O failure and malformed
-- input (the wire readers raise by design) and returns a reason for protocol
-- refusals; an application gets (nil, { code = ..., detail = ... }) for all of
-- them, from every method, and never has to pcall.
--
-- Two raises are handled differently. A coded failure (a table with `code`,
-- such as a timeout) already says what happened and is returned as it is.
-- An error from the caller's OWN callback - an on_stdout that raised - is the
-- caller's bug, not a connection failure: it travels up wrapped as
-- { app_error = e } and is raised again here, unchanged.
local function guard(fn, ...)
    local r = table.pack(pcall(fn, ...))
    if r[1] then return table.unpack(r, 2, r.n) end
    local e = r[2]
    if type(e) == "table" then
        if e.app_error ~= nil then error(e.app_error, 0) end
        if e.code then return nil, e end
    end
    return nil, { code = "io_error", detail = tostring(e) }
end

-- A key file that could not be loaded, by what the caller would do about it.
-- hull.ssh.privatekey raises prose; these three phrases are its own, and its
-- tests pin them.
local function key_error(e)
    local msg = tostring(e)
    if msg:find("wrong passphrase", 1, true) then
        return { code = "bad_passphrase", detail = msg }
    end
    if msg:find("is encrypted", 1, true) then
        return { code = "passphrase_required", detail = msg }
    end
    return { code = "bad_key", detail = msg }
end

local Conn = {}
Conn.__index = Conn
Conn.__metatable = false

-- Give `class` a method per name that calls the inner object's method of the
-- same name through guard: the one error shape, with no field of the inner
-- object reachable from the handle.
local function forward(class, names)
    for _, name in ipairs(names) do
        class[name] = function(self, ...)
            local o = inner[self]
            return guard(o[name], o, ...)
        end
    end
end

-- An SFTP session. What each method does is documented where it is
-- implemented, in hull.ssh.sftp_client, and for applications in docs/ssh.md.
local SftpHandle = {}
SftpHandle.__index = SftpHandle
SftpHandle.__metatable = false
forward(SftpHandle, { "realpath", "list", "read", "write", "stat", "lstat",
                      "mkdir", "rmdir", "remove", "rename", "chmod", "setstat" })
function SftpHandle:close() return inner[self]:close() end

-- An open file: read / write / seek / tell / stat / close.
local FileHandle = {}
FileHandle.__index = FileHandle
FileHandle.__metatable = false
forward(FileHandle, { "read", "write", "seek", "tell", "stat", "close" })

--- Open a file on the server; see hull.ssh.sftp_client's Sftp:open.
function SftpHandle:open(path, mode, opts)
    local f, err = guard(inner[self].open, inner[self], path, mode, opts)
    if not f then return nil, err end
    return handle(FileHandle, f)
end

--- Run a command. Returns { status, signal, stdout, stderr }, or nil plus a
--- reason table.
---
--- By default output is accumulated and handed back whole, which suits a
--- command that prints a line and exits. Pass `on_stdout` / `on_stderr` to
--- receive each chunk as it arrives instead: that is what makes a deploy log,
--- a `tail -f`, or output too large to hold in memory usable. A stream with a
--- callback is not also accumulated, so its field comes back empty.
---
---   conn:exec("journalctl -fu app", {
---       on_stdout = function(chunk) io_write(chunk) end,
---   })
---
--- `opts.stdin` is a string written to the command before its output is
--- drained, then closed. `opts.max_output` (default 8 MiB) bounds only what
--- is accumulated.
---
--- `opts.stdin` is capped at 128 KiB; a larger one is refused with
--- `stdin_too_large` before anything reaches the wire. Beyond roughly that,
--- a command writing output while we write input wedges both directions on
--- full buffers (measured against OpenSSH). Bulk data belongs in
--- `conn:sftp()`, which moves one direction at a time and has no such limit.
function Conn:exec(command, opts) return guard(inner[self].exec, inner[self], command, opts) end

--- Open an SFTP session. Paths travel inside the subsystem as
--- length-prefixed strings, so a filename never becomes a shell word.
function Conn:sftp()
    local s, err = guard(inner[self].sftp, inner[self])
    if not s then return nil, err end
    return handle(SftpHandle, s)
end
function Conn:close() return inner[self]:close() end
function Conn:fingerprint() return inner[self].host_fingerprint end
function Conn:negotiated() return inner[self].negotiated end

--- Ask the server for new keys now. Returns true, or nil plus a reason.
---
--- A long-lived connection does not need this: hull/ssh already asks on its
--- own once the current keys have protected a gigabyte (hull.ssh.cipher's
--- REKEY_BYTES), and absorbs the server's request whenever it arrives. This
--- is here for the caller who wants the exchange at a moment of their own
--- choosing - after handing a connection to less-trusted code, say.
---
--- Call it BETWEEN operations. It reads packets, so calling it from inside an
--- `on_stdout` callback would consume the output that callback is being fed.
function Conn:rekey() return guard(inner[self].rekey, inner[self]) end

--- What this connection has moved, and how many times it has re-keyed:
--- { rekeys, bytes_sent, bytes_received, packets_sent, packets_received,
---   rekey_due }. The byte counts span the whole connection; the packet
--- counts are for the current keys, which is what the limit is about.
function Conn:stats() return inner[self]:stats() end

-- Reach the host through a WebSocket relay instead of dialling it directly.
--
-- Two layers, and neither knows about the other: the binding opens a TLS
-- stream to the RELAY (and checks ssh.tunnel for it, while still checking
-- ssh.connect for the host behind it), then hull.web.ws-stream turns that
-- into the byte stream the SSH transport already takes. So SSH itself needs
-- no change at all - this is only a different `open_stream`.
--
-- `tunnel.headers` is where a provider's authentication goes, as raw
-- "Name: value" lines. For Cloudflare Access that is Cf-Access-Client-Id and
-- Cf-Access-Client-Secret. This module does not read those.
--
-- It DOES own the one header that picks the machine behind the relay:
-- `tunnel.destination_header` (default "Cf-Access-Jump-Destination"). The
-- manifest's ssh.connect grant is checked against `host`, but a relay that
-- routes by header connects wherever that header says - so if the app wrote
-- it freely, an app granted `web1` could reach any machine the relay can,
-- and the host key it met there would be filed under `web1`. The header is
-- therefore written from the granted host and port, and a caller-supplied one
-- is accepted only if it names exactly that. A relay that routes by some
-- other header names it here; `destination_header = false` means the relay
-- does not route by header at all, and then the grant cannot constrain what
-- it reaches - that is the caller's statement, not a default.
local DEFAULT_DESTINATION_HEADER = "Cf-Access-Jump-Destination"

-- host:port as a relay reads it; an IPv6 literal is bracketed.
local function destination_value(host, port)
    if host:find(":", 1, true) then return "[" .. host .. "]:" .. tostring(port) end
    return host .. ":" .. tostring(port)
end

-- The headers to send: the caller's, with the destination header checked
-- against the granted destination, or added. Returns headers, or nil plus a
-- reason.
local function tunnel_headers(tunnel, host, port)
    local name = tunnel.destination_header
    if name == nil then name = DEFAULT_DESTINATION_HEADER end
    local out = {}
    for i, line in ipairs(tunnel.headers or {}) do out[i] = line end
    if name == false then return out end

    local want = destination_value(host, port)
    local lname, seen = name:lower(), false
    for _, line in ipairs(out) do
        local n, v = tostring(line):match("^%s*([^:]-)%s*:%s*(.-)%s*$")
        if n and n:lower() == lname then
            if v ~= want then
                return nil, { code = "denied", detail = name .. " names " .. v
                    .. ", but this connection is granted for " .. want }
            end
            seen = true
        end
    end
    if not seen then out[#out + 1] = name .. ": " .. want end
    return out
end
-- `dial` is how the RELAY is reached, defaulting to the capability-checked
-- binding. It is a parameter for the same reason `crypto` is one: the layer
-- above it is a byte transform that a test can drive without a socket, and a
-- seam that only production uses is a seam nothing checks.
local function tunnel_opener(tunnel, crypto, dial)
    if type(tunnel) ~= "table" then
        error("ssh.connect: tunnel must be a table", 3)
    end
    if type(tunnel.host) ~= "string" or tunnel.host == "" then
        error("ssh.connect: tunnel.host is required", 3)
    end
    local dh = tunnel.destination_header
    if dh ~= nil and dh ~= false and (type(dh) ~= "string" or dh == "") then
        error("ssh.connect: tunnel.destination_header must be a header name or false", 3)
    end
    local ws   = require('hull.web.ws-stream')
    local port = tunnel.port or 443
    dial = dial or function(o) return require('hull.ssh._stream').connect(o) end
    -- TLS unless the caller explicitly says otherwise. A relay carries the
    -- credentials above in plain headers, so the safe reading of an omitted
    -- flag is "encrypted"; `tls = false` is for a local test shim.
    local tls = tunnel.tls ~= false

    return function(o)
        -- Before anything is dialled: a mismatch is a refusal, not a
        -- connection that fails later.
        local headers, herr = tunnel_headers(tunnel, o.host, o.port)
        if not headers then return nil, herr end

        local raw, err, code = dial({
            host       = o.host,      -- the SSH destination: what the grant,
            port       = o.port,      -- the host key and the login are about
            user       = o.user,
            timeout_ms = o.timeout_ms,
            via        = {            -- the machine actually dialled
                host         = tunnel.host,
                port         = port,
                tls          = tls,
                tls_hostname = tunnel.tls_hostname,
            },
        })
        if not raw then return nil, { code = code or "connect_failed", detail = err } end

        local s, werr = ws.connect(raw, {
            host    = tunnel.tls_hostname or tunnel.host,
            path    = tunnel.path,
            headers = headers,
            random  = crypto.random,
            sha1    = crypto.sha1,
        })
        if not s then
            raw:close()     -- the upgrade failed; the socket is ours to drop
            return nil, werr
        end
        return s
    end
end

--- Open a connection. Returns a connection, or nil plus a reason table:
---
---   { code = "host_unknown", fingerprint = ... }
---   { code = "host_changed", fingerprint = ..., stored_fingerprint = ... }
---   { code = "auth_failed",  methods = {...} }
---   { code = "denied",       detail = ... }      manifest refused it
---
--- A `code` rather than a message, because the caller has to branch on the
--- host-key cases and matching on prose is how that goes wrong later.
function M.connect(opts)
    if type(opts) ~= "table" then
        error("ssh.connect: expected an options table", 2)
    end
    for _, req in ipairs({ "host", "user", "key" }) do
        if opts[req] == nil then
            error("ssh.connect: " .. req .. " is required", 2)
        end
    end
    local offer_ok, offer_why = require('hull.ssh.kexinit').validate_offer(opts.offer)
    if not offer_ok then error("ssh.connect: " .. offer_why, 2) end

    local crypto = opts.crypto or require('hull.crypto')
    -- A passphrase-protected key needs to say where its passphrase comes from.
    -- `passphrase_env` names an environment variable and is preferred: the
    -- value is read, used and scrubbed in C, so it never becomes a Lua string
    -- (which could not be wiped). `passphrase` takes the bytes directly.
    local key = opts.key
    if type(key) ~= "table" then
        local kok, loaded = pcall(privatekey.load, opts.key, {
            passphrase     = opts.passphrase,
            passphrase_env = opts.passphrase_env,
            crypto         = crypto,
        })
        if not kok then return nil, key_error(loaded) end
        key = loaded
    end
    local trust = opts.trust or M.memory_store()

    -- One budget for everything up to an authenticated connection: TCP (and a
    -- relay's TLS), the version exchange, the key exchange and userauth. A
    -- server that accepts the socket and then stalls in any of them runs it
    -- out, rather than holding the caller forever.
    local timeout = opts.timeout_ms or M.CONNECT_TIMEOUT_MS

    -- The stream is obtained ONLY after the manifest check inside the
    -- binding, and only the SSH stdlib can obtain one at all.
    -- With a tunnel, `open_stream` (if given) dials the RELAY and ws-stream
    -- wraps what comes back: the option means "how to open the underlying
    -- byte stream" either way, and the tunnel is a transform on top of it.
    local open
    if opts.tunnel then
        open = tunnel_opener(opts.tunnel, crypto, opts.open_stream)
    elseif opts.open_stream then
        open = opts.open_stream
    else
        local _stream = require('hull.ssh._stream')
        open = function(o) return _stream.connect(o) end
    end

    local stream, serr, scode = open({
        host = opts.host,
        port = opts.port or 22,
        user = opts.user,
        timeout_ms = timeout,
    })
    if not stream then
        -- A reason that already carries a code keeps it: the tunnel path
        -- distinguishes a relay refusing the upgrade (`upgrade_refused`) from
        -- the manifest refusing. Otherwise the binding's code says which
        -- happened: `denied` (the manifest), `connect_failed` (DNS, refused,
        -- unreachable, TLS) or `timeout`. A host that is down used to be
        -- reported as `denied`, which is the one distinction a health check
        -- exists to make.
        if type(serr) == "table" and serr.code then return nil, serr end
        return nil, { code = scode or "connect_failed", detail = serr }
    end

    local t = transport.new(stream, crypto, {
        software      = opts.software,
        keepalive_ms  = opts.keepalive_ms,
        keepalive_max = opts.keepalive_max,
        idle_ms       = opts.idle_ms,
    })
    t:set_deadline(timeout)

    local function step(fn, ...)
        local r1, r2 = guard(fn, ...)
        if r1 == nil and r2 and r2.code == "deadline" then
            r2 = { code = "timeout", detail = "not connected and authenticated within "
                                              .. tostring(timeout) .. " ms" }
        end
        return r1, r2
    end

    local ok, herr = step(t.handshake, t, {
        host = opts.host,
        port = opts.port or 22,      -- part of the host key's identity
        trust = trust,
        offer = opts.offer,
        on_banner = opts.on_banner,
    })
    if not ok then
        t:close()
        return nil, herr
    end

    local aok, aerr = step(t.authenticate, t, opts.user, key, opts.on_banner)
    if not aok then
        t:close()
        return nil, aerr
    end

    t:set_deadline(0)       -- connected: from here on, liveness is keepalives
    return handle(Conn, t)
end

--- Accept a host key the caller has decided to trust, so the next connect
--- succeeds. Separate from connect() on purpose: accepting is an act, not a
--- flag on the call that discovered the key.
---
--- `port` (default 22) is part of the identity: a key is trusted for the
--- host AND port it was met on, as in OpenSSH's known_hosts.
function M.accept_host(trust, host, key_blob, port)
    local name = hostkey.store_name(host, port)
    -- Refusing to overwrite is the point of trust-on-first-use; it is an
    -- answer, not a crash, so it comes back like every other refusal.
    if type(trust) == "table" and type(trust.get) == "function"
       and trust.get(name) ~= nil then
        return nil, { code = "already_trusted", detail = name
                      .. " already has a stored key; forget_host it first" }
    end
    return hostkey.accept_new(trust, name, key_blob)
end

function M.forget_host(trust, host, port)
    return hostkey.forget(trust, hostkey.store_name(host, port))
end

--- Fingerprint a key blob, for showing one to an operator.
function M.fingerprint(crypto, key_blob)
    return hostkey.fingerprint(kex.raw_hash(crypto.sha256), key_blob)
end

return M

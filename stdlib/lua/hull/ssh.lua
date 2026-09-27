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

local Conn = {}
Conn.__index = Conn
Conn.__metatable = false

local SftpHandle = {}
SftpHandle.__index = SftpHandle
SftpHandle.__metatable = false

--- Resolve a path on the server. Returns the canonical path, or nil + text.
function SftpHandle:realpath(path) return inner[self]:realpath(path) end
--- List a directory: the entries safe to use as local names, and the ones
--- refused and why.
function SftpHandle:list(path) return inner[self]:list(path) end
--- Read a whole file; `max` (default 16 MiB) bounds it.
function SftpHandle:read(path, max) return inner[self]:read(path, max) end
--- Write a whole file, creating or truncating it.
function SftpHandle:write(path, data) return inner[self]:write(path, data) end
function SftpHandle:close() return inner[self]:close() end

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
function Conn:exec(command, opts) return inner[self]:exec(command, opts) end

--- Open an SFTP session. Paths travel inside the subsystem as
--- length-prefixed strings, so a filename never becomes a shell word.
function Conn:sftp()
    local s, err = inner[self]:sftp()
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
function Conn:rekey() return inner[self]:rekey() end

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

        local raw, err = dial({
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
        if not raw then return nil, { code = "denied", detail = err } end

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

    local crypto = opts.crypto or require('hull.crypto')
    -- A passphrase-protected key needs to say where its passphrase comes from.
    -- `passphrase_env` names an environment variable and is preferred: the
    -- value is read, used and scrubbed in C, so it never becomes a Lua string
    -- (which could not be wiped). `passphrase` takes the bytes directly.
    local key = type(opts.key) == "table" and opts.key
        or privatekey.load(opts.key, {
               passphrase     = opts.passphrase,
               passphrase_env = opts.passphrase_env,
               crypto         = crypto,
           })
    local trust = opts.trust or M.memory_store()

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

    local stream, serr = open({
        host = opts.host,
        port = opts.port or 22,
        user = opts.user,
        timeout_ms = opts.timeout_ms,
    })
    if not stream then
        -- A reason that already carries a code keeps it. The tunnel path
        -- distinguishes cases the caller acts on differently - a 403 from an
        -- Access policy (`upgrade_refused`) is not the manifest refusing, and
        -- calling both "denied" sends the operator to the wrong file.
        if type(serr) == "table" and serr.code then return nil, serr end
        return nil, { code = "denied", detail = serr }
    end

    local t = transport.new(stream, crypto, { software = opts.software })

    -- The transport raises on I/O failure and on malformed input (the wire
    -- readers raise by design), and returns a reason for protocol refusals.
    -- This is the boundary where both become one shape: an application
    -- calling connect() should never have to pcall to find out whether it
    -- got a connection.
    local function step(fn, ...)
        local results = table.pack(pcall(fn, ...))
        if not results[1] then
            return nil, { code = "io_error", detail = tostring(results[2]) }
        end
        return results[2], results[3]
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

    return handle(Conn, t)
end

--- Accept a host key the caller has decided to trust, so the next connect
--- succeeds. Separate from connect() on purpose: accepting is an act, not a
--- flag on the call that discovered the key.
---
--- `port` (default 22) is part of the identity: a key is trusted for the
--- host AND port it was met on, as in OpenSSH's known_hosts.
function M.accept_host(trust, host, key_blob, port)
    return hostkey.accept_new(trust, hostkey.store_name(host, port), key_blob)
end

function M.forget_host(trust, host, port)
    return hostkey.forget(trust, hostkey.store_name(host, port))
end

--- Fingerprint a key blob, for showing one to an operator.
function M.fingerprint(crypto, key_blob)
    return hostkey.fingerprint(kex.raw_hash(crypto.sha256), key_blob)
end

return M

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
--   local r = conn:exec("uname -a")    -- { status, stdout, stderr }
--   conn:close()
--
-- Reaching the host at all requires the manifest to say so:
--
--   ssh = { connect = { hosts = {"*.example.com"}, ports = {22},
--                       users = {"operator"} } }
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
function M.load_key(text)
    return privatekey.load(text)
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

local Conn = {}
Conn.__index = Conn

function Conn:exec(command, opts) return self.t:exec(command, opts) end

--- Open an SFTP session. Paths travel inside the subsystem as
--- length-prefixed strings, so a filename never becomes a shell word.
function Conn:sftp() return self.t:sftp() end
function Conn:close() return self.t:close() end
function Conn:fingerprint() return self.t.host_fingerprint end
function Conn:negotiated() return self.t.negotiated end

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
    local key = type(opts.key) == "table" and opts.key or privatekey.load(opts.key)
    local trust = opts.trust or M.memory_store()

    -- The stream is obtained ONLY after the manifest check inside the
    -- binding, and only the SSH stdlib can obtain one at all.
    local open = opts.open_stream
    if not open then
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

    return setmetatable({ t = t }, Conn)
end

--- Accept a host key the caller has decided to trust, so the next connect
--- succeeds. Separate from connect() on purpose: accepting is an act, not a
--- flag on the call that discovered the key.
function M.accept_host(trust, host, key_blob)
    return hostkey.accept_new(trust, host, key_blob)
end

function M.forget_host(trust, host)
    return hostkey.forget(trust, host)
end

--- Fingerprint a key blob, for showing one to an operator.
function M.fingerprint(crypto, key_blob)
    return hostkey.fingerprint(kex.raw_hash(crypto.sha256), key_blob)
end

return M

-- fuzz/ssh_driver.lua - the Lua half of fuzz/fuzz_ssh.c.
--
-- Everything an SSH server sends is parsed by hull.ssh.*, most of it before
-- authentication. The parsers are pure Lua, so a bounds mistake cannot corrupt
-- memory - but it can still raise the WRONG error. "attempt to index a nil
-- value" escaping hull.ssh is a crash as far as a caller is concerned: it is
-- not a refusal they can match on, and it usually means a check is missing.
--
-- The contract every target asserts:
--   * a malformed input is refused with an `ssh...:` message or a table
--     carrying a `code` (the two shapes hull.ssh raises and returns);
--   * nothing else escapes - no Lua runtime error, no bad-argument error;
--   * what a parser DOES return is well-formed (target-specific checks).
--
-- The first input byte selects a target; the rest is what the peer sent.
-- Returns a function(input) -> ok, message. A false return aborts the fuzzer.

local packet      = require("hull.ssh.packet")
local wire        = require("hull.ssh.wire")
local kexinit     = require("hull.ssh.kexinit")
local kex         = require("hull.ssh.kex")
local hostkey     = require("hull.ssh.hostkey")
local sftp        = require("hull.ssh.sftp")
local userauth    = require("hull.ssh.userauth")
local channel     = require("hull.ssh.channel")
local known_hosts = require("hull.ssh.known_hosts")
local privatekey  = require("hull.ssh.privatekey")
local transport   = require("hull.ssh.transport")
local ws_stream   = require("hull.web.ws-stream")

-- Oracle ----------------------------------------------------------------------

-- Phrases only the Lua VM produces. An `ssh.x:` prefix alone is not enough:
-- a runtime error inside a module that happened to interpolate one would pass.
local VM_ERRORS = {
    "attempt to ", "bad argument", "stack overflow", "number has no integer",
    "string length overflow", "invalid value", "wrong number of arguments",
    "invalid option", "data string too short", "out of range",
}

local function expected_error(e)
    if type(e) == "table" then
        return type(e.code) == "string"
    end
    if type(e) ~= "string" then return false end
    -- "ssh: ...", "ssh.wire: ...", "ssh.sftp_client: ...", and the tunnel's
    -- "web.ws-stream: ..." - position prefix optional, since error() at
    -- level 1 adds one.
    if not (e:find("ssh[%w%._]*: ") or e:find("web%.ws%-stream: ")) then return false end
    for _, p in ipairs(VM_ERRORS) do
        -- "out of range" is also a legitimate wire phrase ("byte out of
        -- range"), so only reject it when it is not ours.
        if e:find(p, 1, true) and not (p == "out of range" and e:find("ssh%.wire: ")) then
            return false
        end
    end
    return true
end

-- Run fn; an escaping error must be an expected one. Returns ok, result...
local function guarded(fn, ...)
    local r = table.pack(pcall(fn, ...))
    if r[1] then return true, table.unpack(r, 2, r.n) end
    local e = r[2]
    if expected_error(e) then return false end
    error({ fuzz_bug = true, detail = tostring(type(e) == "table" and (e.detail or e.code) or e) }, 0)
end

local function check(cond, what)
    if not cond then error({ fuzz_bug = true, detail = what }, 0) end
end

-- A peer-supplied name that reaches a terminal must be printable ASCII.
local function printable(s) return type(s) == "string" and not s:find("[^\32-\126]") end

-- safe_text/safe_name output: valid UTF-8, no C0 (bar tab/newline for text),
-- no DEL, no C1, no bidi controls.
local function terminal_safe(s, keep_layout)
    if type(s) ~= "string" or utf8.len(s) == nil then return false end
    for _, cp in utf8.codes(s) do
        if cp < 0x20 and not (keep_layout and (cp == 0x09 or cp == 0x0A)) then return false end
        if cp == 0x7F or (cp >= 0x80 and cp <= 0x9F) then return false end
        if (cp >= 0x202A and cp <= 0x202E) or (cp >= 0x2066 and cp <= 0x2069)
           or cp == 0x200E or cp == 0x200F then return false end
    end
    return true
end

-- Stubs -----------------------------------------------------------------------
--
-- Deterministic, and permissive where that lets the fuzzer reach further:
-- every signature verifies, and the AEAD is the identity (tag = 16 zero bytes),
-- so a fuzzed "encrypted" packet is its own plaintext and the userauth and
-- channel code behind NEWKEYS is reachable.

local ZERO_TAG = string.rep("\0", 16)

local function stub_crypto()
    return {
        random          = function(n) return string.rep("\0", n) end,
        sha256          = function(s) return string.pack(">I4", #s) .. string.rep("\171", 28) end,
        x25519_keypair  = function() return string.rep("\9", 32), string.rep("\1", 32) end,
        x25519          = function(_, q) if #q ~= 32 then return nil, "bad point" end
                                         return string.rep("\2", 32) end,
        ed25519_verify  = function() return true end,
        ed25519_sign    = function() return string.rep("\3", 64) end,
        gcm_seal        = function(_, _, _, p) return p, ZERO_TAG end,
        gcm_open        = function(_, _, _, c, t) if t == ZERO_TAG then return c end return nil end,
        hmac_sha1       = function(k, m) return (k .. m .. string.rep("\0", 20)):sub(1, 20) end,
        BCRYPT_MAX_ROUNDS = 64,
        bcrypt_pbkdf    = function(_, _, _, want) return string.rep("\4", want) end,
        aes256ctr       = function(_, _, data) return data end,
        sha1            = function(sv) return (sv .. string.rep("\0", 20)):sub(1, 20) end,
    }
end

-- The host key the handshake target trusts; seeds present this one.
local HOST_KEY = wire.writer():string(hostkey.ALGORITHM):string(string.rep("K", 32)):build()

local function trust_store()
    return { get = function() return HOST_KEY end }
end

-- A stream over the fuzz bytes. Short reads, like the real binding. When the
-- script is spent it reports the peer closing, which the transport raises as
-- an I/O error - not a hang.
local function stream(inbound, chunk)
    return {
        _in = inbound, _pos = 1,
        read = function(self, n)
            if self._pos > #self._in then return "" end
            local take = math.min(n, chunk, #self._in - self._pos + 1)
            local s = self._in:sub(self._pos, self._pos + take - 1)
            self._pos = self._pos + take
            return s
        end,
        write = function() return true end,
        close = function() end,
        deadline = function() end,
    }
end

-- A transport call returns true / a value, or (nil, {code=...}), or raises.
local function refusal_ok(ok, v, err)
    if not ok then return end                   -- raised an expected error
    if v == nil then
        check(type(err) == "table" and type(err.code) == "string",
              "transport returned nil without a coded reason")
    end
end

-- Targets ---------------------------------------------------------------------

local T = {}

T[#T + 1] = function(s)             -- binary packet framing
    for _, block in ipairs({ 8, 16 }) do
        local ok, payload, used = guarded(packet.parse, s, block)
        if ok and payload ~= nil then
            check(type(used) == "number" and used >= 5 and used <= #s, "packet.parse consumed out of range")
            check(#payload < used, "payload longer than its packet")
        elseif ok then
            check(used == "need_more", "packet.parse nil without need_more")
        end
    end
end

T[#T + 1] = function(s)             -- identification line
    local ok, id = guarded(packet.parse_ident, s)
    if ok and id then check(type(id) == "table", "parse_ident returned a non-table") end
end

T[#T + 1] = function(s)             -- KEXINIT + negotiation
    local ok, server = guarded(kexinit.parse, s)
    if not ok then return end
    for _, field in pairs(server) do
        if type(field) == "table" then
            for _, name in ipairs(field) do check(printable(name), "unprintable algorithm name") end
        end
    end
    local neg_ok, neg, why = guarded(kexinit.negotiate, kexinit.DEFAULT_OFFER, server)
    if neg_ok and neg == nil then
        check(type(why) == "table" or type(why) == "string", "negotiate nil without a reason")
    end
    guarded(kexinit.server_is_strict, server)
    if neg_ok and neg then guarded(kexinit.guess_was_wrong, server, neg) end
end

T[#T + 1] = function(s)             -- KEX_ECDH_REPLY
    local ok, r = guarded(kex.parse_ecdh_reply, s)
    if ok then check(#r.q_s == kex.POINT_LEN, "ECDH reply point length not enforced") end
end

T[#T + 1] = function(s)             -- host key and signature blobs
    local ok, k = guarded(hostkey.parse_key, s)
    if ok then check(#k.key == hostkey.KEY_LEN, "host key length not enforced") end
    local sok, sig = guarded(hostkey.parse_signature, s)
    if sok then check(#sig.signature == hostkey.SIG_LEN, "signature length not enforced") end
    local vok, good = guarded(hostkey.verify_signature, stub_crypto(), s, s, "h")
    if vok then check(type(good) == "boolean", "verify_signature not boolean") end
    guarded(hostkey.fingerprint, stub_crypto().sha256, s)
end

T[#T + 1] = function(s)             -- SFTP frames and replies
    local ok, payload = guarded(sftp.parse_frame, s)
    local body = (ok and payload) or s
    local pok, m = guarded(sftp.parse, body)
    if pok and type(m) == "table" and type(m.names) == "table" then
        guarded(sftp.safe_names, m.names)
    end
end

T[#T + 1] = function(s)             -- userauth replies
    local ok, svc = guarded(userauth.parse_service_accept, s)
    if ok then check(type(svc) == "string", "service name not a string") end
    local rok, r = guarded(userauth.parse_response, s)
    if rok then
        check(type(r) == "table" and type(r.type) == "string", "userauth response without a type")
        if r.type == "banner" then check(terminal_safe(r.message, true), "banner reaches the terminal unsanitized") end
        guarded(userauth.can_retry_publickey, r)
    end
end

T[#T + 1] = function(s)             -- channel messages
    local ok, m = guarded(channel.parse, s)
    if ok then check(type(m) == "table", "channel.parse returned a non-table") end
end

T[#T + 1] = function(s)             -- known_hosts text
    local c = stub_crypto()
    for line in (s .. "\n"):gmatch("([^\n]*)\n") do
        local ok, e = guarded(known_hosts.parse_line, line)
        if ok and type(e) == "table" then guarded(known_hosts.matches, e, "host", c.hmac_sha1) end
        guarded(known_hosts.revoked_blob, line)
    end
    guarded(known_hosts.blob_type, s)
    guarded(known_hosts.without, s, "host", c.hmac_sha1)
end

T[#T + 1] = function(s)             -- OpenSSH private key files
    guarded(privatekey.load, s, { passphrase = "p", crypto = stub_crypto() })
    local ok, raw = guarded(privatekey.unarmour, s)
    guarded(privatekey.parse_container, (ok and raw) or s)
end

T[#T + 1] = function(s)             -- terminal sanitizers never raise and always clean
    check(terminal_safe(wire.safe_text(s), true), "safe_text let a control through")
    local n = wire.safe_name(s)
    check(terminal_safe(n, false), "safe_name let a control through")
    check(#n <= 64 + 3, "safe_name not bounded")
end

T[#T + 1] = function(s)             -- whole connection: handshake, auth, exec, sftp
    local c = stub_crypto()
    local t = transport.new(stream(s, 7), c)
    local ok, v, err = guarded(t.handshake, t, { host = "h", trust = trust_store() })
    refusal_ok(ok, v, err)
    if not (ok and v) then return end
    ok, v, err = guarded(t.authenticate, t, "u", { blob = HOST_KEY, secret = string.rep("s", 64) })
    refusal_ok(ok, v, err)
    if not (ok and v) then return end
    ok, v, err = guarded(t.exec, t, "cmd", { max_output = 4096 })
    refusal_ok(ok, v, err)
end

T[#T + 1] = function(s)             -- exec over a plaintext session (no KEX)
    local t = transport.new(stream(s, 5), stub_crypto())
    local ok, v, err = guarded(t.exec, t, "cmd", {
        max_output = 4096, stdin = "in",
        on_stderr = function() end,        -- stderr streamed, stdout buffered
    })
    refusal_ok(ok, v, err)
    if ok and v then
        check(type(v.stdout) == "string" and #v.stdout <= 4096, "exec output cap not enforced")
        -- A streamed stream is handed to the callback, never also buffered.
        check(v.stderr == nil or v.stderr == "", "streamed stderr was buffered too")
    end
end

T[#T + 1] = function(s)             -- sftp over a plaintext session (no KEX)
    local t = transport.new(stream(s, 11), stub_crypto())
    local ok, f, err = guarded(t.sftp, t, { reply_timeout_ms = 0 })
    refusal_ok(ok, f, err)
    if not (ok and f) then return end
    guarded(f.stat, f, "/x")
    guarded(f.list, f, "/")
    guarded(f.read, f, "/x", 4096)
end

T[#T + 1] = function(s)             -- WebSocket frames (the tunnel's framing)
    local buf = s
    for _ = 1, 64 do
        local ok, frame, used = guarded(ws_stream.decode, buf)
        if not ok or frame == nil then return end
        check(type(used) == "number" and used >= 2 and used <= #buf, "ws decode consumed out of range")
        local sok, size = guarded(ws_stream.frame_size, buf)
        check(sok and size == used, "frame_size disagrees with decode")
        buf = buf:sub(used + 1)
    end
end

T[#T + 1] = function(s)             -- SSH over a WebSocket tunnel: upgrade, then exec
    local c = stub_crypto()
    local ok, ws, err = guarded(ws_stream.connect, stream(s, 13), {
        host = "h", random = c.random, sha1 = c.sha1,
    })
    refusal_ok(ok, ws, err)
    if not (ok and ws) then return end
    local t = transport.new(ws, c)
    local eok, v, eerr = guarded(t.exec, t, "cmd", { max_output = 4096 })
    refusal_ok(eok, v, eerr)
end

return function(input)
    if #input == 0 then return true end
    local target = T[(input:byte(1) % #T) + 1]
    local ok, e = pcall(target, input:sub(2))
    if ok then return true end
    if type(e) == "table" and e.fuzz_bug then return false, e.detail end
    -- An error in the driver itself (a target raising outside guarded) is a
    -- harness bug; report it rather than hide it.
    return false, "driver: " .. tostring(e)
end

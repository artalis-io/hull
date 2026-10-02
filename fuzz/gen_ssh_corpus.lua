-- fuzz/gen_ssh_corpus.lua - regenerate fuzz/corpus_ssh/ (seeds for fuzz_ssh).
--
--   lua fuzz/gen_ssh_corpus.lua            (from the repo root, plain Lua 5.4)
--
-- Seeds are built with the hull.ssh builders rather than written by hand, so
-- they stay well-formed as the protocol code changes. Each file's first byte
-- is the target index in fuzz/ssh_driver.lua (0-based); keep the two in step.
-- Well-formed seeds are what let the fuzzer get past the identification line
-- and the key exchange into the code behind them.

package.path = "stdlib/lua/?.lua;stdlib/lua/?/init.lua;" .. package.path

local packet     = require("hull.ssh.packet")
local wire       = require("hull.ssh.wire")
local kexinit    = require("hull.ssh.kexinit")
local kex        = require("hull.ssh.kex")
local hostkey    = require("hull.ssh.hostkey")
local sftp       = require("hull.ssh.sftp")
local cipher     = require("hull.ssh.cipher")
local privatekey = require("hull.ssh.privatekey")
local base64     = require("hull.encoding").base64
local ws         = require("hull.ssh.ws_stream")

local T = {                 -- target indices, as in ssh_driver.lua
    packet = 0, ident = 1, kexinit = 2, ecdh = 3, hostkey = 4, sftp = 5,
    userauth = 6, channel = 7, known_hosts = 8, privatekey = 9, sanitize = 10,
    connection = 11, exec = 12, sftp_session = 13, ws_frames = 14, ws_tunnel = 15,
}

local DIR = "fuzz/corpus_ssh/"
local count = 0
local function seed(target, name, bytes)
    local f = assert(io.open(DIR .. string.format("%02d_%s", target, name), "wb"))
    f:write(string.char(target), bytes)
    f:close()
    count = count + 1
end

local function zeros(n) return string.rep("\0", n) end
local function plain(payload) return packet.frame(payload, 8, zeros) end
local function w() return wire.writer() end

-- Must match ssh_driver.lua: the key the connection target trusts.
local HOST_KEY = w():string(hostkey.ALGORITHM):string(string.rep("K", 32)):build()
local HOST_SIG = w():string(hostkey.ALGORITHM):string(string.rep("S", 64)):build()
local SERVER_KEXINIT = kexinit.build(kexinit.DEFAULT_OFFER, zeros(16))
local ECDH_REPLY = w():byte(kex.SSH_MSG_KEX_ECDH_REPLY):string(HOST_KEY)
                      :string(string.rep("Q", 32)):string(HOST_SIG):build()

-- Channel traffic, as the server sends it for channel 0.
local function conf() return w():byte(91):uint32(0):uint32(0):uint32(2097152):uint32(32768):build() end
local function ok_reply() return w():byte(99):uint32(0):build() end
local function data(s) return w():byte(94):uint32(0):string(s):build() end
local function errdata(s) return w():byte(95):uint32(0):uint32(1):string(s):build() end
local function status(code)
    return w():byte(98):uint32(0):string("exit-status"):boolean(false):uint32(code):build()
end
local function grant(n) return w():byte(93):uint32(0):uint32(n):build() end
local function eof() return w():byte(96):uint32(0):build() end
local function fin() return w():byte(97):uint32(0):build() end

-- SFTP replies, framed and carried in channel data.
local function sreply(t, id, body)
    local b = w():byte(t)
    if id then b:uint32(id) end
    return sftp.frame(b:raw(body or ""):build())
end
local function s_version() return sreply(sftp.FXP_VERSION, nil, wire.uint32(3)) end
local function s_status(id, code) return sreply(sftp.FXP_STATUS, id, w():uint32(code):string("msg"):string(""):build()) end
local function s_attrs(id) return sreply(sftp.FXP_ATTRS, id, w():uint32(0x0F):uint64(5):uint32(1):uint32(2):uint32(420):uint32(1):uint32(2):build()) end
local function s_handle(id) return sreply(sftp.FXP_HANDLE, id, w():string("h1"):build()) end
local function s_data(id, s) return sreply(sftp.FXP_DATA, id, w():string(s):build()) end
local function s_name(id)
    return sreply(sftp.FXP_NAME, id, w():uint32(2)
        :string("a.txt"):string("-rw-r--r-- 1 u g 5 a.txt"):uint32(0)
        :string("b\27[2J"):string("evil"):uint32(0):build())
end

-- packet framing
seed(T.packet, "ignore", plain("\2" .. "hello"))
seed(T.packet, "kexinit", plain(SERVER_KEXINIT))
seed(T.packet, "partial", plain("\2abc"):sub(1, 6))

-- identification lines
seed(T.ident, "openssh", "SSH-2.0-OpenSSH_9.6p1 Ubuntu-3ubuntu13")
seed(T.ident, "compat", "SSH-1.99-Cisco-1.25")
seed(T.ident, "bare", "SSH-2.0-x")

-- negotiation
seed(T.kexinit, "default", SERVER_KEXINIT)
seed(T.kexinit, "strict", kexinit.build({
    kex = { "curve25519-sha256", "kex-strict-s-v00@openssh.com" },
    host_key = { "ssh-ed25519" }, cipher = { "aes256-gcm@openssh.com" },
    mac = { "hmac-sha2-256" }, compression = { "none" }, languages = {},
}, zeros(16)))

seed(T.ecdh, "reply", ECDH_REPLY)
seed(T.hostkey, "key", HOST_KEY)
seed(T.hostkey, "sig", HOST_SIG)

-- ECDSA and RSA shapes: a well-formed blob of each, so mutations reach the
-- curve, point, exponent and modulus checks rather than dying at the name.
local P256_KEY = w():string("ecdsa-sha2-nistp256"):string("nistp256")
                    :string("\4" .. string.rep("E", 64)):build()
local P384_KEY = w():string("ecdsa-sha2-nistp384"):string("nistp384")
                    :string("\4" .. string.rep("F", 96)):build()
local RSA_KEY = w():string("ssh-rsa"):mpint("\1\0\1"):mpint("\193" .. string.rep("\3", 255)):build()
local function ecdsa_sig(alg, n)
    return w():string(alg):string(w():mpint("\128" .. string.rep("\1", n - 1))
                                     :mpint(string.rep("\2", n)):build()):build()
end
local P256_SIG = ecdsa_sig("ecdsa-sha2-nistp256", 32)
seed(T.hostkey, "p256_key", P256_KEY)
seed(T.hostkey, "p384_key", P384_KEY)
seed(T.hostkey, "rsa_key", RSA_KEY)
seed(T.hostkey, "p256_sig", P256_SIG)
seed(T.hostkey, "p384_sig", ecdsa_sig("ecdsa-sha2-nistp384", 48))
seed(T.hostkey, "rsa512_sig", w():string("rsa-sha2-512"):string(string.rep("\9", 256)):build())

for name, s in pairs({
    version = s_version(), status = s_status(1, 2), attrs = s_attrs(1),
    handle = s_handle(1), data = s_data(1, "abc"), name = s_name(1),
}) do seed(T.sftp, name, s) end

seed(T.userauth, "accept", w():byte(6):string("ssh-userauth"):build())
seed(T.userauth, "success", "\52")
seed(T.userauth, "failure", w():byte(51):namelist({ "publickey", "password" }):boolean(true):build())
seed(T.userauth, "banner", w():byte(53):string("Welcome\n\27[31mred"):string("en"):build())
seed(T.userauth, "pk_ok", w():byte(60):string("ssh-ed25519"):string(HOST_KEY):build())

for name, s in pairs({
    confirm = conf(), success = ok_reply(), data = data("out"), ext = errdata("err"),
    status = status(0), window = grant(4096), eof = eof(), close = fin(),
    signal = w():byte(98):uint32(0):string("exit-signal"):boolean(false)
              :string("KILL"):boolean(false):string("killed"):string(""):build(),
    open_fail = w():byte(92):uint32(0):uint32(1):string("prohibited"):string(""):build(),
}) do seed(T.channel, name, s) end

local salt = string.rep("s", 20)
seed(T.known_hosts, "plain", "host,10.0.0.1 ssh-ed25519 " .. base64.encode(HOST_KEY) .. "\n")
seed(T.known_hosts, "hashed", "|1|" .. base64.encode(salt) .. "|" .. base64.encode(salt)
     .. " ssh-ed25519 " .. base64.encode(HOST_KEY) .. "\n# comment\n\n")
seed(T.known_hosts, "revoked", "@revoked * ssh-ed25519 " .. base64.encode(HOST_KEY) .. "\n")
seed(T.known_hosts, "port", "[host]:2222 ssh-ed25519 " .. base64.encode(HOST_KEY) .. "\n")

-- An OpenSSH key file. The fuzz crypto stub is an identity cipher, so an
-- "encrypted" key's private section is readable and its parser still runs.
local function key_file(cipher_name, kdf, kdfopts)
    local pub = string.rep("\11", 32)
    local body = w():uint32(7):uint32(7):string("ssh-ed25519"):string(pub)
                    :string(string.rep("\22", 32) .. pub):string("c"):build()
    local block = cipher_name == "none" and 8 or 16
    local pad, i = {}, 1
    while (#body + #pad) % block ~= 0 do pad[#pad + 1] = string.char(i); i = i + 1 end
    local raw = w():raw(privatekey.MAGIC):string(cipher_name):string(kdf):string(kdfopts)
                  :uint32(1):string(w():string("ssh-ed25519"):string(pub):build())
                  :string(body .. table.concat(pad)):build()
    return privatekey.BEGIN .. "\n" .. base64.encode(raw) .. "\n" .. privatekey.END .. "\n"
end
seed(T.privatekey, "plain", key_file("none", "none", ""))
-- An ECDSA P-256 key file: curve, point, scalar.
do
    local q = "\4" .. string.rep("E", 64)
    local body = w():uint32(9):uint32(9):string("ecdsa-sha2-nistp256"):string("nistp256")
                    :string(q):mpint(string.rep("\7", 32)):string("c"):build()
    local pad, i = {}, 1
    while (#body + #pad) % 8 ~= 0 do pad[#pad + 1] = string.char(i); i = i + 1 end
    local raw = w():raw(privatekey.MAGIC):string("none"):string("none"):string("")
                  :uint32(1):string(w():string("ecdsa-sha2-nistp256"):string("nistp256"):string(q):build())
                  :string(body .. table.concat(pad)):build()
    seed(T.privatekey, "ecdsa", privatekey.BEGIN .. "\n" .. base64.encode(raw) .. "\n"
         .. privatekey.END .. "\n")
end
seed(T.privatekey, "encrypted", key_file("aes256-ctr", "bcrypt",
     w():string(string.rep("t", 16)):uint32(16):build()))

seed(T.sanitize, "ansi", "ok\27[2J\rFAILED\t\n\194\155x\226\128\174y")
seed(T.sanitize, "utf8", "caf\195\169 \240\159\152\128 \237\160\128 \192\175")

-- A whole connection: identification, KEX, NEWKEYS, then the post-NEWKEYS
-- packets sealed with the fuzz stub's identity AEAD (tag = 16 zero bytes).
-- `skip` packets already sent under this key: the counter is in the IV, and
-- although the identity AEAD ignores it, the seed should still be faithful.
local function sealed_from(skip, payloads)
    local c = cipher.new(zeros(32), zeros(12))
    for _ = 1, skip do c:advance() end
    local aead = { seal = function(_, _, _, p) return p, zeros(16) end }
    local out = {}
    for i, p in ipairs(payloads) do out[i] = c:seal(aead, p, zeros) end
    return table.concat(out)
end
local function sealed(payloads) return sealed_from(0, payloads) end
local HANDSHAKE = "SSH-2.0-OpenSSH_9.6\r\n" .. plain(SERVER_KEXINIT) .. plain(ECDH_REPLY) .. plain("\21")
seed(T.connection, "exec", HANDSHAKE .. sealed({
    w():byte(6):string("ssh-userauth"):build(), "\52",
    conf(), ok_reply(), data("hello\n"), errdata("warn\n"), status(0), eof(), fin(),
}))
seed(T.connection, "banner_first", "maintenance tonight\r\n" .. HANDSHAKE .. sealed({
    w():byte(6):string("ssh-userauth"):build(),
    w():byte(53):string("Welcome"):string(""):build(),
    w():byte(51):namelist({ "publickey" }):boolean(false):build(),
}))
-- A server with only an ECDSA host key: the ES256 path of the handshake.
local ECDSA_KEXINIT = kexinit.build({
    kex = kexinit.DEFAULT_OFFER.kex, host_key = { "ecdsa-sha2-nistp256" },
    cipher = kexinit.DEFAULT_OFFER.cipher, mac = kexinit.DEFAULT_OFFER.mac,
    compression = { "none" }, languages = {},
}, zeros(16))
seed(T.connection, "ecdsa_host", "SSH-2.0-OpenSSH_9.6\r\n" .. plain(ECDSA_KEXINIT)
     .. plain(w():byte(kex.SSH_MSG_KEX_ECDH_REPLY):string(P256_KEY)
                 :string(string.rep("Q", 32)):string(P256_SIG):build())
     .. plain("\21"))
seed(T.connection, "rekey", HANDSHAKE .. sealed({
    w():byte(6):string("ssh-userauth"):build(), "\52",
    conf(), ok_reply(), data("before\n"), SERVER_KEXINIT,
}) .. sealed_from(6, {                  -- the rekey itself, then the command ends
    ECDH_REPLY, "\21", data("after\n"), status(0), eof(), fin(),
}))

seed(T.exec, "ok", plain(conf()) .. plain(ok_reply()) .. plain(data("one\n"))
     .. plain(errdata("two\n")) .. plain(status(0)) .. plain(eof()) .. plain(fin()))
seed(T.exec, "window", plain(w():byte(91):uint32(0):uint32(0):uint32(1):uint32(32768):build())
     .. plain(ok_reply()) .. plain(grant(1)) .. plain(status(1)) .. plain(fin()))
seed(T.exec, "global_req", plain(w():byte(80):string("keepalive@openssh.com"):boolean(true):build())
     .. plain(conf()) .. plain(ok_reply()) .. plain(fin()))

seed(T.sftp_session, "ops", plain(conf()) .. plain(ok_reply()) .. plain(data(s_version()))
     .. plain(data(s_attrs(1))) .. plain(data(s_handle(2) .. s_name(3) .. s_status(4, 1)))
     .. plain(data(s_status(5, 0))) .. plain(data(s_handle(6) .. s_data(7, "abc") .. s_status(8, 1)))
     .. plain(data(s_status(9, 0))))

-- A reply of the wrong type to READDIR (data, where a name or a status
-- belongs): Sftp:list once indexed its missing names and raised.
seed(T.sftp_session, "readdir_data", plain(conf()) .. plain(ok_reply()) .. plain(data(s_version()))
     .. plain(data(s_attrs(1))) .. plain(data(s_handle(2) .. s_data(3, "abc"))))

-- WebSocket framing. Server frames are unmasked; opcodes 2 (binary),
-- 9 (ping), 8 (close), and the 126 extended-length form.
local function sframe(opcode, payload)
    local n = #payload
    local head = string.char(0x80 | opcode)
    if n < 126 then head = head .. string.char(n)
    else head = head .. string.char(126) .. string.pack(">I2", n) end
    return head .. payload
end
seed(T.ws_frames, "mixed", sframe(2, "abc") .. sframe(9, "p") .. sframe(2, string.rep("x", 300))
     .. sframe(8, "\3\232"))
seed(T.ws_frames, "fragmented", string.char(0x02, 3) .. "abc" .. string.char(0x80, 2) .. "de")

-- The tunnel: a 101 whose accept matches the harness stubs (random = zeros,
-- sha1 = the input padded to 20 bytes), then SSH exec traffic in frames.
local key = ws.key(zeros)
local accept = ws.accept(function(sv) return (sv .. zeros(20)):sub(1, 20) end, key)
local UPGRADE = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
             .. "Connection: Upgrade\r\nSec-WebSocket-Accept: " .. accept .. "\r\n\r\n"
local session = plain(conf()) .. plain(ok_reply()) .. plain(data("via ws\n"))
             .. plain(status(0)) .. plain(eof()) .. plain(fin())
seed(T.ws_tunnel, "exec", UPGRADE .. sframe(2, session:sub(1, 40)) .. sframe(9, "")
     .. sframe(2, session:sub(41)))
seed(T.ws_tunnel, "refused", "HTTP/1.1 403 Forbidden\r\nCf-Ray: x\r\n\r\n")

io.stderr:write(string.format("gen_ssh_corpus: wrote %d seeds to %s\n", count, DIR))

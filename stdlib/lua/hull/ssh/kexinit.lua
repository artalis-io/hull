-- hull.ssh.kexinit - the KEXINIT message and algorithm negotiation.
--
-- RFC 4253 section 7.1. Pure: builds and parses the message and applies the
-- negotiation rule. The exchange it negotiates lives in hull.ssh.kex.
--
-- What Hull offers is deliberately short. Every algorithm listed here is one
-- a peer may steer us onto, so the list is a security surface rather than a
-- compatibility knob: a name is present only if Hull can do it properly.

local wire = require('hull.ssh.wire')

local M = {}

M.SSH_MSG_KEXINIT = 20

-- What we offer, in preference order. The client's order decides (see
-- negotiate), so this table IS the policy.
M.DEFAULT_OFFER = {
    -- RFC 8731. The @libssh.org name is the same exchange under the name it
    -- shipped under before standardisation; servers still advertise it.
    kex = { "curve25519-sha256", "curve25519-sha256@libssh.org" },

    -- Ed25519 only. RSA host keys would mean carrying RSA verification, and
    -- every server worth reaching has offered Ed25519 for a decade.
    host_key = { "ssh-ed25519" },

    -- AEAD. Hull has AES-256-GCM through the TLS feature; adding a
    -- non-authenticated cipher would only create a downgrade target.
    cipher = { "aes256-gcm@openssh.com" },

    -- Under an AEAD cipher the MAC field is unused: authentication comes from
    -- the cipher. The list is still sent because servers expect a non-empty
    -- one, and it takes effect only if a non-AEAD cipher is ever negotiated,
    -- which the cipher list above prevents.
    mac = { "hmac-sha2-256" },

    -- No compression. Compressing attacker-influenced plaintext before
    -- encrypting it is how CRIME worked.
    compression = { "none" },

    languages = {},
}

-- Build ---------------------------------------------------------------

-- Build a KEXINIT payload. `cookie` must be 16 random bytes supplied by the
-- caller: this module has no capability access.
function M.build(offer, cookie)
    if type(cookie) ~= "string" or #cookie ~= 16 then
        error("ssh.kexinit: cookie must be exactly 16 bytes", 2)
    end
    local o = offer or M.DEFAULT_OFFER
    local w = wire.writer()
    w:byte(M.SSH_MSG_KEXINIT)
    w:raw(cookie)
    w:namelist(o.kex)
    w:namelist(o.host_key)
    w:namelist(o.cipher)          -- client to server
    w:namelist(o.cipher)          -- server to client
    w:namelist(o.mac)
    w:namelist(o.mac)
    w:namelist(o.compression)
    w:namelist(o.compression)
    w:namelist(o.languages or {})
    w:namelist(o.languages or {})
    -- Hull never guesses: a wrong guess costs a wasted round trip and the
    -- bookkeeping to discard the guessed packet, for no benefit to a tool
    -- that opens one connection per host.
    w:boolean(false)
    w:uint32(0)                   -- reserved
    return w:build()
end

-- Parse ---------------------------------------------------------------

-- Parse a KEXINIT payload. Raises on anything malformed (hull.ssh.wire
-- readers raise), so the caller pcalls at the packet boundary.
function M.parse(payload)
    local r = wire.reader(payload)
    local msg = r:byte()
    if msg ~= M.SSH_MSG_KEXINIT then
        error("ssh.kexinit: expected KEXINIT (20), got " .. tostring(msg))
    end
    local out = {}
    out.cookie          = r:raw(16)
    out.kex             = r:namelist()
    out.host_key        = r:namelist()
    out.cipher_c2s      = r:namelist()
    out.cipher_s2c      = r:namelist()
    out.mac_c2s         = r:namelist()
    out.mac_s2c         = r:namelist()
    out.compression_c2s = r:namelist()
    out.compression_s2c = r:namelist()
    out.languages_c2s   = r:namelist()
    out.languages_s2c   = r:namelist()
    out.first_kex_packet_follows = r:boolean()
    out.reserved        = r:uint32()
    -- Trailing bytes are not an error: RFC 4253 reserves room for extension,
    -- and a future field we do not know about must not make us hang up.
    return out
end

-- Negotiation -----------------------------------------------------------

-- RFC 4253 section 7.1: walk the CLIENT list in order and take the first
-- name the server also offers. The client's preference decides, which is why
-- DEFAULT_OFFER is the policy and the server's order is ignored.
function M.choose(client_list, server_list)
    local have = {}
    for _, name in ipairs(server_list) do have[name] = true end
    for _, name in ipairs(client_list) do
        if have[name] then return name end
    end
    return nil
end

-- Negotiate every algorithm at once.
--
-- Returns a table on success, or nil plus a message naming the first
-- category with no overlap. The message names the category and both sides,
-- because "no matching algorithm" on its own tells an operator nothing about
-- which knob to turn.
function M.negotiate(offer, server)
    local o = offer or M.DEFAULT_OFFER

    local function pick(what, mine, theirs)
        local got = M.choose(mine, theirs)
        if not got then
            return nil, ("ssh: no common %s (offered %s, peer offered %s)")
                :format(what, table.concat(mine, ","),
                        table.concat(theirs, ",") ~= "" and
                        table.concat(theirs, ",") or "nothing")
        end
        return got
    end

    local kex, err = pick("key exchange", o.kex, server.kex)
    if not kex then return nil, err end

    local hostkey; hostkey, err = pick("host key algorithm", o.host_key, server.host_key)
    if not hostkey then return nil, err end

    local c2s; c2s, err = pick("client-to-server cipher", o.cipher, server.cipher_c2s)
    if not c2s then return nil, err end

    local s2c; s2c, err = pick("server-to-client cipher", o.cipher, server.cipher_s2c)
    if not s2c then return nil, err end

    -- MAC is negotiated for completeness but unused under an AEAD cipher; a
    -- server offering no MAC alongside an AEAD is not an error.
    local mac_c2s = M.choose(o.mac, server.mac_c2s)
    local mac_s2c = M.choose(o.mac, server.mac_s2c)

    local comp_c2s; comp_c2s, err = pick("client-to-server compression",
                                          o.compression, server.compression_c2s)
    if not comp_c2s then return nil, err end

    local comp_s2c; comp_s2c, err = pick("server-to-server compression",
                                          o.compression, server.compression_s2c)
    if not comp_s2c then return nil, err end

    return {
        kex = kex, host_key = hostkey,
        cipher_c2s = c2s, cipher_s2c = s2c,
        mac_c2s = mac_c2s, mac_s2c = mac_s2c,
        compression_c2s = comp_c2s, compression_s2c = comp_s2c,
    }
end

-- Whether the peer sent a guessed packet we must discard.
--
-- RFC 4253 section 7.1: a guess is WRONG unless both the kex algorithm and
-- the host key algorithm match what was actually negotiated. A wrong guess
-- means the next packet from that peer is to be ignored entirely.
function M.guess_was_wrong(server, negotiated)
    if not server.first_kex_packet_follows then return false end
    return server.kex[1] ~= negotiated.kex
        or server.host_key[1] ~= negotiated.host_key
end

return M

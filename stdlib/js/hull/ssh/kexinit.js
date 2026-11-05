// hull:ssh:kexinit - the KEXINIT message and algorithm negotiation.
//
// The JS twin of hull.ssh.kexinit (stdlib/lua/hull/ssh/kexinit.lua): RFC 4253
// section 7.1. Pure. What Hull offers is deliberately short: every algorithm
// listed is one a peer may steer us onto.

import { wire } from "hull:ssh:wire";

const SSH_MSG_KEXINIT = 20;

// Strict KEX (the Terrapin mitigation, CVE-2023-48795); see the Lua twin.
const STRICT_C = "kex-strict-c-v00@openssh.com";
const STRICT_S = "kex-strict-s-v00@openssh.com";

// What we offer, in preference order. The client's order decides, so this
// table IS the policy. Field names are camelCase (Lua: host_key).
const DEFAULT_OFFER = {
    kex: ["curve25519-sha256", "curve25519-sha256@libssh.org",
          "kex-strict-c-v00@openssh.com"],
    hostKey: ["ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
              "rsa-sha2-512", "rsa-sha2-256"],
    cipher: ["aes256-gcm@openssh.com", "chacha20-poly1305@openssh.com"],
    mac: ["hmac-sha2-256"],
    compression: ["none"],
    languages: [],
};

const OFFER_LISTS = ["kex", "hostKey", "cipher", "mac", "compression"];

// A caller may narrow or reorder the offer, never widen it.
// -> [true] | [null, reason]
function validateOffer(offer) {
    if (offer === undefined || offer === null) return [true];
    if (typeof offer !== "object" || Array.isArray(offer)) return [null, "offer must be a table"];
    for (const list of OFFER_LISTS) {
        const names = offer[list];
        if (!Array.isArray(names)) {
            return [null, "offer." + list + " must be a list"];
        }
        const known = new Set(DEFAULT_OFFER[list]);
        let real = 0;
        for (const n of names) {
            if (!known.has(n)) {
                return [null, "offer." + list + " names " + wire.safeName(String(n))
                    + ", which this client does not implement"];
            }
            if (n !== STRICT_C) real += 1;
        }
        if (real === 0) {
            return [null, "offer." + list + " names no algorithm"];
        }
    }
    return [true];
}

// Build a KEXINIT payload. `cookie` is 16 random bytes from the caller.
function build(offer, cookie) {
    if (typeof cookie !== "string" || cookie.length !== 16) {
        throw new TypeError("ssh.kexinit: cookie must be exactly 16 bytes");
    }
    const o = offer || DEFAULT_OFFER;
    // Our strict-KEX marker is always sent, whatever the caller's offer says.
    const kexList = o.kex.slice();
    if (kexList.indexOf(STRICT_C) < 0) kexList.push(STRICT_C);
    const w = wire.writer();
    w.byte(SSH_MSG_KEXINIT);
    w.raw(cookie);
    w.namelist(kexList);
    w.namelist(o.hostKey);
    w.namelist(o.cipher);          // client to server
    w.namelist(o.cipher);          // server to client
    w.namelist(o.mac);
    w.namelist(o.mac);
    w.namelist(o.compression);
    w.namelist(o.compression);
    w.namelist(o.languages || []);
    w.namelist(o.languages || []);
    w.boolean(false);              // Hull never guesses
    w.uint32(0);                   // reserved
    return w.build();
}

// Parse a KEXINIT payload. Throws on anything malformed.
function parse(payload) {
    const r = wire.reader(payload);
    const msg = r.byte();
    if (msg !== SSH_MSG_KEXINIT) {
        throw new Error("ssh.kexinit: expected KEXINIT (20), got " + String(msg));
    }
    const out = {};
    out.cookie = r.raw(16);
    out.kex = r.namelist();
    out.hostKey = r.namelist();
    out.cipherC2s = r.namelist();
    out.cipherS2c = r.namelist();
    out.macC2s = r.namelist();
    out.macS2c = r.namelist();
    out.compressionC2s = r.namelist();
    out.compressionS2c = r.namelist();
    out.languagesC2s = r.namelist();
    out.languagesS2c = r.namelist();
    out.firstKexPacketFollows = r.boolean();
    out.reserved = r.uint32();
    // Trailing bytes are not an error (RFC 4253 reserves room for extension).
    return out;
}

// RFC 4253 section 7.1: the first CLIENT name the server also offers.
function choose(clientList, serverList) {
    const have = new Set(serverList || []);
    for (const name of clientList || []) {
        if (have.has(name)) return name;
    }
    return null;
}

// Whether the peer will enforce strict KEX.
function serverIsStrict(server) {
    for (const name of server.kex || []) {
        if (name === STRICT_S) return true;
    }
    return false;
}

// The strict-KEX names are SIGNALLING, not exchanges.
function withoutMarkers(list) {
    const out = [];
    for (const name of list || []) {
        if (name !== STRICT_C && name !== STRICT_S) out.push(name);
    }
    return out;
}

// Negotiate every algorithm at once.
// -> [ { kex, hostKey, cipherC2s, cipherS2c, macC2s, macS2c,
//        compressionC2s, compressionS2c } ] | [null, message]
function negotiate(offer, server, strict) {
    const o = offer || DEFAULT_OFFER;
    if (strict === undefined || strict === null) strict = serverIsStrict(server);

    function pick(what, mine, theirs) {
        const got = choose(mine, theirs);
        if (got === null) {
            const peer = (theirs || []).join(",");
            return [null, "ssh: no common " + what + " (offered " + mine.join(",")
                + ", peer offered " + (peer !== "" ? wire.safeName(peer, 200) : "nothing") + ")"];
        }
        return [got];
    }

    let err;
    let kexName;
    [kexName, err] = pick("key exchange", withoutMarkers(o.kex), withoutMarkers(server.kex));
    if (kexName === null) return [null, err];

    let hostkey;
    [hostkey, err] = pick("host key algorithm", o.hostKey, server.hostKey);
    if (hostkey === null) return [null, err];

    // chacha20-poly1305 only under strict KEX (Terrapin).
    let ciphers = o.cipher;
    if (!strict) {
        ciphers = [];
        for (const name of o.cipher) {
            if (name !== "chacha20-poly1305@openssh.com") ciphers.push(name);
        }
        if (ciphers.length === 0) {
            return [null, "ssh: chacha20-poly1305 needs strict KEX (the Terrapin "
                + "mitigation), which the server does not offer"];
        }
    }

    let c2s;
    [c2s, err] = pick("client-to-server cipher", ciphers, server.cipherC2s);
    if (c2s === null) return [null, err];

    let s2c;
    [s2c, err] = pick("server-to-client cipher", ciphers, server.cipherS2c);
    if (s2c === null) return [null, err];

    const macC2s = choose(o.mac, server.macC2s);
    const macS2c = choose(o.mac, server.macS2c);

    let compC2s;
    [compC2s, err] = pick("client-to-server compression", o.compression, server.compressionC2s);
    if (compC2s === null) return [null, err];

    let compS2c;
    [compS2c, err] = pick("server-to-client compression", o.compression, server.compressionS2c);
    if (compS2c === null) return [null, err];

    return [{
        kex: kexName, hostKey: hostkey,
        cipherC2s: c2s, cipherS2c: s2c,
        macC2s, macS2c,
        compressionC2s: compC2s, compressionS2c: compS2c,
    }];
}

// Whether the peer sent a guessed packet we must discard.
function guessWasWrong(server, negotiated) {
    if (!server.firstKexPacketFollows) return false;
    return server.kex[0] !== negotiated.kex
        || server.hostKey[0] !== negotiated.hostKey;
}

export const kexinit = {
    SSH_MSG_KEXINIT, STRICT_C, STRICT_S, DEFAULT_OFFER,
    validateOffer, build, parse, choose, serverIsStrict, negotiate, guessWasWrong,
};
export default kexinit;

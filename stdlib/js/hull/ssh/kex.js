// hull:ssh:kex - the curve25519-sha256 key exchange.
//
// The JS twin of hull.ssh.kex (stdlib/lua/hull/ssh/kex.lua): RFC 8731 for the
// exchange, RFC 4253 section 7.2 for key derivation. Crypto is passed in; the
// byte-sequence builders are pure functions returning exactly what is hashed.

import { wire } from "hull:ssh:wire";

const SSH_MSG_NEWKEYS = 21;
const SSH_MSG_KEX_ECDH_INIT = 30;
const SSH_MSG_KEX_ECDH_REPLY = 31;

// X25519 public values are always 32 bytes (RFC 7748).
const POINT_LEN = 32;

// SSH_MSG_KEX_ECDH_INIT: our ephemeral public value.
function buildEcdhInit(qC) {
    if (typeof qC !== "string" || qC.length !== POINT_LEN) {
        throw new TypeError("ssh.kex: Q_C must be exactly 32 bytes");
    }
    return wire.writer().byte(SSH_MSG_KEX_ECDH_INIT).string(qC).build();
}

// SSH_MSG_KEX_ECDH_REPLY -> { hostKey, qS, signature }
function parseEcdhReply(payload) {
    const r = wire.reader(payload);
    const msg = r.byte();
    if (msg !== SSH_MSG_KEX_ECDH_REPLY) {
        throw new Error("ssh.kex: expected KEX_ECDH_REPLY (31), got " + String(msg));
    }
    const hostKey = r.string();
    const qS = r.string();
    const signature = r.string();
    if (qS.length !== POINT_LEN) {
        throw new Error("ssh.kex: server Q_S is " + String(qS.length) + " bytes, expected 32");
    }
    return { hostKey, qS, signature };
}

// H = hash(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K): every field a
// `string` except K, an `mpint`. Fields: { vC, vS, iC, iS, kS, qC, qS, k }.
function exchangeHashInput(f) {
    for (const name of ["vC", "vS", "iC", "iS", "kS", "qC", "qS", "k"]) {
        if (typeof f[name] !== "string") {
            throw new TypeError("ssh.kex: exchange hash field " + name + " is missing");
        }
    }
    return wire.writer()
        .string(f.vC)
        .string(f.vS)
        .string(f.iC)
        .string(f.iS)
        .string(f.kS)
        .string(f.qC)
        .string(f.qS)
        .mpint(f.k)
        .build();
}

// The server signs H itself.
function signedData(h) {
    return h;
}

const KEY_IDS = { ivC2s: "A", ivS2c: "B", keyC2s: "C",
                  keyS2c: "D", macC2s: "E", macS2c: "F" };

// K1 = HASH(K || H || X || session_id), K2 = HASH(K || H || K1), ...
function deriveKey(hash, kMpint, h, sessionId, id, want) {
    if (typeof hash !== "function") {
        throw new TypeError("ssh.kex: derive_key needs a hash function");
    }
    if (id.length !== 1) {
        throw new TypeError("ssh.kex: key id must be a single letter");
    }
    const prefix = kMpint + h;
    let out = hash(prefix + id + sessionId);
    // Each round hashes EVERY byte produced so far, not just the last digest.
    while (out.length < want) {
        out = out + hash(prefix + out);
    }
    return out.slice(0, want);
}

// The whole key set for the negotiated ciphers; a size of 0 skips that key.
function deriveKeys(hash, kRaw, h, sessionId, sizes) {
    const kMpint = wire.writer().mpint(kRaw).build();
    const out = {};
    for (const field of Object.keys(KEY_IDS)) {
        const want = sizes[field];
        if (want && want > 0) {
            out[field] = deriveKey(hash, kMpint, h, sessionId, KEY_IDS[field], want);
        }
    }
    return out;
}

const SIZES = {
    "aes256-gcm@openssh.com": {
        ivC2s: 12, ivS2c: 12,
        keyC2s: 32, keyS2c: 32,
        macC2s: 0, macS2c: 0,
    },
    // Two 32-byte keys per direction, and no IV: the nonce is the sequence number.
    "chacha20-poly1305@openssh.com": {
        ivC2s: 0, ivS2c: 0,
        keyC2s: 64, keyS2c: 64,
        macC2s: 0, macS2c: 0,
    },
};

// The sizes for a negotiation whose two directions may use different ciphers.
function sizesFor(cipherC2s, cipherS2c) {
    const a = Object.prototype.hasOwnProperty.call(SIZES, cipherC2s) ? SIZES[cipherC2s] : null;
    const b = Object.prototype.hasOwnProperty.call(SIZES, cipherS2c) ? SIZES[cipherS2c] : null;
    if (!a || !b) return null;
    return {
        ivC2s: a.ivC2s, keyC2s: a.keyC2s, macC2s: a.macC2s,
        ivS2c: b.ivS2c, keyS2c: b.keyS2c, macS2c: b.macS2c,
    };
}

export const kex = {
    SSH_MSG_NEWKEYS, SSH_MSG_KEX_ECDH_INIT, SSH_MSG_KEX_ECDH_REPLY, POINT_LEN,
    KEY_IDS, SIZES,
    buildEcdhInit, parseEcdhReply, exchangeHashInput, signedData,
    deriveKey, deriveKeys, sizesFor,
};
export default kex;

// hull:ssh:bytes - byte strings, and hull:crypto over them, for the SSH stack.
//
// The SSH protocol layer (hull:ssh:*) is a line-for-line twin of the Lua one
// (stdlib/lua/hull/ssh/), where every packet, key and digest is a Lua string -
// a byte string. The JS twin keeps that representation: a "byte string" is a
// JS string whose every code unit is 0..255, one per byte, the same convention
// hull:encoding's decoders return. So framing, parsing and slicing read the
// same in both runtimes, and so do the test vectors.
//
// hull:crypto takes bytes as BUFFERS (a string argument is UTF-8 text there),
// so this module is the one place the two meet: `crypto` below is hull:crypto
// with the Lua shapes - byte strings in and out, multiple returns as arrays -
// which is what every hull:ssh:* module calls through (`opts.crypto` is the
// seam a test replaces it at, as in Lua).
//
//   bytes.toU8(s)      byte string -> Uint8Array (throws on a code unit > 255)
//   bytes.fromBuf(b)   ArrayBuffer / typed array -> byte string
//   bytes.crypto       the adapter (see below)

import { crypto as hc } from "hull:crypto";

const MAX_APPLY = 8192;

/** A byte string as a fresh Uint8Array. Throws when `s` is not a byte string. */
function toU8(s) {
    if (typeof s !== "string") {
        if (s instanceof ArrayBuffer) return new Uint8Array(s.slice(0));
        if (ArrayBuffer.isView(s)) {
            return new Uint8Array(s.buffer.slice(s.byteOffset, s.byteOffset + s.byteLength));
        }
        throw new TypeError("ssh: expected a byte string, got " + typeof s);
    }
    const n = s.length;
    const u8 = new Uint8Array(n);
    for (let i = 0; i < n; i++) {
        const c = s.charCodeAt(i);
        if (c > 255) throw new TypeError("ssh: not a byte string (a character above 0xFF)");
        u8[i] = c;
    }
    return u8;
}

/** An ArrayBuffer or typed array as a byte string. */
function fromBuf(b) {
    let u8;
    if (b instanceof ArrayBuffer) u8 = new Uint8Array(b);
    else if (ArrayBuffer.isView(b)) u8 = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    else if (typeof b === "string") return b;
    else throw new TypeError("ssh: expected a buffer");
    if (u8.length <= MAX_APPLY) return String.fromCharCode.apply(null, u8);
    const parts = [];
    for (let i = 0; i < u8.length; i += MAX_APPLY) {
        parts.push(String.fromCharCode.apply(null, u8.subarray(i, i + MAX_APPLY)));
    }
    return parts.join("");
}

/** True when `s` is a string of code units 0..255 only. */
function isBytes(s) {
    if (typeof s !== "string") return false;
    for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 255) return false;
    return true;
}

// A byte-string argument for hull:crypto: a Uint8Array view.
const b = (s) => toU8(s);

// hull:crypto, Lua-shaped. Each entry matches the Lua function of the same
// (snake_case) name in hull.crypto, argument for argument:
//
//   random(n)                          -> bytes
//   sha1(s) / sha256(s)                -> bytes (raw digest)
//   hmacSha1(msg, key)                 -> bytes
//   x25519Keypair()                    -> [publicKey, secretKey]
//   x25519(sk, pk)                     -> [shared] | [null, reason] (low order)
//   ed25519Sign(msg, sk)               -> bytes
//   ed25519Verify(msg, sig, pk)        -> boolean
//   sign(alg, pem, data)               -> bytes (throws on a key it cannot use)
//   verify(alg, pem, data, sig)        -> boolean
//   rsaPrivatePem(n, e, d, p, q)       -> PEM text (throws on bad components)
//   gcmSeal(key, iv, aad, pt)          -> [ciphertext, tag]
//   gcmOpen(key, iv, aad, ct, tag)     -> bytes | null
//   aes256ctr(key, iv, data)           -> bytes
//   chacha20(key, nonce, counter, d)   -> bytes
//   poly1305(key, msg)                 -> bytes (16)
//   bcryptPbkdf(pass, salt, rounds, n) -> bytes   (pass: text, as typed)
//   bcryptPbkdfEnv(var, salt, rounds, n) -> bytes (the passphrase stays in C)
//   constantTimeEq(a, b)               -> boolean
//   BCRYPT_MAX_ROUNDS
const crypto = {
    random(n) { return fromBuf(hc.random(n)); },
    sha1(s) { return fromBuf(hc.sha1(b(s))); },
    sha256(s) { return fromBuf(hc.sha256(b(s))); },
    hmacSha1(msg, key) { return fromBuf(hc.hmacSha1(b(msg), b(key))); },
    x25519Keypair() {
        const kp = hc.x25519Keypair();
        return [fromBuf(kp.publicKey), fromBuf(kp.secretKey)];
    },
    x25519(sk, pk) {
        const shared = hc.x25519(b(sk), b(pk));
        if (shared === null) return [null, "peer sent a low-order point"];
        return [fromBuf(shared)];
    },
    ed25519Sign(msg, sk) { return fromBuf(hc.ed25519Sign(b(msg), b(sk))); },
    ed25519Verify(msg, sig, pk) { return hc.ed25519Verify(b(msg), b(sig), b(pk)); },
    sign(alg, pem, data) { return fromBuf(hc.sign(alg, pem, b(data))); },
    verify(alg, pem, data, sig) { return hc.verify(alg, pem, b(data), b(sig)); },
    rsaPrivatePem(n, e, d, p, q) {
        return hc.rsaPrivatePem(b(n), b(e), b(d), b(p), b(q));
    },
    gcmSeal(key, iv, aad, pt) {
        const r = hc.gcmSeal(b(key), b(iv), b(aad || ""), b(pt || ""));
        return [fromBuf(r.ciphertext), fromBuf(r.tag)];
    },
    gcmOpen(key, iv, aad, ct, tag) {
        const r = hc.gcmOpen(b(key), b(iv), b(aad || ""), b(ct || ""), b(tag));
        return r === null ? null : fromBuf(r);
    },
    aes256ctr(key, iv, data) { return fromBuf(hc.aes256ctr(b(key), b(iv), b(data || ""))); },
    chacha20(key, nonce, counter, data) {
        return fromBuf(hc.chacha20(b(key), b(nonce), counter, b(data || "")));
    },
    poly1305(key, msg) { return fromBuf(hc.poly1305(b(key), b(msg || ""))); },
    bcryptPbkdf(pass, salt, rounds, n) {
        return fromBuf(hc.bcryptPbkdf(pass, b(salt), rounds, n));
    },
    bcryptPbkdfEnv(name, salt, rounds, n) {
        return fromBuf(hc.bcryptPbkdfEnv(name, b(salt), rounds, n));
    },
    constantTimeEq(x, y) { return hc.constantTimeEq(b(x), b(y)); },
    BCRYPT_MAX_ROUNDS: hc.BCRYPT_MAX_ROUNDS,
};

export const bytes = { toU8, fromBuf, isBytes, crypto };
export default bytes;

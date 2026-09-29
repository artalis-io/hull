/*
 * hull:crypto:hkdf - HKDF-SHA256 (RFC 5869): one secret in, several keys out.
 * JS twin of hull.crypto.hkdf; both derive the same bytes.
 *
 * For when a single secret has to feed more than one use - an encryption key
 * and a MAC key, or a key per tenant - without any two of them being the same
 * bytes. Each derived key is bound to a label (`info`) saying what it is for.
 *
 *   extract: PRK = HMAC-SHA256(salt, IKM)          salt defaults to 32 zeros
 *   expand:  T(i) = HMAC-SHA256(PRK, T(i-1) || info || i), OKM = T(1) || ...
 *
 * Inputs are bytes: a buffer, or a byte string (one char per byte, as
 * hull:encoding's decoders return). Outputs are ArrayBuffers, like
 * hull:crypto's. HKDF is for inputs that are already high-entropy; a
 * password needs crypto.hashPassword.
 */

import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";

const HASH_LEN = 32;
const MAX_LENGTH = 255 * HASH_LEN;          // RFC 5869 section 2.3

function bytesArg(fn, v, what) {
    if (typeof v === "string" || v instanceof ArrayBuffer || ArrayBuffer.isView(v)) {
        return encoding.bytes.toU8(v);
    }
    throw new TypeError(`hkdf.${fn}: ${what} must be bytes (a buffer or a byte string)`);
}

/** PRK = HMAC-SHA256(salt, ikm). An empty or absent salt is HashLen zeros. */
function extract(salt, ikm) {
    const s = (salt === undefined || salt === null) ? new Uint8Array(0) : bytesArg("extract", salt, "salt");
    const key = s.length === 0 ? new Uint8Array(HASH_LEN) : s;
    return crypto.hmacSha256(bytesArg("extract", ikm, "input key material"), key);
}

/** `length` bytes of output keying material from a PRK and a label. */
function expand(prk, info, length) {
    const p = bytesArg("expand", prk, "prk");
    const i8 = (info === undefined || info === null) ? new Uint8Array(0) : bytesArg("expand", info, "info");
    if (!Number.isInteger(length) || length < 1 || length > MAX_LENGTH) {
        throw new RangeError(`hkdf.expand: length must be 1..${MAX_LENGTH}`);
    }
    if (p.length < HASH_LEN) {
        throw new RangeError(`hkdf.expand: prk must be at least ${HASH_LEN} bytes`);
    }
    const n = Math.ceil(length / HASH_LEN);
    const out = new Uint8Array(n * HASH_LEN);
    let t = new Uint8Array(0);
    for (let i = 1; i <= n; i++) {
        const msg = new Uint8Array(t.length + i8.length + 1);
        msg.set(t, 0);
        msg.set(i8, t.length);
        msg[msg.length - 1] = i;
        t = new Uint8Array(crypto.hmacSha256(msg, p));
        out.set(t, (i - 1) * HASH_LEN);
    }
    return out.slice(0, length).buffer;
}

/**
 * Extract then expand: `length` bytes derived from `ikm` for the use `info`
 * names. opts: { salt?, info? }.
 *
 *   const enc = hkdf.derive(master, 32, { info: "app v1 encryption" });
 *   const mac = hkdf.derive(master, 32, { info: "app v1 mac" });
 */
function derive(ikm, length, opts) {
    const o = opts === undefined || opts === null ? {} : opts;
    if (typeof o !== "object") throw new TypeError("hkdf.derive: opts must be an object");
    return expand(extract(o.salt, ikm), o.info, length);
}

export const hkdf = { extract, expand, derive, HASH_LEN, MAX_LENGTH };
export default hkdf;

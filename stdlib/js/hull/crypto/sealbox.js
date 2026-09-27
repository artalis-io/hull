/*
 * Versioned, authenticated sealing of values at rest.
 *
 * @module hull:crypto:sealbox
 * @license AGPL-3.0-or-later
 *
 * The JS twin of hull.crypto.sealbox (stdlib/lua/hull/crypto/sealbox.lua),
 * shared by hull:kv's encrypted handles and hull:web:middleware:totp. Both
 * runtimes produce the same bytes for the same key, nonce and value, so a
 * store written by one opens in the other. Design and threat model:
 * docs/kv_encryption_design.md.
 *
 * A sealed blob is version(u32 BE) || nonce(24) || secretbox(frame), where the
 * frame is the value, or each context string length-prefixed and then the
 * value. secretbox has no associated data, so the context goes inside the box
 * and is checked on open.
 *
 * Values, keys and blobs are BYTE STRINGS (each char a byte, 0-255). Bytes
 * cross the crypto binding as Uint8Array or hex, never as a JS string, which
 * the binding would UTF-8-encode - see docs/stdlib_style.md §4.
 */

import { crypto } from "hull:crypto";
import { _hex }   from "hull:crypto:_hex";

const VERSION_LEN = 4;
const NONCE_LEN   = 24;
const MAC_LEN     = 16;
const MIN_LEN     = VERSION_LEN + NONCE_LEN + MAC_LEN;

const toHex = _hex.toHex;

function hexToBin(h) {
    let s = "";
    for (let i = 0; i < h.length; i += 2) s += String.fromCharCode(parseInt(h.substr(i, 2), 16));
    return s;
}

function binToU8(s) {
    const u8 = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u8[i] = s.charCodeAt(i) & 0xff;
    return u8;
}

function abToBin(ab) {
    const u8 = new Uint8Array(ab);
    let s = "";
    for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
    return s;
}

function be32(v) {
    return String.fromCharCode((v >>> 24) & 0xff, (v >>> 16) & 0xff,
                               (v >>> 8) & 0xff, v & 0xff);
}

function readBe32(s, at) {
    return ((s.charCodeAt(at) << 24) | (s.charCodeAt(at + 1) << 16)
          | (s.charCodeAt(at + 2) << 8) | s.charCodeAt(at + 3)) >>> 0;
}

function isByteString(s) {
    if (typeof s !== "string") return false;
    for (let i = 0; i < s.length; i++) if (s.charCodeAt(i) > 0xff) return false;
    return true;
}

/**
 * A keyring from { keys: { [id]: 32-byte key, ... }, current: id }. Throws on
 * anything else: a keyring is configuration, and a wrong one should be heard
 * about at startup.
 */
function keyring(opts) {
    if (!opts || typeof opts.keys !== "object" || opts.keys === null) {
        throw new Error("sealbox.keyring: expected { keys: {[id]: key, ...}, current: id }");
    }
    const keys = {};
    for (const idStr of Object.keys(opts.keys)) {
        const id = Number(idStr);
        if (!Number.isInteger(id) || id < 0 || id > 0xffffffff) {
            throw new Error("sealbox.keyring: key ids must be integers 0..2^32-1");
        }
        const k = opts.keys[idStr];
        if (!isByteString(k) || k.length !== 32) {
            throw new Error("sealbox.keyring: key " + idStr + " must be exactly 32 bytes");
        }
        keys[id] = toHex(k);
    }
    if (keys[opts.current] === undefined) {
        throw new Error("sealbox.keyring: current key id " + String(opts.current)
                        + " is not in keys");
    }
    return { keys, current: opts.current };
}

function frame(context, value) {
    if (!context) return value;
    let s = "";
    for (const c of context) s += be32(c.length) + c;
    return s + value;
}

function unframe(context, plain) {
    if (!context) return plain;
    let pos = 0;
    for (const want of context) {
        if (pos + 4 > plain.length) return null;
        const n = readBe32(plain, pos);
        pos += 4;
        if (pos + n > plain.length || plain.substr(pos, n) !== want) return null;
        pos += n;
    }
    return plain.substring(pos);
}

/** Seal `value` under the ring's current key, bound to `context` (an array of
 *  byte strings, or null). Returns the blob, a byte string. */
function seal(ring, value, context) {
    const key = ring.keys[ring.current];
    const nonce = abToBin(crypto.random(NONCE_LEN));
    const ctHex = crypto.secretbox(binToU8(frame(context, value)), toHex(nonce), key);
    return be32(ring.current) + nonce + hexToBin(ctHex);
}

/**
 * Open a blob sealed with `context`. Returns { ok: true, value, version }, or
 * { ok: false, reason } where reason is "unknown_version" (the blob names a key
 * this ring does not hold) or "open_failed" (anything else: altered, forged,
 * sealed for another context, not a sealed blob). One reason covers every way
 * a blob can fail to be genuine, so it tells an attacker nothing.
 */
function open(ring, blob, context) {
    if (typeof blob !== "string" || blob.length < MIN_LEN) return { ok: false, reason: "open_failed" };
    const version = readBe32(blob, 0);
    const key = ring.keys[version];
    if (key === undefined) return { ok: false, reason: "unknown_version" };
    const nonce = blob.substring(VERSION_LEN, VERSION_LEN + NONCE_LEN);
    const ct    = blob.substring(VERSION_LEN + NONCE_LEN);
    const ab = crypto.secretboxOpen(toHex(ct), toHex(nonce), key);
    if (!ab) return { ok: false, reason: "open_failed" };
    const value = unframe(context, abToBin(ab));
    if (value === null) return { ok: false, reason: "open_failed" };
    return { ok: true, value, version };
}

/** Open a blob in the older unversioned shape, nonce(24) || box, with key
 *  `id`. Only TOTP has rows like this. Returns the value, or null. */
function openUnversioned(ring, id, blob) {
    const key = ring.keys[id];
    if (key === undefined || typeof blob !== "string" || blob.length < NONCE_LEN + MAC_LEN) {
        return null;
    }
    const ab = crypto.secretboxOpen(toHex(blob.substring(NONCE_LEN)),
                                    toHex(blob.substring(0, NONCE_LEN)), key);
    return ab ? abToBin(ab) : null;
}

export const sealbox = {
    VERSION_LEN, NONCE_LEN, MAC_LEN, MIN_LEN,
    keyring, seal, open, openUnversioned,
};
export default sealbox;

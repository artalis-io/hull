/**
 * @file hull:web:pwned
 * @module hull:web:pwned
 * @description k-anonymity pwned-password check via HIBP range API.
 *
 * Mirror of stdlib/lua/hull/web/pwned.lua. See the Lua module
 * header for the design rationale + manifest host requirement.
 *
 * @license AGPL-3.0-or-later
 */

import { httpClient } from "hull:http-client";
import { log }        from "hull:log";

const DEFAULT_ENDPOINT = "https://api.pwnedpasswords.com/range/";

// Inline SHA-1 (RFC 3174). Pure JS, used ONLY for the HIBP
// query path — never for password storage or authentication.
// Returns uppercase hex.
function sha1Hex(msg) {
    const rol = (n, b) => ((n << b) | (n >>> (32 - b))) >>> 0;
    let h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
        h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Encode message as bytes (treat as UTF-8 binary stream).
    const bytes = [];
    for (let i = 0; i < msg.length; i++) {
        const c = msg.charCodeAt(i) & 0xff;
        bytes.push(c);
    }
    const bitLen = bytes.length * 8;
    bytes.push(0x80);
    while ((bytes.length % 64) !== 56) bytes.push(0);
    // 64-bit length BE. Split hi/lo manually because JS's `>>>`
    // masks shift amounts to 5 bits (`>>> 32` is a no-op), so a
    // single-uint32 loop would emit garbage in the high word.
    const bitLenHi = Math.floor(bitLen / 0x100000000) >>> 0;
    const bitLenLo = bitLen >>> 0;
    bytes.push((bitLenHi >>> 24) & 0xff);
    bytes.push((bitLenHi >>> 16) & 0xff);
    bytes.push((bitLenHi >>>  8) & 0xff);
    bytes.push( bitLenHi         & 0xff);
    bytes.push((bitLenLo >>> 24) & 0xff);
    bytes.push((bitLenLo >>> 16) & 0xff);
    bytes.push((bitLenLo >>>  8) & 0xff);
    bytes.push( bitLenLo         & 0xff);

    for (let chunk = 0; chunk < bytes.length; chunk += 64) {
        const w = new Array(80);
        for (let j = 0; j < 16; j++) {
            const o = chunk + j * 4;
            w[j] = ((bytes[o] << 24) | (bytes[o + 1] << 16)
                  | (bytes[o + 2] << 8) | bytes[o + 3]) >>> 0;
        }
        for (let j = 16; j < 80; j++) {
            w[j] = rol(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
        }
        let a = h0, b = h1, c = h2, d = h3, e = h4;
        for (let j = 0; j < 80; j++) {
            let f, k;
            if (j < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999; }
            else if (j < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1; }
            else if (j < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                       k = 0xCA62C1D6; }
            const temp = (rol(a, 5) + f + e + k + w[j]) >>> 0;
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0;
        h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
    }
    const toHex = (n) =>
        ("00000000" + n.toString(16).toUpperCase()).slice(-8);
    return toHex(h0) + toHex(h1) + toHex(h2) + toHex(h3) + toHex(h4);
}

async function check(password, opts) {
    if (typeof password !== "string" || password === "") return false;
    opts = opts || {};
    const endpoint = opts.endpoint || DEFAULT_ENDPOINT;

    const hash = sha1Hex(password);
    const prefix = hash.substring(0, 5);
    const suffix = hash.substring(5);

    let resp;
    try {
        resp = await httpClient.async.get(endpoint + prefix, {
            headers: { "User-Agent": "hull-pwned-check/1" },
        });
    } catch (_e) {
        log.warn("pwned: HIBP fetch failed; failing open");
        return false;
    }
    if (!resp || resp.status !== 200 || !resp.body) {
        log.warn("pwned: HIBP fetch failed; failing open");
        return false;
    }

    const lines = resp.body.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
        const sep = lines[i].indexOf(":");
        if (sep > 0 && lines[i].substring(0, sep) === suffix) return true;
    }
    return false;
}

const pwned = { check, _sha1Hex: sha1Hex };
export { pwned };

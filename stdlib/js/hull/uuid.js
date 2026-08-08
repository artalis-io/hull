/**
 * @file hull:uuid
 * @module hull:uuid
 * @description RFC 9562 UUID generation (v4 random, v7 time-ordered).
 *   Lua parity: `hull.uuid`.
 *
 * Both variants are 36-char canonical strings
 * (`xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx`, `M` the version nibble, `N` the
 * variant). Built on `crypto.random` (CSPRNG) + `time.nowMs`; no new authority.
 * Prefer these over ad-hoc ids from `crypto.base64urlEncode(crypto.random(16))`:
 * canonical, externally interoperable, and v7 is lexically sortable.
 *
 * @license AGPL-3.0-or-later
 * @example
 *   import { uuid } from "hull:uuid";
 *   const id = uuid.v7();   // time-ordered; ideal for DB primary keys
 *   const r  = uuid.v4();   // fully random
 */

import { crypto } from "hull:crypto";
import { time } from "hull:time";

// Byte -> 2-hex-char lookup, built once.
const HEX = new Array(256);
for (let i = 0; i < 256; i++) HEX[i] = (i + 0x100).toString(16).slice(1);

function fmt(b) {
    return HEX[b[0]] + HEX[b[1]] + HEX[b[2]] + HEX[b[3]] + "-" +
           HEX[b[4]] + HEX[b[5]] + "-" + HEX[b[6]] + HEX[b[7]] + "-" +
           HEX[b[8]] + HEX[b[9]] + "-" +
           HEX[b[10]] + HEX[b[11]] + HEX[b[12]] + HEX[b[13]] + HEX[b[14]] + HEX[b[15]];
}

/**
 * Random (version 4) UUID.
 * @returns {string} 36-char canonical UUID.
 */
function v4() {
    const b = new Uint8Array(crypto.random(16));
    b[6] = (b[6] & 0x0f) | 0x40;   // version 4
    b[8] = (b[8] & 0x3f) | 0x80;   // variant 10
    return fmt(b);
}

/**
 * Time-ordered (version 7) UUID: 48-bit big-endian Unix-ms timestamp followed
 * by 74 random bits. Lexically sortable by creation time - good for database
 * primary keys.
 * @returns {string} 36-char canonical UUID.
 */
function v7() {
    const b = new Uint8Array(16);
    // 48-bit ms timestamp, big-endian. ms exceeds 2^32, so peel bytes by
    // division (JS bitwise ops are 32-bit and would overflow).
    let t = time.nowMs();
    for (let i = 5; i >= 0; i--) { b[i] = t % 256; t = Math.floor(t / 256); }
    const r = new Uint8Array(crypto.random(10));
    for (let i = 0; i < 10; i++) b[6 + i] = r[i];
    b[6] = (b[6] & 0x0f) | 0x70;   // version 7
    b[8] = (b[8] & 0x3f) | 0x80;   // variant 10
    return fmt(b);
}

export const uuid = { v4, v7 };
export default uuid;

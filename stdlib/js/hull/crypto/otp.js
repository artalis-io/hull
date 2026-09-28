/*
 * hull:crypto:otp - one-time passwords: HOTP (RFC 4226), which TOTP
 * (RFC 6238) is at a time-derived counter. JS twin of hull.crypto.otp; both
 * produce the same codes.
 *
 * The algorithm, and nothing about enrolment, storage, windows or lockout -
 * that policy is hull:web:middleware:totp's.
 *
 *   1. digest = HMAC-SHA1(key, counter as 8-byte big-endian)
 *   2. offset = low 4 bits of digest[19]
 *   3. P = digest[offset .. offset+3] as a big-endian u32, top bit cleared
 *   4. code = P mod 10^digits, zero-padded to `digits`
 */

import { crypto } from "hull:crypto";
import { encoding } from "hull:encoding";

/**
 * The HOTP code for `key` (a byte string) at `counter` (a non-negative safe
 * integer), `digits` long (6 by default; 6 to 8 per RFC 4226).
 */
function hotp(key, counter, digits) {
    if (digits === undefined) digits = 6;
    if (typeof key !== "string") throw new Error("otp.hotp: key must be a byte string");
    if (!Number.isSafeInteger(counter) || counter < 0) {
        throw new Error("otp.hotp: counter must be a non-negative integer");
    }
    if (!Number.isInteger(digits) || digits < 6 || digits > 8) {
        throw new Error("otp.hotp: digits must be 6, 7 or 8");
    }
    // Bitwise operators truncate to 32 bits, so the 64-bit counter is split
    // by division. The bytes go to C as a Uint8Array: a string of high-byte
    // chars would be UTF-8-inflated at the boundary and give the wrong MAC.
    const hi = Math.floor(counter / 0x100000000);
    const lo = counter - hi * 0x100000000;
    const msg = new Uint8Array([
        (hi >>> 24) & 0xff, (hi >>> 16) & 0xff, (hi >>> 8) & 0xff, hi & 0xff,
        (lo >>> 24) & 0xff, (lo >>> 16) & 0xff, (lo >>> 8) & 0xff, lo & 0xff,
    ]).buffer;
    const macHex = crypto.hmacSha1(msg, encoding.hex.encode(key));
    const byte = (i) => parseInt(macHex.substr(i * 2, 2), 16);
    const offset = byte(19) & 0x0f;
    const p = (byte(offset) & 0x7f) * 0x1000000 + byte(offset + 1) * 0x10000
            + byte(offset + 2) * 0x100 + byte(offset + 3);
    return String(p % Math.pow(10, digits)).padStart(digits, "0");
}

/** The TOTP time step for unix time `now` and a period in seconds. */
function step(now, period) {
    if (period === undefined || period === null) period = 30;
    if (typeof now !== "number" || !(now >= 0) || typeof period !== "number" || !(period > 0)) {
        throw new Error("otp.step: time must be >= 0 and period > 0");
    }
    const s = Math.floor(now / period);
    if (!Number.isSafeInteger(s)) throw new Error("otp.step: bad time or period");
    return s;
}

export const otp = { hotp, step };
export default otp;

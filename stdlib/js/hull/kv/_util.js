/*
 * hull:kv:_util - shared internals for the KV/cache subsystem (JS mirror of
 * hull.kv._util). Coded errors, key/value validation, binary-safe base64 + hex
 * codecs, capability vocabulary. Values are BYTE STRINGS (each char a code unit
 * 0-255, one char per byte - Hull's JS byte convention, same as crypto/_hex),
 * so `.length` is the byte count and there is no UTF-8 step.
 *
 * Internal (underscore-prefixed). SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { time } from "hull:time";

const MAX_KEY = 1024;
// Far-future "no expiry" sentinel for the SQL backend (keeps expires_at NOT
// NULL and off the mid-array-nil binding path). Fits a 64-bit BIGINT.
const NO_EXPIRY = Number.MAX_SAFE_INTEGER;

function codedError(code, message) {
    const e = new Error(message);
    e.code = code;
    throw e;
}

function checkKey(k) {
    if (typeof k !== "string")
        codedError("invalid_argument", "kv: key must be a string (bytes), got " + typeof k);
    if (k.length === 0)
        codedError("invalid_argument", "kv: key must be non-empty");
    if (k.length > MAX_KEY)
        codedError("invalid_argument", "kv: key exceeds " + MAX_KEY + " bytes");
    return k;
}

function checkValue(v) {
    if (typeof v !== "string")
        codedError("invalid_argument", "kv: value must be a string (bytes), got " + typeof v);
    return v;
}

// ttl (seconds) -> absolute expiry ms, or null for no expiry. undefined/null =
// use default; false = no expiry overriding a default; number = seconds.
function expiryMs(ttl, defaultTtl) {
    if (ttl === undefined || ttl === null) ttl = defaultTtl;
    if (ttl === undefined || ttl === null || ttl === false) return null;
    if (typeof ttl !== "number")
        codedError("invalid_argument", "kv: ttl must be a number of seconds");
    return time.nowMs() + Math.floor(ttl * 1000);
}

function nowMs() { return time.nowMs(); }

function toInt(bytes) {
    const n = Number(bytes);
    if (!Number.isInteger(n))
        codedError("invalid_argument", "kv: value is not an integer for incr()");
    return n;
}

// ---- binary-safe base64 (standard alphabet, padded) over byte strings ----
const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const DEC = {};
for (let i = 0; i < B64.length; i++) DEC[B64[i]] = i;

function b64encode(s) {
    const out = [];
    const n = s.length;
    for (let i = 0; i < n; i += 3) {
        const b1 = s.charCodeAt(i) & 0xff;
        const has2 = i + 1 < n, has3 = i + 2 < n;
        const b2 = has2 ? s.charCodeAt(i + 1) & 0xff : 0;
        const b3 = has3 ? s.charCodeAt(i + 2) & 0xff : 0;
        out.push(B64[b1 >> 2]);
        out.push(B64[((b1 & 0x3) << 4) | (b2 >> 4)]);
        out.push(has2 ? B64[((b2 & 0xf) << 2) | (b3 >> 6)] : "=");
        out.push(has3 ? B64[b3 & 0x3f] : "=");
    }
    return out.join("");
}

function b64decode(str) {
    const out = [];
    let buf = 0, bits = 0;
    for (let i = 0; i < str.length; i++) {
        const c = str[i];
        if (c === "=") continue;
        const d = DEC[c];
        if (d === undefined) codedError("invalid_argument", "kv: corrupt base64 in store");
        buf = (buf << 6) | d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push(String.fromCharCode((buf >> bits) & 0xff)); }
    }
    return out.join("");
}

// ---- hex (prefix-preserving; lets the SQL backend scan keys with LIKE) ----
function hexencode(s) {
    let out = "";
    for (let i = 0; i < s.length; i++)
        out += (s.charCodeAt(i) & 0xff).toString(16).padStart(2, "0");
    return out;
}

function hexdecode(hex) {
    if (hex.length % 2 !== 0) codedError("invalid_argument", "kv: corrupt hex in store");
    const out = [];
    for (let i = 0; i < hex.length; i += 2) {
        const n = parseInt(hex.substr(i, 2), 16);
        if (Number.isNaN(n)) codedError("invalid_argument", "kv: corrupt hex in store");
        out.push(String.fromCharCode(n));
    }
    return out.join("");
}

const util = {
    MAX_KEY, NO_EXPIRY, error: codedError, checkKey, checkValue, expiryMs,
    nowMs, toInt, b64encode, b64decode, hexencode, hexdecode,
};

export default util;

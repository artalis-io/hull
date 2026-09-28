/*
 * hull:kv:_util - shared internals for the KV/cache subsystem (JS mirror of
 * hull.kv._util). Coded errors, key/value validation, the store's base64 + hex
 * (hull:encoding, with kv's coded errors), capability vocabulary. Values are
 * BYTE STRINGS (each char a code unit 0-255, one char per byte - Hull's JS
 * byte convention, same as hull:encoding),
 * so `.length` is the byte count and there is no UTF-8 step.
 *
 * Internal (underscore-prefixed). SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { time } from "hull:time";
import { encoding } from "hull:encoding";

const MAX_KEY = 1024;
// Far-future "no expiry" sentinel for the SQL backend (keeps expires_at NOT
// NULL and off the mid-array-nil binding path). Fits a 64-bit BIGINT.
const NO_EXPIRY = Number.MAX_SAFE_INTEGER;

// Any code unit > 255 means the caller passed a non-byte string (UTF-16 text
// such as U+0100). Such a string has no byte form (hull:encoding refuses it),
// so reject it here with kv's own error code. Lua strings are already bytes,
// so this keeps the two runtimes identical.
const NON_BYTE = /[^\u0000-\u00ff]/;

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
    if (NON_BYTE.test(k))
        codedError("invalid_argument", "kv: key must be a byte string (code units 0-255)");
    return k;
}

function checkValue(v) {
    if (typeof v !== "string")
        codedError("invalid_argument", "kv: value must be a string (bytes), got " + typeof v);
    if (NON_BYTE.test(v))
        codedError("invalid_argument", "kv: value must be a byte string (code units 0-255)");
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
    // Reject empty / whitespace-only up front: Number("") and Number("  ") are
    // 0 in JS (both Number.isInteger), but Lua's tonumber("") / tonumber(" ")
    // are nil -> throw. Guard so incr() on such a value throws in BOTH runtimes.
    if (typeof bytes !== "string" || bytes.trim() === "")
        codedError("invalid_argument", "kv: value is not an integer for incr()");
    const n = Number(bytes);
    if (!Number.isInteger(n))
        codedError("invalid_argument", "kv: value is not an integer for incr()");
    return n;
}

// Validate an optional non-negative integer (a count/limit); undefined/null
// passes through as undefined. Mirrors Lua's string.format("%d", ...) strictness
// so a bad limit/max never reaches an inlined SQL LIMIT as "NaN" / "Infinity".
function checkCount(v, what) {
    if (v === undefined || v === null) return undefined;
    if (typeof v !== "number" || !Number.isInteger(v) || v < 0)
        codedError("invalid_argument", "kv: " + what + " must be a non-negative integer");
    return v;
}

// ---- the store's encodings: standard padded base64 for values, lowercase
// hex for keys (prefix-preserving, so the SQL backend can scan with LIKE).
// A value that does not decode is corruption in the store, reported with
// kv's own code. ----
const b64encode = (s) => encoding.base64.encode(s);
const hexencode = (s) => encoding.hex.encode(s);

function b64decode(str) {
    const v = encoding.base64.decode(str);
    if (v === null) codedError("invalid_argument", "kv: corrupt base64 in store");
    return v;
}

function hexdecode(hex) {
    const v = encoding.hex.decode(hex);
    if (v === null) codedError("invalid_argument", "kv: corrupt hex in store");
    return v;
}

const util = {
    MAX_KEY, NO_EXPIRY, error: codedError, checkKey, checkValue, checkCount,
    expiryMs, nowMs, toInt, b64encode, b64decode, hexencode, hexdecode,
};

export default util;

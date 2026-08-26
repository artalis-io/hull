/**
 * @file hull:web:pwned
 * @module hull:web:pwned
 * @description k-anonymity pwned-password check via HIBP range API.
 *
 * Mirror of stdlib/lua/hull/web/pwned.lua. See the Lua module
 * header for the design rationale + manifest host requirement.
 *
 * Two-tier check:
 *   1. Embedded local blocklist (SecLists top 10K, ~80KB, binary-
 *      searched in-process). A hit short-circuits - no network
 *      round-trip needed. Safe for air-gapped deployments.
 *   2. HIBP range API. Fail-open on outage, with a once-per-process
 *      warn so the operator sees the gap.
 *
 * @license AGPL-3.0-or-later
 */

import { crypto }     from "hull:crypto";
import { httpClient } from "hull:http-client";
import { log }        from "hull:log";
import { time }       from "hull:time";
import { blocklist }  from "hull:web:_pwned_blocklist";

const DEFAULT_ENDPOINT = "https://api.pwnedpasswords.com/range/";

// Health state. Updated after every HIBP attempt. ok=true only when
// the last attempt produced a real answer. Use pwned.health() to
// read this from middleware health checks.
const _health = {
    ok:            null,
    last_check_at: null,
    last_error:    null,
};
let _warnedFailopen = false;

// SHA-1 of the password, uppercase hex (HIBP wire format). Delegates
// to crypto.sha1 - a legacy-interop primitive surfaced specifically
// for protocols like HIBP that hardcode SHA-1. Do NOT use SHA-1
// for any new cryptographic purpose.
function sha1Hex(msg) {
    return crypto.hexEncode(crypto.sha1(msg)).toUpperCase();
}

// Binary search the embedded blocklist. hashes is a packed sort of
// 8-char uppercase-hex SHA-1 prefixes; each stride is exactly
// blocklist.stride (= 8) chars. Returns true if the first 8 hex
// chars of the SHA-1 match a known entry.
function inLocalBlocklist(hashHexUpper) {
    const stride = blocklist.stride;
    const needle = hashHexUpper.substring(0, stride);
    const hashes = blocklist.hashes;
    let lo = 0, hi = blocklist.count - 1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        const pos = mid * stride;
        const s = hashes.substring(pos, pos + stride);
        if (s === needle) return true;
        if (s < needle) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

async function check(password, opts) {
    if (typeof password !== "string" || password === "") return false;
    opts = opts || {};
    const endpoint = opts.endpoint || DEFAULT_ENDPOINT;

    const hash = sha1Hex(password);

    // Local blocklist first. Cheap (~14 comparisons) and works
    // when HIBP is unreachable. A hit is conclusive.
    if (inLocalBlocklist(hash)) return true;

    const prefix = hash.substring(0, 5);
    const suffix = hash.substring(5);

    let resp;
    try {
        resp = await httpClient.async.get(endpoint + prefix, {
            headers: { "User-Agent": "hull-pwned-check/1" },
        });
    } catch (_e) {
        _health.ok            = false;
        _health.last_check_at = time.now();
        _health.last_error    = "HIBP fetch failed";
        if (!_warnedFailopen) {
            _warnedFailopen = true;
            log.warn("pwned: HIBP fetch failed; failing open after local "
                + "blocklist miss. Subsequent failures will be silent. "
                + "Check connectivity to " + endpoint);
        }
        return false;
    }
    if (!resp || resp.status !== 200 || !resp.body) {
        _health.ok            = false;
        _health.last_check_at = time.now();
        _health.last_error    = "HIBP fetch failed";
        if (!_warnedFailopen) {
            _warnedFailopen = true;
            log.warn("pwned: HIBP fetch failed; failing open after local "
                + "blocklist miss. Subsequent failures will be silent. "
                + "Check connectivity to " + endpoint);
        }
        return false;
    }

    _health.ok            = true;
    _health.last_check_at = time.now();
    _health.last_error    = null;
    _warnedFailopen       = false;

    const lines = resp.body.split(/\r?\n/);
    for (let i = 0; i < lines.length; i++) {
        const sep = lines[i].indexOf(":");
        if (sep > 0 && lines[i].substring(0, sep) === suffix) return true;
    }
    return false;
}

function health() {
    return {
        ok:            _health.ok,
        last_check_at: _health.last_check_at,
        last_error:    _health.last_error,
    };
}

const pwned = {
    check, health,
    _sha1Hex: sha1Hex,
    _inLocalBlocklist: inLocalBlocklist,
};
export { pwned };

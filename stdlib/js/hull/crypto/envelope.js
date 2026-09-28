/**
 * @file hull:crypto:envelope
 * @module hull:crypto:envelope
 * @description HMAC-signed, JSON-payload, stateless tokens.
 *
 * Mirror of stdlib/lua/hull/crypto/envelope.lua - see the Lua
 * module header for the full design rationale.
 *
 *     base64url(JSON-payload) "." hex(HMAC-SHA256(secret_hex, body))
 *
 * API:
 *   envelope.sign(payload, secretHex)  -> tokenString
 *   envelope.verify(token, secretHex)  -> [payload, null]
 *                                      | [null, reason]
 *
 * Semantic checks (action match, expiry, single-use) live with
 * the caller; this helper only frames the signature.
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { json }   from "hull:json";
import { encoding } from "hull:encoding";

// `secret` is the HMAC key: a buffer (for a byte string, encoding.bytes.toU8),
// or a string taken as its UTF-8 text. The tag is hex.
function sign(payload, secret) {
    const body = encoding.base64.encode(encoding.utf8.encode(json.encode(payload)), { url: true });
    const tag  = encoding.hex.encode(crypto.hmacSha256(body, secret));
    return body + "." + tag;
}

function verify(token, secret) {
    if (typeof token !== "string" || token === "") return [null, "missing"];
    const dot = token.indexOf(".");
    if (dot < 0) return [null, "malformed"];
    const body = token.substring(0, dot);
    const tag  = token.substring(dot + 1);

    // A tag that is not hex (or not a MAC's length) is simply a bad tag; the
    // comparison is constant-time.
    const mac = encoding.hex.decode(tag);
    if (mac === null || !crypto.hmacSha256Verify(body, secret, encoding.bytes.toU8(mac))) {
        return [null, "bad tag"];
    }

    const bytes = encoding.base64.decode(body, { url: true });
    const raw = bytes === null ? null : encoding.utf8.decode(bytes);
    if (raw === null) return [null, "bad encoding"];
    let payload;
    try { payload = json.decode(raw); }
    catch (_e) { return [null, "bad json"]; }
    if (!payload || typeof payload !== "object") return [null, "bad json"];
    return [payload, null];
}

const envelope = { sign, verify };
export { envelope };

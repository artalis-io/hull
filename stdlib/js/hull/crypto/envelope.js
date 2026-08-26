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

function sign(payload, secretHex) {
    const body = crypto.base64urlEncode(json.encode(payload));
    const tag  = crypto.hmacSha256(body, secretHex);
    return body + "." + tag;
}

function verify(token, secretHex) {
    if (typeof token !== "string" || token === "") return [null, "missing"];
    const dot = token.indexOf(".");
    if (dot < 0) return [null, "malformed"];
    const body = token.substring(0, dot);
    const tag  = token.substring(dot + 1);

    // crypto.hmacSha256Verify throws on malformed-hex input -
    // catch so a junk token from the wire returns "bad tag"
    // rather than an unhandled exception in the handler.
    let valid = false;
    try {
        valid = crypto.hmacSha256Verify(body, secretHex, tag);
    } catch (_e) {
        return [null, "bad tag"];
    }
    if (!valid) return [null, "bad tag"];

    const raw = crypto.base64urlDecode(body);
    if (raw === null || raw === undefined) return [null, "bad encoding"];
    let payload;
    try { payload = json.decode(raw); }
    catch (_e) { return [null, "bad json"]; }
    if (!payload || typeof payload !== "object") return [null, "bad json"];
    return [payload, null];
}

const envelope = { sign, verify };
export { envelope };

/*
 * hull:jwt -- JWT HS256 sign/verify/decode
 *
 * jwt.sign(payload, secret)   - returns JWT token string
 * jwt.verify(token, secret)   - returns payload object or [null, "reason"]
 * jwt.decode(token)           - decode without verification, returns payload or null
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";
import { time } from "hull:time";
import { json } from "hull:json";

// Pre-computed base64url of {"alg":"HS256","typ":"JWT"}
const HEADER_B64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

function constantTimeCompare(a, b) {
    if (a.length !== b.length) return false;
    let diff = 0;
    for (let i = 0; i < a.length; i++)
        diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
    return diff === 0;
}

function secretToHex(secret) {
    let hex = "";
    for (let i = 0; i < secret.length; i++)
        hex += secret.charCodeAt(i).toString(16).padStart(2, "0");
    return hex;
}

function computeSignature(headerB64, payloadB64, secret) {
    const signingInput = headerB64 + "." + payloadB64;
    const keyHex = secretToHex(secret);
    const sigHex = crypto.hmacSha256(signingInput, keyHex);

    // Convert hex to raw bytes string for base64url encoding
    let raw = "";
    for (let i = 0; i < sigHex.length; i += 2)
        raw += String.fromCharCode(parseInt(sigHex.substring(i, i + 2), 16));

    return crypto.base64urlEncode(raw);
}

function sign(payload, secret) {
    if (!payload || typeof payload !== "object")
        throw new Error("payload must be an object");
    if (!secret || typeof secret !== "string")
        throw new Error("secret is required");

    // Set iat if not already present
    const p = Object.assign({}, payload);
    if (p.iat === undefined)
        p.iat = time.now();

    const payloadB64 = crypto.base64urlEncode(json.encode(p));
    const sig = computeSignature(HEADER_B64, payloadB64, secret);

    return HEADER_B64 + "." + payloadB64 + "." + sig;
}

function verify(token, secret) {
    if (!token || typeof token !== "string")
        return [null, "invalid token"];
    if (!secret || typeof secret !== "string")
        return [null, "secret is required"];

    const parts = token.split(".");
    if (parts.length !== 3)
        return [null, "malformed token"];

    // Verify header
    if (parts[0] !== HEADER_B64)
        return [null, "unsupported algorithm"];

    // Verify signature
    const expectedSig = computeSignature(parts[0], parts[1], secret);
    if (!constantTimeCompare(parts[2], expectedSig))
        return [null, "invalid signature"];

    // Decode payload
    const payloadStr = crypto.base64urlDecode(parts[1]);
    if (payloadStr === null)
        return [null, "invalid payload encoding"];

    let payload;
    try {
        payload = json.decode(payloadStr);
    } catch (e) {
        return [null, "invalid payload JSON"];
    }

    if (!payload || typeof payload !== "object")
        return [null, "payload is not an object"];

    // Check expiration
    if (payload.exp !== undefined) {
        const now = time.now();
        if (now >= payload.exp)
            return [null, "token expired"];
    }

    // Check not-before
    if (payload.nbf !== undefined) {
        const now = time.now();
        if (now < payload.nbf)
            return [null, "token not yet valid"];
    }

    return payload;
}

function decode(token) {
    if (!token || typeof token !== "string")
        return null;

    const parts = token.split(".");
    if (parts.length !== 3)
        return null;

    const payloadStr = crypto.base64urlDecode(parts[1]);
    if (payloadStr === null)
        return null;

    try {
        return json.decode(payloadStr);
    } catch (e) {
        return null;
    }
}

const jwt = { sign, verify, decode };
export { jwt };

/**
 * @file hull:middleware:csp
 * @module hull:middleware:csp
 * @description Content-Security-Policy middleware with per-request nonce.
 *
 * Lua parity: same surface as `hull.middleware.csp` with camelCase
 * options (`reportOnly` vs `report_only`).
 *
 * Generates a fresh 128-bit nonce on every request, exposes it via
 * `req.ctx.cspNonce`, and sets the `Content-Security-Policy` header
 * on the outbound response. Templates that need a nonce read it
 * from the context and emit it on `<script>` and `<style>` tags:
 *
 *     <script nonce="{{ cspNonce }}">...</script>
 *     <style nonce="{{ cspNonce }}">...</style>
 *
 * Three named profiles, identical to the Lua side:
 *
 *  - `csp.htmx()`. Pico-compatible (allows inline `style=` attributes
 *    via `style-src-attr 'unsafe-inline'`).
 *  - `csp.strict()`. No inline-style escape.
 *  - `csp.custom(opts)`. Caller-controlled directives.
 *
 * @license AGPL-3.0-or-later
 */

import { crypto } from "hull:crypto";

// Base64url-encode raw bytes (128-bit by default) for use as a CSP
// nonce. 16 bytes -> 22 base64url chars unpadded.
function nonceB64url(nBytes) {
    nBytes = nBytes || 16;
    const raw = crypto.random(nBytes);
    if (!raw || raw.length !== nBytes) return null;
    const b64 = crypto.base64urlEncode(raw);
    if (!b64) return null;
    // Defensive: strip trailing '=' if the encoder ever returns padded.
    return b64.replace(/=+$/, "");
}

// Build the CSP header value for a given profile + nonce.
function buildHeader(profile, nonce) {
    const nonceToken = `'nonce-${nonce}'`;
    const parts = [
        "default-src 'self'",
        `script-src 'self' ${nonceToken}`,
        `style-src 'self' ${nonceToken}`,
        "img-src 'self' data:",
        "form-action 'self'",
        "frame-ancestors 'none'",
        "base-uri 'self'",
    ];
    if (profile === "htmx") {
        // Pico v2 classless uses inline style="..." attributes.
        // style-src-attr controls ONLY inline style attributes;
        // <style> blocks still require the nonce above.
        parts.push("style-src-attr 'unsafe-inline'");
    }
    return parts.join("; ");
}

function makeMiddleware(profile, opts) {
    opts = opts || {};
    const headerName = opts.reportOnly
        ? "Content-Security-Policy-Report-Only"
        : "Content-Security-Policy";
    return function (req, res) {
        const nonce = nonceB64url(16);
        if (!nonce) {
            res.status(500);
            res.json({ error: "csp: nonce generation failed" });
            return 1;
        }
        req.ctx = req.ctx || {};
        req.ctx.cspNonce = nonce;
        res.header(headerName, buildHeader(profile, nonce));
        return 0;
    };
}

/**
 * Pico-compatible CSP profile. Allows inline `style="..."` attributes
 * (Pico v2 classless uses them on `<details>`, `<dialog>`, and some
 * form controls). `<script>` and `<style>` blocks still require the
 * per-request nonce.
 *
 * @param {{ reportOnly?: boolean }} [opts]
 * @returns {Function} Middleware suitable for `app.use("*", "/*", mw)`.
 */
function htmx(opts) {
    return makeMiddleware("htmx", opts);
}

/**
 * Strict CSP profile (no inline-style escape). Blocks all inline
 * styles including `style="..."` attributes. Use when you control
 * every template and ship a CSS framework without inline styles.
 *
 * @param {{ reportOnly?: boolean }} [opts]
 * @returns {Function}
 */
function strict(opts) {
    return makeMiddleware("strict", opts);
}

/**
 * Generate a fresh CSP nonce (base64url, 128-bit, unpadded). Useful
 * for callers that want to compose their own CSP middleware but reuse
 * Hull's nonce generation. Returns null on crypto.random failure.
 *
 * @returns {string|null}
 */
function nonce() {
    return nonceB64url(16);
}

export const csp = { htmx, strict, nonce };
export default csp;

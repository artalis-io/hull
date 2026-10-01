/*
 * Internal request helpers shared across the web stdlib.
 *
 * @module hull:web:_request
 * @license AGPL-3.0-or-later
 *
 * Contributor-only (the `_` prefix): imported by other stdlib modules, never
 * declared by apps. Centralizes request-derived values that several middleware
 * (session, audit-log, totp, auth-flows) each extracted by hand - subtly
 * differently. See docs/stdlib_style.md §4.
 */

/**
 * The client's source IP, honoring a `trustProxy` policy.
 *
 * With `trustProxy` false (the safe default), returns the un-spoofable socket
 * peer (`req.remote_addr`). With `trustProxy` true - only correct behind a
 * trusted reverse proxy - returns the LAST address of the `X-Forwarded-For`
 * chain, trimmed, falling back to the socket peer. Proxies append the peer
 * they saw; everything to its left came from the client and is whatever the
 * client wrote, so the first entry was spoofable (a fresh rate limit bucket
 * per request). Behind more than one proxy layer, put the outer one in charge
 * of the header (or strip it) so the last entry is the client. Capped at 64 chars (IPv6 with headroom) so a hostile
 * multi-kilobyte XFF header can't land in an indexed column or a rate key.
 *
 * @param {object} req
 * @param {boolean} [trustProxy=false]
 * @returns {string|null}  the client IP, or null if unavailable
 */
function clientIp(req, trustProxy) {
    if (!req || !req.headers) return null;
    let ip;
    const xff = req.headers["x-forwarded-for"];
    if (trustProxy && typeof xff === "string" && xff !== "") {
        const parts = xff.split(",");
        const last = (parts[parts.length - 1] || "").trim();
        if (last) ip = last;
    }
    if (!ip && typeof req.remote_addr === "string" && req.remote_addr !== "") {
        ip = req.remote_addr;
    }
    if (typeof ip === "string" && ip.length > 64) ip = ip.slice(0, 64);
    return ip || null;
}

export const _request = { clientIp };
export default _request;

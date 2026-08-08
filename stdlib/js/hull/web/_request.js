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
 * trusted reverse proxy that sets it - returns the FIRST address of the
 * `X-Forwarded-For` chain (the original client), trimmed, falling back to the
 * socket peer. Capped at 64 chars (IPv6 with headroom) so a hostile
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
        const first = (xff.split(",")[0] || "").trim();
        if (first) ip = first;
    }
    if (!ip && typeof req.remote_addr === "string" && req.remote_addr !== "") {
        ip = req.remote_addr;
    }
    if (typeof ip === "string" && ip.length > 64) ip = ip.slice(0, 64);
    return ip || null;
}

export const _request = { clientIp };
export default _request;

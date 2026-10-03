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

/**
 * The key a per-client limit (a rate limit, a lockout) counts an address
 * under: an IPv4 address as itself, an IPv6 address as its /64. Anyone with
 * one IPv6 host holds a whole /64 and can take a fresh address per request, so
 * keyed by the full address every request had its own budget - the TOTP
 * per-IP gate and any login rate limit did nothing. IPv4-mapped addresses
 * (::ffff:a.b.c.d) count as the IPv4 address; anything that does not parse is
 * returned as it is. Same as hull.web._request.limit_key.
 */
function limitKey(ip) {
    if (typeof ip !== "string" || ip === "" || ip.indexOf(":") < 0) return ip;
    ip = ip.replace(/%.*$/, "");                            // zone ("fe80::1%eth0")
    const m4 = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/i.exec(ip);
    if (m4) return m4[1];
    const hex = /^[0-9a-fA-F]{1,4}$/;
    const split = (s) => (s === "" ? [] : s.split(":"));
    let groups;
    const dc = ip.indexOf("::");
    if (dc >= 0) {
        const h = split(ip.slice(0, dc));
        const t = split(ip.slice(dc + 2));
        if (h.length + t.length > 7) return ip;
        groups = h.concat(new Array(8 - h.length - t.length).fill("0"), t);
    } else {
        groups = ip.split(":");
        if (groups.length !== 8) return ip;
    }
    if (!groups.every((g) => hex.test(g))) return ip;
    return groups.slice(0, 4).map((g) => parseInt(g, 16).toString(16)).join(":") + "::/64";
}

/**
 * A user id as the stdlib stores and compares it (the user_id columns are
 * text): a non-empty string as is, a safe integer or bigint as its decimal
 * string. Every stdlib entry point that takes a user id goes through this,
 * so 42 and "42" are the same user everywhere - session, audit-log, totp,
 * rbac. Anything else (null, "", a float, an object) is no id: null. Same as
 * hull.web._request.user_id.
 */
function userId(id) {
    if (typeof id === "string") return id === "" ? null : id;
    if (typeof id === "number" && Number.isSafeInteger(id)) return String(id);
    if (typeof id === "bigint") return id.toString();
    return null;
}

export const _request = { clientIp, limitKey, userId };
export default _request;

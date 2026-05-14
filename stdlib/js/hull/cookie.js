/*
 * hull:cookie -- Cookie parsing and serialization
 *
 * cookie.parse(headerString)        -> object of name/value pairs
 * cookie.serialize(name, value, opts) -> Set-Cookie header string
 * cookie.clear(name, opts)          -> Set-Cookie header that expires the cookie
 *
 * Defaults: httpOnly=true, secure=true, sameSite="Lax", path="/"
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

function parse(headerString) {
    const result = {};
    if (!headerString || typeof headerString !== "string")
        return result;

    const pairs = headerString.split(";");
    for (let i = 0; i < pairs.length; i++) {
        const pair = pairs[i].trim();
        if (pair.length === 0) continue;

        const eqIdx = pair.indexOf("=");
        if (eqIdx < 0) continue;

        const name = pair.substring(0, eqIdx).trim();
        if (name.length === 0) continue;

        let value = pair.substring(eqIdx + 1).trim();

        // Strip surrounding quotes if present
        if (value.length >= 2 && value[0] === '"' && value[value.length - 1] === '"')
            value = value.substring(1, value.length - 1);

        // Decode percent-encoded values
        try {
            result[name] = decodeURIComponent(value);
        } catch (e) {
            result[name] = value;
        }
    }

    return result;
}

function serialize(name, value, opts) {
    if (!name || typeof name !== "string")
        throw new Error("cookie name is required");
    if (/[=;,\s\x00-\x1f\x7f]/.test(name))
        throw new Error("cookie name contains invalid characters");
    if (value !== null && value !== undefined && /[\x00-\x1f\x7f;]/.test(String(value)))
        throw new Error("cookie value contains invalid characters");

    const o = opts || {};

    // Percent-encode the value
    let encoded;
    if (value === null || value === undefined)
        encoded = "";
    else
        encoded = encodeURIComponent(String(value));

    let str = name + "=" + encoded;

    // Path (default: "/")
    const path = o.path !== undefined ? o.path : "/";
    if (path)
        str += "; Path=" + path;

    // Domain
    if (o.domain) {
        if (!/^[a-zA-Z0-9._-]+$/.test(o.domain))
            throw new Error("cookie: invalid domain");
        str += "; Domain=" + o.domain;
    }

    // MaxAge
    if (o.maxAge !== undefined && o.maxAge !== null) {
        const maxAge = Math.floor(o.maxAge);
        if (isNaN(maxAge))
            throw new Error("maxAge must be a number");
        str += "; Max-Age=" + maxAge;
    }

    // Expires
    if (o.expires) {
        if (typeof o.expires === "string") {
            if (/[\x00-\x1f;]/.test(o.expires))
                throw new Error("cookie: invalid expires value");
            str += "; Expires=" + o.expires;
        } else if (typeof o.expires === "number")
            str += "; Expires=" + new Date(o.expires * 1000).toUTCString();
    }

    // HttpOnly (default: true)
    const httpOnly = o.httpOnly !== undefined ? o.httpOnly : true;
    if (httpOnly)
        str += "; HttpOnly";

    // Secure=true by default. Set secure=false explicitly for local HTTP dev.
    const secure = o.secure !== undefined ? o.secure : true;
    if (secure)
        str += "; Secure";

    // SameSite (default: "Lax")
    const sameSite = o.sameSite !== undefined ? o.sameSite : "Lax";
    if (sameSite) {
        const ss = String(sameSite);
        if (ss === "Strict" || ss === "Lax" || ss === "None")
            str += "; SameSite=" + ss;
        else
            throw new Error("sameSite must be Strict, Lax, or None");
    }

    return str;
}

// Whitelisted Set-Cookie attributes. Avoiding Object.assign here prevents
// prototype-pollution if the caller's opts object inherits adversarial keys.
const COOKIE_ATTRS = ["path", "domain", "httpOnly", "secure", "sameSite",
                       "maxAge", "expires"];

function clear(name, opts) {
    const o = Object.create(null);
    if (opts) {
        for (const k of COOKIE_ATTRS) {
            if (Object.prototype.hasOwnProperty.call(opts, k))
                o[k] = opts[k];
        }
    }
    o.maxAge = 0;
    o.expires = "Thu, 01 Jan 1970 00:00:00 GMT";
    return serialize(name, "", o);
}

const cookie = { parse, serialize, clear };
export { cookie };

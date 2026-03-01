/*
 * hull:cookie -- Cookie parsing and serialization
 *
 * cookie.parse(headerString)        -> object of name/value pairs
 * cookie.serialize(name, value, opts) -> Set-Cookie header string
 * cookie.clear(name, opts)          -> Set-Cookie header that expires the cookie
 *
 * Defaults: httpOnly=true, secure=false, sameSite="Lax", path="/"
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
    if (o.domain)
        str += "; Domain=" + o.domain;

    // MaxAge
    if (o.maxAge !== undefined && o.maxAge !== null) {
        const maxAge = Math.floor(o.maxAge);
        if (isNaN(maxAge))
            throw new Error("maxAge must be a number");
        str += "; Max-Age=" + maxAge;
    }

    // Expires
    if (o.expires) {
        if (typeof o.expires === "string")
            str += "; Expires=" + o.expires;
        else if (typeof o.expires === "number")
            str += "; Expires=" + new Date(o.expires * 1000).toUTCString();
    }

    // HttpOnly (default: true)
    const httpOnly = o.httpOnly !== undefined ? o.httpOnly : true;
    if (httpOnly)
        str += "; HttpOnly";

    // Secure (default: false — set true when serving over HTTPS)
    const secure = o.secure !== undefined ? o.secure : false;
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

function clear(name, opts) {
    const o = Object.assign({}, opts || {});
    o.maxAge = 0;
    o.expires = "Thu, 01 Jan 1970 00:00:00 GMT";
    return serialize(name, "", o);
}

const cookie = { parse, serialize, clear };
export { cookie };

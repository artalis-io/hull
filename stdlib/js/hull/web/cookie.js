/**
 * @file hull:web:cookie
 * @module hull:web:cookie
 * @description HTTP cookie parsing and serialization.
 *
 * Lua parity: same surface as `hull.web.cookie` with camelCase options.
 *
 * @license AGPL-3.0-or-later
 */

/**
 * Parse a `Cookie` header string into a name-value object.
 *
 * Empty or non-string input returns an empty object (never throws).
 *
 * @param {string|null|undefined} headerString  Value of the inbound `Cookie` header.
 * @returns {Object.<string, string>}  Parsed cookies. `{}` when input is empty.
 *
 * @example
 * const cookies = cookie.parse(req.headers.cookie);
 * if (cookies.session_id) { ... }
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

/**
 * Serialize a cookie into a `Set-Cookie` header value.
 *
 * @param {string} name           Cookie name.
 * @param {string} value          Cookie value.
 * @param {Object} [opts]         Options.
 * @param {string}  [opts.path="/"]
 * @param {boolean} [opts.httpOnly=true]
 * @param {boolean} [opts.secure=true]
 * @param {"Lax"|"Strict"|"None"} [opts.sameSite="Lax"]
 * @param {number}  [opts.maxAge]
 * @param {string}  [opts.domain]
 * @param {string}  [opts.expires]  RFC 1123 date.
 * @returns {string}  `Set-Cookie` header value. Pair with `res.header("Set-Cookie", v)`.
 *
 * @example
 * res.header("Set-Cookie", cookie.serialize("session", sid, { sameSite: "Strict" }));
 */
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

    // Path (default: "/"). Reject control bytes and `;` so a
    // user-supplied path can't inject directives (e.g.
    // `; HttpOnly=false`) or split the Set-Cookie header.
    const path = o.path !== undefined ? o.path : "/";
    if (path) {
        if (/[;\r\n\x00]/.test(path))
            throw new Error("cookie: invalid path (contains ';', "
                + "CR, LF, or NUL)");
        str += "; Path=" + path;
    }

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

/**
 * Build a `Set-Cookie` value that deletes the named cookie.
 *
 * @param {string} name  Cookie name to clear.
 * @param {Object} [opts]  Same options as {@link serialize}; `path`/`domain` should match the original.
 * @returns {string}  `Set-Cookie` header value with `Max-Age=0`.
 */
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

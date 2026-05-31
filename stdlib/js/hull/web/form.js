/**
 * @file hull:web:form
 * @module hull:web:form
 * @description URL-encoded form-body parsing.
 *
 * Lua parity: same surface as `hull.web.form` with camelCase options.
 *
 * @license AGPL-3.0-or-later
 */

function decodePart(s) {
    const decoded = s.split("+").join(" ");
    try { return decodeURIComponent(decoded); }
    catch (e) { return decoded; }
}

/**
 * Parse a URL-encoded form body into a name-value object.
 *
 * Handles `+` → space and `%XX` percent-decoding. Last value wins for
 * duplicate keys. Empty/non-string input returns `{}` (never throws
 * except on `maxFields` overflow).
 *
 * @param {string} body              Body bytes from a POST `application/x-www-form-urlencoded` request.
 * @param {Object} [opts]            Options.
 * @param {number} [opts.maxFields=1000]  Hard cap on `&`-separated pairs.
 *   Exceeding throws.
 * @returns {Object.<string, string>}     Parsed fields, with a `null` prototype.
 * @throws {Error} If `body` contains more than `opts.maxFields` pairs.
 *
 * @example
 * const fields = form.parse(req.body);
 * const email = fields.email;
 */
function parse(body, opts) {
    const result = Object.create(null);
    if (!body || typeof body !== "string" || body.length === 0)
        return result;

    const maxFields = (opts && opts.maxFields) || 1000;
    const pairs = body.split("&");
    if (pairs.length > maxFields)
        throw new Error("form.parse: exceeded maxFields limit (" + maxFields + ")");
    for (let i = 0; i < pairs.length; i++) {
        const pair = pairs[i];
        if (pair.length === 0) continue;

        const eqIdx = pair.indexOf("=");
        if (eqIdx < 0) continue;

        const rawKey = pair.substring(0, eqIdx);
        if (rawKey.length === 0) continue;

        const rawValue = pair.substring(eqIdx + 1);
        result[decodePart(rawKey)] = decodePart(rawValue);
    }

    return result;
}

const form = { parse };
export { form };

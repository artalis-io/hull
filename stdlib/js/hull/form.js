/*
 * hull:form -- URL-encoded form body parsing
 *
 * form.parse(body) -> object of name/value pairs
 *
 * Decodes application/x-www-form-urlencoded strings.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

function decodePart(s) {
    var decoded = s.split("+").join(" ");
    try { return decodeURIComponent(decoded); }
    catch (e) { return decoded; }
}

function parse(body) {
    const result = {};
    if (!body || typeof body !== "string" || body.length === 0)
        return result;

    const pairs = body.split("&");
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

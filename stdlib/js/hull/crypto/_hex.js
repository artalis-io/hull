/*
 * Internal raw-byte hex helper shared across the crypto/auth stdlib.
 *
 * @module hull:crypto:_hex
 * @license AGPL-3.0-or-later
 *
 * Contributor-only (the `_` prefix). Encodes a byte string as lowercase hex,
 * ONE hex pair per byte / code unit (0-255).
 *
 * This deliberately does NOT use crypto.hexEncode. A JS string is UTF-8-encoded
 * at the C boundary, so crypto.hexEncode inflates any code unit >= 0x80
 * (0xFF -> "c3bf", not "ff") - wrong for hashing / HMAC keys / token wire
 * formats, and it diverges from Lua's crypto.hex_encode (which is raw, because
 * Lua strings are byte strings). This helper masks each code unit to a byte and
 * is byte-identical to the Lua sibling. See docs/stdlib_style.md §4.
 */

/**
 * Lowercase hex of a byte string (2 chars per code unit 0-255).
 * @param {string} s  raw bytes (each char a byte value 0-255)
 * @returns {string}
 */
function toHex(s) {
    let out = "";
    for (let i = 0; i < s.length; i++) {
        out += (s.charCodeAt(i) & 0xff).toString(16).padStart(2, "0");
    }
    return out;
}

export const _hex = { toHex };
export default _hex;

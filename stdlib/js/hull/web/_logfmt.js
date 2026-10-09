/**
 * @file hull:web:_logfmt
 * @module hull:web:_logfmt
 * @description Internal logfmt value formatting shared across the logging
 *   stdlib. Lua parity: `hull.web._logfmt`.
 *
 * Contributor-only (the `_` prefix): imported by `hull:web:middleware:logger`
 * and `hull:logx`, never declared by apps. One place for the escape + quote
 * rules so the two logfmt producers can't drift. See docs/stdlib_style.md
 * section 4.
 *
 * @license AGPL-3.0-or-later
 */

// Control characters (< 0x20, 0x7f) other than the three with a short escape
// go out as \xHH, so no raw control byte reaches the line. Same output as Lua.
const CTRL_ESC = { "\n": "\\n", "\r": "\\r", "\t": "\\t" };
function ctrl(c) {
    const e = CTRL_ESC[c];
    if (e !== undefined) return e;
    const h = c.charCodeAt(0).toString(16);
    return "\\x" + (h.length < 2 ? "0" + h : h);
}
const CTRL_RE = /[\x00-\x1f\x7f]/g;
// The C1 controls (U+0080..U+009F, NEL among them) and U+2028 / U+2029: line
// breaks to some log viewers and JSON-lines shippers (audit 11). Escaped as
// \uXXXX, as the Lua side does from their UTF-8 bytes.
const UBREAK_RE = /[\u0080-\u009f\u2028\u2029]/g;
const UBREAK_TEST = /[\u0080-\u009f\u2028\u2029]/;
function ubreak(c) {
    const h = c.charCodeAt(0).toString(16);
    return "\\u" + "0000".slice(h.length) + h;
}

/**
 * Escape a value for safe logfmt output (log-injection defense): a raw newline
 * could otherwise forge a second log line. Escapes backslash and double-quote,
 * \n \r \t, and every other character < 0x20 or 0x7f as \xHH.
 * @param {*} v
 * @returns {string}
 */
function sanitize(v) {
    return String(v)
        .replace(/\\/g, "\\\\")
        .replace(/"/g, '\\"')
        .replace(CTRL_RE, ctrl)
        .replace(UBREAK_RE, ubreak);
}

/**
 * Format a free-text log message as the line's leading msg="..." field,
 * always quoted and escaped as a value (audit 11): written bare, a message
 * carrying " user=admin" forged a field for any logfmt reader.
 * @param {*} v
 * @returns {string} e.g. msg="handled"
 */
function message(v) {
    return 'msg="' + sanitize(v) + '"';
}

// UTF-8 length of one code point (a lone surrogate counts 3, as WTF-8), so a
// non-ASCII key maps to as many "_" as the Lua side's per-byte replacement.
function utf8Len(c) {
    const cp = c.codePointAt(0);
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

/**
 * Make a logfmt key: every character outside [A-Za-z0-9_.-] becomes "_" (one
 * per UTF-8 byte, as in Lua; an empty key is "_"), so a key cannot carry a
 * space, "=", a quote or a newline and split or forge a pair.
 * @param {*} k
 * @returns {string}
 */
function key(k) {
    const s = String(k).replace(/[^A-Za-z0-9_.\-]/gu, (c) => "_".repeat(utf8Len(c)));
    return s === "" ? "_" : s;
}

/**
 * Format one key=value logfmt pair: the key through key(), the value through
 * sanitize(), quoted when the RAW value contains a space, "=", '"' or any
 * control character.
 * @param {string} k
 * @param {*} v
 * @returns {string}
 */
function pair(k, v) {
    const raw = String(v);
    const s = sanitize(raw);
    if (/[ ="\x00-\x1f\x7f]/.test(raw) || UBREAK_TEST.test(raw))
        return key(k) + '="' + s + '"';
    return key(k) + "=" + s;
}

export const _logfmt = { sanitize, message, key, pair };
export default _logfmt;

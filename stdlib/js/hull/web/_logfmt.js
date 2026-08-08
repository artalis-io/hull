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

/**
 * Escape a value for safe logfmt output (log-injection defense): a raw newline
 * could otherwise forge a second log line. Escapes backslash, CR, LF, and
 * double-quote.
 * @param {*} v
 * @returns {string}
 */
function sanitize(v) {
    return String(v)
        .replace(/\\/g, "\\\\")
        .replace(/\n/g, "\\n")
        .replace(/\r/g, "\\r")
        .replace(/"/g, '\\"');
}

/**
 * Format one key=value logfmt pair, quoting the value when the RAW value
 * contains a space, `=`, `"`, or a CR/LF.
 * @param {string} k
 * @param {*} v
 * @returns {string}
 */
function pair(k, v) {
    const raw = String(v);
    const s = sanitize(raw);
    if (/[ ="\n\r]/.test(raw)) return k + '="' + s + '"';
    return k + "=" + s;
}

export const _logfmt = { sanitize, pair };
export default _logfmt;

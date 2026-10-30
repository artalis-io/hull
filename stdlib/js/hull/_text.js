// hull:_text - text helpers for the stdlib (internal: the underscore keeps it
// out of app code).
//
// storable(v, max, ellipsis, unconvertible): app, request or remote text as
// text every database backend stores (audits 38 / 39). Postgres refuses a NUL
// and text that is not valid UTF-8 - a lone surrogate (an emoji sliced in
// half by substring) encodes as such - and MySQL's utf8mb4 columns refuse
// invalid UTF-8 and values over the column (64 KiB for TEXT). The stdlib
// writes such text - a User-Agent, a handler's error, a filename, a remote
// error body - after a step it cannot undo (a session rotated, a single-use
// token consumed, a message delivered), and a refused write there wedged the
// row or failed the request after the side effect. So, for any value:
//   * a non-string is converted under try (toString / a getter is app code
//     that may throw), and one that cannot be converted becomes
//     `unconvertible` (default UNCONVERTIBLE);
//   * NULs are dropped;
//   * a lone surrogate becomes U+FFFD;
//   * the text is capped at `max` UTF-8 bytes (default TEXT_MAX), cut on a
//     code point, with "..." appended when `ellipsis` is true and it was cut.
// The Lua twin (hull._text.storable) applies the same rules to bytes. A new
// stdlib write of app, request or remote text goes through this (CLAUDE.md).

const TEXT_MAX = 4096;
const UNCONVERTIBLE = "(a value that could not be converted to text)";

function storable(v, max, ellipsis, unconvertible) {
    if (typeof max !== "number" || !(max > 0)) max = TEXT_MAX;
    const fallback = typeof unconvertible === "string" ? unconvertible : UNCONVERTIBLE;
    let s;
    if (typeof v === "string") s = v;
    else {
        try { s = String(v); } catch (_e) { s = fallback; }
        if (typeof s !== "string") s = fallback;
    }
    // Every kept unit is at least one byte, so 2 x max units bound the scan
    // for a huge string of NULs.
    let cut = false;
    if (s.length > 2 * max) { s = s.slice(0, 2 * max); cut = true; }
    // Kept runs are sliced out whole (in this QuickJS `out += c` copies
    // `out` each time) and joined once.
    const parts = [];
    let start = 0, bytes = 0, i = 0;
    const n = s.length;
    for (; i < n; i++) {
        const c = s.charCodeAt(i);
        let w;
        if (c === 0) {
            parts.push(s.slice(start, i)); start = i + 1;
            continue;
        }
        if (c < 0x80) w = 1;
        else if (c < 0x800) w = 2;
        else if (c >= 0xd800 && c <= 0xdbff && i + 1 < n &&
                 (s.charCodeAt(i + 1) & 0xfc00) === 0xdc00) w = 4;
        else w = 3;
        if (bytes + w > max) { cut = true; break; }
        bytes += w;
        if (w === 4) { i++; continue; }
        if (c >= 0xd800 && c <= 0xdfff) {               // a lone surrogate
            parts.push(s.slice(start, i), "\ufffd"); start = i + 1;
        }
    }
    parts.push(s.slice(start, i));
    const out = parts.join("");
    return cut && ellipsis ? out + "..." : out;
}

// A byte string (one character per byte) read as UTF-8, each byte that does
// not start a valid sequence (an overlong, a surrogate, past U+10FFFF, a
// truncated one) as U+FFFD - the Lua stdlib's rule. For bytes a Lua writer
// stored (a base64 blob) that JS must read as text (audit 39).
function fromUtf8Lenient(bs) {
    const parts = [];
    let buf = [];
    const push = (c) => {
        buf.push(c);
        if (buf.length >= 8192) { parts.push(String.fromCharCode.apply(null, buf)); buf = []; }
    };
    const n = bs.length;
    for (let i = 0; i < n;) {
        const b = bs.charCodeAt(i) & 0xff;
        if (b < 0x80) { push(b); i++; continue; }
        let k, c, min;
        if (b >= 0xc2 && b <= 0xdf) { k = 1; c = b & 0x1f; min = 0x80; }
        else if (b >= 0xe0 && b <= 0xef) { k = 2; c = b & 0x0f; min = 0x800; }
        else if (b >= 0xf0 && b <= 0xf4) { k = 3; c = b & 0x07; min = 0x10000; }
        else { push(0xfffd); i++; continue; }
        let ok = i + k <= n - 1;
        for (let j = 1; ok && j <= k; j++) {
            const x = bs.charCodeAt(i + j) & 0xff;
            if ((x & 0xc0) !== 0x80) ok = false;
            else c = (c << 6) | (x & 0x3f);
        }
        if (!ok || c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
            push(0xfffd); i++; continue;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            push(0xd800 + (c >> 10)); push(0xdc00 + (c & 0x3ff));
        } else push(c);
        i += k + 1;
    }
    if (buf.length) parts.push(String.fromCharCode.apply(null, buf));
    return parts.join("");
}

export const _text = { TEXT_MAX, UNCONVERTIBLE, storable, fromUtf8Lenient };
export default _text;

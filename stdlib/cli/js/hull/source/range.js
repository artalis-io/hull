// hull:source:range - language-neutral SourceRange + byte->line/col line map (JS mirror of
// hull.source.range / range.lua).
//
// A range is a half-open byte interval { start, stop } over the source bytes, using Hull's
// 1-BASED byte convention (matching range.lua and the neutral ProjectDiscovery model): the
// first byte is offset 1 and `stop` is one-past-end, so the exact bytes are
// [start, stop). An empty range has start === stop. The JS lexer scans a 0-based
// Uint8Array with a byte cursor and records 1-based offsets (cursor + 1), so ranges from
// the JS and Lua frontends are directly comparable on the wire.
//
// Line/column are resolved ON DEMAND from a per-source line-start index (built once,
// binary-searched), not duplicated onto every node. Columns are 1-based BYTE columns
// within the line. Line terminators follow ECMAScript: LF, CR, CRLF (one break), and the
// Unicode LINE/PARAGRAPH SEPARATORS U+2028 / U+2029 (each 3 UTF-8 bytes E2 80 A8 / A9).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Construct a half-open range. No validation here (callers pass lexer positions).
export function newRange(start, stop) {
    return { start: start, stop: stop };
}

// Build the line-start index: 1-based byte offset of the first byte of each line. Line 1
// starts at offset 1. A CRLF pair counts as ONE break. U+2028 / U+2029 also start a new
// line. `bytes` is a Uint8Array. A final line without a trailing terminator adds no entry.
export function linemap(bytes) {
    const starts = [1];
    const n = bytes.length;
    let i = 0;                        // 0-based cursor into bytes
    while (i < n) {
        const c = bytes[i];
        if (c === 0x0a) {             // LF
            i += 1;
            starts.push(i + 1);
        } else if (c === 0x0d) {      // CR or CRLF
            i += (i + 1 < n && bytes[i + 1] === 0x0a) ? 2 : 1;
            starts.push(i + 1);
        } else if (c === 0xe2 && i + 2 < n && bytes[i + 1] === 0x80 &&
                   (bytes[i + 2] === 0xa8 || bytes[i + 2] === 0xa9)) {  // U+2028 / U+2029
            i += 3;
            starts.push(i + 1);
        } else {
            i += 1;
        }
    }
    return starts;
}

// Resolve a 1-based byte offset to { line, col }, both 1-based; col is the byte column
// within the line. An offset one past end-of-source resolves to the last line at the column
// after its last byte. Binary search for the largest line-start <= off.
export function position(starts, off) {
    if (off < 1) off = 1;
    let lo = 0, hi = starts.length - 1;
    while (lo < hi) {
        const mid = (lo + hi + 1) >> 1;
        if (starts[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return { line: lo + 1, col: off - starts[lo] + 1 };
}

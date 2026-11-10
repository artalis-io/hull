// hull:_tui_layout - the pure layout and line-editing helpers behind hull:tui
// (internal: the underscore keeps it out of app code).
//
// Split out of tui.js so the arithmetic can be tested without a terminal:
// hull:tui itself needs the native terminal bindings, which a TUI-free build
// does not have. Nothing here touches the terminal. The Lua twin is
// hull._tui_layout; both count text by code point (one cell each - the cap
// layer's East Asian width table is not exposed to scripts, so a wide
// character still counts as one).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

/* A codepoint counts as "printable input" only if it's outside the C0
 * control range (< 0x20), not DEL (0x7F), and not in the C1 range
 * (0x80..0x9F). tui.input uses this to reject keystrokes that would
 * otherwise inject terminal escape bytes into the returned buffer. */
function isPrintableCp(cp) {
    if (!cp) return false;
    if (cp < 0x20) return false;
    if (cp === 0x7F) return false;
    if (cp >= 0x80 && cp <= 0x9F) return false;
    return true;
}

/* Code points in s (a lone surrogate counts as one). */
function codepoints(s) {
    let n = 0;
    for (let i = 0; i < s.length; i++) {
        n++;
        if (isHigh(s.charCodeAt(i)) && isLow(s.charCodeAt(i + 1))) i++;
    }
    return n;
}

function isHigh(u) { return u >= 0xD800 && u <= 0xDBFF; }
function isLow(u)  { return u >= 0xDC00 && u <= 0xDFFF; }

/* Truncate by code point count, never cutting a surrogate pair. */
function truncateByCodepoints(s, maxCells) {
    if (maxCells <= 0) return "";
    const cps = Array.from(s);
    if (cps.length <= maxCells) return s;
    return cps.slice(0, maxCells).join("");
}

/* UTF-16 index of the character before `i` / just past the one at `i`:
 * a surrogate pair is one step. */
function prevChar(s, i) {
    if (i >= 2 && isLow(s.charCodeAt(i - 1)) && isHigh(s.charCodeAt(i - 2))) return i - 2;
    return i - 1;
}
function nextChar(s, i) {
    if (isHigh(s.charCodeAt(i)) && isLow(s.charCodeAt(i + 1))) return i + 2;
    return i + 1;
}

/* One key event applied to a line being edited: returns the new
 * { buf, cursor }, or { done: value } when the edit ends. `cursor` is a
 * UTF-16 index that always sits on a character boundary - it stepped one
 * unit, so backspace after an emoji left a lone high surrogate and
 * left / right parked between the halves (audit 44). `maxLen` counts code
 * points, as the Lua twin's max_len does. */
function inputStep(buf, cursor, ev, maxLen) {
    const k = ev.key;
    if (k === "enter") return { done: buf };
    if (k === "escape" || k === "ctrl+c") return { done: false };
    if (k === "backspace") {
        if (cursor > 0) {
            const p = prevChar(buf, cursor);
            buf = buf.slice(0, p) + buf.slice(cursor);
            cursor = p;
        }
    } else if (k === "delete") {
        if (cursor < buf.length) buf = buf.slice(0, cursor) + buf.slice(nextChar(buf, cursor));
    } else if (k === "left") {
        if (cursor > 0) cursor = prevChar(buf, cursor);
    } else if (k === "right") {
        if (cursor < buf.length) cursor = nextChar(buf, cursor);
    } else if (k === "home") {
        cursor = 0;
    } else if (k === "end") {
        cursor = buf.length;
    } else if (k === "ctrl+u") {
        buf = buf.slice(cursor);
        cursor = 0;
    } else if (k === "ctrl+k") {
        buf = buf.slice(0, cursor);
    } else if (isPrintableCp(ev.codepoint) && !ev.ctrl && !ev.alt) {
        /* isPrintableCp has already filtered C0/DEL/C1, so ev.key here
         * can't carry an ANSI-injection payload. */
        if (typeof ev.key === "string" && codepoints(buf) < maxLen) {
            buf = buf.slice(0, cursor) + ev.key + buf.slice(cursor);
            cursor += ev.key.length;
        }
    }
    return { buf, cursor };
}

/* What tui.input draws: the (masked) text, its column and the cursor's
 * column, all counted in code points. */
function inputView(promptStr, buf, cursor, password) {
    const display = password ? "*".repeat(codepoints(buf)) : buf;
    const x = codepoints(promptStr) + 1;
    return { display, x, cursorX: x + codepoints(buf.slice(0, cursor)) };
}

/* The top edge of a frame `w` cells wide with the title slotted in:
 * ┌─ title ─...─┐. The framing takes five cells, so the title gets at
 * most w-5; the fill is counted in code points (counted in UTF-16 units,
 * an astral title made the edge short - audit 44). */
function frameTop(b, w, title) {
    let top = b.tl + b.h.repeat(w - 2) + b.tr;
    if (title && w - 5 > 0) {
        const tt = truncateByCodepoints(String(title), w - 5);
        const prefix = b.tl + b.h + " ";
        const fill = Math.max(0, w - codepoints(prefix) - codepoints(tt) - 2);
        top = prefix + tt + " " + b.h.repeat(fill) + b.tr;
    }
    return top;
}

/* Rows tui.list shows items in: the terminal less the one title row, or
 * opts.height; at least 1, at most `count`. */
function listHeight(rows, title, height, count) {
    return clamp(height ?? (rows - (title ? 1 : 0)), 1, count);
}

export const _tui_layout = {
    clamp, isPrintableCp, codepoints, truncateByCodepoints, inputStep,
    inputView, frameTop, listHeight,
};
export default _tui_layout;

// hull:source:annotations - the generic JSDoc @tag scanner + declaration attachment.
//
// Turns each @tag inside a JSDoc block comment (/** ... */) into a structured annotation
// record and attaches contiguous leading runs of them to the declaration they document.
// Deliberately WHITELIST-FREE: `name` is whatever follows @, so an app's own @query /
// @compute / @route is captured with the same fidelity as a standard @param / @returns.
// Consumers give names meaning; this layer only records them, with exact byte ranges.
//
// Annotation record (mirrors the Lua parse layer, docs/js_frontend_slice3_annotations.md 3.3):
//   { name: "param",             // identifier after @
//     args: "a, b" | undefined,  // raw text inside a balanced (...) group after the name
//     text: "x f64" | undefined, // trailing free text after name/(...), cleaned + trimmed
//     raw:  "@param x f64",      // exact source bytes of the tag (verbatim)
//     range: { start, stop } }   // half-open 1-based byte range of the tag itself
//
// On a jsdoc comment: comment.annotationList = its own tags (array, possibly empty).
// On a declaration target: node.annotationList (flattened run, top->down) + node.annotations
// (name -> first). Attachment targets are VariableDeclaration / FunctionDeclaration /
// ClassDeclaration (and the inner declaration of an export wrapper).
//
// NEVER raises. Malformed tag CONTENT is handled by deterministic, metadata-preserving
// fallbacks (never js.internal). An unexpected internal defect (a recognized declaration
// target with an invalid range, or an attach exception) emits js.internal through the shared
// budget and keeps the AST, so `diagnostics empty` guarantees attachment did not internally
// fail. See docs/js_frontend_slice3_annotations.md 9.1.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

function isWs(b) { return b === 0x09 || b === 0x0a || b === 0x0b || b === 0x0c || b === 0x0d || b === 0x20; }
function isNameStart(b) { return (b >= 0x41 && b <= 0x5a) || (b >= 0x61 && b <= 0x7a) || b === 0x5f; }

// 1-based line of a 1-based byte offset (binary search over linemap starts).
function lineOf(starts, off) {
    if (off < 1) off = 1;
    let lo = 0, hi = starts.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (starts[mid] <= off) lo = mid; else hi = mid - 1; }
    return lo + 1;
}

// Decode a [start1, stop1) 1-based byte span to a string (UTF-8, ASCII fast path; an invalid
// sequence yields U+FFFD, matching the lexer). Ranges are byte offsets, never derived from this.
function decodeSpan(bytes, start1, stop1) {
    let s = "";
    for (let i = start1 - 1; i < stop1 - 1; i++) {
        const c = bytes[i];
        if (c < 0x80) { s += String.fromCharCode(c); continue; }
        let cp, len;
        if ((c & 0xe0) === 0xc0) { cp = c & 0x1f; len = 2; }
        else if ((c & 0xf0) === 0xe0) { cp = c & 0x0f; len = 3; }
        else if ((c & 0xf8) === 0xf0) { cp = c & 0x07; len = 4; }
        else { s += "\uFFFD"; continue; }
        let ok = true;
        for (let k = 1; k < len; k++) { const cc = bytes[i + k]; if (cc === undefined || (cc & 0xc0) !== 0x80) { ok = false; break; } cp = (cp << 6) | (cc & 0x3f); }
        if (!ok) { s += "\uFFFD"; continue; }
        s += String.fromCodePoint(cp);
        i += len - 1;
    }
    return s;
}

// Parse one tag's already-margin-stripped content lines into a record. `raw` is the verbatim
// source span [start1, stop1); the joined body drives name/args/text. Malformed groups fall
// back deterministically (docs 3.2): an unmatched ( becomes text, a balanced-but-quote-agnostic
// ) closes the group early. Returns null only when there is no name (defensive).
export function parseTag(parts, start1, stop1, bytes) {
    const raw = decodeSpan(bytes, start1, stop1);
    const body = parts.join(" ");                          // margin-stripped, single-space joined
    const m = body.match(/^@([A-Za-z_][A-Za-z0-9_]*)/);
    if (!m) return null;
    const name = m[1];
    let rest = body.slice(m[0].length).replace(/^\s+/, "");
    let args, text;
    if (rest.charCodeAt(0) === 0x28) {                     // '('
        let depth = 0, end = -1;
        for (let i = 0; i < rest.length; i++) {
            const ch = rest.charCodeAt(i);
            if (ch === 0x28) depth++;
            else if (ch === 0x29) { depth--; if (depth === 0) { end = i; break; } }
        }
        if (end < 0) { text = rest; }                     // unmatched ( -> whole remainder is text
        else { args = rest.slice(1, end); text = rest.slice(end + 1).replace(/^\s+|\s+$/g, ""); }
    } else {
        text = rest.replace(/\s+$/, "");
    }
    if (text === "") text = undefined;
    return { name: name, args: args, text: text, raw: raw, range: { start: start1, stop: stop1 } };
}

// Scan one jsdoc comment's interior into an ordered array of annotation records. A tag opens at
// a line-leading @ (after optional whitespace + optional single star margin + whitespace) and
// extends over continuation lines until the next tag or the block end.
export function scanBlock(comment, bytes, linemap) {
    const out = [];
    const n = bytes.length;
    const intStart = comment.start + 3;                   // after /**
    const intEnd = comment.stop - 2;                      // before */ (1-based exclusive)
    if (intEnd <= intStart) return out;

    const firstLine = lineOf(linemap, intStart);
    const lastLine = lineOf(linemap, intEnd - 1);

    // Per interior line: content start (after margin), content end (trailing ws trimmed), and
    // whether it opens a tag (line-leading @ + name-start).
    const lines = [];
    for (let L = firstLine; L <= lastLine; L++) {
        let ls = linemap[L - 1];
        let le = (L < linemap.length ? linemap[L] : n + 1);
        if (ls < intStart) ls = intStart;
        if (le > intEnd) le = intEnd;
        if (ls >= le) { lines.push({ open: false, at: ls, end: ls }); continue; }
        let j = ls;
        while (j < le && isWs(bytes[j - 1])) j++;
        if (j < le && bytes[j - 1] === 0x2a) j++;         // one star margin
        while (j < le && isWs(bytes[j - 1])) j++;
        let end = le;
        while (end > j && isWs(bytes[end - 2])) end--;
        const opensTag = j < end && bytes[j - 1] === 0x40 && isNameStart(bytes[j]);   // @ + name-start
        lines.push({ open: opensTag, at: j, end: end });
    }

    // Group: a tag = an opening line plus subsequent non-opening (continuation) lines.
    let k = 0;
    while (k < lines.length) {
        if (!lines[k].open) { k++; continue; }
        const tagStart = lines[k].at;                     // the @
        const parts = [];
        let lastEnd = lines[k].end;
        if (lines[k].end > tagStart) parts.push(decodeSpan(bytes, tagStart, lines[k].end));
        let m = k + 1;
        while (m < lines.length && !lines[m].open) {
            if (lines[m].end > lines[m].at) { parts.push(decodeSpan(bytes, lines[m].at, lines[m].end)); lastEnd = lines[m].end; }
            m++;
        }
        if (lastEnd < tagStart) lastEnd = tagStart;
        const rec = parseTag(parts, tagStart, lastEnd, bytes);
        if (rec) out.push(rec);
        k = m;
    }
    return out;
}

// True iff every physical line in [sLine, eLine] is COMMENT-ONLY: each non-whitespace byte is
// inside some comment's [start, stop). `sorted` is the comments sorted by start (non-overlapping).
function insideAnyComment(p1, sorted) {
    let lo = 0, hi = sorted.length - 1, idx = -1;
    while (lo <= hi) { const mid = (lo + hi) >> 1; if (sorted[mid].start <= p1) { idx = mid; lo = mid + 1; } else hi = mid - 1; }
    return idx >= 0 && p1 < sorted[idx].stop;
}
function linesAreCommentOnly(sLine, eLine, sorted, bytes, linemap, n) {
    for (let L = sLine; L <= eLine; L++) {
        const ls = linemap[L - 1];
        const le = (L < linemap.length ? linemap[L] : n + 1);
        for (let p1 = ls; p1 < le; p1++) {
            const b = bytes[p1 - 1];
            if (isWs(b)) continue;
            if (!insideAnyComment(p1, sorted)) return false;
        }
    }
    return true;
}

// Scan every jsdoc comment into comment.annotationList, then attach leading runs to declaration
// targets. Best-effort + hardened: an internal defect emits js.internal via `budget` (keeps the
// AST); malformed tag content never does.
export function attach(ast, comments, bytes, linemap, budget, path) {
    const n = bytes.length;
    // `where` is any object carrying a flat start/stop (an AST node or a comment); null if absent.
    function emitInternal(msg, where) {
        const r = (where && typeof where.start === "number" && typeof where.stop === "number") ? { start: where.start, stop: where.stop } : null;
        budget.push("error", "js.internal", "annotation attachment: " + msg, r);
    }
    try {
        if (!ast || typeof ast !== "object" || !Array.isArray(comments)) return;   // non-target shape: skip

        // 1. scan jsdoc comments; a broken lexer range on a jsdoc comment is an internal defect.
        for (let i = 0; i < comments.length; i++) {
            const c = comments[i];
            if (!c || c.kind !== "jsdoc") continue;
            if (typeof c.start !== "number" || typeof c.stop !== "number" || c.start < 1 || c.stop < c.start || c.stop > n + 1) {
                emitInternal("jsdoc comment has invalid range", c);
                c.annotationList = [];
                continue;
            }
            c.annotationList = scanBlock(c, bytes, linemap);
        }

        // 2. index participating comments (all their lines comment-only) by end line.
        const sorted = [];
        for (let i = 0; i < comments.length; i++) {
            const c = comments[i];
            if (c && typeof c.start === "number" && typeof c.stop === "number" && c.start >= 1 && c.stop >= c.start) sorted.push(c);
        }
        sorted.sort(function (a, b) { return a.start - b.start; });
        const byEndLine = new Map();
        for (let i = 0; i < sorted.length; i++) {
            const c = sorted[i];
            const sLine = lineOf(linemap, c.start);
            const eLine = lineOf(linemap, c.stop - 1);
            if (linesAreCommentOnly(sLine, eLine, sorted, bytes, linemap, n)) byEndLine.set(eLine, { startLine: sLine, comment: c });
        }

        // 3. walk the AST; attach a leading run to each recognized declaration target.
        function attachRun(node, effStart) {
            const declLine = lineOf(linemap, effStart);
            const run = [];
            let target = declLine - 1;
            while (byEndLine.has(target)) { const info = byEndLine.get(target); run.push(info.comment); target = info.startLine - 1; }
            if (run.length === 0) return;
            const list = [], byName = {};
            for (let i = run.length - 1; i >= 0; i--) {           // top comment first
                const c = run[i];
                const tags = (c.kind === "jsdoc" && Array.isArray(c.annotationList)) ? c.annotationList : [];
                for (let t = 0; t < tags.length; t++) { const a = tags[t]; list.push(a); if (byName[a.name] === undefined) byName[a.name] = a; }
            }
            if (list.length > 0) { node.annotationList = list; node.annotations = byName; }
        }
        function isTarget(t) { return t === "VariableDeclaration" || t === "FunctionDeclaration" || t === "ClassDeclaration"; }
        // Nodes carry FLAT start/stop (1-based byte offsets); this half-open pair IS the range.
        function badRange(node) {
            return typeof node.start !== "number" || typeof node.stop !== "number"
                || node.start < 1 || node.stop < node.start || node.stop > n + 1;
        }
        function walk(node, parent) {
            if (!node || typeof node !== "object") return;
            const t = node.type;
            if (isTarget(t)) {
                if (badRange(node)) {
                    emitInternal("declaration target has invalid range", node);        // recognized target must satisfy the invariant
                } else {
                    const isExportInner = parent && (parent.type === "ExportNamedDeclaration" || parent.type === "ExportDefaultDeclaration")
                        && parent.declaration === node && typeof parent.start === "number";
                    attachRun(node, isExportInner ? parent.start : node.start);
                }
            }
            for (const kk in node) {
                if (kk === "start" || kk === "stop" || kk === "annotations" || kk === "annotationList") continue;
                const v = node[kk];
                if (Array.isArray(v)) { for (let a = 0; a < v.length; a++) walk(v[a], node); }
                else if (v && typeof v === "object") walk(v, node);
            }
        }
        walk(ast, null);
    } catch (e) {
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        budget.push("error", "js.internal", "annotation attachment failed: " + msg, null);
    }
}

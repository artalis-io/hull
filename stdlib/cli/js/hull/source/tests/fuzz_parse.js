// hull:source:tests:fuzz_parse - the TEST-ONLY compact entry the JS parser libFuzzer drives.
//
// Under source/tests/, so the production cli-js registry generator (which prunes any tests
// directory) NEVER embeds it; it is reachable only via the test/fuzz registry. Design:
// docs/js_source_fuzz_design.md.
//
// Contract: fuzz(srcBuf, path, opts) calls parse() DIRECTLY (retains nothing, unlike the
// frontend adapter), validates the whole SourceUnit internally, retains NO AST/declaration
// state, and returns a TINY verdict: { ok:true } or { ok:false, reason }. It adds no
// production API (raw-equality uses the reviewed reference decoder refSlice below, mirroring
// the lexer's sliceText/decode byte-for-byte -- NOT a unit:text() method).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { parse } from "hull:source:parser";

// Reviewed reference byte->string decoder, byte-for-byte identical to lexer.js's
// decode()/sliceText(): ASCII verbatim; a valid 2/3/4-byte UTF-8 sequence -> its code point;
// ANY decode error -> a single U+FFFD advancing exactly one byte (not `len`). If the lexer's
// decoder changes, this reviewed copy must change with it.
function refDecode(bytes, i) {
    var b0 = bytes[i];
    if (b0 < 0x80) return { cp: b0, len: 1 };
    var need, cp, min;
    if ((b0 & 0xe0) === 0xc0) { need = 1; cp = b0 & 0x1f; min = 0x80; }
    else if ((b0 & 0xf0) === 0xe0) { need = 2; cp = b0 & 0x0f; min = 0x800; }
    else if ((b0 & 0xf8) === 0xf0) { need = 3; cp = b0 & 0x07; min = 0x10000; }
    else return { err: true, len: 1 };
    for (var k = 1; k <= need; k++) {
        var bk = bytes[i + k];
        if (bk === undefined || (bk & 0xc0) !== 0x80) return { err: true, len: 1 };
        cp = (cp << 6) | (bk & 0x3f);
    }
    if (cp < min) return { err: true, len: need + 1 };
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return { err: true, len: need + 1 };
    return { cp: cp, len: need + 1 };
}
// refSlice(bytes, a1, b1): decode the 1-based half-open byte range [a1, b1) the lexer's way.
function refSlice(bytes, a1, b1) {
    var s = "";
    for (var i = a1 - 1; i < b1 - 1; i++) {
        var c = bytes[i];
        if (c < 0x80) { s += String.fromCharCode(c); }
        else { var d = refDecode(bytes, i); if (d.err) { s += "\uFFFD"; } else { s += String.fromCodePoint(d.cp); i += d.len - 1; } }
    }
    return s;
}

function isInt(x) { return typeof x === "number" && isFinite(x) && Math.floor(x) === x; }

// 4.1 range integrality + bounds. Returns a reason string on breach, else null.
function checkRange(r, n, what) {
    if (r === null || r === undefined) return null;   // a null range is allowed (whole-unit)
    if (!isInt(r.start) || !isInt(r.stop)) return what + ": non-integral range";
    if (r.start < 1 || r.stop < r.start || r.stop > n + 1) return what + ": range out of [1," + (n + 1) + "] or inverted";
    return null;
}

function fuzz(srcBuf, path, opts) {
    var bytes = new Uint8Array(srcBuf);
    var n = bytes.length;
    var maxDiagnostics = (opts && isInt(opts.maxDiagnostics) && opts.maxDiagnostics >= 0) ? opts.maxDiagnostics : 4096;

    var u = parse(bytes, { path: path || "f.js", maxDiagnostics: maxDiagnostics });

    // -- diagnostics: classification + budget (4.5, 4.6) --
    var ordinary = 0, sawInternal = false, terminalDiag = false;
    var hasSyntax = false, hasDepth = false, hasTokens = false;
    var diags = u.diagnostics || [];
    for (var di = 0; di < diags.length; di++) {
        var d = diags[di];
        var code = d && d.code;
        var e = checkRange(d && d.range, n, "diagnostic");
        if (e) return { schema_version: 1, ok: false, reason: e };
        if (typeof code !== "string") return { schema_version: 1, ok: false, reason: "diagnostic: missing code" };
        if (code === "js.internal") sawInternal = true;                 // 4.6: internal is a FAILURE
        else if (code.lastIndexOf("js.limit.", 0) === 0) {              // parser-level bounded outcomes
            if (code === "js.limit.diagnostics") terminalDiag = true;
            else if (code === "js.limit.depth") hasDepth = true;
            else if (code === "js.limit.tokens") hasTokens = true;
        } else { if (code === "js.syntax") hasSyntax = true; ordinary++; }  // js.syntax / js.unsupported / other
    }

    // Recovery classification -- gates the nesting exemption below. js.limit.diagnostics is NOT
    // by itself evidence of SYNTAX recovery: with maxDiagnostics small, a valid-but-UNSUPPORTED
    // input emits only js.unsupported, which is suppressed and replaced by js.limit.diagnostics.
    //   - js.syntax present -> recovery.
    //   - js.limit.depth / js.limit.tokens (an incomplete/truncated AST) -> recovery/incomplete.
    //   - js.limit.diagnostics WITHOUT visible js.syntax -> REPARSE once with a generous budget
    //     (2n+64) SOLELY to reveal whether the suppressed diagnostics included js.syntax:
    //       reparse has js.syntax                 -> recovery;
    //       reparse is unsupported-only / clean   -> STRICT;
    //       reparse still budget-exhausted        -> INDETERMINATE -> bounded (recovery) path,
    //                                                never silently "clean". (Practically
    //                                                unreachable: needs >2n+64 diagnostics.)
    // The reparse fires only on budget exhaustion, so it does not materially cost throughput.
    var recovery;
    if (hasSyntax || hasDepth || hasTokens) {
        recovery = true;
    } else if (terminalDiag) {
        var u2 = parse(bytes, { path: path || "f.js", maxDiagnostics: 2 * n + 64 });
        var r2syntax = false, r2budget = false, r2incomplete = false, d2 = u2.diagnostics || [];
        for (var j = 0; j < d2.length; j++) {
            var c2 = d2[j] && d2[j].code;
            if (c2 === "js.syntax") r2syntax = true;
            else if (c2 === "js.limit.diagnostics") r2budget = true;
            else if (c2 === "js.limit.depth" || c2 === "js.limit.tokens") r2incomplete = true;
        }
        recovery = r2syntax || r2incomplete || r2budget;   // r2budget => indeterminate -> bounded path
    } else {
        recovery = false;                                  // clean or unsupported-only -> STRICT
    }
    if (sawInternal) return { schema_version: 1, ok: false, reason: "js.internal in SourceUnit" };
    if (ordinary > maxDiagnostics) return { schema_version: 1, ok: false, reason: "ordinary diagnostics " + ordinary + " exceed maxDiagnostics " + maxDiagnostics };
    // when the budget was hit, the terminal js.limit.diagnostics must remain visible
    if (ordinary === maxDiagnostics && diags.length > maxDiagnostics && !terminalDiag)
        return { schema_version: 1, ok: false, reason: "budget hit but js.limit.diagnostics missing" };

    // -- comments: range + raw slice equality (4.1, 4.3) --
    var comments = u.comments || [];
    for (var ci = 0; ci < comments.length; ci++) {
        var cm = comments[ci];
        var cr = checkRange({ start: cm.start, stop: cm.stop }, n, "comment");
        if (cr) return { schema_version: 1, ok: false, reason: cr };
        if (cm.raw !== refSlice(bytes, cm.start, cm.stop))
            return { schema_version: 1, ok: false, reason: "comment.raw != byte slice" };
    }

    // -- AST: range + nesting + acyclic (ancestor-path) + size-bounded traversal (4.1,4.2,4.4)
    //         + annotation raw. Iterative DFS tracking the current root-to-node PATH: a node in
    //         its own ancestor set is a true cycle, while a node reachable via two DIFFERENT
    //         paths (a legitimate DAG -- e.g. a shorthand Property whose key and value are the
    //         SAME Identifier) is fine. A global visited set would wrongly flag that sharing. --
    var MAX_NODES = 8 * n + 64;
    var MAX_EXEMPT = 2 * n + 64;           // bound on frontier-marker nesting exemptions (below)
    var exemptEscapes = 0;
    var ancestors = new Set();
    var count = 0;
    // frame = { node, parent, kids:null|Array, idx }
    var frames = [{ node: u.ast, parent: null, kids: null, idx: 0 }];
    while (frames.length) {
        var fr = frames[frames.length - 1];
        if (fr.kids === null) {
            var node = fr.node, parent = fr.parent;
            if (node === null || typeof node !== "object") { frames.pop(); continue; }
            if (ancestors.has(node)) return { schema_version: 1, ok: false, reason: "AST cycle @" + node.type };
            if (++count > MAX_NODES) return { schema_version: 1, ok: false, reason: "AST node count exceeds 8n+64" };

            // 4.1 range + bounds on any node that carries start/stop.
            if (node.start !== undefined || node.stop !== undefined) {
                var nr = checkRange({ start: node.start, stop: node.stop }, n, "ast node");
                if (nr) return { schema_version: 1, ok: false, reason: nr + " @" + node.type + " [" + node.start + "," + node.stop + "] n=" + n };
                // 4.2 nesting within the parent. An escape is a BREACH unless it is a
                // frontier-anchored recovery MARKER in a unit that actually underwent syntax
                // recovery. The marker classes -- "Error" recovery nodes (errNode uses cur.start,
                // any width) and zero-width empty markers (start === stop) -- anchor at the
                // failure frontier, which can sit just past a parent finalized to its last
                // CONSUMED token. The exemption is GATED on `recovery` (so a clean or
                // unsupported-only unit gets STRICT nesting for EVERY child, catching a
                // clean-parse range bug) and COUNTED against MAX_EXEMPT (so recovery cannot mint
                // unbounded synthetic escaping nodes). Every substantive child of a clean tree
                // must nest.
                if (parent && parent.start !== undefined && parent.stop !== undefined &&
                    (node.start < parent.start || node.stop > parent.stop)) {
                    var marker = (node.type === "Error") || (node.stop === node.start);
                    if (recovery && marker) {
                        if (++exemptEscapes > MAX_EXEMPT)
                            return { schema_version: 1, ok: false, reason: "recovery markers escaping parent exceed 2n+64 (" + exemptEscapes + ")" };
                    } else {
                        return { schema_version: 1, ok: false, reason: "AST child escapes parent range: " + node.type + "[" + node.start + "," + node.stop + "] in " + parent.type + "[" + parent.start + "," + parent.stop + "] recovery=" + recovery };
                    }
                }
            }

            // 4.1/4.3 attached annotations (on declaration nodes): range + raw slice equality.
            if (Array.isArray(node.annotations)) {
                for (var ai = 0; ai < node.annotations.length; ai++) {
                    var an = node.annotations[ai];
                    var ar = checkRange(an && an.range, n, "annotation");
                    if (ar) return { schema_version: 1, ok: false, reason: ar };
                    if (an && an.range && an.raw !== refSlice(bytes, an.range.start, an.range.stop))
                        return { schema_version: 1, ok: false, reason: "annotation.raw != byte slice" };
                }
            }

            ancestors.add(node);
            // Collect child edges: any own object/array-of-object property (not annotations).
            var kids = [];
            for (var key in node) {
                if (!Object.prototype.hasOwnProperty.call(node, key)) continue;
                if (key === "annotations") continue;
                var v = node[key];
                if (Array.isArray(v)) {
                    for (var vi = 0; vi < v.length; vi++)
                        if (v[vi] && typeof v[vi] === "object") kids.push(v[vi]);
                } else if (v && typeof v === "object") {
                    kids.push(v);
                }
            }
            fr.kids = kids;
        }
        if (fr.idx < fr.kids.length) {
            frames.push({ node: fr.kids[fr.idx++], parent: fr.node, kids: null, idx: 0 });
        } else {
            ancestors.delete(fr.node);   // leaving this node -> off the current path
            frames.pop();
        }
    }

    // `recovery` is surfaced (test-only entry) so a regression can assert the classification --
    // e.g. valid-but-unsupported input at maxDiagnostics=0 must be recovery:false (STRICT). The
    // fuzzer harness ignores it (checks only ok).
    return { schema_version: 1, ok: true, recovery: recovery };
}

globalThis.__hull_frontend = { fuzz: function (srcBuf, path, opts) { return fuzz(srcBuf, path, opts || {}); } };

// hull:source:lexer - byte-oriented ECMAScript lexer (JS mirror of hull.source.lexer).
//
// Scans the raw source BYTES (a Uint8Array) with a 0-based byte cursor and produces tokens
// carrying exact half-open 1-based byte ranges (see range.js). UTF-8 is decoded only where
// classification needs it; recorded positions are always byte offsets, never code-unit
// indices. The lexer NEVER throws: lexical problems become js.syntax / js.unsupported /
// js.limit.* diagnostics and it recovers so a full token stream is always returned.
//
// Public: lex(bytes, opts?) -> { tokens, comments, diagnostics, linemap }
//   opts: { path?, maxTokens?, maxDiagnostics? }
//
// Token shape (the stable contract the parser depends on):
//   { type, start, stop, nlBefore, ...typed }
//   type in identifier | number | string | template | regex | punctuator | eof
//   start/stop : 1-based half-open byte range (text is bytes[start-1 .. stop-2])
//   nlBefore   : boolean - a LineTerminator occurred since the previous token (whitespace OR
//                inside a block comment). Drives ASI in the parser.
//   identifier : value = decoded name; escaped = true if a \u escape was used.
//   number     : value = raw lexeme; bigint = true for a trailing `n`.
//   string     : value = cooked (best-effort escapes); raw = raw lexeme incl quotes.
//   template   : mode in noSubstitution | head | middle | tail; value = cooked; raw.
//   regex      : pattern (between the slashes); flags; raw = full lexeme.
//   punctuator : value = the operator text.
//
// Comment shape (returned separately so Slice 3 attaches JSDoc without rescanning):
//   { kind: "line" | "block" | "jsdoc", start, stop, raw, text }
//
// Regex-vs-division uses an acorn-style exprAllowed state machine + a context stack for
// ( ) and { } (control-flow head vs call/group; block vs object literal), so control flow
// like `if (ok) /x/.test(s)` and `x = {} / y` tokenize correctly.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { linemap as buildLinemap } from "hull:source:range";
import { error as diagError } from "hull:source:diagnostic";

// Correct Unicode identifier classification via the engine's own tables (u-mode property
// escapes). Built with `new RegExp` and cached lazily on first non-ASCII code point, so the
// (expensive) \p{...} pattern compiles at RUNTIME, not at module-parse time -- the tooling
// precompile step is COMPILE_ONLY under a bounded stack and must not compile it. $ and _ are
// ID_Start (ASCII-handled); ZWNJ/ZWJ are ID_Continue.
let _reIdStart = null, _reIdCont = null;
function reIdStart() { return _reIdStart || (_reIdStart = new RegExp("\\p{ID_Start}", "u")); }
function reIdContinue() { return _reIdCont || (_reIdCont = new RegExp("\\p{ID_Continue}", "u")); }

export const KEYWORDS = new Set([
    "break", "case", "catch", "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends", "finally", "for", "function", "if",
    "import", "in", "instanceof", "new", "return", "super", "switch", "this", "throw",
    "try", "typeof", "var", "void", "while", "with", "yield", "enum", "null", "true", "false",
]);

// Keywords after which an expression (hence a regex) is expected.
const KW_BEFORE_EXPR = new Set([
    "return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "do", "else",
    "yield", "await", "case", "throw", "extends",
]);
// Keywords whose following `(...)` is a control-flow head (a statement follows the `)`).
const CONTROL_FLOW_KW = new Set(["if", "for", "while", "with", "switch", "catch"]);
// Punctuators after which an expression (hence a regex) is expected.
const PUNCT_BEFORE_EXPR = new Set([
    "{", "(", "[", ",", ";", ":", "?", "=>", "...",
    "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??=",
    "+", "-", "*", "/", "%", "**", "==", "===", "!=", "!==", "<", ">", "<=", ">=", "<<", ">>", ">>>",
    "&", "|", "^", "&&", "||", "??", "!", "~",
]);

function isDigit(b) { return b >= 0x30 && b <= 0x39; }
function isHex(b) { return isDigit(b) || (b >= 0x41 && b <= 0x46) || (b >= 0x61 && b <= 0x66); }
function isOctal(b) { return b >= 0x30 && b <= 0x37; }
function isBinary(b) { return b === 0x30 || b === 0x31; }
function isIdStartAscii(b) { return (b >= 0x41 && b <= 0x5a) || (b >= 0x61 && b <= 0x7a) || b === 0x5f || b === 0x24; }
function isIdContinueAscii(b) { return isIdStartAscii(b) || isDigit(b); }
function isSpaceAscii(b) { return b === 0x20 || b === 0x09 || b === 0x0b || b === 0x0c; }

export function lex(bytes, opts) {
    opts = opts || {};
    const path = opts.path || null;
    const maxTokens = (typeof opts.maxTokens === "number" && opts.maxTokens > 0) ? Math.floor(opts.maxTokens) : 1000000;
    const maxDiagnostics = (typeof opts.maxDiagnostics === "number" && opts.maxDiagnostics > 0) ? Math.floor(opts.maxDiagnostics) : 4096;

    const n = bytes.length;
    let p = 0;
    const tokens = [];
    const comments = [];
    const diagnostics = [];
    let limited = false;
    let ordinaryDiags = 0;
    let diagCapNoted = false;

    // Context stack for ( { ${ resolving regex/division. Each entry: { t, afterClose }.
    // afterClose = the exprAllowed value to restore when this `(`/`{` closes.
    const ctx = [];
    let exprAllowed = true;          // at input start an expression (hence a regex) is allowed
    let prev = null;
    let nlPending = false;

    function isIdStartCp(cp) { if (cp < 0x80) return isIdStartAscii(cp); return reIdStart().test(String.fromCodePoint(cp)); }
    function isIdContinueCp(cp) {
        if (cp < 0x80) return isIdContinueAscii(cp);
        if (cp === 0x200c || cp === 0x200d) return true;
        return reIdContinue().test(String.fromCodePoint(cp));
    }

    // Push a diagnostic. Ranges are clamped to [1, n+1] so a truncated escape at EOF can never
    // report past the source. Terminal js.limit.* diagnostics are ALWAYS recorded; ordinary
    // diagnostics honor maxDiagnostics.
    function diag(code, message, start, stop) {
        const hi = n + 1;
        if (start < 1) start = 1; if (start > hi) start = hi;
        if (stop < start) stop = start; if (stop > hi) stop = hi;
        const terminal = code.lastIndexOf("js.limit.", 0) === 0;
        if (!terminal) {
            if (ordinaryDiags >= maxDiagnostics) {
                if (!diagCapNoted) {
                    diagCapNoted = true;
                    diagnostics.push(diagError("js.limit.diagnostics", "diagnostic limit exceeded (" + maxDiagnostics + ")", path, { start: start, stop: stop }));
                }
                return;
            }
            ordinaryDiags++;
        }
        diagnostics.push(diagError(code, message, path, { start: start, stop: stop }));
    }

    function decode(i) {
        const b0 = bytes[i];
        if (b0 < 0x80) return { cp: b0, len: 1 };
        let need, cp, min;
        if ((b0 & 0xe0) === 0xc0) { need = 1; cp = b0 & 0x1f; min = 0x80; }
        else if ((b0 & 0xf0) === 0xe0) { need = 2; cp = b0 & 0x0f; min = 0x800; }
        else if ((b0 & 0xf8) === 0xf0) { need = 3; cp = b0 & 0x07; min = 0x10000; }
        else return { err: true, len: 1 };
        for (let k = 1; k <= need; k++) {
            const bk = bytes[i + k];
            if (bk === undefined || (bk & 0xc0) !== 0x80) return { err: true, len: 1 };
            cp = (cp << 6) | (bk & 0x3f);
        }
        if (cp < min) return { err: true, len: need + 1 };
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return { err: true, len: need + 1 };
        return { cp: cp, len: need + 1 };
    }

    function sliceText(a, b) {
        let s = "";
        for (let i = a; i < b; i++) {
            const c = bytes[i];
            if (c < 0x80) { s += String.fromCharCode(c); }
            else { const d = decode(i); if (d.err) { s += "\uFFFD"; } else { s += String.fromCodePoint(d.cp); i += d.len - 1; } }
        }
        return s;
    }

    function isLSPS(i) {
        return bytes[i] === 0xe2 && bytes[i + 1] === 0x80 && (bytes[i + 2] === 0xa8 || bytes[i + 2] === 0xa9);
    }

    // Update exprAllowed after emitting `tok` (except ) and }, whose value was set by the
    // context pop). The acorn-style before-expr state machine.
    function afterToken(tok) {
        switch (tok.type) {
            case "number": case "string": case "regex": exprAllowed = false; break;
            case "template": exprAllowed = (tok.mode === "head" || tok.mode === "middle"); break;
            case "identifier": exprAllowed = KW_BEFORE_EXPR.has(tok.value); break;
            case "punctuator":
                if (tok.value === ")" || tok.value === "}") break;   // set by the context pop
                exprAllowed = PUNCT_BEFORE_EXPR.has(tok.value);
                break;
            default: exprAllowed = false;
        }
    }

    function push(tok) {
        tok.nlBefore = nlPending;
        nlPending = false;
        tokens.push(tok);
        if (tok.type !== "eof") prev = tok;
        afterToken(tok);
        if (tokens.length >= maxTokens && !limited) {
            limited = true;
            diag("js.limit.tokens", "token limit exceeded (" + maxTokens + ")", tok.start, tok.stop);
        }
    }

    // -- whitespace + comments --
    function skipTrivia() {
        for (;;) {
            if (p >= n) return;
            const b = bytes[p];
            if (b === 0x0a) { p += 1; nlPending = true; continue; }
            if (b === 0x0d) { p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; nlPending = true; continue; }
            if (isSpaceAscii(b)) { p += 1; continue; }
            if (b === 0x2f && p + 1 < n && bytes[p + 1] === 0x2f) { scanLineComment(); continue; }
            if (b === 0x2f && p + 1 < n && bytes[p + 1] === 0x2a) { scanBlockComment(); continue; }
            if (b >= 0x80) {
                if (isLSPS(p)) { p += 3; nlPending = true; continue; }
                const d = decode(p);
                if (d.err) return;
                if (d.cp === 0xa0 || d.cp === 0xfeff || (d.cp >= 0x2000 && d.cp <= 0x200a) ||
                    d.cp === 0x3000 || d.cp === 0x1680 || d.cp === 0x205f || d.cp === 0x202f) { p += d.len; continue; }
                return;
            }
            return;
        }
    }

    function scanLineComment() {
        const sp = p; p += 2;
        while (p < n) {
            const b = bytes[p];
            if (b === 0x0a || b === 0x0d) break;
            if (b >= 0x80) {
                if (isLSPS(p)) break;
                const d = decode(p);
                if (d.err) diag("js.syntax", "invalid UTF-8 in comment", p + 1, p + d.len + 1);
                p += d.len; continue;
            }
            p += 1;
        }
        comments.push({ kind: "line", start: sp + 1, stop: p + 1, raw: sliceText(sp, p), text: sliceText(sp + 2, p) });
    }

    function scanBlockComment() {
        const sp = p; p += 2;
        let closed = false;
        while (p < n) {
            const b = bytes[p];
            if (b === 0x2a && p + 1 < n && bytes[p + 1] === 0x2f) { p += 2; closed = true; break; }
            if (b === 0x0a || b === 0x0d) { nlPending = true; p += 1; continue; }
            if (b >= 0x80) {
                if (isLSPS(p)) { nlPending = true; p += 3; continue; }
                const d = decode(p);
                if (d.err) diag("js.syntax", "invalid UTF-8 in comment", p + 1, p + d.len + 1);
                p += d.len; continue;
            }
            p += 1;
        }
        if (!closed) diag("js.syntax", "unterminated block comment", sp + 1, p + 1);
        const isJsdoc = closed && bytes[sp + 2] === 0x2a && (p - sp) > 4;   // /** ... (not /**/)
        const textEnd = closed ? p - 2 : p;
        comments.push({ kind: isJsdoc ? "jsdoc" : "block", start: sp + 1, stop: p + 1,
                        raw: sliceText(sp, p), text: sliceText(sp + 2, textEnd) });
    }

    // -- identifiers (correct start/continue + escape validation) --
    function scanIdentifier() {
        const sp = p;
        let escaped = false, name = "", first = true;
        for (;;) {
            if (p >= n) break;
            const b = bytes[p];
            if (b === 0x5c) {
                const escStart = p;
                const cp = scanUnicodeEscape();
                if (cp === null) break;
                const ok = first ? isIdStartCp(cp) : isIdContinueCp(cp);
                if (!ok) { diag("js.syntax", first ? "invalid escaped identifier start" : "invalid escaped identifier part", escStart + 1, p + 1); break; }
                escaped = true; name += String.fromCodePoint(cp); first = false; continue;
            }
            if (b < 0x80) {
                if (!(first ? isIdStartAscii(b) : isIdContinueAscii(b))) break;
                name += String.fromCharCode(b); p += 1; first = false; continue;
            }
            const d = decode(p);
            if (d.err) break;
            if (!(first ? isIdStartCp(d.cp) : isIdContinueCp(d.cp))) break;
            name += String.fromCodePoint(d.cp); p += d.len; first = false;
        }
        if (name.length > 0) push({ type: "identifier", value: name, escaped: escaped, start: sp + 1, stop: p + 1 });
    }

    // `\uHHHH` or `\u{...}` -> code point, or null on malformed (with a diagnostic). Advances p.
    function scanUnicodeEscape() {
        const start = p;
        if (p + 1 >= n || bytes[p + 1] !== 0x75) { diag("js.syntax", "invalid escape", start + 1, start + 2); p += 1; return null; }
        p += 2;
        let cp = 0;
        if (p < n && bytes[p] === 0x7b) {
            p += 1; let got = 0;
            while (p < n && bytes[p] !== 0x7d) {
                if (!isHex(bytes[p])) { diag("js.syntax", "invalid unicode escape", start + 1, p + 1); return null; }
                cp = cp * 16 + hexVal(bytes[p]); p += 1; got++;
                if (cp > 0x10ffff) { diag("js.syntax", "unicode escape out of range", start + 1, p + 1); return null; }
            }
            if (p >= n || bytes[p] !== 0x7d || got === 0) { diag("js.syntax", "unterminated unicode escape", start + 1, p + 1); return null; }
            p += 1; return cp;
        }
        for (let k = 0; k < 4; k++) {
            if (p >= n || !isHex(bytes[p])) { diag("js.syntax", "invalid unicode escape", start + 1, p + 1); return null; }
            cp = cp * 16 + hexVal(bytes[p]); p += 1;
        }
        return cp;
    }
    function hexVal(b) { if (b <= 0x39) return b - 0x30; if (b <= 0x46) return b - 0x41 + 10; return b - 0x61 + 10; }

    // -- numbers (with numeric-separator validation) --
    // A `_` must sit BETWEEN two `pred` digits (no leading / trailing / doubled / boundary).
    function consumeDigits(pred) {
        let count = 0, prevDigit = false;
        for (;;) {
            if (p >= n) break;
            const b = bytes[p];
            if (pred(b)) { p += 1; count++; prevDigit = true; continue; }
            if (b === 0x5f) {
                if (!prevDigit) diag("js.syntax", "numeric separator not allowed here", p + 1, p + 2);
                p += 1;
                if (p >= n || !pred(bytes[p])) diag("js.syntax", "numeric separator must be between digits", p, p + 1);
                prevDigit = false; continue;
            }
            break;
        }
        return count;
    }

    function scanNumber() {
        const sp = p;
        let bigint = false;
        if (bytes[p] === 0x30 && p + 1 < n) {
            const c = bytes[p + 1] | 0x20;
            if (c === 0x78 || c === 0x6f || c === 0x62) {
                p += 2;
                const pred = c === 0x78 ? isHex : (c === 0x6f ? isOctal : isBinary);
                if (consumeDigits(pred) === 0) diag("js.syntax", "missing digits in numeric literal", sp + 1, p + 1);
                if (p < n && bytes[p] === 0x6e) { bigint = true; p += 1; }
                return finishNumber(sp, bigint);
            }
        }
        consumeDigits(isDigit);                                   // integer part (empty for .5)
        if (p < n && bytes[p] === 0x2e) { p += 1; consumeDigits(isDigit); }   // fraction
        if (p < n && (bytes[p] === 0x65 || bytes[p] === 0x45)) {  // exponent
            p += 1;
            if (p < n && (bytes[p] === 0x2b || bytes[p] === 0x2d)) p += 1;
            if (consumeDigits(isDigit) === 0) diag("js.syntax", "missing exponent in numeric literal", sp + 1, p + 1);
        } else if (p < n && bytes[p] === 0x6e) { bigint = true; p += 1; }
        return finishNumber(sp, bigint);
    }
    function finishNumber(sp, bigint) {
        push({ type: "number", value: sliceText(sp, p), bigint: bigint, start: sp + 1, stop: p + 1 });
    }

    // -- strings + templates --
    function scanString(quote) {
        const sp = p; p += 1;
        let cooked = "";
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated string literal", sp + 1, p + 1); break; }
            const b = bytes[p];
            if (b === quote) { p += 1; break; }
            if (b === 0x0a || b === 0x0d) { diag("js.syntax", "unterminated string literal", sp + 1, p + 1); break; }  // LF/CR; LS/PS valid in strings (ES2019)
            if (b === 0x5c) { cooked += scanEscape(); continue; }
            if (b < 0x80) { cooked += String.fromCharCode(b); p += 1; continue; }
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8 in string literal", p + 1, p + d.len + 1); p += d.len; continue; }
            cooked += String.fromCodePoint(d.cp); p += d.len;
        }
        push({ type: "string", value: cooked, raw: sliceText(sp, p), start: sp + 1, stop: p + 1 });
    }

    function scanEscape() {
        p += 1;
        if (p >= n) { diag("js.syntax", "unterminated escape", p, p + 1); return ""; }
        const b = bytes[p];
        switch (b) {
            case 0x6e: p += 1; return "\n";
            case 0x74: p += 1; return "\t";
            case 0x72: p += 1; return "\r";
            case 0x62: p += 1; return "\b";
            case 0x66: p += 1; return "\f";
            case 0x76: p += 1; return "\v";
            case 0x30: if (p + 1 < n && isDigit(bytes[p + 1])) { p += 1; return "0"; } p += 1; return "\0";
            case 0x78: {
                p += 1; let cp = 0, ok = true;
                for (let k = 0; k < 2; k++) { if (p < n && isHex(bytes[p])) { cp = cp * 16 + hexVal(bytes[p]); p += 1; } else { ok = false; break; } }
                if (!ok) { diag("js.syntax", "invalid hex escape", p, p + 1); return ""; }
                return String.fromCharCode(cp);
            }
            case 0x75: { p -= 1; const cp = scanUnicodeEscape(); return cp === null ? "" : String.fromCodePoint(cp); }
            case 0x0a: p += 1; return "";
            case 0x0d: p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; return "";
            default: {
                if (b < 0x80) { p += 1; return String.fromCharCode(b); }
                const d = decode(p);
                if (d.err) { p += d.len; return ""; }
                p += d.len; return String.fromCodePoint(d.cp);
            }
        }
    }

    function scanTemplate(opener) {
        const sp = p; p += 1;
        let cooked = "", mode;
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated template literal", sp + 1, p + 1); mode = (opener === 0x60) ? "noSubstitution" : "tail"; break; }
            const b = bytes[p];
            if (b === 0x60) { p += 1; mode = (opener === 0x60) ? "noSubstitution" : "tail"; break; }
            if (b === 0x24 && p + 1 < n && bytes[p + 1] === 0x7b) { p += 2; mode = (opener === 0x60) ? "head" : "middle"; ctx.push({ t: "${", afterClose: false }); break; }
            if (b === 0x5c) { cooked += scanEscape(); continue; }
            if (b === 0x0d) { p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; cooked += "\n"; continue; }
            if (b < 0x80) { cooked += String.fromCharCode(b); p += 1; continue; }
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8 in template literal", p + 1, p + d.len + 1); p += d.len; continue; }
            cooked += String.fromCodePoint(d.cp); p += d.len;
        }
        push({ type: "template", mode: mode, value: cooked, raw: sliceText(sp, p), start: sp + 1, stop: p + 1 });
    }

    // -- regex --
    function scanRegex() {
        const sp = p; p += 1;
        let inClass = false;
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated regular expression", sp + 1, p + 1); break; }
            const b = bytes[p];
            if (b === 0x0a || b === 0x0d) { diag("js.syntax", "unterminated regular expression", sp + 1, p + 1); break; }
            if (b >= 0x80 && isLSPS(p)) { diag("js.syntax", "unterminated regular expression", sp + 1, p + 1); break; }  // U+2028/2029 terminate a regex
            if (b === 0x5c) { p += (p + 1 < n) ? 2 : 1; continue; }
            if (b === 0x5b) { inClass = true; p += 1; continue; }
            if (b === 0x5d) { inClass = false; p += 1; continue; }
            if (b === 0x2f && !inClass) { p += 1; break; }
            if (b < 0x80) { p += 1; continue; }
            const d = decode(p);
            if (d.err) diag("js.syntax", "invalid UTF-8 in regular expression", p + 1, p + d.len + 1);
            p += d.len;
        }
        const patEnd = p;
        while (p < n && (isIdContinueAscii(bytes[p]) || bytes[p] >= 0x80)) {
            if (bytes[p] < 0x80) { p += 1; } else { if (isLSPS(p)) break; const d = decode(p); if (d.err) break; p += d.len; }
        }
        push({ type: "regex", pattern: sliceText(sp + 1, patEnd - 1), flags: sliceText(patEnd, p), raw: sliceText(sp, p), start: sp + 1, stop: p + 1 });
    }

    // -- punctuators + context stack --
    function scanPunctuator() {
        const sp = p;
        const b = bytes[p];
        if (b === 0x7d) {
            if (ctx.length && ctx[ctx.length - 1].t === "${") { ctx.pop(); scanTemplate(0x7d); return true; }
            const c = ctx.pop();
            exprAllowed = c ? c.afterClose : true;
            p += 1; return emit("}", sp);
        }
        if (b === 0x7b) { ctx.push({ t: "{", afterClose: exprAllowed ? false : true }); p += 1; return emit("{", sp); }
        if (b === 0x28) {
            const controlFlow = prev !== null && prev.type === "identifier" && CONTROL_FLOW_KW.has(prev.value);
            ctx.push({ t: "(", afterClose: controlFlow });
            p += 1; return emit("(", sp);
        }
        if (b === 0x29) { const c = ctx.pop(); exprAllowed = c ? c.afterClose : false; p += 1; return emit(")", sp); }

        const two = (p + 1 < n) ? bytes[p + 1] : -1;
        const three = (p + 2 < n) ? bytes[p + 2] : -1;
        const four = (p + 3 < n) ? bytes[p + 3] : -1;
        switch (b) {
            case 0x2e:
                if (two === 0x2e && three === 0x2e) { p += 3; return emit("...", sp); }
                return adv(1, ".", sp);
            case 0x3f:
                if (two === 0x3f && three === 0x3d) { p += 3; return emit("??=", sp); }
                if (two === 0x3f) { p += 2; return emit("??", sp); }
                if (two === 0x2e && !(three >= 0x30 && three <= 0x39)) { p += 2; return emit("?.", sp); }
                return adv(1, "?", sp);
            case 0x3d:
                if (two === 0x3d && three === 0x3d) { p += 3; return emit("===", sp); }
                if (two === 0x3d) { p += 2; return emit("==", sp); }
                if (two === 0x3e) { p += 2; return emit("=>", sp); }
                return adv(1, "=", sp);
            case 0x21:
                if (two === 0x3d && three === 0x3d) { p += 3; return emit("!==", sp); }
                if (two === 0x3d) { p += 2; return emit("!=", sp); }
                return adv(1, "!", sp);
            case 0x3c:
                if (two === 0x3c && three === 0x3d) { p += 3; return emit("<<=", sp); }
                if (two === 0x3c) { p += 2; return emit("<<", sp); }
                if (two === 0x3d) { p += 2; return emit("<=", sp); }
                return adv(1, "<", sp);
            case 0x3e:
                if (two === 0x3e && three === 0x3e && four === 0x3d) { p += 4; return emit(">>>=", sp); }
                if (two === 0x3e && three === 0x3e) { p += 3; return emit(">>>", sp); }
                if (two === 0x3e && three === 0x3d) { p += 3; return emit(">>=", sp); }
                if (two === 0x3e) { p += 2; return emit(">>", sp); }
                if (two === 0x3d) { p += 2; return emit(">=", sp); }
                return adv(1, ">", sp);
            case 0x2b:
                if (two === 0x2b) { p += 2; return emit("++", sp); }
                if (two === 0x3d) { p += 2; return emit("+=", sp); }
                return adv(1, "+", sp);
            case 0x2d:
                if (two === 0x2d) { p += 2; return emit("--", sp); }
                if (two === 0x3d) { p += 2; return emit("-=", sp); }
                return adv(1, "-", sp);
            case 0x2a:
                if (two === 0x2a && three === 0x3d) { p += 3; return emit("**=", sp); }
                if (two === 0x2a) { p += 2; return emit("**", sp); }
                if (two === 0x3d) { p += 2; return emit("*=", sp); }
                return adv(1, "*", sp);
            case 0x25:
                if (two === 0x3d) { p += 2; return emit("%=", sp); }
                return adv(1, "%", sp);
            case 0x26:
                if (two === 0x26 && three === 0x3d) { p += 3; return emit("&&=", sp); }
                if (two === 0x26) { p += 2; return emit("&&", sp); }
                if (two === 0x3d) { p += 2; return emit("&=", sp); }
                return adv(1, "&", sp);
            case 0x7c:
                if (two === 0x7c && three === 0x3d) { p += 3; return emit("||=", sp); }
                if (two === 0x7c) { p += 2; return emit("||", sp); }
                if (two === 0x3d) { p += 2; return emit("|=", sp); }
                return adv(1, "|", sp);
            case 0x5e:
                if (two === 0x3d) { p += 2; return emit("^=", sp); }
                return adv(1, "^", sp);
            case 0x2f:
                if (two === 0x3d) { p += 2; return emit("/=", sp); }
                return adv(1, "/", sp);
            case 0x5b: return adv(1, "[", sp);
            case 0x5d: return adv(1, "]", sp);
            case 0x3b: return adv(1, ";", sp);
            case 0x2c: return adv(1, ",", sp);
            case 0x3a: return adv(1, ":", sp);
            case 0x7e: return adv(1, "~", sp);
            default: return false;
        }
    }
    function adv(k, v, sp) { p += k; return emit(v, sp); }
    function emit(v, sp) { push({ type: "punctuator", value: v, start: sp + 1, stop: p + 1 }); return true; }

    // -- main loop --
    while (!limited) {
        skipTrivia();
        if (p >= n) break;
        const b = bytes[p];
        if (b === 0x22 || b === 0x27) { scanString(b); continue; }
        if (b === 0x60) { scanTemplate(0x60); continue; }
        if (isDigit(b)) { scanNumber(); continue; }
        if (b === 0x2e && p + 1 < n && isDigit(bytes[p + 1])) { scanNumber(); continue; }
        if (b === 0x2f && (p + 1 >= n || (bytes[p + 1] !== 0x2f && bytes[p + 1] !== 0x2a)) && exprAllowed) { scanRegex(); continue; }
        if (b === 0x5c || isIdStartAscii(b)) {
            const before = p;
            scanIdentifier();
            if (p === before) { diag("js.syntax", "invalid escape", p + 1, p + 2); p += 1; }   // progress guard
            continue;
        }
        if (b >= 0x80) {
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8", p + 1, p + d.len + 1); p += d.len; continue; }
            if (isIdStartCp(d.cp)) { const before = p; scanIdentifier(); if (p === before) p += d.len; continue; }
            diag("js.syntax", "unexpected character U+" + d.cp.toString(16).toUpperCase(), p + 1, p + d.len + 1);
            p += d.len; continue;
        }
        if (scanPunctuator()) continue;
        diag("js.syntax", "unexpected character", p + 1, p + 2);
        p += 1;
    }

    push({ type: "eof", start: p + 1, stop: p + 1 });
    return { tokens: tokens, comments: comments, diagnostics: diagnostics, linemap: buildLinemap(bytes) };
}

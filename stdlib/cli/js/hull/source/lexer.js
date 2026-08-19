// hull:source:lexer - byte-oriented ECMAScript lexer (JS mirror of hull.source.lexer).
//
// Scans the raw source BYTES (a Uint8Array) with a 0-based byte cursor and produces tokens
// carrying exact half-open 1-based byte ranges (see range.js). UTF-8 is decoded only where
// classification needs it (identifiers, string/template/regex content, comments); the
// recorded positions are always byte offsets, never code-unit indices. The lexer NEVER
// throws: lexical problems become js.syntax / js.unsupported / js.limit.* diagnostics and it
// recovers so a full token stream + diagnostics is always returned.
//
// Public: lex(bytes, opts?) -> { tokens, diagnostics, linemap }
//   opts: { path?, maxTokens? }
//
// Token shape (the stable contract the parser depends on):
//   { type, start, stop, nlBefore, ...typed }
//   type in "identifier" | "number" | "string" | "template" | "regex" | "punctuator" | "eof"
//   start/stop : 1-based half-open byte range ([start, stop); text is bytes[start-1 .. stop-2])
//   nlBefore   : boolean - a LineTerminator occurred since the previous token (whitespace OR
//                inside a block comment). Drives ASI in the parser.
//   identifier : value = decoded name (string). Keywords/true/false/null are identifiers;
//                the parser classifies them (see KEYWORDS). `escaped` = true if the name used
//                a \u escape (matters for keyword validity).
//   number     : value = raw lexeme (string, unparsed); bigint = true for a trailing `n`.
//   string     : value = cooked string (best-effort escape decode); raw = raw lexeme incl quotes.
//   template   : mode in "noSubstitution" | "head" | "middle" | "tail"; cooked; raw.
//   regex      : pattern (between the slashes); flags; raw = full lexeme.
//   punctuator : value = the operator text ("=>", "?.", "===", "...", "}", ...).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { linemap as buildLinemap } from "hull:source:range";
import { error as diagError } from "hull:source:diagnostic";

// -- keyword sets (exported so the parser classifies identifiers uniformly) --
export const KEYWORDS = new Set([
    "break", "case", "catch", "class", "const", "continue", "debugger", "default",
    "delete", "do", "else", "export", "extends", "finally", "for", "function", "if",
    "import", "in", "instanceof", "new", "return", "super", "switch", "this", "throw",
    "try", "typeof", "var", "void", "while", "with", "yield",
    "enum", "null", "true", "false",
    // strict-mode reserved (contextual elsewhere): let, static, await are handled by the parser
]);

// After these tokens a `/` begins a regular expression, not division. A plain identifier or
// this/super/true/false/null is a VALUE, so `/` after it is division.
const KW_BEFORE_EXPR = new Set([
    "return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "do", "else",
    "yield", "await", "case", "throw",
]);

// -- byte classification (ASCII fast paths) --
function isDigit(b) { return b >= 0x30 && b <= 0x39; }                 // 0-9
function isHex(b) { return isDigit(b) || (b >= 0x41 && b <= 0x46) || (b >= 0x61 && b <= 0x66); }
function isIdStartAscii(b) {
    return (b >= 0x41 && b <= 0x5a) || (b >= 0x61 && b <= 0x7a) || b === 0x5f || b === 0x24; // A-Z a-z _ $
}
function isIdContinueAscii(b) { return isIdStartAscii(b) || isDigit(b); }

// ASCII whitespace (space, tab, vertical tab, form feed). Line terminators handled separately.
function isSpaceAscii(b) { return b === 0x20 || b === 0x09 || b === 0x0b || b === 0x0c; }

// -- the lexer --
export function lex(bytes, opts) {
    opts = opts || {};
    const path = opts.path || null;
    const maxTokens = (typeof opts.maxTokens === "number" && opts.maxTokens > 0)
        ? opts.maxTokens : 1000000;

    const n = bytes.length;
    let p = 0;                       // 0-based byte cursor
    const tokens = [];
    const diagnostics = [];
    let limited = false;

    // Template substitution stack: entries are "template" (a `${` boundary) or "block"
    // (an ordinary `{`). A `}` that pops a "template" resumes template scanning.
    const braceStack = [];

    let prev = null;                 // previous significant token (for regex/division)
    let nlPending = false;           // a LineTerminator seen since the last emitted token

    function diag(code, message, start, stop) {
        diagnostics.push(diagError(code, message, path, { start: start, stop: stop }));
    }

    // Decode one UTF-8 code point at 0-based offset i. Returns { cp, len } or { err:true, len }.
    // `len` is the byte length consumed (>=1 even on error, so scans make progress).
    function decode(i) {
        const b0 = bytes[i];
        if (b0 < 0x80) return { cp: b0, len: 1 };
        let need, cp, min;
        if ((b0 & 0xe0) === 0xc0) { need = 1; cp = b0 & 0x1f; min = 0x80; }
        else if ((b0 & 0xf0) === 0xe0) { need = 2; cp = b0 & 0x0f; min = 0x800; }
        else if ((b0 & 0xf8) === 0xf0) { need = 3; cp = b0 & 0x07; min = 0x10000; }
        else return { err: true, len: 1 };
        for (let k = 1; k <= need; k++) {
            const bk = bytes[i + k];             // out-of-range Uint8Array index yields undefined
            if (bk === undefined || (bk & 0xc0) !== 0x80) return { err: true, len: 1 };
            cp = (cp << 6) | (bk & 0x3f);
        }
        if (cp < min) return { err: true, len: need + 1 };          // overlong
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return { err: true, len: need + 1 };
        return { cp: cp, len: need + 1 };
    }

    function isIdStartCp(cp) {
        // ASCII precisely; permissive for non-ASCII (any code point >= 0x80 accepted). Full
        // Unicode ID_Start would need code tables; the conformance oracle catches divergence.
        if (cp < 0x80) return isIdStartAscii(cp);
        return cp !== 0x2028 && cp !== 0x2029 && cp !== 0xfeff;
    }
    function isIdContinueCp(cp) {
        if (cp < 0x80) return isIdContinueAscii(cp);
        return cp !== 0x2028 && cp !== 0x2029 && cp !== 0xfeff;
    }

    // Consume whitespace + comments; set nlPending if any LineTerminator is crossed.
    // Returns false only if a fatal (unterminated block comment) forced EOF.
    function skipTrivia() {
        for (;;) {
            if (p >= n) return;
            const b = bytes[p];
            if (b === 0x0a) { p += 1; nlPending = true; continue; }                 // LF
            if (b === 0x0d) { p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; nlPending = true; continue; } // CR/CRLF
            if (isSpaceAscii(b)) { p += 1; continue; }
            if (b === 0x2f && p + 1 < n && bytes[p + 1] === 0x2f) { skipLineComment(); continue; }   // //
            if (b === 0x2f && p + 1 < n && bytes[p + 1] === 0x2a) { skipBlockComment(); continue; }  // /*
            if (b >= 0x80) {
                // multibyte whitespace: NBSP (C2 A0), BOM (EF BB BF), LS/PS (E2 80 A8/A9), and
                // other Unicode space separators are treated minimally.
                const d = decode(p);
                if (d.err) return;                                   // let the scanner report it
                if (d.cp === 0x2028 || d.cp === 0x2029) { p += d.len; nlPending = true; continue; }
                if (d.cp === 0xa0 || d.cp === 0xfeff || (d.cp >= 0x2000 && d.cp <= 0x200a) ||
                    d.cp === 0x3000 || d.cp === 0x1680 || d.cp === 0x205f || d.cp === 0x202f) {
                    p += d.len; continue;
                }
                return;                                              // an identifier-start etc.
            }
            return;
        }
    }

    function skipLineComment() {
        p += 2;
        while (p < n) {
            const b = bytes[p];
            if (b === 0x0a || b === 0x0d) return;
            if (b === 0xe2 && p + 2 < n && bytes[p + 1] === 0x80 &&
                (bytes[p + 2] === 0xa8 || bytes[p + 2] === 0xa9)) return;  // U+2028/2029 end a line comment
            p += 1;
        }
    }

    function skipBlockComment() {
        const start = p;
        p += 2;
        while (p < n) {
            const b = bytes[p];
            if (b === 0x2a && p + 1 < n && bytes[p + 1] === 0x2f) { p += 2; return; }   // */
            if (b === 0x0a || b === 0x0d) nlPending = true;
            else if (b === 0xe2 && p + 2 < n && bytes[p + 1] === 0x80 &&
                     (bytes[p + 2] === 0xa8 || bytes[p + 2] === 0xa9)) nlPending = true;
            p += 1;
        }
        diag("js.syntax", "unterminated block comment", start + 1, p + 1);
    }

    function push(tok) {
        tok.nlBefore = nlPending;
        nlPending = false;
        tokens.push(tok);
        if (tok.type !== "eof") prev = tok;
        if (tokens.length >= maxTokens && !limited) {
            limited = true;
            diag("js.limit.tokens", "token limit exceeded (" + maxTokens + ")", tok.start, tok.stop);
        }
    }

    // Does a `/` at the current position start a regex (vs division)?
    function regexAllowed() {
        if (prev === null) return true;
        switch (prev.type) {
            case "identifier":
                return KW_BEFORE_EXPR.has(prev.value);   // value-ident/this/super/true/false/null -> division
            case "number": case "string": case "regex": return false;
            case "template": return prev.mode === "head" || prev.mode === "middle";
            case "punctuator": {
                const v = prev.value;
                if (v === ")" || v === "]") return false;   // KNOWN AMBIGUITY: wrong after if(...)/for(...)
                if (v === "}") return true;                 // KNOWN AMBIGUITY: wrong after an object-expr close
                if (v === "++" || v === "--") return false; // postfix -> value
                return true;                                // operators, ( [ { , ; : => ... -> regex
            }
            default: return true;
        }
    }

    // -- token scanners --
    function scanIdentifier() {
        const sp = p;
        let escaped = false;
        let name = "";
        for (;;) {
            if (p >= n) break;
            const b = bytes[p];
            if (b === 0x5c) {                              // backslash: \u escape in an identifier
                const esc = scanUnicodeEscapeInIdentifier();
                if (esc === null) break;
                escaped = true;
                name += String.fromCodePoint(esc);
                continue;
            }
            if (b < 0x80) {
                if (!isIdContinueAscii(b)) break;
                name += String.fromCharCode(b);
                p += 1;
                continue;
            }
            const d = decode(p);
            if (d.err) { break; }
            if (!isIdContinueCp(d.cp)) break;
            name += String.fromCodePoint(d.cp);
            p += d.len;
        }
        push({ type: "identifier", value: name, escaped: escaped, start: sp + 1, stop: p + 1 });
    }

    // `\uHHHH` or `\u{...}` in an identifier -> code point, or null on malformed (with diag).
    function scanUnicodeEscapeInIdentifier() {
        const start = p;
        if (p + 1 >= n || bytes[p + 1] !== 0x75) { diag("js.syntax", "invalid escape in identifier", start + 1, start + 2); p += 1; return null; }
        p += 2;                                            // consume \u
        let cp = 0;
        if (p < n && bytes[p] === 0x7b) {                  // \u{...}
            p += 1;
            let got = 0;
            while (p < n && bytes[p] !== 0x7d) {
                if (!isHex(bytes[p])) { diag("js.syntax", "invalid unicode escape", start + 1, p + 1); return null; }
                cp = cp * 16 + hexVal(bytes[p]); p += 1; got++;
                if (cp > 0x10ffff) { diag("js.syntax", "unicode escape out of range", start + 1, p + 1); return null; }
            }
            if (p >= n || bytes[p] !== 0x7d || got === 0) { diag("js.syntax", "unterminated unicode escape", start + 1, p + 1); return null; }
            p += 1;                                         // consume }
            return cp;
        }
        for (let k = 0; k < 4; k++) {
            if (p >= n || !isHex(bytes[p])) { diag("js.syntax", "invalid unicode escape", start + 1, p + 1); return null; }
            cp = cp * 16 + hexVal(bytes[p]); p += 1;
        }
        return cp;
    }

    function hexVal(b) {
        if (b <= 0x39) return b - 0x30;
        if (b <= 0x46) return b - 0x41 + 10;
        return b - 0x61 + 10;
    }

    function scanNumber() {
        const sp = p;
        let bigint = false;
        if (bytes[p] === 0x30 && p + 1 < n) {              // 0x / 0o / 0b / 0
            const c = bytes[p + 1] | 0x20;                 // lowercase
            if (c === 0x78 || c === 0x6f || c === 0x62) {  // x o b
                p += 2;
                const isValid = c === 0x78 ? isHex : (c === 0x6f ? (b => b >= 0x30 && b <= 0x37) : (b => b === 0x30 || b === 0x31));
                let got = 0;
                while (p < n && (isValid(bytes[p]) || bytes[p] === 0x5f)) { p += 1; got++; }
                if (got === 0) diag("js.syntax", "missing digits in numeric literal", sp + 1, p + 1);
                if (p < n && bytes[p] === 0x6e) { bigint = true; p += 1; }
                return finishNumber(sp, bigint);
            }
        }
        while (p < n && (isDigit(bytes[p]) || bytes[p] === 0x5f)) p += 1;   // integer part
        if (p < n && bytes[p] === 0x2e) {                  // fraction
            p += 1;
            while (p < n && (isDigit(bytes[p]) || bytes[p] === 0x5f)) p += 1;
        }
        if (p < n && (bytes[p] === 0x65 || bytes[p] === 0x45)) {            // exponent e/E
            p += 1;
            if (p < n && (bytes[p] === 0x2b || bytes[p] === 0x2d)) p += 1;
            let got = 0;
            while (p < n && isDigit(bytes[p])) { p += 1; got++; }
            if (got === 0) diag("js.syntax", "missing exponent in numeric literal", sp + 1, p + 1);
        } else if (p < n && bytes[p] === 0x6e) {           // BigInt suffix
            bigint = true; p += 1;
        }
        return finishNumber(sp, bigint);
    }

    function finishNumber(sp, bigint) {
        // A number immediately followed by an identifier-start is a syntax error (e.g. `3in`),
        // but we recover by ending the number; the parser/oracle handle the rest.
        let raw = "";
        for (let i = sp; i < p; i++) raw += String.fromCharCode(bytes[i]);
        push({ type: "number", value: raw, bigint: bigint, start: sp + 1, stop: p + 1 });
    }

    function scanString(quote) {
        const sp = p;
        p += 1;                                            // opening quote
        let cooked = "";
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated string literal", sp + 1, p + 1); break; }
            const b = bytes[p];
            if (b === quote) { p += 1; break; }
            if (b === 0x0a || b === 0x0d) { diag("js.syntax", "unterminated string literal", sp + 1, p + 1); break; }
            if (b === 0x5c) { cooked += scanEscape(); continue; }
            if (b < 0x80) { cooked += String.fromCharCode(b); p += 1; continue; }
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8 in string literal", p + 1, p + d.len + 1); p += d.len; continue; }
            cooked += String.fromCodePoint(d.cp); p += d.len;
        }
        push({ type: "string", value: cooked, raw: sliceText(sp, p), start: sp + 1, stop: p + 1 });
    }

    // Scan a backslash escape inside a string/template; returns the cooked substring.
    function scanEscape() {
        p += 1;                                            // backslash
        if (p >= n) return "";
        const b = bytes[p];
        switch (b) {
            case 0x6e: p += 1; return "\n";
            case 0x74: p += 1; return "\t";
            case 0x72: p += 1; return "\r";
            case 0x62: p += 1; return "\b";
            case 0x66: p += 1; return "\f";
            case 0x76: p += 1; return "\v";
            case 0x30: {                                   // \0 (NUL) unless followed by a digit
                if (p + 1 < n && isDigit(bytes[p + 1])) { p += 1; return "0"; } // legacy octal: cook loosely
                p += 1; return "\0";
            }
            case 0x78: {                                   // \xHH
                p += 1; let cp = 0, ok = true;
                for (let k = 0; k < 2; k++) { if (p < n && isHex(bytes[p])) { cp = cp * 16 + hexVal(bytes[p]); p += 1; } else { ok = false; break; } }
                if (!ok) { diag("js.syntax", "invalid hex escape", p + 1, p + 2); return ""; }
                return String.fromCharCode(cp);
            }
            case 0x75: {                                   // \uHHHH or \u{...}
                p -= 1;                                     // rewind to backslash for the shared scanner
                const cp = scanUnicodeEscapeInIdentifier();
                return cp === null ? "" : String.fromCodePoint(cp);
            }
            case 0x0a: p += 1; return "";                  // LF line continuation
            case 0x0d: p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; return ""; // CR/CRLF continuation
            default: {
                if (b < 0x80) { p += 1; return String.fromCharCode(b); }
                const d = decode(p);
                if (d.err) { p += d.len; return ""; }
                p += d.len; return String.fromCodePoint(d.cp);
            }
        }
    }

    // Scan a template starting at `p` which is either a backtick (whole/head) or a `}` that
    // resumes a substitution (middle/tail). `opener` is 0x60 (`) or 0x7d (}).
    function scanTemplate(opener) {
        const sp = p;
        p += 1;                                            // consume ` or }
        let cooked = "";
        let mode;
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated template literal", sp + 1, p + 1); mode = (opener === 0x60) ? "noSubstitution" : "tail"; break; }
            const b = bytes[p];
            if (b === 0x60) { p += 1; mode = (opener === 0x60) ? "noSubstitution" : "tail"; break; }   // closing `
            if (b === 0x24 && p + 1 < n && bytes[p + 1] === 0x7b) {                                     // ${
                p += 2; mode = (opener === 0x60) ? "head" : "middle"; braceStack.push("template"); break;
            }
            if (b === 0x5c) { cooked += scanEscape(); continue; }
            if (b === 0x0d) { p += (p + 1 < n && bytes[p + 1] === 0x0a) ? 2 : 1; cooked += "\n"; continue; } // normalize CRLF
            if (b < 0x80) { cooked += String.fromCharCode(b); p += 1; continue; }
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8 in template literal", p + 1, p + d.len + 1); p += d.len; continue; }
            cooked += String.fromCodePoint(d.cp); p += d.len;
        }
        push({ type: "template", mode: mode, value: cooked, raw: sliceText(sp, p), start: sp + 1, stop: p + 1 });
    }

    function scanRegex() {
        const sp = p;
        p += 1;                                            // opening /
        let inClass = false;
        for (;;) {
            if (p >= n) { diag("js.syntax", "unterminated regular expression", sp + 1, p + 1); break; }
            const b = bytes[p];
            if (b === 0x0a || b === 0x0d) { diag("js.syntax", "unterminated regular expression", sp + 1, p + 1); break; }
            if (b === 0x5c) { p += (p + 1 < n) ? 2 : 1; continue; }        // escaped char
            if (b === 0x5b) { inClass = true; p += 1; continue; }
            if (b === 0x5d) { inClass = false; p += 1; continue; }
            if (b === 0x2f && !inClass) { p += 1; break; }                  // closing /
            if (b < 0x80) { p += 1; continue; }
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8 in regular expression", p + 1, p + d.len + 1); }
            p += d.len;
        }
        const patEnd = p;                                  // one past closing /
        while (p < n && (isIdContinueAscii(bytes[p]) || bytes[p] >= 0x80)) {   // flags
            if (bytes[p] < 0x80) { p += 1; } else { const d = decode(p); if (d.err) break; p += d.len; }
        }
        const raw = sliceText(sp, p);
        // pattern excludes the delimiting slashes: bytes (sp+1 .. patEnd-1); flags after the close.
        push({ type: "regex", pattern: sliceText(sp + 1, patEnd - 1), flags: sliceText(patEnd, p), raw: raw, start: sp + 1, stop: p + 1 });
    }

    function sliceText(a, b) {   // best-effort text of 0-based [a,b) as a JS string (for raw/pattern)
        let s = "";
        for (let i = a; i < b; i++) {
            const c = bytes[i];
            if (c < 0x80) { s += String.fromCharCode(c); }
            else { const d = decode(i); if (d.err) { s += "\uFFFD"; } else { s += String.fromCodePoint(d.cp); i += d.len - 1; } }
        }
        return s;
    }

    // Emit a punctuator by maximal munch. Returns true if one was scanned.
    function scanPunctuator() {
        const sp = p;
        const b = bytes[p];
        // `}` may resume a template substitution.
        if (b === 0x7d) {
            if (braceStack.length && braceStack[braceStack.length - 1] === "template") {
                braceStack.pop();
                scanTemplate(0x7d);          // `}` resumes the template substitution
                return true;
            }
            if (braceStack.length) braceStack.pop();   // `}` closes an ordinary block
            p += 1; return emitPunct("}", sp);
        }
        if (b === 0x7b) { braceStack.push("block"); p += 1; return emitPunct("{", sp); }

        const two = (p + 1 < n) ? bytes[p + 1] : -1;
        const three = (p + 2 < n) ? bytes[p + 2] : -1;
        const four = (p + 3 < n) ? bytes[p + 3] : -1;

        switch (b) {
            case 0x2e:  // .
                if (two === 0x2e && three === 0x2e) { p += 3; return emitPunct("...", sp); }
                return advance(1, ".", sp);
            case 0x3f:  // ?
                if (two === 0x3f && three === 0x3d) { p += 3; return emitPunct("??=", sp); }
                if (two === 0x3f) { p += 2; return emitPunct("??", sp); }
                // ?. is optional chaining UNLESS followed by a decimal digit (a ? .5 : b).
                if (two === 0x2e && !(three >= 0x30 && three <= 0x39)) { p += 2; return emitPunct("?.", sp); }
                return advance(1, "?", sp);
            case 0x3d:  // =
                if (two === 0x3d && three === 0x3d) { p += 3; return emitPunct("===", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("==", sp); }
                if (two === 0x3e) { p += 2; return emitPunct("=>", sp); }
                return advance(1, "=", sp);
            case 0x21:  // !
                if (two === 0x3d && three === 0x3d) { p += 3; return emitPunct("!==", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("!=", sp); }
                return advance(1, "!", sp);
            case 0x3c:  // <
                if (two === 0x3c && three === 0x3d) { p += 3; return emitPunct("<<=", sp); }
                if (two === 0x3c) { p += 2; return emitPunct("<<", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("<=", sp); }
                return advance(1, "<", sp);
            case 0x3e:  // >
                if (two === 0x3e && three === 0x3e && four === 0x3d) { p += 4; return emitPunct(">>>=", sp); }
                if (two === 0x3e && three === 0x3e) { p += 3; return emitPunct(">>>", sp); }
                if (two === 0x3e && three === 0x3d) { p += 3; return emitPunct(">>=", sp); }
                if (two === 0x3e) { p += 2; return emitPunct(">>", sp); }
                if (two === 0x3d) { p += 2; return emitPunct(">=", sp); }
                return advance(1, ">", sp);
            case 0x2b:  // +
                if (two === 0x2b) { p += 2; return emitPunct("++", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("+=", sp); }
                return advance(1, "+", sp);
            case 0x2d:  // -
                if (two === 0x2d) { p += 2; return emitPunct("--", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("-=", sp); }
                return advance(1, "-", sp);
            case 0x2a:  // *
                if (two === 0x2a && three === 0x3d) { p += 3; return emitPunct("**=", sp); }
                if (two === 0x2a) { p += 2; return emitPunct("**", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("*=", sp); }
                return advance(1, "*", sp);
            case 0x25:  // %
                if (two === 0x3d) { p += 2; return emitPunct("%=", sp); }
                return advance(1, "%", sp);
            case 0x26:  // &
                if (two === 0x26 && three === 0x3d) { p += 3; return emitPunct("&&=", sp); }
                if (two === 0x26) { p += 2; return emitPunct("&&", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("&=", sp); }
                return advance(1, "&", sp);
            case 0x7c:  // |
                if (two === 0x7c && three === 0x3d) { p += 3; return emitPunct("||=", sp); }
                if (two === 0x7c) { p += 2; return emitPunct("||", sp); }
                if (two === 0x3d) { p += 2; return emitPunct("|=", sp); }
                return advance(1, "|", sp);
            case 0x5e:  // ^
                if (two === 0x3d) { p += 2; return emitPunct("^=", sp); }
                return advance(1, "^", sp);
            case 0x2f:  // /  (division; regex handled before calling here)
                if (two === 0x3d) { p += 2; return emitPunct("/=", sp); }
                return advance(1, "/", sp);
            case 0x28: return advance(1, "(", sp);
            case 0x29: return advance(1, ")", sp);
            case 0x5b: return advance(1, "[", sp);
            case 0x5d: return advance(1, "]", sp);
            case 0x3b: return advance(1, ";", sp);
            case 0x2c: return advance(1, ",", sp);
            case 0x3a: return advance(1, ":", sp);
            case 0x7e: return advance(1, "~", sp);
            default: return false;
        }
    }
    function advance(k, v, sp) { p += k; return emitPunct(v, sp); }
    function emitPunct(v, sp) { push({ type: "punctuator", value: v, start: sp + 1, stop: p + 1 }); return true; }

    // -- main loop --
    while (!limited) {
        skipTrivia();
        if (p >= n) break;
        const b = bytes[p];

        if (b === 0x22 || b === 0x27) { scanString(b); continue; }             // " '
        if (b === 0x60) { scanTemplate(0x60); continue; }                      // `
        if (isDigit(b)) { scanNumber(); continue; }
        if (b === 0x2e && p + 1 < n && isDigit(bytes[p + 1])) { scanNumber(); continue; }  // .5
        if (b === 0x2f && (p + 1 >= n || (bytes[p + 1] !== 0x2f && bytes[p + 1] !== 0x2a)) && regexAllowed()) {
            scanRegex(); continue;                                             // / regex
        }
        if (b === 0x5c || isIdStartAscii(b)) { scanIdentifier(); continue; }   // \uXXXX ident or ASCII ident
        if (b >= 0x80) {
            const d = decode(p);
            if (d.err) { diag("js.syntax", "invalid UTF-8", p + 1, p + d.len + 1); p += d.len; continue; }
            if (isIdStartCp(d.cp)) { scanIdentifier(); continue; }
            diag("js.syntax", "unexpected character U+" + d.cp.toString(16).toUpperCase(), p + 1, p + d.len + 1);
            p += d.len; continue;
        }
        if (scanPunctuator()) continue;

        // Unknown ASCII byte (e.g. @, #, stray control): report + skip one byte, recover.
        diag("js.syntax", "unexpected character", p + 1, p + 2);
        p += 1;
    }

    push({ type: "eof", start: p + 1, stop: p + 1 });
    return { tokens: tokens, diagnostics: diagnostics, linemap: buildLinemap(bytes) };
}

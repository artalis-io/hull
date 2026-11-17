// hull:source:parser - recursive-descent ECMAScript parser (JS mirror of hull.source.parser).
//
// Drives the INCREMENTAL tokenizer (hull:source:lexer createTokenizer) and passes an explicit
// grammatical slash goal at every advance: operand position -> a `/` is a regex; operator
// position -> division; overridden at the grammatically-known `}` / `)` sites (a statement
// block vs an object/function-expression value; a control-flow head vs a call/group). This is
// why the parser -- not the raw token stream -- authoritatively resolves `if (ok) {} /re/`
// (regex) from `const f = function(){} / 2` (division).
//
// NEVER raises. A parse problem emits a js.syntax diagnostic and recovers (an Error node +
// synchronize to a statement boundary, with a stall guard). A valid construct the parser
// declines emits js.unsupported (never a wrong AST). parse() drives the tokenizer to EOF, so
// ALL comments and lexical diagnostics are collected and preserved alongside parser
// diagnostics even when recovery continues.
//
// Public: parse(bytes, opts?) -> SourceUnit
//   SourceUnit = { ast, comments, diagnostics, linemap, valid }
//     ast   = { type:"Program", body:[Statement], start, stop }
//     valid = diagnostics has no severity:"error"  (i.e. "did it parse cleanly?")
//   opts: { path?, maxTokens?, maxDiagnostics?, maxDepth?, lang? }
//     lang = "js" (the default when absent) | "ts". Any other value - a typo, or "tsx" / "jsx",
//     which are not supported - parses nothing: the unit is invalid with one js.internal
//     diagnostic, never a silent JS parse.
//
// TypeScript mode (lang "ts"; docs/typescript_design.md section 3) is OFF by default and is
// never chosen from the path. Every TypeScript branch below is guarded by `ts &&`
// and fires only where JavaScript is a syntax error, so JS mode is unchanged byte for byte
// (test_js_golden and scripts/js_frontend_diff.sh hold it) and valid JavaScript parses the same
// in both modes (test_js_typescript holds that). In TS mode the unit also carries `typeSpans` (the
// erasable type syntax, sorted half-open byte ranges, see hull:source:ts_erase), read by the
// type grammar (hull:source:ts_types). Type tokens never move a node's range: a node ends at its
// last JAVASCRIPT token, so the TS-mode AST is exactly the JS-mode AST of the erased bytes.
// Erased in this slice: annotations (variables, parameters incl. arrow and async-arrow ones,
// returns, class fields), optional `?` on parameters and members, definite assignment `!`,
// `as` / `satisfies`, non-null `!`, and whole declarations in a statement list - `interface`,
// `type`, `declare` const / let / var / function, `import type` / `export type` and their
// `export` forms - plus inline `type` import / export specifiers. An erased declaration builds
// no node, and JSDoc attaches as it does over the erased bytes. Every other TypeScript construct
// is refused with a ts.unsupported diagnostic.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { createTokenizer } from "hull:source:lexer";
import { newDiagnostic, makeBudget } from "hull:source:diagnostic";
import { attach as attachAnnotations } from "hull:source:annotations";
import { createTypeGrammar } from "hull:source:ts_types";
import { erase } from "hull:source:ts_erase";

// Reserved words that may NOT be a binding/identifier reference.
const RESERVED = new Set([
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
    "do", "else", "export", "extends", "finally", "for", "function", "if", "import", "in",
    "instanceof", "new", "return", "super", "switch", "this", "throw", "try", "typeof", "var",
    "void", "while", "with", "enum",
]);
// Hull parses MODULE code, which is always strict. These are reserved as BOTH a BindingIdentifier
// and an IdentifierReference (and a LabelIdentifier) in strict/module code: the strict
// FutureReservedWords plus the always-reserved `await`/`yield` (a genuine YieldExpression only
// occurs inside a generator, which Hull declines, so a `yield` reaching an identifier context is
// always a reserved-word misuse). Checked in parseIdentifier -> covers references, bindings,
// class/function names, and labels. Property keys go through parseIdentifierName and are exempt.
const STRICT_RESERVED = new Set([
    "implements", "interface", "let", "package", "private", "protected", "public", "static",
    "yield", "await",
]);
// Restricted as a BINDING target only (a reference/call `eval(x)` / `arguments` stays legal).
const BINDING_RESTRICTED = new Set(["eval", "arguments"]);
// Unary operators (prefix).
const UNARY = new Set(["+", "-", "!", "~", "typeof", "void", "delete"]);
// Assignment operators.
const ASSIGN = new Set(["=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??="]);
// Binary operator precedence (higher binds tighter). Logical/coalesce handled with these too.
// A Map, not an object literal: it is looked up with every identifier at an operator position,
// and an object would answer `constructor` / `toString` / ... from Object.prototype.
const BINPREC = new Map([
    ["??", 1], ["||", 2], ["&&", 3], ["|", 4], ["^", 5], ["&", 6],
    ["==", 7], ["!=", 7], ["===", 7], ["!==", 7],
    ["<", 8], [">", 8], ["<=", 8], [">=", 8], ["instanceof", 8], ["in", 8],
    ["<<", 9], [">>", 9], [">>>", 9],
    ["+", 10], ["-", 10], ["*", 11], ["/", 11], ["%", 11], ["**", 12],
]);

// A validated nonnegative-integer option, else the default.
function nnInt(x, dflt) { return (typeof x === "number" && isFinite(x) && x >= 0 && Math.floor(x) === x) ? x : dflt; }

// The protected boundary shared by parse() and the test-only probe. Runs `body()` (which
// produces a SourceUnit) and ENFORCES "never raises" by containing EVERY catchable exception as
// a SourceUnit with a js.internal diagnostic.
//
// It deliberately does NOT inspect e.name / e.message to decide "is this a resource breach?" --
// that signal is forgeable (any script can name an error "InternalError" and write "out of
// memory" into its .message). Resource discrimination is HOST-owned and authoritative: the
// session refuses over-limit allocations in its own allocator (recording heap / machine-stack
// markers) and raises an uncatchable error when the instruction budget is spent. A spoofed
// error is an ordinary CATCHABLE throw this handler contains as js.internal; a genuine breach
// is either uncatchable (never reaches here) or, having tripped a host marker, is re-classified
// js.limit.* by the session AFTER this returns -- from its markers, never from the error object.
function protectedParse(body, opts) {
    try {
        return body();
    } catch (e) {
        const msg = (e && e.message !== undefined) ? String(e.message) : String(e);
        const p = (opts && opts.path) || null;
        return {
            ast: { type: "Program", start: 1, stop: 1, body: [] },
            comments: [], linemap: [1], valid: false,
            diagnostics: [newDiagnostic("error", "js.internal", "internal parser error: " + msg, p, null)],
        };
    }
}

// Public entry: parse a byte source into a SourceUnit. Never raises (see protectedParse).
export function parse(bytes, opts) {
    return protectedParse(function () { return parseInternal(bytes, opts, null); }, opts);
}

// Test-only probe: run the SAME protected boundary but force parseInternal to throw `inject()`
// first, proving the boundary contains ordinary exceptions and re-throws host resource ones.
// Not part of the production surface (no production caller passes an injector).
export function __parseWithInjection(bytes, opts, inject) {
    return protectedParse(function () { return parseInternal(bytes, opts, inject); }, opts);
}

// The languages parse() reads (opts.lang). "tsx" / "jsx" are known and refused like any
// unknown value, until JSX is supported.
const LANGS = new Set(["js", "ts"]);

function parseInternal(bytes, opts, inject) {
    opts = opts || {};
    const path = opts.path || null;
    const lang = opts.lang === undefined ? "js" : opts.lang;
    if (!LANGS.has(lang)) {
        const shown = typeof lang === "string" ? "'" + lang + "'" : typeof lang;
        return {
            ast: { type: "Program", start: 1, stop: 1, body: [] },
            comments: [], linemap: [1], valid: false,
            diagnostics: [newDiagnostic("error", "js.internal", "unknown lang " + shown + " (expected \"js\" or \"ts\")", path, null)],
        };
    }
    const maxDepth = (typeof opts.maxDepth === "number" && opts.maxDepth > 0) ? Math.floor(opts.maxDepth) : 1000;
    const maxDiagnostics = nnInt(opts.maxDiagnostics, 4096);
    const ts = lang === "ts";

    if (inject) inject();            // test-only: fault the parser to exercise protectedParse

    // ONE budget owner shared with the tokenizer -> maxDiagnostics is authoritative across the
    // whole SourceUnit (tokenizer + parser), and both producers stay memory-bounded.
    const budget = makeBudget(maxDiagnostics, path);
    const topts = { path: path, diagBudget: budget };
    if (opts.maxTokens !== undefined) topts.maxTokens = opts.maxTokens;
    const tk = createTokenizer(bytes, topts);
    const la = [];                   // lookahead buffer of tokens AFTER cur (non-destructive peek)
    let depth = 0;
    let errored = false;             // rate-limit cascading recovery within one statement
    const typeSpans = [];            // TS mode: erasable type syntax (empty until the type grammar lands)
    let condConsequent = 0;          // TS mode: > 0 inside a `?:` consequent or a `case` test, where `(a) :` is JS

    // Module-grammar context (docs/js_test262_design.md). An explicit function-context stack
    // plus a module-item-position flag drive the position-sensitive rules: `await` interpretation
    // (module top level / async body only, never in parameters), `return` (function-only), and
    // import/export declarations (module-top-level only). Dynamic import() is an expression and
    // is unaffected. Save/restore snapshots this state so speculation cannot leak a context
    // mutation; every recovery path restores it structurally (frames are pushed/popped in the
    // same function that parses the construct).
    const ctxStack = [];             // frames: { kind: "regular"|"async", region: "params"|"body", arrow: bool }
    let atModuleItem = true;         // parsing a DIRECT module body item -> import/export legal
    function curFn() { return ctxStack.length ? ctxStack[ctxStack.length - 1] : null; }
    // `await` is an AwaitExpression only at module top level (no function frame) or in an ASYNC
    // function BODY. In parameters (even async) and in any regular function it is not.
    function awaitIsExpr() { const f = curFn(); return f ? (f.kind === "async" && f.region === "body") : true; }
    function inFunction() { return ctxStack.length > 0; }
    // `new.target` is valid iff there is an enclosing NON-ARROW function frame (params or body
    // both count, methods count); arrows are transparent and inherit it, so a NEW.target reaches
    // the nearest non-arrow function through any number of arrow frames. At module top level (no
    // frame) and in a top-level arrow (only arrow frames) it is a syntax error. Matches QuickJS.
    function newTargetAllowed() { for (let i = 0; i < ctxStack.length; i++) if (!ctxStack[i].arrow) return true; return false; }
    // `yield` binds to the nearest enclosing NON-ARROW function; it is a keyword only when that
    // function is a generator. Hull DECLINES generators (js.unsupported at `function*`) but still
    // parses their bodies for recovery, so a yield inside a declined generator must stay quiet
    // (no second diagnostic), while a `yield` in any non-generator function or at module top level
    // is a reserved-word syntax error.
    function inGenerator() { for (let i = ctxStack.length - 1; i >= 0; i--) if (!ctxStack[i].arrow) return ctxStack[i].gen === true; return false; }
    // Run `fn` while parsing a NESTED statement (import/export illegal there).
    function nested(fn) { const t = atModuleItem; atModuleItem = false; const r = fn(); atModuleItem = t; return r; }
    // Parse a function's params then body under a pushed context frame; restored on return.
    function withFn(kind, doParams, doBody, gen) {
        ctxStack.push({ kind: kind, region: "params", arrow: false, gen: gen === true });
        const params = doParams();
        ctxStack[ctxStack.length - 1].region = "body";
        const body = nested(doBody);
        ctxStack.pop();
        return { params: params, body: body };
    }

    // cur/prev tokens. The goal used to READ cur was decided when we advanced past prev.
    let cur = tk.next(true);         // first token is at statement (operand) position
    let prev = null;
    // TS mode: the last consumed token that is not type syntax (typeDepth > 0 while a type is
    // read). Nodes end there, so a trailing type never stretches a node.
    let lastJs = null;
    let firstJs = null;              // TS mode: the first consumed token that is not type syntax
    let typeDepth = 0;

    // advance past cur, reading the next token with an explicit grammatical slash goal:
    //   regexAllowed=true  -> the next token is at an operand/statement position
    //   regexAllowed=false -> the next token is at an operator/continuation position
    // A buffered lookahead token (already lexed) is consumed as-is; the goal only applies to a
    // fresh read from the tokenizer.
    function advance(regexAllowed) {
        prev = cur;
        if (typeDepth === 0) { lastJs = cur; if (firstJs === null) firstJs = cur; }
        cur = la.length > 0 ? la.shift() : tk.next(regexAllowed === false ? false : true);
        return prev;
    }
    // Non-destructive lookahead: the k-th token AFTER cur (1-based). Newly read tokens use
    // `goal` for their slash decision; the disambiguation sites only peek non-slash positions.
    function peekTok(k, goal) { while (la.length < k) la.push(tk.next(goal === false ? false : true)); return la[k - 1]; }
    function peekIsP(k, v) { const t = peekTok(k, false); return t.type === "punctuator" && t.value === v; }

    function atEof() { return cur.type === "eof"; }
    function isP(v) { return cur.type === "punctuator" && cur.value === v; }
    // A contextual/reserved keyword match at a KEYWORD position: an escaped identifier
    // (`as`) is NOT the keyword (a UnicodeEscapeSequence in a ReservedWord/contextual
    // keyword is an early error). Reserved words stay usable as property names / specifiers
    // because those read through parseIdentifierName / parsePropertyKey, not isKw.
    function isKw(v) { return cur.type === "identifier" && cur.value === v && !cur.escaped; }
    function nl() { return cur.nlBefore; }

    // Peek the token after cur under a GUESSED slash goal, and take it back unless `keep(tok)`
    // accepts it - rewinding the tokenizer, the lookahead buffer and the diagnostic budget, so a
    // wrong guess leaves no lexical disagreement or stray diagnostic. O(1): the parser makes no
    // other speculation (the `async (` cover grammar below parses its list once).
    function peekOrRewind(goal, keep) {
        if (la.length > 0) return keep(la[0]);       // already lexed: nothing to take back
        const m = tk.peekMark(), b = budget.mark();
        const t = peekTok(1, goal);
        if (keep(t)) return true;
        tk.peekRewind(m); la.length = 0; budget.reset(b);
        return false;
    }

    // Every parser diagnostic flows through the ONE shared budget so the combined tokenizer +
    // parser ordinary count can never exceed maxDiagnostics; terminal js.limit.* are always kept.
    function diag(sev, code, message, start, stop) { budget.push(sev, code, message, { start: start, stop: stop }); }
    function synErr(message, tok) { const t = tok || cur; diag("error", "js.syntax", message, t.start, t.stop); }
    function unsupported(message, tok) { const t = tok || cur; diag("error", "js.unsupported", message, t.start, t.stop); }
    // TS mode: a TypeScript construct this parser does not (yet) accept. The caller then goes on
    // as JS mode would, so the JS diagnostics that follow are JS mode's own.
    // One refusal per position: JS-mode recovery can come back to the same token (a class
    // member loop retrying at a `:`), and two hooks can see one `<`.
    let tsRefusedAt = 0;
    function tsRefuse(what, tok) {
        const t = tok || cur;
        if (t.start === tsRefusedAt) return;
        tsRefusedAt = t.start;
        diag("error", "ts.unsupported", "TypeScript " + what + " are not supported", t.start, t.stop);
    }
    // The token after cur (operator goal: every caller peeks after an identifier or a binding).
    function tsPeek() { return peekTok(1, false); }
    function isIdTok(t) { return t.type === "identifier" && !t.escaped; }
    // A TypeScript declaration at statement start: `interface X`, `type X`, `enum X`,
    // `namespace X`, `module X`, `declare <anything>`, `abstract class`. Each word followed by a
    // name ON THE SAME LINE is a JS syntax error (two identifiers with no ASI between them), and
    // `enum` is reserved; with a line break between, JS reads two statements and so does this.
    function tsDeclarationStart() {
        if (!isIdTok(cur)) return null;
        const v = cur.value;
        if (v !== "interface" && v !== "type" && v !== "enum" && v !== "namespace" && v !== "module" && v !== "declare" && v !== "abstract") return null;
        const nx = tsPeek();
        if (nx.nlBefore) return null;
        if (v === "abstract") return (isIdTok(nx) && nx.value === "class") ? "abstract classes" : null;
        if (v === "declare") return nx.type === "identifier" ? "'declare' declarations" : null;
        if (v === "module") return (nx.type === "identifier" || nx.type === "string") ? "module declarations" : null;
        if (nx.type !== "identifier") return null;
        if (v === "interface") return "interfaces";
        if (v === "type") return "type aliases";
        if (v === "enum") return "enums";
        return "namespaces";
    }
    // A class member / parameter modifier followed by more of the member on the same line.
    const TS_MODIFIERS = new Set(["public", "private", "protected", "readonly", "abstract", "override", "declare"]);
    function tsModifierStart() {
        if (!isIdTok(cur) || !TS_MODIFIERS.has(cur.value)) return false;
        const nx = tsPeek();
        if (nx.nlBefore) return false;
        return nx.type === "identifier" || nx.type === "string" || nx.type === "number" ||
               (nx.type === "punctuator" && nx.value === "[");
    }
    // TS mode: split a `>>`-family token at cur so its first `>` closes a type list. Through the
    // tokenizer when nothing was read past cur; by hand when a lookahead already was.
    function splitGreaterTok() {
        if (la.length === 0) { const g = tk.splitGreater(); if (g) cur = g; return; }
        const t = cur;
        la.unshift({ type: "punctuator", value: t.value.substring(1), start: t.start + 1, stop: t.stop, nlBefore: false });
        cur = { type: "punctuator", value: ">", start: t.start, stop: t.start + 1, nlBefore: t.nlBefore };
    }
    const types = ts ? createTypeGrammar({
        cur: function () { return cur; }, advance: advance, peek: function (k) { return peekTok(k, false); },
        isP: isP, isKw: isKw, nl: nl, splitGreater: splitGreaterTok,
        synErr: function (m, t) { synErr(m, t); }, guard: guard, unguard: unguard,
    }) : null;
    // TS mode: consume type syntax with `body` (starting at cur) and record it as one span.
    function tsSpan(body) {
        const start = cur.start;
        const outer = typeDepth === 0;               // a span inside an erased declaration is part of it
        typeDepth++;
        body();
        typeDepth--;
        const stop = prev.stop;
        if (outer && stop > start) typeSpans.push({ start: start, stop: stop });
    }
    // `: T` (cur is the `:`).
    function tsAnnotation() { tsSpan(function () { advance(false); types.parseType(); }); }
    // `: R` for a return type (a type predicate allowed).
    function tsReturnType() { tsSpan(function () { advance(false); types.parseReturnType(); }); }
    // `?` [`: T`] (cur is the `?`).
    function tsOptional() { tsSpan(function () { advance(false); if (isP(":")) { advance(false); types.parseType(); } }); }
    // `!: T` (cur is the `!`); TypeScript requires the type.
    function tsDefinite() {
        tsSpan(function () {
            advance(false);
            if (isP(":")) { advance(false); types.parseType(); } else synErr("expected ':' after a definite assignment assertion");
        });
    }
    // A line terminator (LF, CR, U+2028, U+2029) in the bytes [from, to) - 1-based positions.
    // Erasure keeps them, so one inside a return type lands between `)` and `=>`.
    function tsLineBreakBetween(from, to) {
        for (let i = from - 1; i < to - 1; i++) {
            const b = bytes[i];
            if (b === 0x0a || b === 0x0d) return true;
            if (b === 0xe2 && bytes[i + 1] === 0x80 && (bytes[i + 2] === 0xa8 || bytes[i + 2] === 0xa9)) return true;
        }
        return false;
    }
    // `name?` followed by `:` `,` `)` `=`: an optional arrow parameter (cur is `name`). The token
    // after `?` is peeked with the operand goal, as JavaScript reads it after a `?`.
    function tsOptionalParamAhead() {
        if (!isIdTok(cur) || RESERVED.has(cur.value) || !peekIsP(1, "?")) return false;
        const t = peekTok(2, true);
        return t.type === "punctuator" && (t.value === ":" || t.value === "," || t.value === ")" || t.value === "=");
    }
    // `item = default` after an annotated arrow parameter: the AssignmentExpression JS mode's
    // parseAssignment builds for `(item = default)`, which the cover grammar turns into a default.
    function tsDefaultAfter(item) {
        if (!isP("=")) return item;
        if (!isAssignTarget(item)) synErr("invalid assignment target", item);
        const node = mk("AssignmentExpression", item.start);
        node.operator = "="; node.left = item; advance(true);
        node.right = parseAssignment();
        return fin(node);
    }
    // One item of an arrow-candidate list (a parenthesized expression or `async (` arguments) in
    // TS mode: an optional parameter `a?`, an annotation `a: T`, then a default. Sets info.typed
    // when the list can only be an arrow's parameters.
    function tsArrowItem(info) {
        if (tsOptionalParamAhead()) {
            const id = parseIdentifier();
            tsOptional();
            info.typed = true;
            return tsDefaultAfter(id);
        }
        const item = parseAssignment();
        if (!isP(":")) return item;
        tsAnnotation();
        info.typed = true;
        return tsDefaultAfter(item);
    }
    // After an arrow-candidate list's `)`: a return type, when the list is typed or `(a) :`
    // cannot be a conditional's or a case's colon. Then `=>` must follow on the same line (a
    // line break there would make the erased arrow a syntax error, docs/typescript_design.md 3.4).
    function tsArrowTail(info) {
        const close = prev;                          // the list's `)`
        if (isP(":") && (info.typed || condConsequent === 0)) { tsReturnType(); info.typed = true; }
        if (info.typed && isP("=>") && tsLineBreakBetween(close.stop, cur.start))
            diag("error", "ts.unsupported", "a line break before '=>' after TypeScript parameter or return types is not supported: keep '=>' on the line the types end", cur.start, cur.stop);
    }

    function mk(type, start) { return { type: type, start: start, stop: start }; }
    // Finalize a node's half-open range. stop = the last consumed token's stop, but NEVER before
    // the node's own start: on an error-recovery path that consumed nothing after the node began
    // (prev is still the token before `start`), clamp to a zero-width span [start, start] rather
    // than emit an inverted range. Keeps the AST range invariant start <= stop <= n+1.
    function fin(node) { const e = ts ? lastJs : prev; const s = e ? e.stop : node.start; node.stop = s < node.start ? node.start : s; return node; }
    // An Error recovery node spans [start, end-of-CONSUMED-input] = prev.stop -- NOT cur.start
    // (the next, not-yet-consumed token, which lies past any trailing trivia). Using prev.stop
    // mirrors fin() so an Error child can never escape its enclosing node's range (also fin'd to
    // prev.stop): `import\n;` must not yield an Error that overshoots into the next line. Clamp
    // to >= start for the zero-width case (nothing consumed after `start`).
    function errNode(start) { const e = mk("Error", start); const t = ts ? lastJs : prev; const s = t ? t.stop : start; e.stop = s < start ? start : s; return e; }

    // Consume a specific punctuator (operator/operand goal after). Returns true if consumed.
    function eatP(v, regexAllowedAfter) { if (isP(v)) { advance(regexAllowedAfter); return true; } return false; }
    // Expect a specific punctuator; on mismatch emit a diagnostic (no advance).
    function expectP(v, regexAllowedAfter) {
        if (isP(v)) { advance(regexAllowedAfter); return true; }
        synErr("expected '" + v + "'");
        return false;
    }

    // Skip to the next statement boundary after a syntax error (stall-guarded by the caller).
    function synchronize() {
        for (;;) {
            if (atEof()) return;
            if (isP(";")) { advance(true); return; }
            if (isP("}")) return;             // let the enclosing block consume it
            if (cur.nlBefore) return;         // ASI-ish: a new line likely starts a new statement
            advance(true);
        }
    }

    function guard() { if (++depth > maxDepth) { diag("error", "js.limit.depth", "maximum nesting depth exceeded (" + maxDepth + ")", cur.start, cur.stop); return false; } return true; }
    function unguard() { depth--; }

    // -- TS mode: erased declarations --
    // The declaration a statement starting with token t0 is, for the forms this slice erases:
    // "interface", "type" (an alias), "declareVar", "declareFunction", or null. peekAt(i) reads
    // the i-th token after t0 - lazily, and only after `interface` / `type` / `declare` (where
    // the next token is an operator-position read, as JavaScript would make it), so no other
    // statement start is ever lexed ahead. With a line break after t0 JS reads two statements,
    // and so does this.
    function tsErasableKind(t0, peekAt) {
        if (!isIdTok(t0) || (t0.value !== "interface" && t0.value !== "type" && t0.value !== "declare")) return null;
        const t1 = peekAt(1);
        if (t1.nlBefore || !isIdTok(t1)) return null;
        if (t0.value === "interface") return "interface";
        if (t0.value === "type") { const t2 = peekAt(2); return (t2.type === "punctuator" && (t2.value === "=" || t2.value === "<")) ? "type" : null; }
        if (t1.value === "const" || t1.value === "let" || t1.value === "var") return "declareVar";
        if (t1.value === "function") return "declareFunction";
        return null;
    }
    function tsPeekFromCur(i) { return peekTok(i, false); }
    // `import type X from` / `import type { X }` / `import type * as n` at cur `import` (`import
    // type from "m"` and `import type, { a }` are JS: a default binding named `type`).
    function tsTypeOnlyImportAhead() {
        const t1 = peekTok(1, false);
        if (!isIdTok(t1) || t1.value !== "type") return false;
        const t2 = peekTok(2, false);
        return (t2.type === "punctuator" && (t2.value === "{" || t2.value === "*")) || (isIdTok(t2) && t2.value !== "from");
    }
    // Erase one whole statement read by `body`, and build no node. A statement ended by automatic
    // semicolon insertion (`asiEnd`) right before it, followed by a token that would continue
    // that statement once the erased bytes are blank (`x` / type T = A; / `(y)` reads `x(y)`),
    // is refused (docs/typescript_design.md 3.4).
    // So is one followed by a `;`, which the statement before it would take once the erased
    // bytes are blank (`x` / `interface I {}` / `;`: one statement in JavaScript, two here).
    let asiEnd = null;                               // TS mode: the last token of a statement ended by ASI
    function tsEraseStatement(body) {
        const before = lastJs;
        tsSpan(body);
        if (before !== null && before === asiEnd) {
            if (tsContinuesExpression(cur))
                diag("error", "ts.unsupported", "an erased TypeScript declaration between a statement without ';' and a line that would continue it is not supported: end the statement before it with ';'", cur.start, cur.stop);
            else if (isP(";"))
                diag("error", "ts.unsupported", "an erased TypeScript declaration between a statement without ';' and a ';' is not supported: end the statement before it with ';'", cur.start, cur.stop);
        }
        return null;
    }
    // A token that both starts a statement and continues an expression on the previous line.
    function tsContinuesExpression(t) {
        if (t.type === "template") return t.mode === "noSubstitution" || t.mode === "head";
        if (t.type === "regex") return true;
        return t.type === "punctuator" && (t.value === "(" || t.value === "[" || t.value === "+" || t.value === "-" || t.value === "/" || t.value === "/=");
    }
    // `;` or automatic semicolon insertion closing an erased declaration.
    function tsDeclEnd() {
        if (isP(";")) { advance(true); return; }
        if (!(isP("}") || atEof() || nl())) synErr("expected ';'");
    }
    // The body of an erasable declaration, cur at its first word.
    function tsDeclarationBody(kind) {
        if (kind === "interface") {
            advance(false); advance(false);         // `interface`, the name
            if (isP("<")) types.parseTypeParameters();
            if (isKw("extends")) {
                advance(false);
                for (;;) { const b = cur.start; types.parseType({ noConditional: true }); if (cur.start === b || !isP(",")) break; advance(false); }
            }
            if (isP("{")) types.parseObjectType(); else synErr("expected '{'");
            return;
        }
        if (kind === "type") {
            advance(false); advance(false);         // `type`, the name
            if (isP("<")) types.parseTypeParameters();
            if (isP("=")) { advance(false); types.parseType(); } else synErr("expected '='");
            tsDeclEnd();
            return;
        }
        advance(false);                             // `declare`
        if (kind === "declareVar") {
            advance(false);                         // const / let / var
            for (;;) {
                if (isIdTok(cur) && !RESERVED.has(cur.value)) advance(false); else { synErr("expected a name"); return; }
                if (isP(":")) { advance(false); types.parseType(); }
                if (isP("=")) { tsRefuse("initializers in 'declare' declarations"); return; }
                if (!isP(",")) break;
                advance(false);
            }
            tsDeclEnd();
            return;
        }
        advance(false);                             // `function`
        if (isIdTok(cur) && !RESERVED.has(cur.value)) advance(false); else { synErr("expected a function name"); return; }
        if (isP("<")) types.parseTypeParameters();
        types.parseParameters();
        if (isP(":")) { advance(false); types.parseReturnType(); }
        tsDeclEnd();
    }
    // `export type { A } [from "m"]`, `export type * [as n] from "m"` after `export type`.
    function tsTypeOnlyExportRest() {
        if (isP("*")) {
            advance(false);
            if (isKw("as")) { advance(false); advance(false); }
        } else if (isP("{")) {
            advance(false);
            while (!isP("}") && !atEof()) {
                const b = cur.start;
                if (cur.type === "identifier" || cur.type === "string") advance(false); else { synErr("expected an export name"); return; }
                if (isKw("as")) { advance(false); advance(false); }
                if (cur.start === b || !isP(",")) break;
                advance(false);
            }
            if (!isP("}")) { synErr("expected '}'"); return; }
            advance(false);
        } else { synErr("expected '{' or '*'"); return; }
        if (isKw("from")) { advance(false); if (cur.type === "string") advance(false); else { synErr("expected module specifier"); return; } }
        tsDeclEnd();
    }
    // An inline `type` specifier, `{ type A as B, ... }` (cur at `type`). Peeks only in TS mode.
    function tsTypeSpecifierAhead() {
        if (!isKw("type")) return false;
        const t1 = tsPeek();
        if (t1.type !== "identifier" && t1.type !== "string") return false;
        if (t1.type === "identifier" && t1.value === "as") {
            // `type as as x` / `type as x` (a specifier named `type`): TypeScript reads the
            // second form as JS; only `type as as x` and `type as,` / `type as }` are types.
            const t2 = peekTok(2, false);
            return t2.type === "identifier" && t2.value === "as";
        }
        return true;
    }
    // Erase `type A [as B]` and the comma after it; returns whether a comma was erased.
    function tsEraseSpecifier() {
        let comma = false;
        tsSpan(function () {
            advance(false);                         // `type`
            advance(false);                         // the name
            if (isKw("as")) { advance(false); advance(false); }
            if (isP(",")) { advance(true); comma = true; }
        });
        return comma;
    }

    // A statement-list item (Program, block, function body, switch case): the only place a
    // TypeScript declaration can be erased away entirely.
    let stmtListItem = false;
    function parseListStatement() { if (ts) stmtListItem = true; return parseStatement(); }

    // -- Program --
    function parseProgram() {
        const prog = mk("Program", cur.start);
        prog.body = [];
        while (!atEof()) {
            const before = cur.start;
            atModuleItem = true;                        // direct module body item: import/export legal
            const st = parseListStatement();
            if (st) prog.body.push(st);
            if (cur.start === before && !atEof()) advance(true);   // stall guard
            errored = false;
        }
        // Drain to EOF is implicit: cur is eof; the tokenizer already produced all tokens,
        // so tk.comments / tk.diagnostics are complete.
        // TS mode: the erased JavaScript starts at its first token that is not type syntax (or
        // at EOF when everything was erased).
        if (ts) prog.start = firstJs !== null ? firstJs.start : cur.start;
        return fin(prog);
    }

    // -- Statements --
    function parseStatement() {
        const listItem = stmtListItem;
        stmtListItem = false;
        if (!guard()) { synchronize(); unguard(); return errNode(cur.start); }
        let st;
        if (cur.type === "punctuator") {
            if (cur.value === "{") st = parseBlock();
            else if (cur.value === ";") { st = mk("EmptyStatement", cur.start); advance(true); st = fin(st); }
            else st = parseExpressionStatement();
        } else if (cur.type === "identifier" && !cur.escaped) {
            // LabeledStatement: LabelIdentifier `:` Statement (`outer: for (...) ...`,
            // `label: { ... }`). Peeking the token after the identifier can lex a following `/`
            // under the WRONG slash goal (e.g. `await /re/` -> `/` as division, cached), which
            // would then corrupt the real parse: on a non-label the peek is taken back so the
            // real parse re-lexes.
            const isLabel = peekOrRewind(false, function (t) { return t.type === "punctuator" && t.value === ":"; });
            if (isLabel) {
                const lbl = mk("LabeledStatement", cur.start);
                lbl.label = parseIdentifier(); advance(true);   // label, then past `:`
                lbl.body = nested(parseStatement);
                st = fin(lbl);
            } else
            switch (cur.value) {
                case "var": case "let": case "const": st = parseVarDeclaration(); break;
                case "function": st = parseFunctionDeclaration(false); break;
                case "async": st = maybeAsyncFunctionDecl(); break;
                case "class": st = parseClassDeclaration(); break;
                case "if": st = parseIf(); break;
                case "for": st = parseFor(); break;
                case "while": st = parseWhile(); break;
                case "do": st = parseDoWhile(); break;
                case "switch": st = parseSwitch(); break;
                case "try": st = parseTry(); break;
                case "return": st = parseReturnLike("ReturnStatement", true); break;
                case "throw": st = parseReturnLike("ThrowStatement", false); break;
                case "break": st = parseBreakContinue("BreakStatement"); break;
                case "continue": st = parseBreakContinue("ContinueStatement"); break;
                case "import": {
                    // `import` at statement position is an ImportDeclaration UNLESS it is a
                    // dynamic import CALL `import(...)` or the `import.meta` meta-property, which
                    // are EXPRESSIONS -> parse an expression statement (parsePrimary handles both).
                    const nx = peekTok(1, false);
                    if (nx.type === "punctuator" && (nx.value === "(" || nx.value === ".")) st = parseExpressionStatement();
                    else if (ts && tsTypeOnlyImportAhead()) st = tsEraseStatement(function () { parseImport(); });
                    else st = parseImport();
                    break;
                }
                case "export": st = parseExport(); break;
                case "with": { const ws = cur.start; unsupported("with statement is not supported"); advance(true); expectP("(", true); parseExpression(); expectP(")", true); nested(parseStatement); st = errNode(ws); break; }
                case "debugger": { st = mk("DebuggerStatement", cur.start); advance(true); semicolon(); st = fin(st); break; }
                default: {
                    const kind = ts ? tsErasableKind(cur, tsPeekFromCur) : null;
                    if (kind !== null && listItem) { st = tsEraseStatement(function () { tsDeclarationBody(kind); }); break; }
                    const what = ts ? (kind !== null ? "declarations outside a statement list" : tsDeclarationStart()) : null;
                    if (what !== null) tsRefuse(what);
                    st = parseExpressionStatement();
                }
            }
        } else {
            st = parseExpressionStatement();
        }
        unguard();
        return st;
    }

    function parseBlock() {
        const b = mk("BlockStatement", cur.start);
        b.body = [];
        advance(true);                              // past `{` -> statement position
        while (!isP("}") && !atEof()) {
            const before = cur.start;
            const s = nested(parseListStatement);
            if (s) b.body.push(s);
            if (cur.start === before && !atEof() && !isP("}")) advance(true);
            errored = false;
        }
        expectP("}", true);                         // a block `}` -> a statement follows (regex ok)
        return fin(b);
    }

    function parseVarDeclaration() {
        const d = mk("VariableDeclaration", cur.start);
        d.kind = cur.value;
        d.declarations = [];
        advance(true);                              // past kind
        for (;;) {
            const decl = mk("VariableDeclarator", cur.start);
            decl.id = parseBindingTarget();
            if (ts && isP("!") && !nl()) tsDefinite();
            else if (ts && isP(":")) tsAnnotation();
            decl.init = null;
            if (eatP("=", true)) decl.init = parseAssignment();
            d.declarations.push(fin(decl));
            if (!eatP(",", true)) break;
        }
        semicolon();
        return fin(d);
    }

    function parseIf() {
        const s = mk("IfStatement", cur.start);
        advance(true);                              // past `if`
        expectP("(", true);
        s.test = parseExpression();
        expectP(")", true);                         // control-flow head -> a statement follows (regex ok)
        s.consequent = nested(parseStatement);
        s.alternate = null;
        if (isKw("else")) { advance(true); s.alternate = nested(parseStatement); }
        return fin(s);
    }

    function parseWhile() {
        const s = mk("WhileStatement", cur.start);
        advance(true); expectP("(", true);
        s.test = parseExpression();
        expectP(")", true);
        s.body = nested(parseStatement);
        return fin(s);
    }

    // A declined-but-valid construct emits js.unsupported ONCE and is CONSUMED cleanly (its full
    // grammar is parsed and discarded), so it never also emits js.syntax -- "unsupported" stays a
    // clean, first-class outcome distinct from "malformed".
    function parseDoWhile() {
        const start = cur.start;
        unsupported("do-while statement is not supported");
        advance(true);                              // past `do`
        nested(parseStatement);                     // the loop body
        if (isKw("while")) { advance(true); expectP("(", true); parseExpression(); expectP(")", true); }
        semicolon();
        return errNode(start);
    }

    function parseFor() {
        const s = mk("ForStatement", cur.start);
        advance(true);                              // past `for`
        let isAwait = false;
        if (isKw("await")) { isAwait = true; advance(true); }   // `for await (x of y)` (async iteration)
        expectP("(", true);
        // for-of / for-in / for-await-of / C-style for are all supported.
        let init = null;
        if (isP(";")) { /* empty init */ }
        else if ((isKw("var") || isKw("let") || isKw("const"))) {
            const kind = cur.value; const vd = mk("VariableDeclaration", cur.start); vd.kind = kind; vd.declarations = [];
            advance(true);
            const decl = mk("VariableDeclarator", cur.start); decl.id = parseBindingTarget(); decl.init = null;
            if (isKw("of")) { return finishForOf(s, fin2(vd, decl), isAwait); }
            if (isKw("in")) return finishForIn(s, fin2(vd, decl));
            if (eatP("=", true)) decl.init = parseAssignment();
            vd.declarations.push(fin(decl));
            while (eatP(",", true)) { const d2 = mk("VariableDeclarator", cur.start); d2.id = parseBindingTarget(); d2.init = null; if (eatP("=", true)) d2.init = parseAssignment(); vd.declarations.push(fin(d2)); }
            init = fin(vd);
        } else {
            init = parseExpression();
            if (isKw("of")) { return finishForOf(s, init, isAwait); }
            if (isKw("in")) return finishForIn(s, init);
            // The ES for-head init is Expression[~In] (NoIn) so a top-level `in` reads as the
            // for-in keyword, not the binary operator. parseExpression() has no NoIn mode, so a
            // `for (LHS in EXPR)` head arrives here as a top-level BinaryExpression(in). Recover
            // the equivalent for-in shape rather than falling through to the C-style `;` error.
            if (init && init.type === "BinaryExpression" && init.operator === "in") {
                s.type = "ForInStatement"; s.left = init.left; s.right = init.right;
                checkForHeadTarget(init.left);
                expectP(")", true); s.body = nested(parseStatement); return fin(s);
            }
        }
        s.init = init;
        expectP(";", true);
        s.test = isP(";") ? null : parseExpression();
        expectP(";", true);
        s.update = isP(")") ? null : parseExpression();
        expectP(")", true);
        s.body = nested(parseStatement);
        return fin(s);
    }
    function fin2(vd, decl) { vd.declarations = [fin(decl)]; return fin(vd); }
    function finishForOf(s, left, isAwait) {
        s.type = "ForOfStatement"; s.left = left; s.await = isAwait === true;
        checkForHeadTarget(left);
        advance(true);                              // past `of`
        s.right = parseAssignment();
        expectP(")", true);
        s.body = nested(parseStatement);
        return fin(s);
    }
    // A for-in/for-of head whose left is an EXPRESSION (not a `var`/`let`/`const` declaration)
    // must be a valid assignment target; e.g. `for (import.meta of x)` is invalid.
    function checkForHeadTarget(left) {
        if (left && left.type !== "VariableDeclaration" && !isAssignTarget(left)) synErr("invalid left-hand side in for-loop", left);
    }
    function finishForIn(s, left) {
        s.type = "ForInStatement"; s.left = left;
        checkForHeadTarget(left);
        advance(true);                              // past `in`
        s.right = parseExpression();
        expectP(")", true);
        s.body = nested(parseStatement);
        return fin(s);
    }

    function parseSwitch() {
        const s = mk("SwitchStatement", cur.start);
        advance(true); expectP("(", true);
        s.discriminant = parseExpression();
        expectP(")", false);
        expectP("{", true);
        s.cases = [];
        while (!isP("}") && !atEof()) {
            const c = mk("SwitchCase", cur.start); c.test = null; c.consequent = [];
            if (isKw("case")) { advance(true); if (ts) condConsequent++; c.test = parseExpression(); if (ts) condConsequent--; }
            else if (isKw("default")) { advance(true); }
            else { synErr("expected 'case' or 'default'"); advance(true); continue; }
            expectP(":", true);
            while (!isP("}") && !isKw("case") && !isKw("default") && !atEof()) {
                const before = cur.start; const st = nested(parseListStatement); if (st) c.consequent.push(st);
                if (cur.start === before && !atEof()) advance(true);
            }
            s.cases.push(fin(c));
        }
        expectP("}", true);
        return fin(s);
    }

    function parseTry() {
        const s = mk("TryStatement", cur.start);
        advance(true);
        s.block = isP("{") ? parseBlock() : (synErr("expected block"), errNode(cur.start));
        s.handler = null; s.finalizer = null;
        if (isKw("catch")) {
            const h = mk("CatchClause", cur.start); advance(true);
            if (isP("(")) { advance(true); h.param = parseBindingTarget(); expectP(")", true); }
            else { h.param = null; }              // optional catch binding: supported structurally
            h.body = isP("{") ? parseBlock() : (synErr("expected block"), errNode(cur.start));
            s.handler = fin(h);
        }
        if (isKw("finally")) { advance(true); s.finalizer = isP("{") ? parseBlock() : (synErr("expected block"), errNode(cur.start)); }
        if (!s.handler && !s.finalizer) synErr("missing catch or finally after try");
        return fin(s);
    }

    function parseReturnLike(type, allowEmpty) {
        const s = mk(type, cur.start);
        if (type === "ReturnStatement" && !inFunction()) synErr("return outside of a function");
        advance(true);
        s.argument = null;
        if (allowEmpty && (isP(";") || isP("}") || atEof() || nl())) { /* empty */ }
        else s.argument = parseExpression();
        semicolon();
        return fin(s);
    }

    function parseBreakContinue(type) {
        const s = mk(type, cur.start);
        advance(false);
        s.label = null;
        if (cur.type === "identifier" && !nl() && !RESERVED.has(cur.value)) { s.label = parseIdentifier(); }
        semicolon();
        return fin(s);
    }

    function parseExpressionStatement() {
        const start = cur.start;
        const expr = parseExpression();
        semicolon();
        const s = mk("ExpressionStatement", start); s.expression = expr; return fin(s);
    }

    // ASI-aware semicolon consumption.
    function semicolon() {
        if (eatP(";", true)) return;
        if (isP("}") || atEof() || nl()) { if (ts) asiEnd = lastJs; return; }   // automatic semicolon insertion
        synErr("expected ';'");
    }

    // -- functions / classes --
    function maybeAsyncFunctionDecl() {
        // `async function ...` declaration (no LineTerminator between `async` and `function`);
        // otherwise `async` is an ordinary identifier -> expression statement.
        const start = cur.start;
        if (asyncFunctionAhead()) {
            advance(true);                          // consume `async` -> cur is `function`
            return parseFunctionDeclaration(true, start);
        }
        return parseExpressionStatement();
    }
    // `async function` at cur, with no line break between them (else `async` is an identifier).
    function asyncFunctionAhead() {
        if (!isKw("async")) return false;
        const nx = peekTok(1, false);
        return nx.type === "identifier" && nx.value === "function" && !nx.nlBefore;
    }

    function parseFunctionDeclaration(isAsync, start) {
        const f = mk("FunctionDeclaration", start !== undefined ? start : cur.start);
        f.async = isAsync === true;
        advance(true);                              // past `function`
        f.generator = eatP("*", true);
        if (f.generator) { unsupported("generator functions are not supported", prev); }
        f.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        if (ts && isP("<")) tsRefuse("type parameters");
        const r = withFn(f.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(true); }, f.generator);
        f.params = r.params; f.body = r.body;       // declaration body `}` -> statement follows
        return fin(f);
    }

    function parseFunctionExpr(isAsync, start) {
        const f = mk("FunctionExpression", start !== undefined ? start : cur.start);
        f.async = isAsync === true;
        advance(true);
        f.generator = eatP("*", true);
        if (f.generator) unsupported("generator functions are not supported", prev);
        f.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        if (ts && isP("<")) tsRefuse("type parameters");
        const r = withFn(f.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); }, f.generator);
        f.params = r.params; f.body = r.body;       // expression body `}` -> a value (division)
        return fin(f);
    }

    function parseParams() {
        const params = [];
        if (!expectP("(", true)) return params;
        while (!isP(")") && !atEof()) {
            if (isP("...")) { const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget(); if (ts && isP(":")) tsAnnotation(); params.push(fin(r)); break; }
            if (ts && isKw("this") && (peekIsP(1, ":") || peekIsP(1, ",") || peekIsP(1, ")"))) tsRefuse("'this' parameters");
            else if (ts && tsModifierStart()) tsRefuse("parameter properties");
            let target = parseBindingTarget();
            if (ts && isP("?")) tsOptional();
            else if (ts && isP(":")) tsAnnotation();
            if (eatP("=", true)) { const ap = mk("AssignmentPattern", target.start); ap.left = target; ap.right = parseAssignment(); target = fin(ap); }
            params.push(target);
            if (!eatP(",", true)) break;
        }
        expectP(")", true);
        if (ts && isP(":")) tsReturnType();
        return params;
    }

    function parseFunctionBody(isDecl) {
        if (!isP("{")) {
            if (ts && (isP(";") || nl() || isP("}") || atEof())) tsRefuse("overload signatures");
            synErr("expected function body"); return errNode(cur.start);
        }
        const b = mk("BlockStatement", cur.start);
        b.body = [];
        advance(true);
        while (!isP("}") && !atEof()) {
            const before = cur.start; const s = parseListStatement(); if (s) b.body.push(s);
            if (cur.start === before && !atEof() && !isP("}")) advance(true);
            errored = false;
        }
        expectP("}", isDecl === true);              // decl -> statement (regex); expr -> value (division)
        return fin(b);
    }

    function parseClassDeclaration() { return parseClass("ClassDeclaration"); }
    function parseClassExpr() { return parseClass("ClassExpression"); }
    function parseClass(type) {
        const c = mk(type, cur.start);
        advance(true);                              // past `class`
        c.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        if (ts && isP("<")) tsRefuse("type parameters");
        c.superClass = null;
        // ClassHeritage : extends LeftHandSideExpression -- which INCLUDES call expressions
        // (`class C extends fn(x) {}` is valid), so allow calls. (Passing false left the call
        // arguments unconsumed, which drove non-terminating error recovery -> heap blowup.)
        if (isKw("extends")) { advance(true); c.superClass = parseLeftHandSide(true); }
        if (ts && isKw("implements")) tsRefuse("implements clauses");
        c.body = [];
        expectP("{", true);
        while (!isP("}") && !atEof()) {
            if (eatP(";", true)) continue;
            const before = cur.start;
            const m = parseClassMember();
            if (m) c.body.push(m);
            // A member that consumed nothing (e.g. `async . x`: the key parse
            // fails on `.` without advancing) must still move the loop on:
            // retried at the same token it pushed an error node per pass
            // until the session heap ran out.
            if (cur.start === before && !isP("}") && !atEof()) advance(true);
            else if (!m && !isP("}")) advance(true);
        }
        expectP("}", type === "ClassDeclaration");   // class DECLARATION -> statement; expr -> value
        return fin(c);
    }

    function parseClassMember() {
        const start = cur.start;
        if (cur.type === "punctuator" && cur.value === "#") {
            unsupported("private class members are not supported");
            advance(true);                          // past `#`
            if (cur.type === "identifier") advance(true);   // the private name
            if (isP("(")) { withFn("regular", parseParams, function () { return parseFunctionBody(false); }); }   // a private method (consumed under a function frame so its body parses)
            else { if (eatP("=", true)) parseAssignment(); semicolon(); } // a private field
            return errNode(start);
        }
        if (ts && tsModifierStart()) tsRefuse("'" + cur.value + "' modifiers");
        // Non-destructive one-token lookahead decides whether a keyword-like token is a
        // modifier or the member NAME (e.g. a field named `static` / `async` / `get`, which may
        // end the class body: `class C { get }`).
        let isStatic = false;
        if (isKw("static")) {
            const nx = peekTok(1, false);
            if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";" || nx.value === "}"))) { isStatic = true; advance(true); }
        }
        let kind = "method", isAsync = false, isGen = false;
        if (isKw("async")) { const nx = peekTok(1, false); if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";" || nx.value === "}")) && !nx.nlBefore) { isAsync = true; advance(true); } }
        if (isP("*")) { isGen = true; unsupported("generator methods are not supported"); advance(true); }
        if (isKw("get") || isKw("set")) { const nx = peekTok(1, false); if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";" || nx.value === "}"))) { kind = cur.value; advance(true); } }
        const key = parsePropertyKey();
        if (ts && isP("?")) tsOptional();
        else if (ts && isP("!") && !nl()) tsDefinite();
        else if (ts && isP(":")) tsAnnotation();
        else if (ts && isP("<")) tsRefuse("type parameters");
        const m = mk("MethodDefinition", start); m.static = isStatic; m.kind = kind; m.key = key; m.async = isAsync; m.generator = isGen;
        if (isP("(")) {
            const fe = mk("FunctionExpression", cur.start); fe.async = isAsync; fe.generator = isGen; fe.id = null;
            const fr = withFn(fe.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); }, fe.generator); fe.params = fr.params; fe.body = fr.body;
            m.value = fin(fe);
            return fin(m);
        }
        // class field
        m.type = "PropertyDefinition"; m.value = null;
        if (eatP("=", true)) m.value = parseAssignment();
        semicolon();
        return fin(m);
    }

    function parsePropertyKey() {
        if (isP("[")) { advance(true); const k = parseAssignment(); expectP("]", false); return k; }
        if (cur.type === "string" || cur.type === "number") return parseLiteral();
        return parseIdentifierName();
    }

    // -- binding targets / patterns --
    function parseBindingTarget() {
        if (isP("[")) return parseArrayPattern();
        if (isP("{")) return parseObjectPattern();
        return parseBindingIdentifier();
    }
    function parseBindingIdentifier() {
        if (cur.type !== "identifier" || RESERVED.has(cur.value)) { synErr("expected a binding name"); return errNode(cur.start); }
        // `eval` / `arguments` are legal references but illegal as a BINDING target in strict code.
        if (BINDING_RESTRICTED.has(cur.value)) { synErr("'" + cur.value + "' may not be a binding name in module code"); advance(false); return errNode(cur.start); }
        return parseIdentifier();       // also rejects the STRICT_RESERVED words
    }
    function parseArrayPattern() {
        const n = mk("ArrayPattern", cur.start); n.elements = [];
        advance(true);
        while (!isP("]") && !atEof()) {
            if (isP(",")) { n.elements.push(null); advance(true); continue; }
            if (isP("...")) { const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget(); n.elements.push(fin(r)); break; }
            let el = parseBindingTarget();
            if (eatP("=", true)) { const ap = mk("AssignmentPattern", el.start); ap.left = el; ap.right = parseAssignment(); el = fin(ap); }
            n.elements.push(el);
            if (!eatP(",", true)) break;
        }
        expectP("]", false);
        return fin(n);
    }
    function parseObjectPattern() {
        const n = mk("ObjectPattern", cur.start); n.properties = [];
        advance(true);
        while (!isP("}") && !atEof()) {
            if (isP("...")) { const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget(); n.properties.push(fin(r)); break; }
            const pr = mk("Property", cur.start); pr.computed = isP("[");
            pr.key = parsePropertyKey();
            if (eatP(":", true)) { pr.value = parseBindingTarget(); pr.shorthand = false; }
            else { pr.value = pr.key; pr.shorthand = true; }
            if (eatP("=", true)) { const ap = mk("AssignmentPattern", pr.value.start); ap.left = pr.value; ap.right = parseAssignment(); pr.value = fin(ap); }
            n.properties.push(fin(pr));
            if (!eatP(",", true)) break;
        }
        expectP("}", false);
        return fin(n);
    }

    // -- expressions --
    function parseExpression() {
        let e = parseAssignment();
        if (isP(",")) {
            const seq = mk("SequenceExpression", e.start); seq.expressions = [e];
            while (eatP(",", true)) seq.expressions.push(parseAssignment());
            return fin(seq);
        }
        return e;
    }

    // Assignment-target (lvalue) validation. A SIMPLE target is an Identifier or a
    // MemberExpression; `=` additionally accepts a destructuring pattern (an array/object literal
    // reinterpreted, recursively), while a COMPOUND assignment (`+=` etc.) requires a simple
    // target. This rejects calls, literals, `import.meta`, `this`, parenthesized binary
    // expressions, etc. as targets (the AssignmentTargetType early error), including nested
    // (`[import.meta] = []`). Mirrors the Lua parser's lvalue check.
    function isSimpleTarget(n) { return !!n && (n.type === "Identifier" || n.type === "MemberExpression"); }
    function isAssignTarget(n) {
        if (!n) return false;
        if (isSimpleTarget(n)) return true;
        if (n.type === "ArrayExpression") {
            for (let i = 0; i < n.elements.length; i++) {
                const el = n.elements[i];
                if (el === null) continue;                                   // elision hole
                if (el.type === "SpreadElement") { if (!isAssignTarget(el.argument)) return false; continue; }
                if (el.type === "AssignmentExpression" && el.operator === "=") { if (!isAssignTarget(el.left)) return false; continue; }
                if (!isAssignTarget(el)) return false;
            }
            return true;
        }
        if (n.type === "ObjectExpression") {
            for (let i = 0; i < n.properties.length; i++) {
                const pr = n.properties[i];
                if (pr.type === "SpreadElement") { if (!isSimpleTarget(pr.argument)) return false; continue; }
                if (pr.method || pr.kind === "get" || pr.kind === "set") return false;   // not a target
                let v = pr.value;
                if (v && v.type === "AssignmentPattern") v = v.left;         // { a = default }
                if (!isAssignTarget(v)) return false;
            }
            return true;
        }
        return false;
    }

    function parseAssignment() {
        if (!guard()) { unguard(); return errNode(cur.start); }
        // arrow-function detection is limited without arbitrary lookahead; handled in primary
        // for `(params) =>` and `ident =>`.
        const left = parseConditional();
        if (cur.type === "punctuator" && ASSIGN.has(cur.value)) {
            const op = cur.value;
            // Reject an invalid assignment target (still build the node + consume the RHS so
            // recovery continues; the js.syntax makes the unit invalid).
            if (op === "=" ? !isAssignTarget(left) : !isSimpleTarget(left)) synErr("invalid assignment target", left);
            const node = mk("AssignmentExpression", left.start);
            node.operator = op; node.left = left; advance(true);
            node.right = parseAssignment();
            unguard();
            return fin(node);
        }
        unguard();
        return left;
    }

    function parseConditional() {
        const test = parseBinary(0);
        if (isP("?")) {
            const node = mk("ConditionalExpression", test.start);
            node.test = test; advance(true);
            if (ts) condConsequent++;
            node.consequent = parseAssignment();
            if (ts) condConsequent--;
            expectP(":", true);
            node.alternate = parseAssignment();
            return fin(node);
        }
        return test;
    }

    function parseBinary(minPrec) {
        let left = parseUnary();
        for (;;) {
            const op = (cur.type === "punctuator" || cur.type === "identifier") ? cur.value : null;
            const prec = op !== null ? BINPREC.get(op) : undefined;
            // `x as T` / `x satisfies T`: relational precedence, left-associative, no line break
            // before the keyword (as TypeScript). It builds no node: the left operand stands.
            if (ts && prec === undefined && isIdTok(cur) && (op === "as" || op === "satisfies") && !nl()) {
                if (8 <= minPrec) break;
                const asConst = op === "as";
                tsSpan(function () { advance(false); types.parseType({ asConst: asConst }); });
                // The type ends the expression, so TypeScript ends the statement at a line break
                // before `(` `[` `.` `?.` or a template; once erased, JavaScript would continue it
                // (`x as T` / `[1].f()` reads `x[1].f()`; docs/typescript_design.md 3.4).
                if (nl() && (isP("(") || isP("[") || isP(".") || isP("?.") ||
                             (cur.type === "template" && (cur.mode === "noSubstitution" || cur.mode === "head"))))
                    diag("error", "ts.unsupported", "a line that would continue the expression after the type of '" + op + "' is not supported: end the statement with ';'", cur.start, cur.stop);
                continue;
            }
            if (prec === undefined || prec <= minPrec) break;
            if ((op === "in" || op === "instanceof") && cur.type !== "identifier") break;
            const node = mk(op === "&&" || op === "||" || op === "??" ? "LogicalExpression" : "BinaryExpression", left.start);
            node.operator = op; node.left = left; advance(true);
            const nextMin = (op === "**") ? prec - 1 : prec;   // ** is right-associative
            node.right = parseBinary(nextMin);
            left = fin(node);
        }
        return left;
    }

    function parseUnary() {
        if (cur.type === "punctuator" || cur.type === "identifier") {
            const v = cur.value;
            if (UNARY.has(v)) { const node = mk("UnaryExpression", cur.start); node.operator = v; node.prefix = true; advance(true); node.argument = parseUnary(); return fin(node); }
            if (v === "++" || v === "--") { const node = mk("UpdateExpression", cur.start); node.operator = v; node.prefix = true; advance(true); node.argument = parseUnary(); if (!isSimpleTarget(node.argument)) synErr("invalid target for update expression", node.argument); return fin(node); }
            if (v === "await" && !cur.escaped) {   // an escaped spelling is not the keyword -> falls through to a (reserved) identifier
                const astart = cur.start;
                if (awaitIsExpr()) {
                    // AwaitExpression : await UnaryExpression -- an operand is REQUIRED.
                    if (isExprStartTok(peekTok(1, true))) { advance(true); const node = mk("AwaitExpression", astart); node.argument = parseUnary(); return fin(node); }
                    synErr("await requires an operand");
                    advance(true); return fin(mk("AwaitExpression", astart));
                }
                // Not an async body / module top level (a regular function, or ANY parameter
                // region incl. async-function params): `await` is reserved in module code here.
                synErr("await is only valid at module top level or in an async function body");
                advance(true);
                const node = mk("AwaitExpression", astart);
                node.argument = isExprStartTok(cur) ? parseUnary() : null;
                return fin(node);
            }
            if (v === "yield" && !cur.escaped) {
                const ys = cur.start;
                if (inGenerator()) {
                    // Inside a generator Hull already declined at `function*`; consume the yield
                    // operator quietly (a clean YieldExpression, no second diagnostic).
                    advance(true);                  // past `yield`
                    const node = mk("YieldExpression", ys); node.delegate = eatP("*", true);
                    node.argument = (!nl() && isExprStartTok(cur)) ? parseAssignment() : null;
                    return fin(node);
                }
                // Outside a generator, `yield` is a reserved word in module code -> a grammar
                // error (js.syntax), agreeing with QuickJS. It falls through to the reserved
                // identifier path below.
            }
        }
        let e = parsePostfix();
        return e;
    }

    function parsePostfix() {
        let e = parseLeftHandSide(true);
        if ((isP("++") || isP("--")) && !nl()) {
            // Postfix ++/-- requires a simple assignment target (an Identifier or member access);
            // e.g. `import.meta++` (a MetaProperty) is an invalid update target.
            if (!isSimpleTarget(e)) synErr("invalid target for update expression", e);
            const node = mk("UpdateExpression", e.start); node.operator = cur.value; node.prefix = false; node.argument = e; advance(false); return fin(node);
        }
        return e;
    }

    function parseLeftHandSide(allowCall) {
        let e;
        if (isKw("new")) e = parseNew();
        else e = parsePrimary();
        return parseCallMemberTail(e, allowCall);
    }

    function parseNew() {
        const start = cur.start; advance(true);
        if (isP(".")) { advance(false); const meta = mk("MetaProperty", start); meta.meta = "new"; if (cur.type === "identifier" && cur.value === "target" && !cur.escaped) { meta.property = "target"; advance(false); if (!newTargetAllowed()) synErr("'new.target' is only valid inside a function", meta); return parseCallMemberTail(fin(meta), true); } synErr("the only valid meta-property for 'new' is 'new.target'"); return errNode(start); }
        let callee = isKw("new") ? parseNew() : parsePrimary(true);
        callee = parseCallMemberTail(callee, false);   // member tail but no call
        const node = mk("NewExpression", start); node.callee = callee; node.arguments = [];
        if (isP("(")) node.arguments = parseArguments();
        return parseCallMemberTail(fin(node), true);
    }

    function parseCallMemberTail(e, allowCall) {
        for (;;) {
            // TS mode: a non-null assertion `x!` (no line break before it) builds no node.
            if (ts && isP("!") && !nl()) { tsSpan(function () { advance(false); }); continue; }
            if (isP(".")) {
                advance(false);
                if (isP("#")) {   // a private member access (obj.#name) -- declined, consumed cleanly
                    unsupported("private member access is not supported");
                    advance(false); if (cur.type === "identifier") advance(false);
                    e = errNode(e.start); continue;
                }
                const m = mk("MemberExpression", e.start); m.object = e; m.computed = false; m.optional = false; m.property = parseIdentifierName(); e = fin(m); continue;
            }
            if (isP("?.")) {
                advance(false);
                if (isP("(") && allowCall) { const c = mk("CallExpression", e.start); c.callee = e; c.optional = true; c.arguments = parseArguments(); e = fin(c); continue; }
                if (isP("[")) { advance(true); const m = mk("MemberExpression", e.start); m.object = e; m.computed = true; m.optional = true; m.property = parseExpression(); expectP("]", false); e = fin(m); continue; }
                const m = mk("MemberExpression", e.start); m.object = e; m.computed = false; m.optional = true; m.property = parseIdentifierName(); e = fin(m); continue;
            }
            if (isP("[")) { advance(true); const m = mk("MemberExpression", e.start); m.object = e; m.computed = true; m.optional = false; m.property = parseExpression(); expectP("]", false); e = fin(m); continue; }
            if (isP("(") && allowCall) { const c = mk("CallExpression", e.start); c.callee = e; c.optional = false; c.arguments = parseArguments(); e = fin(c); continue; }
            // A tagged template only begins on a NEW template (noSubstitution/head); a
            // middle/tail token is the continuation of an enclosing template, not a tag. A line
            // break before it does not end the expression (`tag` LF `` `s` `` is one tagged
            // template: no ASI rule stops it).
            if (cur.type === "template" && (cur.mode === "noSubstitution" || cur.mode === "head")) { const t = mk("TaggedTemplateExpression", e.start); t.tag = e; t.quasi = parseTemplate(); e = fin(t); continue; }
            break;
        }
        return e;
    }

    // `info` (optional) learns whether a spread was followed by a comma, which an arrow's
    // parameter list (the `async (` cover grammar) refuses: a rest parameter is last, with no
    // trailing comma.
    function parseArguments(info) {
        const args = [];
        advance(true);                              // past `(`
        while (!isP(")") && !atEof()) {
            const spread = isP("...");
            if (spread) {
                const sp = mk("SpreadElement", cur.start); advance(true); sp.argument = parseAssignment();
                if (ts && info && isP(":")) { tsAnnotation(); info.typed = true; }
                args.push(fin(sp));
            }
            else args.push(ts && info ? tsArrowItem(info) : parseAssignment());
            if (!eatP(",", true)) break;
            if (spread && info) info.spreadComma = true;
        }
        expectP(")", false);                        // a call `)` -> a value (division)
        return args;
    }

    // Reinterpret a parsed `async (...)` argument list as arrow parameters - the cover grammar
    // (CoverCallExpressionAndAsyncArrowHead). Builds the nodes parseParams builds (Identifier,
    // AssignmentPattern, ArrayPattern / ObjectPattern, RestElement) and refuses what is no
    // binding (a member access, a call, a literal, `eval`, a rest that is not last).
    function argsToParams(args, info) {
        const params = [];
        for (let i = 0; i < args.length; i++) {
            const a = args[i];
            if (a.type === "SpreadElement") {
                if (info.spreadComma) synErr("a rest parameter must be last, with no trailing comma", a);
                params.push(toRest(a, false));
            } else params.push(toBinding(a, true));
        }
        return params;
    }
    function toRest(sp, idOnly) {
        const r = { type: "RestElement", start: sp.start, stop: sp.stop };
        r.argument = toBinding(sp.argument, false);
        if (idOnly && r.argument.type !== "Identifier" && r.argument.type !== "Error") synErr("an object rest element must be a name", sp.argument);
        return r;
    }
    // `withDefault`: the node may be `target = default`.
    function toBinding(n, withDefault) {
        switch (n.type) {
            case "Error": return n;                 // already diagnosed
            case "Identifier":
                if (BINDING_RESTRICTED.has(n.name)) synErr("'" + n.name + "' may not be a binding name in module code", n);
                return n;
            case "AssignmentExpression":
                if (n.operator === "=" && withDefault) return { type: "AssignmentPattern", start: n.start, stop: n.stop, left: toBinding(n.left, false), right: n.right };
                break;
            case "AssignmentPattern":               // `{ a = 1 }` shorthand, already a pattern
                if (withDefault) return { type: "AssignmentPattern", start: n.start, stop: n.stop, left: toBinding(n.left, false), right: n.right };
                break;
            case "ArrayExpression": {
                const ap = { type: "ArrayPattern", start: n.start, stop: n.stop, elements: [] };
                for (let i = 0; i < n.elements.length; i++) {
                    const el = n.elements[i];
                    if (el === null) { ap.elements.push(null); continue; }
                    if (el.type === "SpreadElement") {
                        if (i !== n.elements.length - 1) synErr("a rest element must be last", el);
                        ap.elements.push(toRest(el, false));
                    } else ap.elements.push(toBinding(el, true));
                }
                return ap;
            }
            case "ObjectExpression": {
                const op = { type: "ObjectPattern", start: n.start, stop: n.stop, properties: [] };
                for (let i = 0; i < n.properties.length; i++) {
                    const pr = n.properties[i];
                    if (pr.type === "SpreadElement") {
                        if (i !== n.properties.length - 1) synErr("a rest element must be last", pr);
                        op.properties.push(toRest(pr, true));
                        continue;
                    }
                    if (pr.method || pr.kind === "get" || pr.kind === "set") { synErr("invalid destructuring target", pr); continue; }
                    op.properties.push({ type: "Property", start: pr.start, stop: pr.stop, computed: pr.computed, key: pr.key,
                        value: toBinding(pr.value, true), shorthand: pr.shorthand });
                }
                return op;
            }
        }
        synErr("invalid arrow function parameter", n);
        return n;
    }

    function isExprStartTok(t) {
        if (!t || t.type === "eof") return false;
        if (t.type === "number" || t.type === "string" || t.type === "template" || t.type === "regex") return true;
        if (t.type === "identifier") return true;
        if (t.type === "punctuator") return t.value === "(" || t.value === "[" || t.value === "{" || t.value === "!" || t.value === "~" || t.value === "+" || t.value === "-" || t.value === "++" || t.value === "--" || t.value === "...";
        return false;
    }
    // A punctuator that means "the identifier just seen is a property key/name, not a modifier".
    function isKeyBoundary(t) { return t.type === "punctuator" && (t.value === ":" || t.value === "," || t.value === "}" || t.value === "(" || t.value === "="); }

    // `noCall`: the primary is a `new` callee, whose `(...)` is the new's own argument list.
    function parsePrimary(noCall) {
        const start = cur.start;
        switch (cur.type) {
            case "number": case "string": return parseLiteral();
            case "regex": { const n = mk("Literal", start); n.regex = { pattern: cur.pattern, flags: cur.flags }; n.raw = cur.raw; advance(false); return fin(n); }
            case "template": return parseTemplate();
            case "identifier": return parseIdentifierExpr(noCall === true);
            case "punctuator":
                if (cur.value === "(") return parseParenOrArrow();
                if (cur.value === "[") return parseArrayExpr();
                if (cur.value === "{") return parseObjectExpr();
                // `<` cannot start a JS expression: a generic arrow `<T>(x) => x` or an
                // angle-bracket assertion `<T>x`.
                if (ts && cur.value === "<") tsRefuse("type parameters and angle-bracket assertions");
                break;
        }
        synErr("unexpected token");
        const e = errNode(start);
        if (!errored) { errored = true; } else { advance(true); }   // avoid infinite loop on junk
        return e;
    }

    function parseIdentifierExpr(noCall) {
        const v = cur.value, start = cur.start;
        if (v === "function") return parseFunctionExpr(false);
        if (v === "class") return parseClassExpr();
        if (v === "this") { advance(false); return fin2n("ThisExpression", start); }
        if (v === "super") { advance(false); return fin2n("Super", start); }
        if (v === "true" || v === "false") { const n = mk("Literal", start); n.value = (v === "true"); n.raw = v; advance(false); return fin(n); }
        if (v === "null") { const n = mk("Literal", start); n.value = null; n.raw = "null"; advance(false); return fin(n); }
        if (v === "new") return parseNew();
        if (v === "import") { if (cur.escaped) { synErr("'import' is a reserved word and may not be escaped"); advance(false); return errNode(start); } advance(false); if (isP("(")) { const ie = mk("ImportExpression", start); ie.arguments = parseArguments(); return fin(ie); } if (isP(".")) { advance(false); const meta = mk("MetaProperty", start); meta.meta = "import"; if (cur.type === "identifier" && cur.value === "meta" && !cur.escaped) { meta.property = "meta"; advance(false); return fin(meta); } synErr("the only valid meta-property for 'import' is 'import.meta'"); return errNode(start); } synErr("unexpected 'import'"); return errNode(start); }
        if (v === "async") {
            const nx = peekTok(1, false);
            if (nx.type === "identifier" && nx.value === "function" && !nx.nlBefore) { advance(true); return parseFunctionExpr(true, start); }
            if (!nx.nlBefore) {
                // `async ident =>`
                if (nx.type === "identifier" && !RESERVED.has(nx.value) && peekIsP(2, "=>")) {
                    advance(true);                  // consume `async` -> cur is the param ident
                    const id = parseIdentifier();
                    return finishArrow(start, [id], true);
                }
                // `async ( ... )` -- an async arrow's parameters or a call's arguments. The list
                // is parsed ONCE, as arguments (each slash lexed under its real grammatical goal:
                // a default may hold a regex OR a division), and reinterpreted as parameters when
                // `=>` follows: the cover grammar, as a parenthesized expression's. (A params
                // parse that was rewound and re-parsed as a call when no `=>` followed did every
                // nested `async (` twice per enclosing one - exponential: ~330 bytes of nested
                // `async(a=` never finished parsing.) A `new` callee's `(...)` is the new's.
                if (nx.type === "punctuator" && nx.value === "(" && !noCall) {
                    const callee = parseIdentifier();   // `async` -> cur is `(`
                    const info = { spreadComma: false, typed: false };
                    const args = parseArguments(info);
                    if (ts) tsArrowTail(info);
                    if (isP("=>") && !nl()) return finishArrow(start, argsToParams(args, info), true);
                    if (ts && info.typed && !isP("=>")) synErr("expected '=>' after TypeScript parameter types");
                    const call = mk("CallExpression", start); call.callee = callee; call.optional = false; call.arguments = args;
                    return fin(call);
                }
            }
            // otherwise `async` is an ordinary identifier -> fall through
        }
        // `ident =>` arrow
        const id = parseIdentifier();
        if (isP("=>") && !nl()) return finishArrow(id.start, [id], false);
        return id;
    }
    function fin2n(type, start) { const e = ts ? lastJs : prev; const n = mk(type, start); n.stop = e.stop < start ? start : e.stop; return n; }

    function parseParenOrArrow() {
        // Parse a parenthesized expression; if followed by `=>`, reinterpret as arrow params.
        const start = cur.start;
        advance(true);
        if (isP(")")) {
            advance(false);
            if (ts) tsArrowTail({ typed: true });   // `()` is only ever an arrow's parameters
            if (isP("=>")) return finishArrow(start, [], false);
            synErr("unexpected ')'"); return errNode(start);
        }
        const items = [];
        let sawRest = false;
        let trailing = null;                        // a `,` right before `)`: legal for an arrow only
        const info = { typed: false };              // TS mode: the list holds parameter types
        for (;;) {
            if (isP("...")) {
                const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget();
                if (ts && isP(":")) { tsAnnotation(); info.typed = true; }
                items.push(fin(r)); sawRest = true; break;
            }
            items.push(ts ? tsArrowItem(info) : parseAssignment());
            if (!eatP(",", true)) break;
            if (isP(")")) { trailing = prev; break; }
        }
        expectP(")", false);
        if (ts) tsArrowTail(info);
        if (isP("=>") && !nl()) return finishArrow(start, items, false);
        if (trailing) synErr("unexpected trailing ','", trailing);
        if (ts && info.typed && !isP("=>")) synErr("expected '=>' after TypeScript parameter types");
        if (sawRest) { synErr("rest element outside arrow parameters"); }
        if (items.length === 1) return items[0];
        const seq = mk("SequenceExpression", start); seq.expressions = items; return fin(seq);
    }

    function finishArrow(start, params, isAsync) {
        const a = mk("ArrowFunctionExpression", start); a.async = isAsync === true; a.params = params;
        expectP("=>", true);
        // Arrow params were already parsed (as a parenthesized expression) under the enclosing
        // context; the body gets its own frame so `await` in an async arrow body is valid.
        ctxStack.push({ kind: a.async ? "async" : "regular", region: "body", arrow: true });
        a.body = nested(parseArrowBody);
        ctxStack.pop();
        return fin(a);
    }
    function parseArrowBody() {
        if (isP("{")) return parseFunctionBody(false);   // block body is a value on close -> division
        return parseAssignment();                        // concise body
    }

    function parseArrayExpr() {
        const n = mk("ArrayExpression", cur.start); n.elements = [];
        advance(true);
        while (!isP("]") && !atEof()) {
            if (isP(",")) { n.elements.push(null); advance(true); continue; }
            if (isP("...")) { const sp = mk("SpreadElement", cur.start); advance(true); sp.argument = parseAssignment(); n.elements.push(fin(sp)); }
            else n.elements.push(parseAssignment());
            if (!eatP(",", true)) break;
        }
        expectP("]", false);
        return fin(n);
    }

    function parseObjectExpr() {
        const n = mk("ObjectExpression", cur.start); n.properties = [];
        advance(true);
        while (!isP("}") && !atEof()) {
            if (isP("...")) { const sp = mk("SpreadElement", cur.start); advance(true); sp.argument = parseAssignment(); n.properties.push(fin(sp)); if (!eatP(",", true)) break; continue; }
            n.properties.push(parseObjectMember());
            if (!eatP(",", true)) break;
        }
        expectP("}", false);                        // an object `}` -> a value (division)
        return fin(n);
    }

    function parseObjectMember() {
        const start = cur.start;
        let isAsync = false, isGen = false, kind = "init";
        if (isKw("async")) { const nx = peekTok(1, false); if (!isKeyBoundary(nx) && !nx.nlBefore) { isAsync = true; advance(true); } }
        if (isP("*")) { isGen = true; unsupported("generator methods are not supported"); advance(true); }
        if (isKw("get") || isKw("set")) { const nx = peekTok(1, false); if (!isKeyBoundary(nx)) { kind = cur.value; advance(true); } }
        const computed = isP("[");
        const key = parsePropertyKey();
        const pr = mk("Property", start); pr.computed = computed; pr.key = key; pr.kind = kind === "init" ? "init" : kind;
        if (isP("(")) {   // method
            const fe = mk("FunctionExpression", cur.start); fe.async = isAsync; fe.generator = isGen; fe.id = null;
            const fr = withFn(fe.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); }, fe.generator); fe.params = fr.params; fe.body = fr.body;
            pr.value = fin(fe); pr.method = true; pr.shorthand = false;
            return fin(pr);
        }
        if (eatP(":", true)) { pr.value = parseAssignment(); pr.shorthand = false; pr.method = false; return fin(pr); }
        // shorthand { x } or { x = default } (pattern context)
        pr.value = key; pr.shorthand = true; pr.method = false;
        if (eatP("=", true)) { const ap = mk("AssignmentPattern", key.start); ap.left = key; ap.right = parseAssignment(); pr.value = fin(ap); }
        return fin(pr);
    }

    function parseTemplate() {
        const t = mk("TemplateLiteral", cur.start); t.quasis = []; t.expressions = [];
        if (cur.mode === "noSubstitution") { t.quasis.push(quasi(cur)); advance(false); return fin(t); }
        // head ... expr (middle ... expr)* tail
        t.quasis.push(quasi(cur)); advance(true);
        for (;;) {
            t.expressions.push(parseExpression());
            if (cur.type !== "template") { synErr("unterminated template"); break; }
            if (cur.mode === "tail") { t.quasis.push(quasi(cur)); advance(false); break; }
            if (cur.mode === "middle") { t.quasis.push(quasi(cur)); advance(true); continue; }
            synErr("malformed template"); break;
        }
        return fin(t);
    }
    function quasi(tok) { return { type: "TemplateElement", value: tok.value, start: tok.start, stop: tok.stop }; }

    function parseLiteral() {
        const n = mk("Literal", cur.start);
        if (cur.type === "number") { n.value = cur.value; n.bigint = cur.bigint === true; }
        else n.value = cur.value;
        n.raw = cur.raw !== undefined ? cur.raw : cur.value;
        advance(false);
        return fin(n);
    }
    function parseIdentifier() {
        // Strict/module reserved words (STRICT_RESERVED, incl. await/yield) may not be a
        // BindingIdentifier, IdentifierReference, or LabelIdentifier. An AwaitExpression /
        // YieldExpression is intercepted earlier in parseUnary, so a reserved word reaching here is
        // always a reserved-word misuse. An escaped spelling is equally reserved (same word).
        if (STRICT_RESERVED.has(cur.value)) { synErr("'" + cur.value + "' is reserved in module code"); advance(false); return errNode(cur.start); }
        const n = mk("Identifier", cur.start); n.name = cur.value; n.escaped = cur.escaped === true; advance(false); return fin(n);
    }
    // an identifier name where keywords are allowed (property names, member access).
    function parseIdentifierName() {
        if (cur.type !== "identifier") { synErr("expected a name"); return errNode(cur.start); }
        const n = mk("Identifier", cur.start); n.name = cur.value; advance(false); return fin(n);
    }

    // -- modules --
    function parseImport() {
        const s = mk("ImportDeclaration", cur.start);
        // A static ImportDeclaration is a module ITEM: legal only at the module top level, never
        // inside a block, function, or other nested statement (dynamic import() reaches
        // parseExpressionStatement, not here, so it stays legal everywhere).
        if (!atModuleItem) synErr("import declarations may only appear at the top level of a module");
        advance(true);
        s.specifiers = [];
        if (cur.type === "string") { s.source = parseLiteral(); semicolon(); return fin(s); }   // side-effect import
        // TS mode, `import type ...` being erased (tsTypeOnlyImportAhead chose it): the rest reads
        // as an ordinary import clause.
        if (ts && typeDepth > 0 && isKw("type")) advance(false);
        // ImportClause: a default binding, then - only after a comma - a namespace import or
        // named imports. `import a { b }` / `import a * as n` (no comma) and `import a, from`
        // (a comma with nothing after it) are syntax errors.
        let needMore = true;
        if (cur.type === "identifier" && !RESERVED.has(cur.value)) {
            const d = mk("ImportDefaultSpecifier", cur.start); d.local = parseIdentifier(); s.specifiers.push(fin(d));
            if (ts && isP("=")) tsRefuse("import-require declarations");
            needMore = eatP(",", true);
            // Missing comma before `{` / `*`: report it, then read them as if it were there.
            if (!needMore && (isP("{") || isP("*"))) { synErr("expected ','"); needMore = true; }
        }
        if (!needMore) { /* a default binding alone */ }
        else if (isP("*")) {
            const ns = mk("ImportNamespaceSpecifier", cur.start); advance(true);
            if (isKw("as")) advance(true); else synErr("expected 'as'");
            ns.local = parseBindingIdentifier(); s.specifiers.push(fin(ns));
        }
        else if (s.specifiers.length > 0 && !isP("{")) synErr("expected '*' or '{'");
        else if (isP("{")) {
            advance(true);
            while (!isP("}") && !atEof()) {
                if (ts && tsTypeSpecifierAhead()) { if (!tsEraseSpecifier()) break; continue; }
                const spec = mk("ImportSpecifier", cur.start); spec.imported = parseIdentifierName(); spec.local = spec.imported;
                if (isKw("as")) { advance(true); spec.local = parseBindingIdentifier(); }
                // Without `as`, the imported name IS the local binding, so it must be a valid
                // binding identifier (`import { eval }` / `import { public }` are invalid).
                else if (spec.imported && spec.imported.name !== undefined &&
                         (RESERVED.has(spec.imported.name) || STRICT_RESERVED.has(spec.imported.name) || BINDING_RESTRICTED.has(spec.imported.name)))
                    synErr("'" + spec.imported.name + "' may not be a binding name in module code", spec.imported);
                s.specifiers.push(fin(spec));
                if (!eatP(",", true)) break;
            }
            expectP("}", false);
        }
        if (isKw("from")) { advance(true); s.source = (cur.type === "string") ? parseLiteral() : (synErr("expected module specifier"), errNode(cur.start)); }
        else synErr("expected 'from'");
        semicolon();
        return fin(s);
    }

    function parseExport() {
        const s = mk("ExportNamedDeclaration", cur.start);
        // Every export form is a module ITEM: legal only at the module top level.
        if (!atModuleItem) synErr("export declarations may only appear at the top level of a module");
        if (ts) {
            // `export type { A }` / `export type * from "m"`, and `export` + an erasable
            // declaration: the whole statement goes, `export` included.
            const t1 = peekTok(1, true);
            if (isIdTok(t1) && t1.value === "type" && !peekTok(2, false).nlBefore && (peekIsP(2, "{") || peekIsP(2, "*")))
                return tsEraseStatement(function () { advance(false); advance(false); tsTypeOnlyExportRest(); });
            const kind = tsErasableKind(t1, function (i) { return peekTok(1 + i, false); });
            if (kind !== null) return tsEraseStatement(function () { advance(false); tsDeclarationBody(kind); });
        }
        advance(true);
        if (ts && isP("=")) tsRefuse("export assignments");
        if (isKw("default")) {
            s.type = "ExportDefaultDeclaration"; advance(true);
            if (isKw("function")) s.declaration = parseFunctionDeclaration(false);
            else if (asyncFunctionAhead()) s.declaration = maybeAsyncFunctionDecl();
            else if (isKw("class")) s.declaration = parseClassDeclaration();
            else { s.declaration = parseAssignment(); semicolon(); }
            return fin(s);
        }
        if (isP("*")) {
            // ExportAllDeclaration: `export * from "m"` and `export * as ns from "m"`. A re-export
            // creates NO local binding/reference (the scope resolver's exportInner only binds a
            // wrapped declaration, which this has none of). `from` is mandatory.
            const ea = mk("ExportAllDeclaration", s.start);
            advance(true);                          // past `*`
            ea.exported = null;
            if (isKw("as")) { advance(true); ea.exported = parseIdentifierName(); }   // `as ns`
            ea.source = null;
            if (isKw("from")) {
                advance(true);
                ea.source = (cur.type === "string") ? parseLiteral() : (synErr("expected module specifier"), errNode(cur.start));
            } else {
                synErr("expected 'from'");           // `export *` / `export * as ns` require `from`
            }
            semicolon();
            return fin(ea);
        }
        if (isP("{")) {
            s.specifiers = []; s.declaration = null; advance(true);
            while (!isP("}") && !atEof()) {
                if (ts && tsTypeSpecifierAhead()) { if (!tsEraseSpecifier()) break; continue; }
                const spec = mk("ExportSpecifier", cur.start); spec.local = parseIdentifierName(); spec.exported = spec.local;
                if (isKw("as")) { advance(true); spec.exported = parseIdentifierName(); }
                s.specifiers.push(fin(spec));
                if (!eatP(",", true)) break;
            }
            expectP("}", false);
            if (isKw("from")) { advance(true); s.source = (cur.type === "string") ? parseLiteral() : (synErr("expected specifier"), errNode(cur.start)); }
            semicolon();
            return fin(s);
        }
        // export <declaration>
        s.specifiers = []; s.source = null;
        if (isKw("var") || isKw("let") || isKw("const")) s.declaration = parseVarDeclaration();
        else if (isKw("function")) s.declaration = parseFunctionDeclaration(false);
        else if (asyncFunctionAhead()) s.declaration = maybeAsyncFunctionDecl();
        else if (isKw("class")) s.declaration = parseClassDeclaration();
        else {
            if (ts) {
                const what = tsDeclarationStart();
                if (what !== null) tsRefuse(what);
            }
            synErr("unexpected export"); s.declaration = errNode(cur.start); synchronize();
        }
        return fin(s);
    }

    const ast = parseProgram();
    // Scan JSDoc @tags and attach leading runs to declaration targets. Mirrors
    // lua.parse calling annotations.attach. Best-effort + hardened: an internal defect emits
    // js.internal through the SAME budget (so diagnostics-empty guarantees attachment succeeded),
    // malformed tag content does not. The tokenizer's linemap is already computed.
    if (ts) {
        // TS mode: attach over the erased bytes, without the comments erased with the types, so
        // JSDoc attaches exactly as it would to the erased JavaScript (a line holding only an
        // erased declaration is blank there).
        typeSpans.sort(function (a, b) { return a.start - b.start; });
        const er = erase(bytes, typeSpans);
        if (er.ok) {
            const kept = [];
            let k = 0;
            for (let i = 0; i < tk.comments.length; i++) {
                const c = tk.comments[i];
                while (k < typeSpans.length && typeSpans[k].stop <= c.start) k++;
                if (!(k < typeSpans.length && typeSpans[k].start <= c.start && c.stop <= typeSpans[k].stop)) kept.push(c);
            }
            attachAnnotations(ast, kept, er.bytes, tk.linemap, budget, path);
        } else {
            attachAnnotations(ast, tk.comments, bytes, tk.linemap, budget, path);   // refused by typeErasure
        }
    } else {
        attachAnnotations(ast, tk.comments, bytes, tk.linemap, budget, path);
    }
    // Lexical + parser diagnostics already share ONE budget list (tk.diagnostics === budget.list),
    // so it is a single authoritative sequence -- no merge, no double-counting.
    const allDiags = budget.list;
    const valid = allDiags.every(function (d) { return d.severity !== "error"; });
    const unit = { ast: ast, comments: tk.comments, diagnostics: allDiags, linemap: tk.linemap, valid: valid };
    if (ts) unit.typeSpans = typeSpans;
    return unit;
}

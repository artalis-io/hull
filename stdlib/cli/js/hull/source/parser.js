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
//   opts: { path?, maxTokens?, maxDiagnostics?, maxDepth? }
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { createTokenizer } from "hull:source:lexer";
import { newDiagnostic, makeBudget } from "hull:source:diagnostic";
import { attach as attachAnnotations } from "hull:source:annotations";

// Reserved words that may NOT be a binding/identifier reference.
const RESERVED = new Set([
    "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete",
    "do", "else", "export", "extends", "finally", "for", "function", "if", "import", "in",
    "instanceof", "new", "return", "super", "switch", "this", "throw", "try", "typeof", "var",
    "void", "while", "with", "enum",
]);
// Unary operators (prefix).
const UNARY = new Set(["+", "-", "!", "~", "typeof", "void", "delete"]);
// Assignment operators.
const ASSIGN = new Set(["=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??="]);
// Binary operator precedence (higher binds tighter). Logical/coalesce handled with these too.
const BINPREC = {
    "??": 1, "||": 2, "&&": 3, "|": 4, "^": 5, "&": 6,
    "==": 7, "!=": 7, "===": 7, "!==": 7,
    "<": 8, ">": 8, "<=": 8, ">=": 8, "instanceof": 8, "in": 8,
    "<<": 9, ">>": 9, ">>>": 9,
    "+": 10, "-": 10, "*": 11, "/": 11, "%": 11, "**": 12,
};

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

function parseInternal(bytes, opts, inject) {
    opts = opts || {};
    const path = opts.path || null;
    const maxDepth = (typeof opts.maxDepth === "number" && opts.maxDepth > 0) ? Math.floor(opts.maxDepth) : 1000;
    const maxDiagnostics = nnInt(opts.maxDiagnostics, 4096);

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
    // Run `fn` while parsing a NESTED statement (import/export illegal there).
    function nested(fn) { const t = atModuleItem; atModuleItem = false; const r = fn(); atModuleItem = t; return r; }
    // Parse a function's params then body under a pushed context frame; restored on return.
    function withFn(kind, doParams, doBody) {
        ctxStack.push({ kind: kind, region: "params", arrow: false });
        const params = doParams();
        ctxStack[ctxStack.length - 1].region = "body";
        const body = nested(doBody);
        ctxStack.pop();
        return { params: params, body: body };
    }

    // cur/prev tokens. The goal used to READ cur was decided when we advanced past prev.
    let cur = tk.next(true);         // first token is at statement (operand) position
    let prev = null;

    // advance past cur, reading the next token with an explicit grammatical slash goal:
    //   regexAllowed=true  -> the next token is at an operand/statement position
    //   regexAllowed=false -> the next token is at an operator/continuation position
    // A buffered lookahead token (already lexed) is consumed as-is; the goal only applies to a
    // fresh read from the tokenizer.
    function advance(regexAllowed) {
        prev = cur;
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

    // Full speculative-parse checkpoint: the tokenizer's lexical state + the parser's token
    // window (cur/prev/lookahead) + the shared diagnostic budget. Restoring rewinds EVERY
    // producer, so a failed speculative parse (e.g. an arrow guess) leaves no lexical
    // disagreement or stray diagnostic, no matter what content it lexed.
    function saveState() { return { tk: tk.checkpoint(), cur: cur, prev: prev, la: la.slice(), budget: budget.mark(), depth: depth, errored: errored,
        ctx: ctxStack.map(function (f) { return { kind: f.kind, region: f.region, arrow: f.arrow }; }), atModuleItem: atModuleItem }; }
    function restoreState(st) {
        tk.restore(st.tk); cur = st.cur; prev = st.prev;
        la.length = 0; for (let i = 0; i < st.la.length; i++) la.push(st.la[i]);
        budget.reset(st.budget); depth = st.depth; errored = st.errored;
        ctxStack.length = 0; for (let i = 0; i < st.ctx.length; i++) ctxStack.push(st.ctx[i]); atModuleItem = st.atModuleItem;
    }

    // Every parser diagnostic flows through the ONE shared budget so the combined tokenizer +
    // parser ordinary count can never exceed maxDiagnostics; terminal js.limit.* are always kept.
    function diag(sev, code, message, start, stop) { budget.push(sev, code, message, { start: start, stop: stop }); }
    function synErr(message, tok) { const t = tok || cur; diag("error", "js.syntax", message, t.start, t.stop); }
    function unsupported(message, tok) { const t = tok || cur; diag("error", "js.unsupported", message, t.start, t.stop); }

    function mk(type, start) { return { type: type, start: start, stop: start }; }
    // Finalize a node's half-open range. stop = the last consumed token's stop, but NEVER before
    // the node's own start: on an error-recovery path that consumed nothing after the node began
    // (prev is still the token before `start`), clamp to a zero-width span [start, start] rather
    // than emit an inverted range. Keeps the AST range invariant start <= stop <= n+1.
    function fin(node) { const s = prev ? prev.stop : node.start; node.stop = s < node.start ? node.start : s; return node; }
    function errNode(start) { const e = mk("Error", start); e.stop = cur.start < start ? start : cur.start; return e; }

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

    // -- Program --
    function parseProgram() {
        const prog = mk("Program", cur.start);
        prog.body = [];
        while (!atEof()) {
            const before = cur.start;
            atModuleItem = true;                        // direct module body item: import/export legal
            const st = parseStatement();
            if (st) prog.body.push(st);
            if (cur.start === before && !atEof()) advance(true);   // stall guard
            errored = false;
        }
        // Drain to EOF is implicit: cur is eof; the tokenizer already produced all tokens,
        // so tk.comments / tk.diagnostics are complete.
        return fin(prog);
    }

    // -- Statements --
    function parseStatement() {
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
            // would then corrupt the real parse. Guard the peek with save/restore: on a
            // non-label, restore rewinds the tokenizer + lookahead so the real parse re-lexes.
            const _lblState = saveState();
            const lblNext = peekTok(1, false);
            const isLabel = (lblNext.type === "punctuator" && lblNext.value === ":");
            if (!isLabel) restoreState(_lblState);
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
                    else st = parseImport();
                    break;
                }
                case "export": st = parseExport(); break;
                case "with": { const ws = cur.start; unsupported("with statement is not supported"); advance(true); expectP("(", true); parseExpression(); expectP(")", true); nested(parseStatement); st = errNode(ws); break; }
                case "debugger": { st = mk("DebuggerStatement", cur.start); advance(true); semicolon(); st = fin(st); break; }
                default: st = parseExpressionStatement();
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
            const s = nested(parseStatement);
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
            if (isKw("case")) { advance(true); c.test = parseExpression(); }
            else if (isKw("default")) { advance(true); }
            else { synErr("expected 'case' or 'default'"); advance(true); continue; }
            expectP(":", true);
            while (!isP("}") && !isKw("case") && !isKw("default") && !atEof()) {
                const before = cur.start; const st = nested(parseStatement); if (st) c.consequent.push(st);
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
        if (isP("}") || atEof() || nl()) return;   // automatic semicolon insertion
        synErr("expected ';'");
    }

    // -- functions / classes --
    function maybeAsyncFunctionDecl() {
        // `async function ...` declaration (no LineTerminator between `async` and `function`);
        // otherwise `async` is an ordinary identifier -> expression statement.
        const start = cur.start;
        const nx = peekTok(1, true);
        if (nx.type === "identifier" && nx.value === "function" && !nx.nlBefore) {
            advance(true);                          // consume `async` -> cur is `function`
            return parseFunctionDeclaration(true, start);
        }
        return parseExpressionStatement();
    }

    function parseFunctionDeclaration(isAsync, start) {
        const f = mk("FunctionDeclaration", start !== undefined ? start : cur.start);
        f.async = isAsync === true;
        advance(true);                              // past `function`
        f.generator = eatP("*", true);
        if (f.generator) { unsupported("generator functions are not supported", prev); }
        f.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        const r = withFn(f.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(true); });
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
        const r = withFn(f.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); });
        f.params = r.params; f.body = r.body;       // expression body `}` -> a value (division)
        return fin(f);
    }

    function parseParams() {
        const params = [];
        if (!expectP("(", true)) return params;
        while (!isP(")") && !atEof()) {
            if (isP("...")) { const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget(); params.push(fin(r)); break; }
            let target = parseBindingTarget();
            if (eatP("=", true)) { const ap = mk("AssignmentPattern", target.start); ap.left = target; ap.right = parseAssignment(); target = fin(ap); }
            params.push(target);
            if (!eatP(",", true)) break;
        }
        expectP(")", true);
        return params;
    }

    function parseFunctionBody(isDecl) {
        if (!isP("{")) { synErr("expected function body"); return errNode(cur.start); }
        const b = mk("BlockStatement", cur.start);
        b.body = [];
        advance(true);
        while (!isP("}") && !atEof()) {
            const before = cur.start; const s = parseStatement(); if (s) b.body.push(s);
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
        c.superClass = null;
        // ClassHeritage : extends LeftHandSideExpression -- which INCLUDES call expressions
        // (`class C extends fn(x) {}` is valid), so allow calls. (Passing false left the call
        // arguments unconsumed, which drove non-terminating error recovery -> heap blowup.)
        if (isKw("extends")) { advance(true); c.superClass = parseLeftHandSide(true); }
        c.body = [];
        expectP("{", true);
        while (!isP("}") && !atEof()) {
            if (eatP(";", true)) continue;
            const m = parseClassMember();
            if (m) c.body.push(m);
            else if (!isP("}")) advance(true);
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
        // Non-destructive one-token lookahead decides whether a keyword-like token is a
        // modifier or the member NAME (e.g. a field named `static` / `async` / `get`).
        let isStatic = false;
        if (isKw("static")) {
            const nx = peekTok(1, false);
            if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";"))) { isStatic = true; advance(true); }
        }
        let kind = "method", isAsync = false, isGen = false;
        if (isKw("async")) { const nx = peekTok(1, false); if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";")) && !nx.nlBefore) { isAsync = true; advance(true); } }
        if (isP("*")) { isGen = true; unsupported("generator methods are not supported"); advance(true); }
        if (isKw("get") || isKw("set")) { const nx = peekTok(1, false); if (!(nx.type === "punctuator" && (nx.value === "(" || nx.value === "=" || nx.value === ";"))) { kind = cur.value; advance(true); } }
        const key = parsePropertyKey();
        const m = mk("MethodDefinition", start); m.static = isStatic; m.kind = kind; m.key = key; m.async = isAsync; m.generator = isGen;
        if (isP("(")) {
            const fe = mk("FunctionExpression", cur.start); fe.async = isAsync; fe.generator = isGen; fe.id = null;
            const fr = withFn(fe.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); }); fe.params = fr.params; fe.body = fr.body;
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
        return parseIdentifier();
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
            node.consequent = parseAssignment();
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
            const prec = op !== null ? BINPREC[op] : undefined;
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
            if (v === "yield") {
                const ys = cur.start;
                unsupported("yield is not supported");
                advance(true);                      // past `yield`
                eatP("*", true);                    // optional `yield*`
                if (!nl() && isExprStartTok(cur)) parseAssignment();   // consume the operand cleanly
                return errNode(ys);
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
        let callee = isKw("new") ? parseNew() : parsePrimary();
        callee = parseCallMemberTail(callee, false);   // member tail but no call
        const node = mk("NewExpression", start); node.callee = callee; node.arguments = [];
        if (isP("(")) node.arguments = parseArguments();
        return parseCallMemberTail(fin(node), true);
    }

    function parseCallMemberTail(e, allowCall) {
        for (;;) {
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
            // middle/tail token is the continuation of an enclosing template, not a tag.
            if (cur.type === "template" && (cur.mode === "noSubstitution" || cur.mode === "head") && !nl()) { const t = mk("TaggedTemplateExpression", e.start); t.tag = e; t.quasi = parseTemplate(); e = fin(t); continue; }
            break;
        }
        return e;
    }

    function parseArguments() {
        const args = [];
        advance(true);                              // past `(`
        while (!isP(")") && !atEof()) {
            if (isP("...")) { const sp = mk("SpreadElement", cur.start); advance(true); sp.argument = parseAssignment(); args.push(fin(sp)); }
            else args.push(parseAssignment());
            if (!eatP(",", true)) break;
        }
        expectP(")", false);                        // a call `)` -> a value (division)
        return args;
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

    function parsePrimary() {
        const start = cur.start;
        switch (cur.type) {
            case "number": case "string": return parseLiteral();
            case "regex": { const n = mk("Literal", start); n.regex = { pattern: cur.pattern, flags: cur.flags }; n.raw = cur.raw; advance(false); return fin(n); }
            case "template": return parseTemplate();
            case "identifier": return parseIdentifierExpr();
            case "punctuator":
                if (cur.value === "(") return parseParenOrArrow();
                if (cur.value === "[") return parseArrayExpr();
                if (cur.value === "{") return parseObjectExpr();
                break;
        }
        synErr("unexpected token");
        const e = errNode(start);
        if (!errored) { errored = true; } else { advance(true); }   // avoid infinite loop on junk
        return e;
    }

    function parseIdentifierExpr() {
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
            const nx = peekTok(1, true);
            if (nx.type === "identifier" && nx.value === "function" && !nx.nlBefore) { advance(true); return parseFunctionExpr(true, start); }
            if (!nx.nlBefore) {
                // `async ident =>`
                if (nx.type === "identifier" && !RESERVED.has(nx.value) && peekIsP(2, "=>")) {
                    advance(true);                  // consume `async` -> cur is the param ident
                    const id = parseIdentifier();
                    return finishArrow(start, [id], true);
                }
                // `async ( ... ) =>` -- the parenthesized content may hold default expressions
                // with a regex OR a division, so it cannot be pre-scanned with one slash goal.
                // SPECULATIVELY parse the params with real grammar (correct goals per token); if
                // no `=>` follows, fully rewind the tokenizer + parser and parse `async(...)` as a
                // call. This is a tokenizer-checkpoint speculation, not a lookahead pre-lex.
                if (nx.type === "punctuator" && nx.value === "(") {
                    const st = saveState();
                    advance(true);                  // consume `async` -> cur is `(`
                    const params = parseParams();
                    if (isP("=>") && !nl()) return finishArrow(start, params, true);
                    restoreState(st);               // not an arrow -> rewind; parse as a call below
                }
            }
            // otherwise `async` is an ordinary identifier -> fall through
        }
        // `ident =>` arrow
        const id = parseIdentifier();
        if (isP("=>") && !nl()) return finishArrow(id.start, [id], false);
        return id;
    }
    function fin2n(type, start) { const n = mk(type, start); n.stop = prev.stop < start ? start : prev.stop; return n; }

    function parseParenOrArrow() {
        // Parse a parenthesized expression; if followed by `=>`, reinterpret as arrow params.
        const start = cur.start;
        advance(true);
        if (isP(")")) { advance(false); if (isP("=>")) return finishArrow(start, [], false); synErr("unexpected ')'"); return errNode(start); }
        const items = [];
        let sawRest = false;
        for (;;) {
            if (isP("...")) { const r = mk("RestElement", cur.start); advance(true); r.argument = parseBindingTarget(); items.push(fin(r)); sawRest = true; break; }
            items.push(parseAssignment());
            if (!eatP(",", true)) break;
        }
        expectP(")", false);
        if (isP("=>") && !nl()) return finishArrow(start, items, false);
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
            const fr = withFn(fe.async ? "async" : "regular", parseParams, function () { return parseFunctionBody(false); }); fe.params = fr.params; fe.body = fr.body;
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
        // Hull parses MODULE code, which is always strict and where `await` is a reserved word:
        // it may not be a BindingIdentifier, IdentifierReference, or LabelIdentifier (an
        // AwaitExpression is intercepted earlier in parseUnary, so a bare `await` reaching here is
        // always a reserved-word misuse). An escaped spelling (await) is likewise reserved.
        if (cur.value === "await") { synErr("'await' is reserved in module code"); advance(false); return errNode(cur.start); }
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
        if (cur.type === "identifier" && !RESERVED.has(cur.value)) {
            const d = mk("ImportDefaultSpecifier", cur.start); d.local = parseIdentifier(); s.specifiers.push(fin(d));
            eatP(",", true);
        }
        if (isP("*")) { const ns = mk("ImportNamespaceSpecifier", cur.start); advance(true); if (isKw("as")) advance(true); ns.local = parseBindingIdentifier(); s.specifiers.push(fin(ns)); }
        else if (isP("{")) {
            advance(true);
            while (!isP("}") && !atEof()) {
                const spec = mk("ImportSpecifier", cur.start); spec.imported = parseIdentifierName(); spec.local = spec.imported;
                if (isKw("as")) { advance(true); spec.local = parseBindingIdentifier(); }
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
        advance(true);
        if (isKw("default")) {
            s.type = "ExportDefaultDeclaration"; advance(true);
            if (isKw("function")) s.declaration = parseFunctionDeclaration(false);
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
        else if (isKw("class")) s.declaration = parseClassDeclaration();
        else { synErr("unexpected export"); s.declaration = errNode(cur.start); synchronize(); }
        return fin(s);
    }

    const ast = parseProgram();
    // Scan JSDoc @tags and attach leading runs to declaration targets (Slice 3). Mirrors
    // lua.parse calling annotations.attach. Best-effort + hardened: an internal defect emits
    // js.internal through the SAME budget (so diagnostics-empty guarantees attachment succeeded),
    // malformed tag content does not. The tokenizer's linemap is already computed.
    attachAnnotations(ast, tk.comments, bytes, tk.linemap, budget, path);
    // Lexical + parser diagnostics already share ONE budget list (tk.diagnostics === budget.list),
    // so it is a single authoritative sequence -- no merge, no double-counting.
    const allDiags = budget.list;
    const valid = allDiags.every(function (d) { return d.severity !== "error"; });
    return { ast: ast, comments: tk.comments, diagnostics: allDiags, linemap: tk.linemap, valid: valid };
}

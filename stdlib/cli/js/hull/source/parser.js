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
import { newDiagnostic } from "hull:source:diagnostic";

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

export function parse(bytes, opts) {
    opts = opts || {};
    const path = opts.path || null;
    const maxDepth = (typeof opts.maxDepth === "number" && opts.maxDepth > 0) ? Math.floor(opts.maxDepth) : 1000;

    const tk = createTokenizer(bytes, opts);
    const diagnostics = [];
    let depth = 0;
    let errored = false;             // rate-limit cascading recovery within one statement

    // cur/prev tokens. The goal used to READ cur was decided when we advanced past prev.
    let cur = tk.next(true);         // first token is at statement (operand) position
    let prev = null;

    // advance past cur, reading the next token with an explicit grammatical slash goal:
    //   regexAllowed=true  -> the next token is at an operand/statement position
    //   regexAllowed=false -> the next token is at an operator/continuation position
    function advance(regexAllowed) { prev = cur; cur = tk.next(regexAllowed === false ? false : true); return prev; }

    function atEof() { return cur.type === "eof"; }
    function isP(v) { return cur.type === "punctuator" && cur.value === v; }
    function isKw(v) { return cur.type === "identifier" && cur.value === v; }
    function nl() { return cur.nlBefore; }

    function diag(sev, code, message, start, stop) {
        diagnostics.push(newDiagnostic(sev, code, message, path, { start: start, stop: stop }));
    }
    function synErr(message, tok) { const t = tok || cur; diag("error", "js.syntax", message, t.start, t.stop); }
    function unsupported(message, tok) { const t = tok || cur; diag("error", "js.unsupported", message, t.start, t.stop); }

    function mk(type, start) { return { type: type, start: start, stop: start }; }
    function fin(node) { node.stop = prev ? prev.stop : node.start; return node; }
    function errNode(start) { const e = mk("Error", start); e.stop = cur.start; return e; }

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
                case "import": st = parseImport(); break;
                case "export": st = parseExport(); break;
                case "with": unsupported("with statement is not supported"); st = errNode(cur.start); synchronize(); break;
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
            const s = parseStatement();
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
        s.consequent = parseStatement();
        s.alternate = null;
        if (isKw("else")) { advance(true); s.alternate = parseStatement(); }
        return fin(s);
    }

    function parseWhile() {
        const s = mk("WhileStatement", cur.start);
        advance(true); expectP("(", true);
        s.test = parseExpression();
        expectP(")", true);
        s.body = parseStatement();
        return fin(s);
    }

    function parseDoWhile() {
        unsupported("do-while statement is not supported");
        const s = errNode(cur.start); synchronize(); return s;
    }

    function parseFor() {
        const s = mk("ForStatement", cur.start);
        advance(true); expectP("(", true);
        // for-in is unsupported; for-of + C-style for are supported.
        let init = null;
        if (isP(";")) { /* empty init */ }
        else if ((isKw("var") || isKw("let") || isKw("const"))) {
            const kind = cur.value; const vd = mk("VariableDeclaration", cur.start); vd.kind = kind; vd.declarations = [];
            advance(true);
            const decl = mk("VariableDeclarator", cur.start); decl.id = parseBindingTarget(); decl.init = null;
            if (isKw("of")) { return finishForOf(s, fin2(vd, decl)); }
            if (isKw("in")) { unsupported("for-in statement is not supported"); synchronize(); return errNode(s.start); }
            if (eatP("=", true)) decl.init = parseAssignment();
            vd.declarations.push(fin(decl));
            while (eatP(",", true)) { const d2 = mk("VariableDeclarator", cur.start); d2.id = parseBindingTarget(); d2.init = null; if (eatP("=", true)) d2.init = parseAssignment(); vd.declarations.push(fin(d2)); }
            init = fin(vd);
        } else {
            init = parseExpression();
            if (isKw("of")) { return finishForOf(s, init); }
            if (isKw("in")) { unsupported("for-in statement is not supported"); synchronize(); return errNode(s.start); }
        }
        s.init = init;
        expectP(";", true);
        s.test = isP(";") ? null : parseExpression();
        expectP(";", true);
        s.update = isP(")") ? null : parseExpression();
        expectP(")", true);
        s.body = parseStatement();
        return fin(s);
    }
    function fin2(vd, decl) { vd.declarations = [fin(decl)]; return fin(vd); }
    function finishForOf(s, left) {
        s.type = "ForOfStatement"; s.left = left;
        advance(true);                              // past `of`
        s.right = parseAssignment();
        expectP(")", true);
        s.body = parseStatement();
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
                const before = cur.start; const st = parseStatement(); if (st) c.consequent.push(st);
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
        // `async function ...` declaration; otherwise `async` is an identifier expression.
        // One-token lookahead is not available, so treat via expression statement fallback.
        return parseExpressionStatement();
    }

    function parseFunctionDeclaration(isAsync) {
        const f = mk("FunctionDeclaration", cur.start);
        f.async = isAsync === true;
        advance(true);                              // past `function`
        f.generator = eatP("*", true);
        if (f.generator) { unsupported("generator functions are not supported", prev); }
        f.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        f.params = parseParams();
        f.body = parseFunctionBody(true);           // declaration body `}` -> statement follows
        return fin(f);
    }

    function parseFunctionExpr(isAsync) {
        const f = mk("FunctionExpression", cur.start);
        f.async = isAsync === true;
        advance(true);
        f.generator = eatP("*", true);
        if (f.generator) unsupported("generator functions are not supported", prev);
        f.id = (cur.type === "identifier" && !RESERVED.has(cur.value)) ? parseIdentifier() : null;
        f.params = parseParams();
        f.body = parseFunctionBody(false);          // expression body `}` -> a value (division)
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
        if (isKw("extends")) { advance(true); c.superClass = parseLeftHandSide(false); }
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
        if (cur.type === "punctuator" && cur.value === "#") { unsupported("private class members are not supported"); advance(true); return errNode(start); }
        let isStatic = false;
        if (isKw("static") && !RESERVED.has("static")) {
            // `static` method/field unless it is the member name itself.
            const save = cur;
            advance(true);
            if (isP("(") || isP("=") || isP(";")) { /* `static` was the name */ cur = save; }
            else isStatic = true;
        }
        let kind = "method", isAsync = false, isGen = false;
        if (isKw("async")) { const save = cur; advance(true); if (!isP("(") && !isP("=")) isAsync = true; else cur = save; }
        if (isP("*")) { isGen = true; unsupported("generator methods are not supported"); advance(true); }
        if ((isKw("get") || isKw("set"))) { const g = cur.value; const save = cur; advance(true); if (!isP("(") && !isP("=") && !isP(";")) kind = g; else cur = save; }
        const key = parsePropertyKey();
        const m = mk("MethodDefinition", start); m.static = isStatic; m.kind = kind; m.key = key; m.async = isAsync; m.generator = isGen;
        if (isP("(")) {
            const fe = mk("FunctionExpression", cur.start); fe.async = isAsync; fe.generator = isGen; fe.id = null;
            fe.params = parseParams(); fe.body = parseFunctionBody(false);
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

    function parseAssignment() {
        if (!guard()) { unguard(); return errNode(cur.start); }
        // arrow-function detection is limited without arbitrary lookahead; handled in primary
        // for `(params) =>` and `ident =>`.
        const left = parseConditional();
        if (cur.type === "punctuator" && ASSIGN.has(cur.value)) {
            const node = mk("AssignmentExpression", left.start);
            node.operator = cur.value; node.left = left; advance(true);
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
            if (v === "++" || v === "--") { const node = mk("UpdateExpression", cur.start); node.operator = v; node.prefix = true; advance(true); node.argument = parseUnary(); return fin(node); }
            if (v === "await") { const save = cur; advance(true); if (isExprStart()) { const node = mk("AwaitExpression", save.start); node.argument = parseUnary(); return fin(node); } cur = save; }
            if (v === "yield") { unsupported("yield is not supported"); advance(true); return errNode(prev.start); }
        }
        let e = parsePostfix();
        return e;
    }

    function parsePostfix() {
        let e = parseLeftHandSide(true);
        if ((isP("++") || isP("--")) && !nl()) {
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
        if (isP(".")) { advance(false); const meta = mk("MetaProperty", start); meta.meta = "new"; meta.property = (cur.type === "identifier" ? cur.value : ""); advance(false); return parseCallMemberTail(fin(meta), true); }
        let callee = isKw("new") ? parseNew() : parsePrimary();
        callee = parseCallMemberTail(callee, false);   // member tail but no call
        const node = mk("NewExpression", start); node.callee = callee; node.arguments = [];
        if (isP("(")) node.arguments = parseArguments();
        return parseCallMemberTail(fin(node), true);
    }

    function parseCallMemberTail(e, allowCall) {
        for (;;) {
            if (isP(".")) { advance(false); const m = mk("MemberExpression", e.start); m.object = e; m.computed = false; m.optional = false; m.property = parseIdentifierName(); e = fin(m); continue; }
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

    function isExprStart() {
        if (atEof()) return false;
        if (cur.type === "number" || cur.type === "string" || cur.type === "template" || cur.type === "regex") return true;
        if (cur.type === "identifier") return true;
        if (cur.type === "punctuator") return cur.value === "(" || cur.value === "[" || cur.value === "{" || cur.value === "!" || cur.value === "~" || cur.value === "+" || cur.value === "-" || cur.value === "++" || cur.value === "--" || cur.value === "...";
        return false;
    }

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
        if (v === "import") { advance(false); if (isP("(")) { const ie = mk("ImportExpression", start); ie.arguments = parseArguments(); return fin(ie); } if (isP(".")) { advance(false); const meta = mk("MetaProperty", start); meta.meta = "import"; meta.property = (cur.type === "identifier" ? cur.value : ""); advance(false); return fin(meta); } synErr("unexpected 'import'"); return errNode(start); }
        if (v === "async") {
            const save = cur; advance(false);
            if (isKw("function") && !prev.nlBefore) return parseFunctionExpr(true);
            // `async ident =>` or `async (params) =>`
            if ((cur.type === "identifier" || isP("(")) && !cur.nlBefore) {
                const maybe = tryArrow(save.start, true);
                if (maybe) return maybe;
            }
            cur = save;                             // plain identifier `async`
        }
        // `ident =>` arrow
        const id = parseIdentifier();
        if (isP("=>") && !nl()) return finishArrow(id.start, [id], false);
        return id;
    }
    function fin2n(type, start) { const n = mk(type, start); n.stop = prev.stop; return n; }

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

    function tryArrow(start, isAsync) {
        // Called after consuming `async`; cur is an identifier or `(`.
        if (cur.type === "identifier") { const id = parseIdentifier(); if (isP("=>") && !nl()) return finishArrow(start, [id], isAsync); return null; }
        if (isP("(")) {
            const p = parseParams();
            if (isP("=>") && !nl()) { const a = mk("ArrowFunctionExpression", start); a.async = isAsync; a.params = p; advance(true); a.body = parseArrowBody(); return fin(a); }
        }
        return null;
    }

    function finishArrow(start, params, isAsync) {
        const a = mk("ArrowFunctionExpression", start); a.async = isAsync === true; a.params = params;
        expectP("=>", true);
        a.body = parseArrowBody();
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
        if (isKw("async")) { const save = cur; advance(false); if (!isP(":") && !isP(",") && !isP("}") && !isP("(")) isAsync = true; else cur = save; }
        if (isP("*")) { isGen = true; unsupported("generator methods are not supported"); advance(true); }
        if ((isKw("get") || isKw("set"))) { const g = cur.value; const save = cur; advance(false); if (!isP(":") && !isP(",") && !isP("}") && !isP("(")) kind = g; else cur = save; }
        const computed = isP("[");
        const key = parsePropertyKey();
        const pr = mk("Property", start); pr.computed = computed; pr.key = key; pr.kind = kind === "init" ? "init" : kind;
        if (isP("(")) {   // method
            const fe = mk("FunctionExpression", cur.start); fe.async = isAsync; fe.generator = isGen; fe.id = null;
            fe.params = parseParams(); fe.body = parseFunctionBody(false);
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
        advance(true);
        if (isKw("default")) {
            s.type = "ExportDefaultDeclaration"; advance(true);
            if (isKw("function")) s.declaration = parseFunctionDeclaration(false);
            else if (isKw("class")) s.declaration = parseClassDeclaration();
            else { s.declaration = parseAssignment(); semicolon(); }
            return fin(s);
        }
        if (isP("*")) { unsupported("export * is not supported"); synchronize(); return errNode(s.start); }
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
    // Merge lexical diagnostics (from the tokenizer, driven to EOF) with parser diagnostics,
    // preserving both even though recovery continued.
    const allDiags = tk.diagnostics.concat(diagnostics);
    const valid = allDiags.every(function (d) { return d.severity !== "error"; });
    return { ast: ast, comments: tk.comments, diagnostics: allDiags, linemap: tk.linemap, valid: valid };
}

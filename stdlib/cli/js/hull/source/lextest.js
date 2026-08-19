// hull:source:lextest - thin Slice-2 lexer test driver (scaffolding, like hull:probe).
//
// Exposes the byte-oriented lexer through the frontend entry contract so the C harness
// (tests/hull/frontend/test_js_lexer.c) can drive representative + adversarial sources and
// assert on the JSON token stream + comments + diagnostics. Absorbed by the real frontend
// entry in a later slice.
// SPDX-License-Identifier: AGPL-3.0-or-later
import { lex, createTokenizer } from "hull:source:lexer";
import { parse, __parseWithInjection } from "hull:source:parser";
import { attach as attachAnnotations } from "hull:source:annotations";
import { makeBudget } from "hull:source:diagnostic";
import { resolve as resolveScopeModel } from "hull:source:scope";

// Structural-default lexing (the standalone convenience path).
function run(srcBuf, path, opts) {
    const o = { path: path || "test.js" };
    if (opts && opts.maxTokens !== undefined) o.maxTokens = opts.maxTokens;
    if (opts && opts.maxDiagnostics !== undefined) o.maxDiagnostics = opts.maxDiagnostics;
    const r = lex(new Uint8Array(srcBuf), o);
    return { schema_version: 1, status: "ok", tokens: r.tokens, comments: r.comments, diagnostics: r.diagnostics };
}

// Parser-directed lexing: drive the incremental tokenizer, forcing the regex/division goal at
// every `/` to opts.forceRegex (true = regex, false = division). Demonstrates that the parser
// controls the slash goal, overriding the structural default (forceRegex affects a `/` only;
// other tokens ignore it).
function runDirected(srcBuf, path, opts) {
    const forced = (opts && typeof opts.forceRegex === "boolean") ? opts.forceRegex : undefined;
    const tk = createTokenizer(new Uint8Array(srcBuf), { path: path || "test.js" });
    const tokens = [];
    for (;;) { const t = tk.next(forced); tokens.push(t); if (t.type === "eof") break; }
    return { schema_version: 1, status: "ok", tokens: tokens, comments: tk.comments, diagnostics: tk.diagnostics };
}

// Parse to a SourceUnit (drives createTokenizer with grammatical slash goals).
function runParse(srcBuf, path, opts) {
    const o = { path: path || "test.js" };
    if (opts && opts.maxDepth !== undefined) o.maxDepth = opts.maxDepth;
    if (opts && opts.maxDiagnostics !== undefined) o.maxDiagnostics = opts.maxDiagnostics;
    const u = parse(new Uint8Array(srcBuf), o);
    return { schema_version: 1, status: "ok", ast: u.ast, comments: u.comments, diagnostics: u.diagnostics, valid: u.valid };
}

// Drive the protected boundary with an INJECTED exception (test-only) to prove containment vs.
// re-throw. opts.inject selects the thrown value:
//   "error-<phrase>"    -> new Error(phrase)      (ordinary; must be CONTAINED as js.internal)
//   "type-<phrase>"     -> new TypeError(phrase)  (ordinary; must be CONTAINED as js.internal)
//   "internal-<phrase>" -> an InternalError-named error (HOST resource; must be RE-THROWN)
// A re-thrown error propagates out of this method to the C session, which host-classifies it.
function runParseInject(srcBuf, path, opts) {
    const spec = (opts && opts.inject) || "error-boom";
    const dash = spec.indexOf("-");
    const kind = spec.substring(0, dash), phrase = spec.substring(dash + 1);
    const inject = function () {
        if (kind === "type") throw new TypeError(phrase);
        if (kind === "internal") { const e = new Error(phrase); e.name = "InternalError"; throw e; }
        throw new Error(phrase);
    };
    const u = __parseWithInjection(new Uint8Array(srcBuf), { path: path || "test.js" }, inject);
    return { schema_version: 1, status: "ok", ast: u.ast, comments: u.comments, diagnostics: u.diagnostics, valid: u.valid };
}

// Idempotency probe: parse once (attachment runs during parseInternal), snapshot the AST, then
// invoke attach() AGAIN on the SAME ast/comments/bytes, and report whether the projection is
// byte-identical. This exercises re-attachment on one unit (not two fresh parses).
function runReattach(srcBuf, path) {
    const p = path || "test.js";
    const bytes = new Uint8Array(srcBuf);
    const u = parse(bytes, { path: p });                    // first attach, inside the parser
    const before = JSON.stringify(u.ast);
    const commentsBefore = JSON.stringify(u.comments);
    const budget = makeBudget(4096, p);
    attachAnnotations(u.ast, u.comments, bytes, u.linemap, budget, p);   // second attach, same unit
    const after = JSON.stringify(u.ast);
    const commentsAfter = JSON.stringify(u.comments);
    return { schema_version: 1, status: "ok",
             ast_identical: before === after, comments_identical: commentsBefore === commentsAfter,
             reattach_diagnostics: budget.list.length };
}

// Latch probe: run attach() on a SYNTHETIC unit carrying MULTIPLE invalid ranges (the parser
// never produces these, so they are only reachable via a hand-built unit). Mode is the first
// source byte: 'd' = several declaration targets with invalid ranges; anything else = several
// jsdoc comments with invalid ranges (plus a bad declaration). Either way the latch must yield
// EXACTLY ONE js.internal and abort. Returns the internal count so the test can lock it to 1.
function runAttachCorrupt(srcBuf, path) {
    const p = path || "test.js";
    const u8 = new Uint8Array(srcBuf);
    const mode = u8.length ? String.fromCharCode(u8[0]) : "c";
    const bytes = new Uint8Array(16);                       // n = 16; valid ranges are [1, 17]
    const linemap = [1];
    let comments, ast;
    if (mode === "d") {
        comments = [];
        ast = { type: "Program", start: 1, stop: 1, body: [
            { type: "VariableDeclaration", start: 999, stop: 1, kind: "const", declarations: [] },   // stop < start
            { type: "FunctionDeclaration", start: 5, stop: 99999, id: null, params: [], body: null }, // stop > n+1
        ] };
    } else {
        comments = [
            { kind: "jsdoc", start: 100, stop: 50 },        // stop < start
            { kind: "jsdoc", start: -5, stop: 3 },          // start < 1
        ];
        ast = { type: "Program", start: 1, stop: 1, body: [
            { type: "VariableDeclaration", start: 999, stop: 2000, kind: "const", declarations: [] },
        ] };
    }
    const budget = makeBudget(4096, p);
    attachAnnotations(ast, comments, bytes, linemap, budget, p);
    let internal = 0;
    for (let i = 0; i < budget.list.length; i++) if (budget.list[i].code === "js.internal") internal++;
    return { schema_version: 1, status: "ok", total: budget.list.length, internal_count: internal };
}

// Scope resolver driver: parse then resolve, returning the scope model as JSON.
function runResolveScope(srcBuf, path, opts) {
    const u = parse(new Uint8Array(srcBuf), { path: path || "test.js" });
    if (opts && opts.corruptAst) u.ast = { type: "Program", get body() { throw new Error("injected internal defect"); } };
    const m = resolveScopeModel(u);
    return { schema_version: 1, status: "ok", ok: m.ok, bindings: m.bindings, refs: m.refs, error: m.error };
}

globalThis.__hull_frontend = { lex: run, lexDirected: runDirected, parse: runParse, parseInject: runParseInject, reattach: runReattach, attachCorrupt: runAttachCorrupt, resolveScope: runResolveScope };

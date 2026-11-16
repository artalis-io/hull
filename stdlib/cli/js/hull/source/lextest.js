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
import { typeErasure, erase } from "hull:source:ts_erase";
import { analyze as feAnalyze, declarationSemantics as feSemantics, scope as feScope,
         __analyzeWithFailure as feAnalyzeFail, __mutate as feMutate } from "hull:source:frontend_javascript";

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
    if (opts && opts.lang !== undefined) o.lang = opts.lang;
    const u = parse(new Uint8Array(srcBuf), o);
    return { schema_version: 1, status: "ok", ast: u.ast, comments: u.comments, diagnostics: u.diagnostics, valid: u.valid };
}

// TypeScript mode (docs/typescript_design.md section 3): the same SourceUnit as runParse, plus
// typeSpans. opts.asJs (test-only) drops the ts.* diagnostics and typeSpans and recomputes
// `valid`, so the result can be compared byte for byte with runParse: the TypeScript hooks
// must never change what JS mode produces for valid JavaScript. A refusal or a type span in a
// unit that is valid JS is a hook firing on JavaScript, so it changes `status` and fails that
// comparison.
function runParseTs(srcBuf, path, opts) {
    const o = { path: path || "test.ts", lang: "ts" };
    if (opts && opts.maxDepth !== undefined) o.maxDepth = opts.maxDepth;
    if (opts && opts.maxDiagnostics !== undefined) o.maxDiagnostics = opts.maxDiagnostics;
    const u = parse(new Uint8Array(srcBuf), o);
    if (opts && opts.asJs) {
        const ds = u.diagnostics.filter(function (d) { return d.code.substring(0, 3) !== "ts."; });
        const ok = ds.every(function (d) { return d.severity !== "error"; });
        const misfired = ok && (ds.length !== u.diagnostics.length || u.typeSpans.length > 0);
        return { schema_version: 1, status: misfired ? "ts-refusal-in-valid-js" : "ok", ast: u.ast, comments: u.comments, diagnostics: ds, valid: ok };
    }
    return { schema_version: 1, status: "ok", ast: u.ast, comments: u.comments, diagnostics: u.diagnostics, valid: u.valid, typeSpans: u.typeSpans };
}

// Lowercase hex of a byte array (the erased bytes go back to C exactly, whatever they hold).
function hex(u8) { let s = ""; for (let i = 0; i < u8.length; i++) s += (u8[i] < 16 ? "0" : "") + u8[i].toString(16); return s; }

// typeErasure over a TS-mode parse, then erase() with its spans - or, when opts.spans is given
// (test-only), erase() with those spans, to exercise the byte rules directly.
function runEraseTs(srcBuf, path, opts) {
    const p = path || "test.ts";
    const bytes = new Uint8Array(srcBuf);
    const u = parse(bytes, { path: p, lang: "ts" });
    const te = typeErasure(u, bytes, p);
    if (!te.ok) return { schema_version: 1, status: "ok", ok: false, error: te.error };
    const spans = (opts && opts.spans !== undefined) ? opts.spans : te.spans;
    const er = erase(bytes, spans);
    // M2: the erased bytes parse as JavaScript to exactly the TS-mode AST (and cleanly, when the
    // TS-mode unit was clean).
    let erasedValid = null, astMatches = null;
    if (er.ok) {
        const ju = parse(er.bytes, { path: p });
        erasedValid = ju.valid;
        astMatches = JSON.stringify(ju.ast) === JSON.stringify(u.ast);
    }
    return { schema_version: 1, status: "ok", ok: true, spans: te.spans, refusals: te.refusals, valid: u.valid,
             erased_ok: er.ok, erased_hex: er.ok ? hex(er.bytes) : null, erase_error: er.ok ? null : er.error,
             erased_valid: erasedValid, ast_matches: astMatches, diagnostics: u.diagnostics,
             // One line per refusal, `<code> <start> <stop> <message>`, for the corpus .expect files.
             refusal_lines: te.refusals.map(function (d) { return d.code + " " + d.range.start + " " + d.range.stop + " " + d.message; }).join("\n") };
}

// splitGreater probe: lex to EOF, splitting every multi-character `>` operator once, so each
// `>` of `>>=` comes out as its own token with its own range.
function runLexSplit(srcBuf, path) {
    const tk = createTokenizer(new Uint8Array(srcBuf), { path: path || "test.ts" });
    const tokens = [];
    for (;;) {
        let t = tk.next(false);
        if (t.type === "punctuator" && t.value.length > 1 && t.value.charCodeAt(0) === 0x3e) t = tk.splitGreater();
        tokens.push(t);
        if (t.type === "eof") break;
    }
    return { schema_version: 1, status: "ok", tokens: tokens, diagnostics: tk.diagnostics, split_none: tk.splitGreater() === null };
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

// Frontend-adapter drivers: analyze returns the facts; semantics/scope read the bridge-private
// id from opts (declId / unitId) and exercise the RETAINED session state.
function runFrontendAnalyze(srcBuf, path, opts) {
    const r = feAnalyze(new Uint8Array(srcBuf), path || "test.js", opts || {});
    return { schema_version: 1, status: "ok", result: r };
}
function runFrontendSemantics(srcBuf, path, opts) {
    const r = feSemantics(opts && opts.declId);
    return { schema_version: 1, status: "ok", result: r };
}
function runFrontendScope(srcBuf, path, opts) {
    const r = feScope(opts && opts.unitId);
    return { schema_version: 1, status: "ok", result: r };
}
// Test-only: force a mid-collection analyze failure (transactional-rollback proof).
function runFrontendAnalyzeFail(srcBuf, path, opts) {
    const r = feAnalyzeFail(new Uint8Array(srcBuf), path || "test.js", opts || {});
    return { schema_version: 1, status: "ok", result: r };
}
// Test-only: mutate a retained declaration's state (corrupt-state boundary proof).
function runFrontendMutate(srcBuf, path, opts) {
    return { schema_version: 1, status: "ok", result: feMutate(opts || {}) };
}

globalThis.__hull_frontend = { lex: run, lexDirected: runDirected, lexSplit: runLexSplit, parse: runParse, parseTs: runParseTs, eraseTs: runEraseTs, parseInject: runParseInject, reattach: runReattach, attachCorrupt: runAttachCorrupt, resolveScope: runResolveScope, frontendAnalyze: runFrontendAnalyze, frontendSemantics: runFrontendSemantics, frontendScope: runFrontendScope, frontendAnalyzeFail: runFrontendAnalyzeFail, frontendMutate: runFrontendMutate };

// hull:source:lextest - thin Slice-2 lexer test driver (scaffolding, like hull:probe).
//
// Exposes the byte-oriented lexer through the frontend entry contract so the C harness
// (tests/hull/frontend/test_js_lexer.c) can drive representative + adversarial sources and
// assert on the JSON token stream + comments + diagnostics. Absorbed by the real frontend
// entry in a later slice.
// SPDX-License-Identifier: AGPL-3.0-or-later
import { lex, createTokenizer } from "hull:source:lexer";

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

globalThis.__hull_frontend = { lex: run, lexDirected: runDirected };

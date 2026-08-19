// hull:source:lextest - thin Slice-2 lexer test driver (scaffolding, like hull:probe).
//
// Exposes the byte-oriented lexer through the frontend entry contract so the C harness
// (tests/hull/frontend/test_js_lexer.c) can drive representative + adversarial sources and
// assert on the JSON token stream + comments + diagnostics. Absorbed by the real frontend
// entry in a later slice.
// SPDX-License-Identifier: AGPL-3.0-or-later
import { lex } from "hull:source:lexer";

function run(srcBuf, path, opts) {
    const o = { path: path || "test.js" };
    if (opts && opts.maxTokens) o.maxTokens = opts.maxTokens;
    if (opts && opts.maxDiagnostics) o.maxDiagnostics = opts.maxDiagnostics;
    const r = lex(new Uint8Array(srcBuf), o);
    return { schema_version: 1, status: "ok", tokens: r.tokens, comments: r.comments, diagnostics: r.diagnostics };
}

globalThis.__hull_frontend = { lex: run };

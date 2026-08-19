// hull:source:lextest - thin Slice-2 lexer test driver (scaffolding, like hull:probe).
//
// Exposes the byte-oriented lexer through the frontend entry contract so the C harness
// (tests/hull/frontend/test_js_lexer.c) can drive representative + adversarial sources and
// assert on the JSON token stream. Absorbed by the real frontend entry in a later slice.
// SPDX-License-Identifier: AGPL-3.0-or-later
import { lex } from "hull:source:lexer";

function run(srcBuf, path, opts) {
    const r = lex(new Uint8Array(srcBuf), { path: path || "test.js", maxTokens: (opts && opts.maxTokens) || 0 });
    return { schema_version: 1, status: "ok", tokens: r.tokens, diagnostics: r.diagnostics };
}

globalThis.__hull_frontend = { lex: run };

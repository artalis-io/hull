// hull:source:diagnostic - language-neutral Diagnostic shape (JS mirror of
// hull.source.diagnostic / diagnostic.lua).
//
// A diagnostic is a structured problem the frontend reports as DATA (never a thrown error):
//   { severity: "error" | "warning",
//     code:     "js.syntax" | "js.unsupported" | "js.limit.<which>" | "js.internal",
//     message:  string,
//     path:     string | null,
//     range:    { start, stop } | null,        // half-open 1-based byte range (see range.js)
//     related:  [ { message, path?, range? }, ... ] | null }
//
// `js.syntax`   = malformed source the parser rejects.
// `js.unsupported` = valid ECMAScript the parser recognizes but declines (never a wrong AST).
// `js.internal` = an impossible/internal defect (kept for parity with the Lua layer).
//
// SPDX-License-Identifier: AGPL-3.0-or-later

export function newDiagnostic(severity, code, message, path, range, related) {
    return {
        severity: severity,
        code: code,
        message: message,
        path: path !== undefined ? path : null,
        range: range !== undefined ? range : null,
        related: related !== undefined ? related : null,
    };
}

export function error(code, message, path, range) {
    return newDiagnostic("error", code, message, path, range);
}

export function warning(code, message, path, range) {
    return newDiagnostic("warning", code, message, path, range);
}

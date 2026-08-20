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

// A single diagnostic-budget OWNER shared by the tokenizer and the parser so that
// maxDiagnostics is authoritative across the WHOLE SourceUnit (neither producer can exceed the
// combined cap, and both stay memory-bounded because push() stops appending ordinary
// diagnostics once the budget is spent). Terminal js.limit.* diagnostics are always kept.
// A nonnegative-integer budget; 0 keeps no ordinary diagnostics.
export function makeBudget(maxDiagnostics, path) {
    const list = [];
    const cap = (typeof maxDiagnostics === "number" && isFinite(maxDiagnostics) && maxDiagnostics >= 0 && Math.floor(maxDiagnostics) === maxDiagnostics) ? maxDiagnostics : 4096;
    let ordinary = 0, capNoted = false;
    function isTerminal(code) { return code.lastIndexOf("js.limit.", 0) === 0; }
    return {
        list: list,
        // push a diagnostic through the shared budget. Returns true if it was appended.
        push: function (severity, code, message, range) {
            if (!isTerminal(code)) {
                if (ordinary >= cap) {
                    if (!capNoted) { capNoted = true; list.push(newDiagnostic("error", "js.limit.diagnostics", "diagnostic limit exceeded (" + cap + ")", path, range || null)); }
                    return false;
                }
                ordinary++;
            }
            list.push(newDiagnostic(severity, code, message, path, range || null));
            return true;
        },
        // Speculation support: capture the budget state, then roll back any diagnostics (and the
        // spent-ordinary count) emitted since, so a failed speculative parse leaves no trace.
        mark: function () { return { len: list.length, ordinary: ordinary, capNoted: capNoted }; },
        reset: function (m) { list.length = m.len; ordinary = m.ordinary; capNoted = m.capNoted; },
    };
}

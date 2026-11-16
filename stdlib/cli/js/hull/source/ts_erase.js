// hull:source:ts_erase - TypeScript type erasure over a TS-mode SourceUnit
// (docs/typescript_design.md sections 3.2 and 4.1).
//
// Public:
//   typeErasure(unit, bytes, path?) -> { ok: true, spans: [ {start, stop} ... ], refusals: [ diagnostic ... ] }
//                             | { ok: false, error }
//       `unit` is parse(bytes, { lang: "ts" }). `spans` are its typeSpans: half-open
//       1-based byte ranges of erasable type syntax, sorted and non-overlapping. `refusals` are
//       the unit's ts.* diagnostics plus one ts.unsupported per span that holds U+2028 / U+2029
//       (a JS line terminator: blanking it would change the lines ASI and positions see). A
//       unit that does not parse cleanly as JS once erased (any other error diagnostic) is the
//       caller's to reject through unit.valid.
//   erase(bytes, spans) -> { ok: true, bytes: Uint8Array } | { ok: false, error }
//       A copy of `bytes` with every span byte replaced by a space (0x20), CR and LF kept, so
//       byte offsets, line numbers and columns are unchanged. Refuses spans out of range,
//       unsorted, overlapping, or holding U+2028 / U+2029. Never raises.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { newDiagnostic } from "hull:source:diagnostic";

// U+2028 / U+2029 encode as E2 80 A8 / E2 80 A9. Returns the 1-based start of the first one
// that begins inside [start, stop), else 0.
function lineSeparatorIn(bytes, start, stop) {
    for (let i = start - 1; i < stop - 1; i++)
        if (bytes[i] === 0xe2 && bytes[i + 1] === 0x80 && (bytes[i + 2] === 0xa8 || bytes[i + 2] === 0xa9)) return i + 1;
    return 0;
}

// Check that `spans` is a sorted, non-overlapping list of in-range half-open ranges.
function checkSpans(spans, n) {
    if (!Array.isArray(spans)) return "spans is not an array";
    let last = 1;
    for (let k = 0; k < spans.length; k++) {
        const sp = spans[k];
        if (sp === null || typeof sp !== "object") return "span " + k + " is not an object";
        const a = sp.start, b = sp.stop;
        if (!Number.isInteger(a) || !Number.isInteger(b)) return "span " + k + " has a non-integer bound";
        if (a < last || b < a || b > n + 1) return "span " + k + " is out of order or out of range";
        last = b;
    }
    return null;
}

export function erase(bytes, spans) {
    const n = bytes.length;
    const bad = checkSpans(spans, n);
    if (bad !== null) return { ok: false, error: bad };
    for (let k = 0; k < spans.length; k++)
        if (lineSeparatorIn(bytes, spans[k].start, spans[k].stop)) return { ok: false, error: "span " + k + " holds a line separator" };
    const out = new Uint8Array(n);
    out.set(bytes);
    for (let k = 0; k < spans.length; k++)
        for (let i = spans[k].start - 1; i < spans[k].stop - 1; i++)
            if (out[i] !== 0x0a && out[i] !== 0x0d) out[i] = 0x20;
    return { ok: true, bytes: out };
}

export function typeErasure(unit, bytes, path) {
    if (!unit || !Array.isArray(unit.typeSpans) || !Array.isArray(unit.diagnostics))
        return { ok: false, error: "not a TypeScript-mode unit" };
    const spans = unit.typeSpans;
    const bad = checkSpans(spans, bytes.length);
    if (bad !== null) return { ok: false, error: bad };
    const refusals = [];
    for (let i = 0; i < unit.diagnostics.length; i++) {
        const d = unit.diagnostics[i];
        if (typeof d.code === "string" && d.code.substring(0, 3) === "ts.") refusals.push(d);
    }
    for (let k = 0; k < spans.length; k++) {
        const at = lineSeparatorIn(bytes, spans[k].start, spans[k].stop);
        if (at) refusals.push(newDiagnostic("error", "ts.unsupported",
            "a line separator (U+2028 / U+2029) inside type syntax is not supported", path || null, { start: at, stop: at + 3 }));
    }
    return { ok: true, spans: spans.slice(), refusals: refusals };
}

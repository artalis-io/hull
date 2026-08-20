// hull:source:frontend_entry - the SHIPPED entry the C frontend session loads (the production
// analog of the test-only hull:source:lextest). It imports the Slice-5 adapter and registers
// globalThis.__hull_frontend with the three production methods, matching the session's
// (ArrayBuffer src, path, options) call convention. The test-only __analyzeWithFailure / __mutate
// are NOT registered here.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

import { analyze, declarationSemantics, scope } from "hull:source:frontend_javascript";

globalThis.__hull_frontend = {
    // analyze(srcBytes, path, opts) -> facts
    analyze: function (srcBuf, path, opts) { return analyze(new Uint8Array(srcBuf), path || null, opts || {}); },
    // declarationSemantics(_, _, { declId }) -> record | { error }
    declarationSemantics: function (srcBuf, path, opts) { return declarationSemantics(opts && opts.declId); },
    // scope(_, _, { unitId }) -> { ok, bindings, refs } | { ok:false, error }
    scope: function (srcBuf, path, opts) { return scope(opts && opts.unitId); },
};

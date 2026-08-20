// hull:source:tests:frontend_probe - a TEST-ONLY authority probe for the JS tooling session.
//
// Under source/tests/, so the production cli-js registry generator (which excludes */tests/*)
// NEVER embeds it; only a test-specific registry includes it. It is loaded solely by the
// test-only C entry hl_js_gen_probe (compiled under HL_JS_GEN_TESTING); no production code
// path can reach it. It registers globalThis.__hull_frontend.probe using the SAME
// (srcBuf, path, opts) call convention the session uses for the production methods, so the
// authority claim is measured THROUGH the generation-manager session the frontend runs in.
//
// The method reads only `typeof` of candidate globals and reports strings; it holds no
// authority and mutates nothing. It PROVES the tooling runtime has minimal authority (no
// eval / Function / capability global). The distinct "an application module cannot be
// imported by tooling code" claim is proven at the C layer by hl_js_gen_probe_import (which
// requires the tooling loader's definitive "entry module not found"), not by a weak in-JS
// dynamic-import outcome.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Names that MUST be absent from a minimal-authority tooling session: dynamic-code
// constructors and every host/application capability global. `globalThis` is deliberately
// NOT listed - it is a legitimate intrinsic; a separate sanity field confirms it is real.
var FORBIDDEN = ["eval", "Function", "db", "fs", "http", "env", "crypto",
                 "process", "app", "hull", "require"];

function probe() {
    var present = [];
    for (var i = 0; i < FORBIDDEN.length; i++) {
        var name = FORBIDDEN[i];
        var t;
        try { t = typeof globalThis[name]; } catch (e) { t = "error"; }
        if (t !== "undefined") present.push(name);
    }
    return {
        schema_version: 1,
        // The authority verdict: forbidden names that leaked into the session (expect empty).
        forbidden_present: present,
        // Sanity: the probe really ran against the real global object.
        global_is_object: (typeof globalThis === "object"),
    };
}

globalThis.__hull_frontend = { probe: function (srcBuf, path, opts) { return probe(); } };

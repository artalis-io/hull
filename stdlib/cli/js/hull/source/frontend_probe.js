// hull:source:frontend_probe - a TEST-ONLY authority probe for the JS tooling session.
//
// Loaded ONLY by the test-only C entry hl_js_gen_probe (compiled under HL_JS_GEN_TESTING);
// no production code path ever loads this module or calls the probe method. It registers
// globalThis.__hull_frontend.probe using the SAME (srcBuf, path, opts) call convention the
// session uses for the production methods, so the authority claim is measured THROUGH the
// generation-manager session the frontend actually runs in - not a fresh or mocked context.
//
// The method reads only `typeof` of candidate globals and reports strings; it holds no
// authority itself and mutates nothing. It exists to PROVE the tooling runtime has minimal
// authority (no eval / Function / capability globals / application-module import).
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

    // Attempt to import a fake application module. A minimal loader must NOT synchronously
    // hand back a usable module: it either throws or yields a promise that never resolves to
    // a capability. We report the SHAPE of the attempt; the test asserts it is not a
    // synchronous capability load.
    var importOutcome;
    try {
        var p = import("hull:__fake_application_module__");
        importOutcome = (p && typeof p.then === "function") ? "promise" : "value";
    } catch (e) {
        importOutcome = "threw:" + ((e && e.name) ? e.name : "error");
    }

    return {
        schema_version: 1,
        // The authority verdict: forbidden names that leaked into the session (expect empty).
        forbidden_present: present,
        import_outcome: importOutcome,
        // Sanity: the probe really ran against the real global object.
        global_is_object: (typeof globalThis === "object"),
    };
}

globalThis.__hull_frontend = { probe: function (srcBuf, path, opts) { return probe(); } };

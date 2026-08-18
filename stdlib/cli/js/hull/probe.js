// hull:probe - Slice 1 crossing / security / limit probe (NOT a parser).
//
// Proves the restricted QuickJS tooling runtime end to end without any parser:
// the raw-byte source crossing (length-aware + NUL-safe), multi-module loading
// (it imports hull:_probe_util), the empty application-authority surface,
// options transport, the adversarial dynamic-code block, and every advertised
// limit. Throwaway scaffolding; Slice 2 replaces it with the real lexer/parser.
// Application JavaScript is never run here - only this trusted, audited,
// embedded bundle.
// SPDX-License-Identifier: AGPL-3.0-or-later
import { marker } from "hull:_probe_util";

// The authorities an ordinary Hull application runtime exposes -- NONE may exist here.
const FORBIDDEN = ["db", "fs", "http", "env", "crypto", "compute", "gpu", "ws", "app",
                   "require", "process", "eval", "Function", "std", "os", "fetch", "log"];

function analyze(srcBuf, path, opts) {
    const byteLength = (srcBuf && typeof srcBuf.byteLength === "number") ? srcBuf.byteLength : -1;
    const present = [];
    for (const k of FORBIDDEN) {
        if (typeof globalThis[k] !== "undefined") present.push(k);
    }
    // Byte fidelity check: report the first and last source byte so the C test can prove
    // the whole buffer (including embedded NUL) crossed intact.
    let firstByte = -1, lastByte = -1;
    if (byteLength > 0) {
        const view = new Uint8Array(srcBuf);
        firstByte = view[0];
        lastByte = view[byteLength - 1];
    }
    return {
        schema_version: 1,
        status: "ok",
        byte_length: byteLength,
        first_byte: firstByte,
        last_byte: lastByte,
        path: path || "",
        util_ok: marker() === "probe-util-ok",
        sandbox_clean: present.length === 0,
        present_authorities: present,
        options_echo: (opts && typeof opts.echo !== "undefined") ? opts.echo : null,
    };
}

// probeDynamic(): adversarial proof that dynamic code is unreachable, INCLUDING the
// prototype-reachable constructors that survive deleting the global eval / Function
// bindings. Every attempt must be blocked -- each dynamic-compile path funnels through
// the runtime's eval hook, which is never enabled in this session.
function probeDynamic() {
    const attempts = {};
    function tryIt(label, fn) {
        try { fn(); attempts[label] = "RAN"; }
        catch (_e) { attempts[label] = "blocked"; }
    }
    tryIt("global_eval", () => globalThis.eval("1 + 1"));
    tryIt("global_Function", () => globalThis.Function("return 1")());
    tryIt("object_ctor_ctor", () => ({}).constructor.constructor("return 1")());
    tryIt("fn_ctor", () => (function () {}).constructor("return 1")());
    tryIt("async_ctor", () => (async function () {}).constructor("return 1")());
    const labels = Object.keys(attempts);
    return {
        schema_version: 1,
        status: "ok",
        dynamic_attempts: attempts,
        all_blocked: labels.every((k) => attempts[k] === "blocked"),
    };
}

// Limit-exercising methods (the Slice-1 limit-contract tests drive these).
function spin() { for (;;) { /* burn the instruction budget */ } }
function recurse(n) { return recurse((n || 0) + 1); }          // stack budget
function hog() {                                               // heap budget
    const a = [];
    for (;;) { a.push(new Array(100000).fill(7)); }
}
function bigResult(opts) {                                     // result-size budget
    const n = (opts && opts.size) || 1000000;
    return { schema_version: 1, status: "ok", blob: "x".repeat(n) };
}
function boom() { throw new Error("probe boom"); }            // ordinary tooling throw

// Trusted tooling entries register on globalThis so the C bridge can reach them without
// module-namespace APIs (this VM is single-purpose + VFS-isolated).
globalThis.__hull_frontend = {
    analyze: analyze,
    probeDynamic: probeDynamic,
    spin: spin,
    recurse: recurse,
    hog: hog,
    bigResult: bigResult,
    boom: boom,
};

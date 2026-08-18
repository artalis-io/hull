// hull:probe — the Slice-1 crossing / security / lifecycle probe for the restricted
// QuickJS tooling runtime. NOT a parser: it proves (1) source bytes cross C -> QuickJS
// length-aware + NUL-safe, (2) a multi-module bundle loads from the cli-js VFS, (3) the
// tooling VM is stripped of every application authority, and (4) the C bridge can invoke a
// bundled entry and read back a validated JSON result. Slice 2 replaces this with the real
// frontend. Trusted, bundled, tooling-only.
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

// Trusted tooling entries register on globalThis so the C bridge can reach them without
// module-namespace APIs (this VM is single-purpose + VFS-isolated).
globalThis.__hull_frontend = { analyze: analyze };

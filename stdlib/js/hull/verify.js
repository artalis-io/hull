/*
 * hull:verify — Verify app signature
 *
 * Usage: hull verify [app_dir]
 *
 * Reads hull.sig, recomputes file hashes, and verifies Ed25519 signature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { sha256, ed25519Verify } from "hull:crypto";

const appDir = globalThis.arg?.[1] ?? ".";
const sigPath = `${appDir}/hull.sig`;

const sigData = tool.readFile(sigPath);
if (!sigData) {
    tool.stderr(`hull verify: no hull.sig found in ${appDir}\n`);
    tool.exit(1);
}

const sig = JSON.parse(sigData);
if (!sig || !sig.files || !sig.signature || !sig.public_key) {
    tool.stderr("hull verify: invalid hull.sig format\n");
    tool.exit(1);
}

// Recompute file hashes
const mismatches = [];
const missing = [];
const fileNames = Object.keys(sig.files);
for (const name of fileNames) {
    const expected = sig.files[name];
    const path = `${appDir}/${name}`;
    const data = tool.readFile(path);
    if (!data) {
        missing.push(name);
    } else {
        const actual = sha256(data);
        if (actual !== expected) {
            mismatches.push({ name, expected, actual });
        }
    }
}

// Report file issues
if (missing.length > 0) {
    tool.stderr("Missing files:\n");
    for (const name of missing) {
        tool.stderr(`  ${name}\n`);
    }
}
if (mismatches.length > 0) {
    tool.stderr("Modified files:\n");
    for (const m of mismatches) {
        tool.stderr(`  ${m.name}\n`);
        tool.stderr(`    expected: ${m.expected}\n`);
        tool.stderr(`    actual:   ${m.actual}\n`);
    }
}

// Verify Ed25519 signature
const payload = JSON.stringify({
    files: sig.files,
    manifest: sig.manifest,
});
const ok = ed25519Verify(payload, sig.signature, sig.public_key);

if (!ok) {
    tool.stderr("hull verify: FAILED — signature is invalid\n");
    tool.exit(1);
}

if (missing.length > 0 || mismatches.length > 0) {
    tool.stderr("hull verify: FAILED — files do not match signature\n");
    tool.exit(1);
}

console.log("hull verify: OK — all files verified, signature valid");

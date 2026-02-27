/*
 * hull:verify — Verify app signature (dual-layer)
 *
 * Usage: hull verify [options] [app_dir]
 *   --platform-key <file>    Platform public key (default: hardcoded gethull.dev key)
 *   --developer-key <file>   Developer public key (required for full verification)
 *
 * Reads package.sig (or hull.sig for backwards compat), verifies platform
 * and app layer Ed25519 signatures, and checks file hashes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { sha256, ed25519Verify } from "hull:crypto";

const GETHULL_DEV_PLATFORM_KEY = "0000000000000000000000000000000000000000000000000000000000000000";

// Parse CLI args
let appDir = ".";
let platformKeySource = null;
let developerKeySource = null;

const args = globalThis.arg ?? [];
for (let i = 1; i < args.length; i++) {
    if (args[i] === "--platform-key" && i + 1 < args.length) {
        platformKeySource = args[++i];
    } else if (args[i] === "--developer-key" && i + 1 < args.length) {
        developerKeySource = args[++i];
    } else if (!args[i].startsWith("-")) {
        appDir = args[i];
    }
}

function readKey(source) {
    if (!source) return null;
    const data = tool.readFile(source);
    if (data) {
        const match = data.match(/^([0-9a-f]+)/i);
        return match ? match[1] : null;
    }
    return null;
}

// Try package.sig first, fall back to hull.sig
let sigPath = `${appDir}/package.sig`;
let sigData = tool.readFile(sigPath);
let isLegacy = false;
if (!sigData) {
    sigPath = `${appDir}/hull.sig`;
    sigData = tool.readFile(sigPath);
    isLegacy = true;
}

if (!sigData) {
    tool.stderr(`hull verify: no package.sig or hull.sig found in ${appDir}\n`);
    tool.exit(1);
}

const sig = JSON.parse(sigData);
if (!sig || !sig.files || !sig.signature || !sig.public_key) {
    tool.stderr("hull verify: invalid signature format\n");
    tool.exit(1);
}

let issues = 0;

// ── Platform layer verification ────────────────────────────────────
if (sig.platform?.signature && sig.platform?.public_key) {
    const platformKeyHex = readKey(platformKeySource) ?? GETHULL_DEV_PLATFORM_KEY;

    if (sig.platform.public_key === platformKeyHex) {
        const platPayload = JSON.stringify(sig.platform.platforms);
        const platOk = ed25519Verify(platPayload, sig.platform.signature, sig.platform.public_key);

        if (platOk) {
            console.log("Platform layer: VALID (signed by Hull Platform)");
        } else {
            tool.stderr("Platform layer: FAILED — signature invalid\n");
            issues++;
        }
    } else {
        tool.stderr(`Platform layer: WARNING — key mismatch\n`);
        issues++;
    }

    if (sig.platform.platforms) {
        const archs = Object.keys(sig.platform.platforms).sort();
        console.log(`  Architectures: ${archs.join(", ")}`);
    }
} else if (!isLegacy) {
    tool.stderr("Platform layer: MISSING\n");
    issues++;
}

// ── App layer verification ─────────────────────────────────────────
const devKeyHex = readKey(developerKeySource);
if (devKeyHex && sig.public_key !== devKeyHex) {
    tool.stderr("App layer: WARNING — developer key mismatch\n");
    issues++;
}

let payload;
if (sig.binary_hash) {
    payload = JSON.stringify({
        binary_hash: sig.binary_hash,
        build: sig.build,
        files: sig.files,
        manifest: sig.manifest,
        platform: sig.platform,
        trampoline_hash: sig.trampoline_hash,
    });
} else {
    payload = JSON.stringify({
        files: sig.files,
        manifest: sig.manifest,
    });
}

const ok = ed25519Verify(payload, sig.signature, sig.public_key);
if (!ok) {
    tool.stderr("App layer: FAILED — signature is invalid\n");
    tool.exit(1);
}

if (sig.build) {
    console.log(`  Built with: ${sig.build.cc_version ?? sig.build.cc ?? "unknown"}`);
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

if (missing.length > 0) {
    tool.stderr("Missing files:\n");
    for (const name of missing) {
        tool.stderr(`  ${name}\n`);
    }
    issues += missing.length;
}
if (mismatches.length > 0) {
    tool.stderr("Modified files:\n");
    for (const m of mismatches) {
        tool.stderr(`  ${m.name}\n`);
        tool.stderr(`    expected: ${m.expected}\n`);
        tool.stderr(`    actual:   ${m.actual}\n`);
    }
    issues += mismatches.length;
}

if (issues > 0) {
    tool.stderr(`hull verify: FAILED — ${issues} issue(s) found\n`);
    tool.exit(1);
}

console.log("App layer: VALID");
console.log("");
console.log("hull verify: OK — all checks passed");

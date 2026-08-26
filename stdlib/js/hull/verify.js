/*
 * hull:verify - Verify app signature (dual-layer + v0.1.3 gethull)
 *
 * NOTE: the CLI `hull verify` always runs the Lua implementation
 * (stdlib/cli/lua/hull/verify.lua) - this JS module is kept for
 * parity / library use only. The Lua version checks the v0.1.3
 * gethull layer using a tool.platform_pubkey() binding; that
 * binding isn't exposed to the JS tool runtime today, so this
 * module gates the gethull layer on --no-verify-platform or
 * --gethull-key <file> instead.
 *
 * Usage: hull verify [options] [app_dir]
 *   --platform-key <file>    Per-app platform key (v0.1.2 layer)
 *   --developer-key <file>   Developer public key (app layer)
 *   --gethull-key <file>     gethull.dev pubkey for the v0.1.3 layer.
 *                            OVERRIDE semantics here (since the JS
 *                            runtime can't read the embedded
 *                            HL_PLATFORM_PUBKEY_HEX) - the file's
 *                            key replaces what would be the embedded
 *                            one. The Lua verifier's same-named flag
 *                            is stricter (cross-check: file MUST
 *                            equal embedded, AND signature must
 *                            verify). Documented asymmetry.
 *   --no-verify-platform     Skip the v0.1.3 gethull layer
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

import { sha256, ed25519Verify } from "hull:crypto";

// All-zeros sentinel: dev hulls / forks built without their own
// pinned platform pubkey. `hull build` writes this when no real key
// has been embedded yet - comparing against it lets us silently
// short-circuit warnings rather than nag the user about a key they
// never opted into.
const ZERO_PUBKEY_HEX =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Parse CLI args
let appDir = ".";
let platformKeySource = null;
let developerKeySource = null;
let gethullKeySource = null;
let noVerifyPlatform = false;

const args = globalThis.arg ?? [];
for (let i = 1; i < args.length; i++) {
    if (args[i] === "--platform-key" && i + 1 < args.length) {
        platformKeySource = args[++i];
    } else if (args[i] === "--developer-key" && i + 1 < args.length) {
        developerKeySource = args[++i];
    } else if (args[i] === "--gethull-key" && i + 1 < args.length) {
        gethullKeySource = args[++i];
    } else if (args[i] === "--no-verify-platform") {
        noVerifyPlatform = true;
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

let sig;
try { sig = JSON.parse(sigData); }
catch (e) {
    tool.stderr("hull verify: invalid JSON in signature file\n");
    tool.exit(1);
}
if (!sig || typeof sig !== "object" ||
    typeof sig.files     !== "object" || sig.files === null ||
    typeof sig.signature !== "string" ||
    typeof sig.public_key !== "string") {
    tool.stderr("hull verify: invalid signature format\n");
    tool.exit(1);
}

let issues = 0;

// ── gethull layer (v0.1.3) ─────────────────────────────────────────
const gethullKeyHex = readKey(gethullKeySource);
if (noVerifyPlatform) {
    console.log("gethull layer: SKIPPED (--no-verify-platform)");
} else if (!gethullKeyHex) {
    console.log("gethull layer: SKIPPED (--gethull-key not provided)");
} else if (sig.platform?.gethull?.manifest && sig.platform?.gethull?.signature) {
    const gethullOk = ed25519Verify(
        sig.platform.gethull.manifest,
        sig.platform.gethull.signature,
        gethullKeyHex);
    if (gethullOk) {
        console.log("gethull layer: VALID (signed by gethull.dev)");
    } else {
        tool.stderr("gethull layer: FAILED - signature invalid\n");
        issues++;
    }
} else if (!isLegacy) {
    tool.stderr("gethull layer: MISSING - package.sig has no platform.gethull block\n");
    issues++;
}

// ── v0.1.2 per-app platform layer (self-consistency only) ──────────
// The pinning role moved to the v0.1.3 gethull layer above.
if (sig.platform?.signature && sig.platform?.public_key) {
    const platformKeyHex = readKey(platformKeySource);
    if (platformKeyHex && platformKeyHex !== ZERO_PUBKEY_HEX &&
        sig.platform.public_key !== platformKeyHex) {
        tool.stderr("Platform layer: WARNING - key does not match --platform-key\n");
        issues++;
    }

    const platPayload = JSON.stringify(sig.platform.platforms);
    const platOk = ed25519Verify(platPayload, sig.platform.signature, sig.platform.public_key);
    if (platOk) {
        console.log("Platform layer: VALID (self-consistent)");
    } else {
        tool.stderr("Platform layer: FAILED - signature invalid\n");
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
    tool.stderr("App layer: WARNING - developer key mismatch\n");
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
    tool.stderr("App layer: FAILED - signature is invalid\n");
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
    if (name.includes("..") || name.startsWith("/")) { issues++; tool.stderr("Suspicious file path: " + name + "\n"); continue; }
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
    tool.stderr(`hull verify: FAILED - ${issues} issue(s) found\n`);
    tool.exit(1);
}

console.log("App layer: VALID");
console.log("");
console.log("hull verify: OK - all checks passed");

/*
 * embed_c — reference native host for the libhull no-runtime flavor.
 *
 * This program links ONLY build/libhull.a (the runtime-free Hull core:
 * kernel sandbox, capability layer, WASM/GPU compute, signed-artifact
 * plumbing) plus vendor/keel/libkeel.a. Neither Lua nor QuickJS is
 * linked. The host owns main() and drives the Hull core directly:
 *
 *   1. hl_sandbox_apply_pledge()  — phase-1 syscall reduction
 *   2. build an HlSandboxPolicy by hand (a native host is trusted; it
 *      does not parse an app.manifest — it declares policy in C)
 *   3. hl_sandbox_apply()          — phase-2 default-deny sandbox
 *   4. hl_cap_fs_write / _read     — capability-mediated I/O
 *   5. hl_cap_crypto_sha256        — capability-mediated crypto
 *   6. hl_release_io_platform /    — signed-artifact / SBOM identity
 *      hl_module_registry_count
 *
 * Its real job is to be a LINK witness: if this binary links with no
 * undefined symbols, the core object subset in build/libhull.a is
 * genuinely runtime-free. `make embed-c-smoke` builds and runs it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "hull/sandbox.h"
#include "hull/cap/fs.h"
#include "hull/cap/crypto.h"
#include "hull/module_registry.h"
#include "hull/release_io.h"

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    printf("embed_c: libhull no-runtime host\n");

    /* A native host owns its working directory. Use a fresh temp dir so
     * the sandbox's read/write allowlist is a single known path. */
    char tmpl[] = "/tmp/hull-embed-c.XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    if (chdir(dir) != 0) {
        perror("chdir");
        return 1;
    }

    /* ── 1. phase-1 sandbox (pledge) ──────────────────────────────── */
    check(hl_sandbox_apply_pledge() == 0, "phase-1 pledge applied");

    /* ── 2. build a policy in C (no app.manifest) ─────────────────── */
    /* Policy fs paths are app_dir-relative — the same contract as a
     * manifest's fs.read/fs.write ("data/", "uploads/"). "." grants the
     * app_dir (our temp cwd) itself. Absolute paths are rejected by the
     * sandbox path resolver, exactly as for a real manifest. */
    const char *rw[] = { "." };
    HlSandboxPolicy policy;
    memset(&policy, 0, sizeof(policy));
    policy.fs_read        = rw;
    policy.fs_read_count  = 1;
    policy.fs_write       = rw;
    policy.fs_write_count = 1;
    policy.network_inbound  = 0;
    policy.network_outbound = 0;
    policy.wx_enforced      = 1;

    /* ── 3. phase-2 sandbox (default-deny) ────────────────────────── */
    int sb = hl_sandbox_apply(&policy, dir, NULL, NULL, NULL, NULL);
    check(sb == 0, "phase-2 sandbox applied");

    /* ── 4. capability-mediated filesystem I/O ────────────────────── */
    HlFsConfig fs = { .base_dir = dir, .base_len = strlen(dir) };
    const char *err = NULL;
    const char *payload = "hull embed_c capability write\n";

    int w = hl_cap_fs_write(&fs, "note.txt", payload, strlen(payload), &err);
    check(w == 0, "hl_cap_fs_write under sandbox");

    char buf[128];
    int64_t n = hl_cap_fs_read(&fs, "note.txt", buf, sizeof(buf), &err);
    check(n == (int64_t)strlen(payload) &&
          memcmp(buf, payload, (size_t)n) == 0,
          "hl_cap_fs_read round-trips");

    /* traversal must still be rejected by the cap layer */
    int bad = hl_cap_fs_exists(&fs, "../escape", &err);
    check(bad == -1, "hl_cap_fs rejects path traversal");

    /* ── 5. capability-mediated crypto ────────────────────────────── */
    uint8_t digest[32];
    int c = hl_cap_crypto_sha256("abc", 3, digest);
    /* SHA-256("abc") = ba7816bf 8f01cfea ... */
    check(c == 0 && digest[0] == 0xba && digest[1] == 0x78 &&
          digest[2] == 0x16 && digest[3] == 0xbf,
          "hl_cap_crypto_sha256(\"abc\") matches known vector");

    /* ── 6. signed-artifact / SBOM identity ───────────────────────── */
    const char *plat = hl_release_io_platform();
    check(plat != NULL && plat[0] != '\0', "hl_release_io_platform reports arch");
    size_t mods = hl_module_registry_count();
    check(mods > 0, "hl_module_registry_count populated");
    printf("  platform=%s modules=%zu\n", plat ? plat : "(null)", mods);

    hl_cap_fs_delete(&fs, "note.txt", &err);
    rmdir(dir);

    printf("embed_c: %s (%d failure%s)\n",
           failures == 0 ? "OK" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

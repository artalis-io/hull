/*
 * embed_c - reference native host for the libhull no-runtime flavor.
 *
 * Links ONLY build/libhull.a (the runtime-free Hull core) plus
 * vendor/keel/libkeel.a. Neither Lua nor QuickJS is linked. The host
 * owns main() and drives the core exclusively through the stable
 * embedding ABI in <hull/embed.h> - it never includes an internal Hull
 * header. That is the whole point of the ABI: an embedder targets
 * hl_embed_* and is insulated from the internal sandbox / capability
 * struct layout.
 *
 * It doubles as a link witness: if this binary links with no undefined
 * symbols, the core object subset in build/libhull.a (including embed.o)
 * is genuinely runtime-free. `make embed-c-smoke` builds and runs it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "hull/embed.h"   /* the ONLY Hull header a native host needs */

static int failures = 0;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void)
{
    printf("embed_c: libhull no-runtime host (ABI v%d)\n",
           hl_embed_abi_version());

    /* A native host owns its working directory. Use a fresh temp dir so
     * the sandbox's read/write allowlist is a single known path. */
    char tmpl[] = "/tmp/hull-embed-c.XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }

    HlEmbed *e = hl_embed_new(dir);
    check(e != NULL, "hl_embed_new(app_dir)");
    if (!e) return 1;

    /* ── 1. phase-1 sandbox (pledge) ──────────────────────────────── */
    check(hl_embed_sandbox_phase1(e) == 0, "phase-1 sandbox applied");

    /* ── 2. build policy in C (app_dir-relative, like a manifest) ────
     * "." is the base-root grant: read/write the whole app dir + descendants
     * (sec. 6). Base-relative, never absolute. */
    check(hl_embed_allow_read(e, ".") == 0,  "allow_read(\".\")");
    check(hl_embed_allow_write(e, ".") == 0, "allow_write(\".\")");
    hl_embed_allow_network(e, 0, 0);

    /* fail-closed: capabilities must refuse before seal */
    const char *pre_err = NULL;
    int pre = hl_embed_fs_exists(e, "note.txt", &pre_err);
    check(pre == -1 && pre_err != NULL, "capabilities fail closed before seal");

    /* ── 3. phase-2 sandbox (default-deny) - MUST check ───────────── */
    int sealed = hl_embed_seal(e, NULL);
    check(sealed == 0, "hl_embed_seal applied sandbox");
    if (sealed != 0) { hl_embed_free(e); return 1; }

    /* ── 4. capability-mediated filesystem I/O ────────────────────── */
    const char *err = NULL;
    const char *payload = "hull embed_c capability write\n";

    int w = hl_embed_fs_write(e, "note.txt", payload, strlen(payload), &err);
    check(w == 0, "hl_embed_fs_write under sandbox");

    char buf[128];
    int64_t n = hl_embed_fs_read(e, "note.txt", buf, sizeof(buf), &err);
    check(n == (int64_t)strlen(payload) &&
          memcmp(buf, payload, (size_t)n) == 0,
          "hl_embed_fs_read round-trips");

    /* traversal must still be rejected by the cap layer */
    int bad = hl_embed_fs_exists(e, "../escape", &err);
    check(bad == -1, "hl_embed_fs rejects path traversal");

    /* ── 5. capability-mediated crypto (stateless) ────────────────── */
    uint8_t digest[32];
    int c = hl_embed_sha256("abc", 3, digest);
    /* SHA-256("abc") = ba7816bf 8f01cfea ... */
    check(c == 0 && digest[0] == 0xba && digest[1] == 0x78 &&
          digest[2] == 0x16 && digest[3] == 0xbf,
          "hl_embed_sha256(\"abc\") matches known vector");

    /* ── 6. signed-artifact / SBOM identity ───────────────────────── */
    const char *plat = hl_embed_platform();
    check(plat != NULL && plat[0] != '\0', "hl_embed_platform reports arch");
    size_t mods = hl_embed_module_count();
    check(mods > 0, "hl_embed_module_count populated");
    printf("  platform=%s modules=%zu\n", plat ? plat : "(null)", mods);

    hl_embed_fs_delete(e, "note.txt", &err);
    hl_embed_free(e);
    rmdir(dir);

    printf("embed_c: %s (%d failure%s)\n",
           failures == 0 ? "OK" : "FAILED",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

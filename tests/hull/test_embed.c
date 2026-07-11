/*
 * test_embed.c — unit tests for the libhull embedding ABI (hull/embed.h).
 *
 * Covers the surface that is safe to exercise inside the shared test
 * runner: version, handle construction, policy limits, the fail-closed
 * guard, stateless crypto, identity, and NULL-safety.
 *
 * The SEALED path (hl_embed_sandbox_phase1 / hl_embed_seal) is
 * deliberately NOT tested here — hl_embed_seal applies a real, on macOS
 * irreversible, kernel sandbox to the calling process, which would
 * restrict the rest of the runner. That path is covered end-to-end by
 * the standalone `make embed-c-smoke` host (examples/embed_c), which runs
 * as its own process.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/embed.h"
#include "hull/embed_internal.h"

#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

UTEST(embed, abi_version)
{
    ASSERT_EQ(hl_embed_abi_version(), HL_EMBED_ABI_VERSION);
    ASSERT_EQ(HL_EMBED_ABI_VERSION, 1);
}

UTEST(embed, new_rejects_bad_args)
{
    ASSERT_TRUE(hl_embed_new(NULL) == NULL);
    ASSERT_TRUE(hl_embed_new("relative/path") == NULL);  /* not absolute */
    ASSERT_TRUE(hl_embed_new("") == NULL);

    HlEmbed *e = hl_embed_new("/tmp");
    ASSERT_TRUE(e != NULL);
    hl_embed_free(e);
    hl_embed_free(NULL);  /* NULL-safe */
}

UTEST(embed, allow_path_limits)
{
    HlEmbed *e = hl_embed_new("/tmp");
    ASSERT_TRUE(e != NULL);

    /* 32 entries accepted, 33rd rejected (HL_MANIFEST_MAX_PATHS). */
    for (int i = 0; i < 32; i++)
        ASSERT_EQ(hl_embed_allow_read(e, "data"), 0);
    ASSERT_EQ(hl_embed_allow_read(e, "data"), -1);

    for (int i = 0; i < 32; i++)
        ASSERT_EQ(hl_embed_allow_write(e, "out"), 0);
    ASSERT_EQ(hl_embed_allow_write(e, "out"), -1);

    /* NULL args rejected without crashing. */
    ASSERT_EQ(hl_embed_allow_read(e, NULL), -1);
    ASSERT_EQ(hl_embed_allow_write(e, NULL), -1);
    ASSERT_EQ(hl_embed_allow_read(NULL, "x"), -1);

    hl_embed_free(e);
}

UTEST(embed, fail_closed_before_seal)
{
    HlEmbed *e = hl_embed_new("/tmp");
    ASSERT_TRUE(e != NULL);

    char buf[16];
    const char *err;

    err = NULL;
    ASSERT_EQ(hl_embed_fs_exists(e, "x", &err), -1);
    ASSERT_TRUE(err != NULL);

    err = NULL;
    ASSERT_EQ(hl_embed_fs_write(e, "x", "y", 1, &err), -1);
    ASSERT_TRUE(err != NULL);

    err = NULL;
    ASSERT_EQ(hl_embed_fs_read(e, "x", buf, sizeof(buf), &err), -1);
    ASSERT_TRUE(err != NULL);

    err = NULL;
    ASSERT_EQ(hl_embed_fs_delete(e, "x", &err), -1);
    ASSERT_TRUE(err != NULL);

    hl_embed_free(e);
}

UTEST(embed, null_safety)
{
    char buf[16];
    const char *err = NULL;

    ASSERT_EQ(hl_embed_seal(NULL, NULL), -1);
    ASSERT_EQ(hl_embed_sandbox_phase1(NULL), -1);
    ASSERT_EQ(hl_embed_fs_read(NULL, "x", buf, sizeof(buf), &err), -1);
    ASSERT_EQ(hl_embed_fs_write(NULL, "x", "y", 1, &err), -1);
    ASSERT_EQ(hl_embed_fs_exists(NULL, "x", &err), -1);
    ASSERT_EQ(hl_embed_fs_delete(NULL, "x", &err), -1);

    /* void setters must be NULL-safe (no crash, no effect). */
    hl_embed_allow_network(NULL, 1, 1);
    hl_embed_allow_gpu(NULL, 1);
    hl_embed_allow_tui(NULL, 1);
}

UTEST(embed, sha256_known_vector)
{
    /* SHA-256("abc") per FIPS 180-4. */
    static const uint8_t expect[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad,
    };
    uint8_t out[32];
    ASSERT_EQ(hl_embed_sha256("abc", 3, out), 0);
    ASSERT_EQ(memcmp(out, expect, 32), 0);
}

UTEST(embed, identity)
{
    const char *plat = hl_embed_platform();
    ASSERT_TRUE(plat != NULL);
    ASSERT_TRUE(plat[0] != '\0');
    ASSERT_TRUE(hl_embed_module_count() > 0);
}

/*
 * Death test: after hl_embed_seal(), the base_dir the capability layer
 * reads on every call must live in a read-only mapping. Fork a child,
 * seal, then write to that mapping — the child MUST die with SIGSEGV /
 * SIGBUS. Runs in a child because hl_embed_seal also applies the (on
 * macOS irreversible) kernel sandbox to the calling process.
 *
 * Child exit codes: 42 = sandbox unavailable in this environment (soft
 * skip), 43 = sealed-flag wrong, 44 = base_dir NULL, 0 = write did NOT
 * fault (which is a failure — the page was writable).
 */
UTEST(embed, sealed_base_dir_is_readonly)
{
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        HlEmbed *e = hl_embed_new("/tmp");
        if (!e) _exit(44);
        if (hl_embed_is_sealed(e) != 0) _exit(43);
        hl_embed_allow_read(e, ".");
        if (hl_embed_seal(e, NULL) != 0) _exit(42);   /* no kernel sandbox here */
        if (hl_embed_is_sealed(e) != 1) _exit(43);

        const char *bd = hl_embed_base_dir(e);
        if (!bd) _exit(44);

        /* Restore default fault handlers so the fault terminates the child
         * (WIFSIGNALED) instead of being caught by a sanitizer handler that
         * prints and _exit(1)s — same rationale as test_seal_arena. */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);

        /* Deliberately cast away const to prove the mapping is RO. */
        char *w = (char *)(uintptr_t)bd;
        w[0] = 'Z';           /* expected: SIGSEGV / SIGBUS */
        _exit(0);             /* reached only if the page was writable */
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        /* Kernel sandbox couldn't be applied (restricted CI sandbox);
         * the RO-arena guarantee is still covered by test_seal_arena. */
        UTEST_SKIP("kernel sandbox unavailable in this environment");
    }

    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);
}

UTEST_MAIN();

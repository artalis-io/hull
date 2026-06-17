/*
 * test_seal_arena.c — Tests for the boot-time sealed arena.
 *
 * Covers:
 *   1. init / destroy lifecycle (success + failure cases)
 *   2. allocation: alignment, overflow guards, capacity
 *   3. seal transition: read still works, alloc fails
 *   4. process-isolated death test: writes to sealed memory SIGSEGV
 *   5. zero-size + NULL guards
 *   6. round-trip via strdup / memdup
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/seal_arena.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── lifecycle ─────────────────────────────────────────────────────── */

UTEST(seal_arena, init_destroy_roundtrip)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, "test"));
    ASSERT_TRUE(a.base != NULL);
    ASSERT_TRUE(a.capacity >= 4096);
    ASSERT_EQ((size_t)0, a.used);
    ASSERT_EQ(0, a.sealed);
    hl_seal_arena_destroy(&a);
    /* destroy zeroes the struct */
    ASSERT_TRUE(a.base == NULL);
    ASSERT_EQ((size_t)0, a.capacity);
}

UTEST(seal_arena, init_zero_size_rounds_up_to_one_page)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 0, NULL));
    ASSERT_TRUE(a.capacity >= 4096); /* page size on every supported platform */
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, init_null_arena_fails)
{
    ASSERT_EQ(-1, hl_seal_arena_init(NULL, 4096, "x"));
}

UTEST(seal_arena, destroy_null_is_safe)
{
    hl_seal_arena_destroy(NULL); /* must not crash */
    HlSealArena zero = {0};
    hl_seal_arena_destroy(&zero); /* must not crash when base==NULL */
}

/* ── allocation ────────────────────────────────────────────────────── */

UTEST(seal_arena, alloc_basic)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    void *p = hl_seal_arena_alloc(&a, 100, 8);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0u, (uintptr_t)p & 7u); /* 8-aligned */
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_alignment_respected)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    (void)hl_seal_arena_alloc(&a, 1, 1);                /* misalign */
    void *p16 = hl_seal_arena_alloc(&a, 64, 16);
    ASSERT_EQ(0u, (uintptr_t)p16 & 15u);
    void *p32 = hl_seal_arena_alloc(&a, 64, 32);
    ASSERT_EQ(0u, (uintptr_t)p32 & 31u);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_non_power_of_two_align)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 16, 3) == NULL);
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 16, 5) == NULL);
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 16, 0) == NULL);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_zero_size)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 0, 8) == NULL);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_returns_null_when_oom)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(hl_seal_arena_alloc(&a, a.capacity + 1, 8) == NULL);
    /* Filling exactly: should succeed */
    void *p = hl_seal_arena_alloc(&a, a.capacity, 1);
    ASSERT_TRUE(p != NULL);
    /* One more byte: should fail */
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 1, 1) == NULL);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_rejects_overflow_size)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(hl_seal_arena_alloc(&a, SIZE_MAX, 8) == NULL);
    ASSERT_TRUE(hl_seal_arena_alloc(&a, SIZE_MAX - 8, 16) == NULL);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, alloc_null_arena_returns_null)
{
    ASSERT_TRUE(hl_seal_arena_alloc(NULL, 16, 8) == NULL);
}

/* ── strdup / memdup ──────────────────────────────────────────────── */

UTEST(seal_arena, strdup_copies_bytes_and_nul_terminates)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    const char *src = "hello world";
    char *dst = hl_seal_arena_strdup(&a, src);
    ASSERT_TRUE(dst != NULL);
    ASSERT_STREQ(src, dst);
    ASSERT_TRUE(dst != src); /* copy, not alias */
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, strdup_null_returns_null)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_TRUE(hl_seal_arena_strdup(&a, NULL) == NULL);
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, memdup_binary_safe)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    const unsigned char src[] = { 0x00, 0xff, 0x42, 0x00, 0x13 };
    void *dst = hl_seal_arena_memdup(&a, src, sizeof src);
    ASSERT_TRUE(dst != NULL);
    ASSERT_EQ(0, memcmp(src, dst, sizeof src));
    hl_seal_arena_destroy(&a);
}

/* ── seal lifecycle ────────────────────────────────────────────────── */

UTEST(seal_arena, seal_blocks_subsequent_allocs)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    char *p = hl_seal_arena_strdup(&a, "before-seal");
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0, hl_seal_arena_seal(&a));
    ASSERT_EQ(1, hl_seal_arena_is_sealed(&a));

    /* alloc must fail post-seal */
    ASSERT_TRUE(hl_seal_arena_alloc(&a, 1, 1) == NULL);
    ASSERT_TRUE(hl_seal_arena_strdup(&a, "after") == NULL);

    /* reads must still work */
    ASSERT_STREQ("before-seal", p);

    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, double_seal_returns_error)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_EQ(0, hl_seal_arena_seal(&a));
    ASSERT_EQ(-1, hl_seal_arena_seal(&a)); /* re-seal is a programming bug */
    hl_seal_arena_destroy(&a);
}

UTEST(seal_arena, is_sealed_handles_null)
{
    ASSERT_EQ(0, hl_seal_arena_is_sealed(NULL));
    HlSealArena zero = {0};
    ASSERT_EQ(0, hl_seal_arena_is_sealed(&zero));
}

UTEST(seal_arena, destroy_after_seal_works)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    ASSERT_EQ(0, hl_seal_arena_seal(&a));
    hl_seal_arena_destroy(&a);
    ASSERT_TRUE(a.base == NULL);
}

/* ── death test: page protection actually works ───────────────────── */

/* Fork-and-segfault test. The child writes to sealed memory; the
 * parent waits and asserts the child died with SIGSEGV (or SIGBUS on
 * some platforms — kernel's choice for protection violations). If the
 * mprotect didn't take effect, the child would write happily and exit
 * 0, and the test would fail. This is the only way to verify that the
 * OS protection is REAL without crashing the test runner. */
UTEST(seal_arena, write_after_seal_faults)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    char *p = hl_seal_arena_strdup(&a, "do-not-overwrite");
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ(0, hl_seal_arena_seal(&a));

    pid_t pid = fork();
    if (pid == 0) {
        /* child: write into the sealed page, exit 0 if it somehow
         * succeeded (which should never happen). */
        p[0] = 'X';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    /* SIGSEGV on Linux/macOS for write to read-only pages. SIGBUS on
     * some BSDs / older kernels. Either is correct evidence that the
     * mprotect actually took effect. */
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    hl_seal_arena_destroy(&a);
}

/* ── diagnostics ──────────────────────────────────────────────────── */

UTEST(seal_arena, diagnostics_report_usage)
{
    HlSealArena a;
    ASSERT_EQ(0, hl_seal_arena_init(&a, 4096, NULL));
    size_t cap = hl_seal_arena_capacity(&a);
    ASSERT_TRUE(cap >= 4096);
    ASSERT_EQ((size_t)0, hl_seal_arena_used(&a));
    ASSERT_EQ(cap, hl_seal_arena_remaining(&a));

    (void)hl_seal_arena_alloc(&a, 100, 8);
    ASSERT_EQ((size_t)100, hl_seal_arena_used(&a));
    ASSERT_EQ(cap - 100, hl_seal_arena_remaining(&a));

    hl_seal_arena_destroy(&a);
}

UTEST_MAIN()

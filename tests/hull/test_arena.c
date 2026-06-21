/*
 * test_arena.c — Tests for Hull's arena lifetime helpers.
 *
 * Covers the scoped-scratch primitives layered on the sh_arena bump
 * allocator: hl_arena_mark / hl_arena_rewind (savepoint + rewind for
 * nested SCRATCH scopes inside one REQUEST arena) and the arena-copy
 * helpers hl_arena_strdup / hl_arena_memdup. These exercise the
 * lifetime-class contract: allocations after a mark are invalidated by
 * the matching rewind, and the arena buffer is reused afterwards.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/alloc.h"
#include <sh_arena.h>
#include <string.h>

/* Create via the Hull wrapper with a NULL allocator (== sh_arena_create),
 * so the test covers the same construction path callers use. */
static SHArena *make(void) { return hl_arena_create(NULL, 4096); }

UTEST(hl_arena, mark_is_used_offset)
{
    SHArena *a = make();
    ASSERT_TRUE(a != NULL);
    ASSERT_EQ(hl_arena_mark(a), (size_t)0);
    (void)sh_arena_alloc(a, 64);
    ASSERT_EQ(hl_arena_mark(a), sh_arena_used(a));
    ASSERT_TRUE(hl_arena_mark(a) >= 64);
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, rewind_reuses_space)
{
    SHArena *a = make();
    size_t base = hl_arena_mark(a);

    void *p1 = sh_arena_alloc(a, 128);
    ASSERT_TRUE(p1 != NULL);
    size_t after = hl_arena_mark(a);
    ASSERT_TRUE(after > base);

    /* Rewind to the savepoint: the next allocation must reuse the same
     * bytes p1 occupied (proves the scope was reclaimed). */
    hl_arena_rewind(a, base);
    ASSERT_EQ(hl_arena_mark(a), base);

    void *p2 = sh_arena_alloc(a, 128);
    ASSERT_EQ(p1, p2);
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, nested_scopes)
{
    SHArena *a = make();
    void *outer = sh_arena_alloc(a, 32);
    ASSERT_TRUE(outer != NULL);

    size_t scope = hl_arena_mark(a);
    void *inner = sh_arena_alloc(a, 256);
    ASSERT_TRUE(inner != NULL);
    ASSERT_TRUE(inner != outer);
    hl_arena_rewind(a, scope);

    /* Outer allocation is untouched; inner space is reclaimed. */
    void *again = sh_arena_alloc(a, 16);
    ASSERT_EQ(again, inner);   /* reused the inner scope's start */
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, rewind_forward_is_noop)
{
    SHArena *a = make();
    (void)sh_arena_alloc(a, 64);
    size_t used = hl_arena_mark(a);
    /* A mark larger than the high-water mark (stale / wrong-arena) must
     * not expand used and expose uninitialised bytes. */
    hl_arena_rewind(a, used + 4096);
    ASSERT_EQ(hl_arena_mark(a), used);
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, strdup_copies_into_arena)
{
    SHArena *a = make();
    const char *src = "capability";
    char *copy = hl_arena_strdup(a, src);
    ASSERT_TRUE(copy != NULL);
    ASSERT_TRUE(copy != src);              /* distinct storage */
    ASSERT_STREQ(copy, src);               /* same contents, NUL-terminated */
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, memdup_copies_bytes)
{
    SHArena *a = make();
    const unsigned char src[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    unsigned char *copy = hl_arena_memdup(a, src, sizeof(src));
    ASSERT_TRUE(copy != NULL);
    ASSERT_TRUE((void *)copy != (void *)src);
    ASSERT_EQ(0, memcmp(copy, src, sizeof(src)));
    /* n == 0 returns a non-crashing result (NULL or empty alloc). */
    (void)hl_arena_memdup(a, src, 0);
    hl_arena_free(NULL, a);
}

UTEST(hl_arena, null_safety)
{
    ASSERT_EQ(hl_arena_mark(NULL), (size_t)0);
    hl_arena_rewind(NULL, 0);                       /* no crash */
    ASSERT_EQ(hl_arena_strdup(NULL, "x"), (char *)NULL);
    ASSERT_EQ(hl_arena_memdup(NULL, "x", 1), (void *)NULL);

    SHArena *a = make();
    ASSERT_EQ(hl_arena_strdup(a, NULL), (char *)NULL);
    ASSERT_EQ(hl_arena_memdup(a, NULL, 4), (void *)NULL);
    hl_arena_free(NULL, a);
}

UTEST_MAIN();

/*
 * test_smtp_inflight.c - per-server in-flight SMTP registry (the shutdown sweep).
 *
 * Pins: add/remove/count bookkeeping; the sweep releases every remaining node
 * EXACTLY ONCE; a node removed on the normal path is NOT released by the sweep
 * (no double-release); each node is unlinked before its release runs, so a
 * release that frees the node's own storage is safe (ASan/UBSan proves it).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/cap/smtp_inflight.h"

#include <stdlib.h>
#include <string.h>

/* A fake async-op that embeds the registry node, mirroring the binding's real
 * struct. Its release frees the heap-allocated op, so ASan catches a
 * double-release (double-free) or a missed release (leak, on Linux LSan). */
typedef struct {
    HlSmtpInflightNode node;
    int                id;
} FakeOp;

static int g_released;          /* total release() invocations */
static int g_last_released_id;

static void fake_release(void *owner)
{
    FakeOp *op = (FakeOp *)owner;
    g_released++;
    g_last_released_id = op->id;
    free(op);                    /* frees the storage embedding node - unlink-first is required */
}

static FakeOp *make_op(int id)
{
    FakeOp *op = (FakeOp *)calloc(1, sizeof *op);
    op->id = id;
    return op;
}

UTEST(smtp_inflight, empty_sweep_is_zero)
{
    HlSmtpInflight r; hl_smtp_inflight_init(&r);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);
    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 0);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);
}

UTEST(smtp_inflight, add_increments_sweep_releases_all_once)
{
    g_released = 0;
    HlSmtpInflight r; hl_smtp_inflight_init(&r);

    for (int i = 0; i < 5; i++) {
        FakeOp *op = make_op(i);
        hl_smtp_inflight_add(&r, &op->node, op, fake_release);
        ASSERT_EQ(hl_smtp_inflight_count(&r), i + 1);
    }

    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 5);   /* all released */
    ASSERT_EQ(g_released, 5);                     /* exactly once each */
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);
    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 0);   /* idempotent: nothing left */
}

UTEST(smtp_inflight, removed_node_is_not_swept)
{
    g_released = 0;
    HlSmtpInflight r; hl_smtp_inflight_init(&r);

    FakeOp *a = make_op(1), *b = make_op(2), *c = make_op(3);
    hl_smtp_inflight_add(&r, &a->node, a, fake_release);
    hl_smtp_inflight_add(&r, &b->node, b, fake_release);
    hl_smtp_inflight_add(&r, &c->node, c, fake_release);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 3);

    /* b completes on the normal path: unlink, then the caller owns its teardown. */
    hl_smtp_inflight_remove(&b->node);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 2);
    free(b);                                      /* caller-owned free (not via sweep) */

    /* The sweep releases only a and c, once each - never the removed b. */
    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 2);
    ASSERT_EQ(g_released, 2);
    ASSERT_TRUE(g_last_released_id == 1 || g_last_released_id == 3);
}

UTEST(smtp_inflight, remove_is_idempotent_and_unlinked_safe)
{
    HlSmtpInflight r; hl_smtp_inflight_init(&r);
    FakeOp *op = make_op(7);
    hl_smtp_inflight_add(&r, &op->node, op, fake_release);

    hl_smtp_inflight_remove(&op->node);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);
    hl_smtp_inflight_remove(&op->node);           /* second remove: no-op */
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);

    /* A never-added node is also safe to remove. */
    FakeOp *loose = make_op(8);
    memset(&loose->node, 0, sizeof loose->node);
    hl_smtp_inflight_remove(&loose->node);

    free(op);
    free(loose);
}

UTEST(smtp_inflight, remove_head_and_tail_relink)
{
    g_released = 0;
    HlSmtpInflight r; hl_smtp_inflight_init(&r);
    FakeOp *a = make_op(1), *b = make_op(2), *c = make_op(3);
    hl_smtp_inflight_add(&r, &a->node, a, fake_release);   /* a is tail after b,c added */
    hl_smtp_inflight_add(&r, &b->node, b, fake_release);
    hl_smtp_inflight_add(&r, &c->node, c, fake_release);   /* c is head */

    /* Remove head (c) and tail (a); b remains. */
    hl_smtp_inflight_remove(&c->node); free(c);
    hl_smtp_inflight_remove(&a->node); free(a);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 1);

    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 1);
    ASSERT_EQ(g_released, 1);
    ASSERT_EQ(g_last_released_id, 2);              /* only b swept */
}

/* ── pass 1: for_each visits all, registry-preserving ────────────────── */
static int g_visited;
static int g_visit_sum;
static void visit_cb(void *owner, void *user)
{
    (void)user;
    FakeOp *op = (FakeOp *)owner;
    g_visited++;
    g_visit_sum += op->id;
}

UTEST(smtp_inflight, for_each_visits_all_without_unlinking)
{
    g_visited = 0; g_visit_sum = 0; g_released = 0;
    HlSmtpInflight r; hl_smtp_inflight_init(&r);
    FakeOp *a = make_op(10), *b = make_op(20), *c = make_op(30);
    hl_smtp_inflight_add(&r, &a->node, a, fake_release);
    hl_smtp_inflight_add(&r, &b->node, b, fake_release);
    hl_smtp_inflight_add(&r, &c->node, c, fake_release);

    /* First shutdown pass: visit every op, registry-preserving. */
    hl_smtp_inflight_for_each(&r, visit_cb, NULL);
    ASSERT_EQ(g_visited, 3);
    ASSERT_EQ(g_visit_sum, 60);                 /* all three seen */
    ASSERT_EQ(hl_smtp_inflight_count(&r), 3);   /* nothing unlinked */
    ASSERT_EQ(g_released, 0);                     /* nothing released */

    /* for_each is idempotent (can run twice - e.g. re-entrant cancel). */
    g_visited = 0;
    hl_smtp_inflight_for_each(&r, visit_cb, NULL);
    ASSERT_EQ(g_visited, 3);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 3);

    /* Second pass: sweep releases each exactly once. */
    ASSERT_EQ(hl_smtp_inflight_sweep(&r), 3);
    ASSERT_EQ(g_released, 3);
    ASSERT_EQ(hl_smtp_inflight_count(&r), 0);
}

UTEST(smtp_inflight, for_each_empty_is_noop)
{
    g_visited = 0;
    HlSmtpInflight r; hl_smtp_inflight_init(&r);
    hl_smtp_inflight_for_each(&r, visit_cb, NULL);
    ASSERT_EQ(g_visited, 0);
}

UTEST_MAIN();

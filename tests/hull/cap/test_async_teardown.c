/*
 * test_async_teardown.c - the runtime teardown list (shared/async.c,
 * audit 14 G1): a detached op still pending when a runtime shuts down is
 * stopped and its continuation released exactly once, while the runtime is
 * alive; an op that retires normally leaves the list; a pool op whose work
 * never ran releases its continuation. Run under ASan in CI: a double free
 * or a leaked ctx / continuation fails there.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/async.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    HlAsyncCont base;
    int *resumed, *cancelled, *destroyed;
} CountCont;

static void cc_resume(HlAsyncCont *self, void *driver)
{ (void)driver; (*((CountCont *)self)->resumed)++; }
static void cc_cancel(HlAsyncCont *self)
{ (*((CountCont *)self)->cancelled)++; }
static void cc_destroy(HlAsyncCont *self)
{ CountCont *c = (CountCont *)self; (*c->destroyed)++; free(c); }

static int resumed, cancelled, destroyed, shutdowns, drivers_freed;

static HlAsyncCtx *make_ctx(int detached)
{
    HlAsyncCtx *ctx = hl_async_ctx_create(NULL, NULL, NULL);
    CountCont *c = calloc(1, sizeof *c);
    if (!ctx || !c) { free(c); hl_async_ctx_free(ctx); return NULL; }
    c->base.resume = cc_resume;
    c->base.cancel = cc_cancel;
    c->base.destroy = cc_destroy;
    c->resumed = &resumed; c->cancelled = &cancelled; c->destroyed = &destroyed;
    ctx->cont = &c->base;
    ctx->detached = detached;
    return ctx;
}

static void reset(void)
{
    resumed = cancelled = destroyed = shutdowns = drivers_freed = 0;
}

static void on_shutdown(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    (void)ctx;
    if (loop == (HlAsyncBackendCtx *)(void *)&shutdowns) shutdowns++;
}

static void free_driver(void *d) { free(d); drivers_freed++; }

UTEST(async_teardown, run_cancels_every_pending_op_once)
{
    reset();
    HlAsyncTeardown *list = NULL;
    for (int i = 0; i < 3; i++) {
        HlAsyncCtx *ctx = make_ctx(1);
        ASSERT_TRUE(ctx != NULL);
        ctx->on_shutdown = on_shutdown;
        ctx->driver = malloc(8);
        ctx->free_driver = free_driver;
        hl_async_ctx_track(ctx, &list);
    }
    ASSERT_TRUE(list != NULL);
    hl_async_teardown_run(&list, (HlAsyncBackendCtx *)(void *)&shutdowns);
    EXPECT_TRUE(list == NULL);
    EXPECT_EQ(shutdowns, 3);
    EXPECT_EQ(cancelled, 3);
    EXPECT_EQ(destroyed, 3);
    EXPECT_EQ(drivers_freed, 3);
    EXPECT_EQ(resumed, 0);
    /* A second run (stop_async is idempotent) finds nothing. */
    hl_async_teardown_run(&list, NULL);
    EXPECT_EQ(cancelled, 3);
}

UTEST(async_teardown, an_op_that_retires_leaves_the_list)
{
    reset();
    HlAsyncTeardown *list = NULL;
    HlAsyncCtx *a = make_ctx(1), *b = make_ctx(1), *c = make_ctx(1);
    ASSERT_TRUE(a && b && c);
    hl_async_ctx_track(a, &list);
    hl_async_ctx_track(b, &list);
    hl_async_ctx_track(c, &list);

    hl_async_ctx_resume_detached(b);       /* completed normally */
    EXPECT_EQ(resumed, 1);
    hl_async_ctx_cancel(a);                /* cancelled by its owner */
    EXPECT_EQ(cancelled, 1);

    hl_async_teardown_run(&list, NULL);    /* only c is left */
    EXPECT_TRUE(list == NULL);
    EXPECT_EQ(cancelled, 2);
    EXPECT_EQ(destroyed, 3);
}

UTEST(async_teardown, freeing_a_never_armed_ctx_unlinks_it)
{
    reset();
    HlAsyncTeardown *list = NULL;
    HlAsyncCtx *ctx = make_ctx(1);
    ASSERT_TRUE(ctx != NULL);
    hl_async_ctx_track(ctx, &list);
    ctx->cont->destroy(ctx->cont);
    ctx->cont = NULL;
    hl_async_ctx_free(ctx);
    EXPECT_TRUE(list == NULL);
}

UTEST(async_teardown, drop_unrun_releases_a_detached_continuation)
{
    reset();
    HlAsyncCtx *ctx = make_ctx(1);
    ASSERT_TRUE(ctx != NULL);
    void *driver = malloc(8);
    ctx->driver = driver;
    ctx->free_driver = free_driver;
    hl_async_ctx_drop_unrun(ctx);          /* the pool op's own cancel_fn */
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(drivers_freed, 0);           /* the caller frees its op */
    free(driver);
}

UTEST(async_teardown, drop_unrun_after_its_own_cancel_frees_only_the_ctx)
{
    reset();
    HlAsyncCtx *ctx = make_ctx(0);
    ASSERT_TRUE(ctx != NULL);
    /* The op's on_cancel already ran (its connection closed): it released
     * the continuation and left the ctx to the pool. */
    ctx->cont->cancel(ctx->cont);
    ctx->cont->destroy(ctx->cont);
    ctx->cont = NULL;
    hl_async_ctx_drop_unrun(ctx);
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(destroyed, 1);
}

/* audit 31: an http.fetch ctx gives its in-flight count back when it goes,
 * on every path (resume, cancel, never armed). */
UTEST(async_teardown, a_ctx_gives_its_inflight_count_back)
{
    reset();
    int inflight = 0;
    HlAsyncTeardown *list = NULL;
    HlAsyncCtx *a = make_ctx(1), *b = make_ctx(1), *c = make_ctx(1);
    ASSERT_TRUE(a && b && c);
    a->inflight = b->inflight = c->inflight = &inflight;
    inflight = 3;
    hl_async_ctx_track(a, &list);
    hl_async_ctx_resume_detached(b);           /* completes */
    EXPECT_EQ(2, inflight);
    hl_async_ctx_free(c);                      /* never armed */
    EXPECT_EQ(1, inflight);
    hl_async_teardown_run(&list, NULL);        /* cancelled at teardown */
    EXPECT_EQ(0, inflight);
}

UTEST_MAIN();

/*
 * test_hull_cap_ws.c - Tests for WebSocket connection registry
 *
 * Uses utest.h for the test framework.
 * Tests the registry in isolation (no Keel server needed).
 *
 * Scope: server-side registry (HlWsRegistry, hl_ws_broadcast_*,
 * hl_ws_connections). Server-side conn class and per-connection methods
 * (send / send_binary / close / ping / conn.data) live in
 * runtime/{lua,js}/mod_ws_server.c and are exercised end-to-end through
 * the chat example (e2e_examples.sh).
 *
 * Client-side WebSocket lifecycle (ws.connect outbound, on_open /
 * on_message / on_close / on_error callbacks, JS finalizer cleanup of
 * UD callback refs) is a thin wrapper over keel's kl_ws_client_* and
 * requires a live event loop. It is covered end-to-end by the
 * irc_chat federation tests (examples/irc_chat/tests/test_app.{lua,js}
 * - 13 federation cases), which exercise the on_open → on_message →
 * on_close → finalizer teardown path under both runtimes. Mocking
 * keel's WS client for a standalone C unit test was considered and
 * rejected as brittle for the value gained.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/ws.h"
#include "hull/utils/alloc.h"
#include <string.h>

/* ── Test fixtures ──────────────────────────────────────────────────── */

static HlAllocator test_alloc;
static HlWsRegistry test_reg;

static void setup_reg(void)
{
    hl_alloc_init(&test_alloc, 0);
    hl_ws_registry_init(&test_reg, &test_alloc);
}

static void teardown_reg(void)
{
    hl_ws_registry_free(&test_reg);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

UTEST(ws_registry, init_free)
{
    setup_reg();
    ASSERT_EQ((size_t)0, hl_ws_connections(&test_reg, "/ws"));
    teardown_reg();
}

UTEST(ws_registry, add_remove)
{
    setup_reg();

    /* Add a connection with NULL kl_ws (mock) */
    HlWsConn *c = hl_ws_registry_add(&test_reg, "/ws", NULL);
    ASSERT_TRUE(c != NULL);
    ASSERT_EQ((uint64_t)1, c->id);
    ASSERT_STREQ("/ws", c->path);
    ASSERT_EQ((size_t)1, hl_ws_connections(&test_reg, "/ws"));

    /* Remove it */
    hl_ws_registry_remove(&test_reg, c);
    ASSERT_EQ((size_t)0, hl_ws_connections(&test_reg, "/ws"));

    teardown_reg();
}

UTEST(ws_registry, broadcast_empty)
{
    setup_reg();

    /* Broadcast to path with no connections - returns 0 */
    int sent = hl_ws_broadcast_text(&test_reg, "/ws", "hello", 5);
    ASSERT_EQ(0, sent);

    sent = hl_ws_broadcast_binary(&test_reg, "/ws", "data", 4);
    ASSERT_EQ(0, sent);

    /* Broadcast to non-existent path */
    sent = hl_ws_broadcast_text(&test_reg, "/none", "hello", 5);
    ASSERT_EQ(0, sent);

    teardown_reg();
}

UTEST(ws_registry, connections_count)
{
    setup_reg();

    HlWsConn *c1 = hl_ws_registry_add(&test_reg, "/ws", NULL);
    HlWsConn *c2 = hl_ws_registry_add(&test_reg, "/ws", NULL);
    HlWsConn *c3 = hl_ws_registry_add(&test_reg, "/ws", NULL);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);
    ASSERT_TRUE(c3 != NULL);
    ASSERT_EQ((size_t)3, hl_ws_connections(&test_reg, "/ws"));

    /* IDs should be monotonically increasing */
    ASSERT_TRUE(c2->id > c1->id);
    ASSERT_TRUE(c3->id > c2->id);

    /* Remove one */
    hl_ws_registry_remove(&test_reg, c2);
    ASSERT_EQ((size_t)2, hl_ws_connections(&test_reg, "/ws"));

    /* Remove another */
    hl_ws_registry_remove(&test_reg, c1);
    ASSERT_EQ((size_t)1, hl_ws_connections(&test_reg, "/ws"));

    /* Remove last */
    hl_ws_registry_remove(&test_reg, c3);
    ASSERT_EQ((size_t)0, hl_ws_connections(&test_reg, "/ws"));

    teardown_reg();
}

UTEST(ws_registry, multiple_paths)
{
    setup_reg();

    HlWsConn *a1 = hl_ws_registry_add(&test_reg, "/a", NULL);
    HlWsConn *a2 = hl_ws_registry_add(&test_reg, "/a", NULL);
    HlWsConn *b1 = hl_ws_registry_add(&test_reg, "/b", NULL);
    ASSERT_TRUE(a1 != NULL);
    ASSERT_TRUE(a2 != NULL);
    ASSERT_TRUE(b1 != NULL);

    ASSERT_EQ((size_t)2, hl_ws_connections(&test_reg, "/a"));
    ASSERT_EQ((size_t)1, hl_ws_connections(&test_reg, "/b"));
    ASSERT_EQ((size_t)0, hl_ws_connections(&test_reg, "/c"));

    /* Removing from /a doesn't affect /b */
    hl_ws_registry_remove(&test_reg, a1);
    ASSERT_EQ((size_t)1, hl_ws_connections(&test_reg, "/a"));
    ASSERT_EQ((size_t)1, hl_ws_connections(&test_reg, "/b"));

    hl_ws_registry_remove(&test_reg, a2);
    hl_ws_registry_remove(&test_reg, b1);

    teardown_reg();
}

UTEST(ws_registry, find_by_kl_ws)
{
    setup_reg();

    /* Use distinct non-NULL pointers as mock kl_ws */
    int fake1 = 0, fake2 = 0;
    KlWsServerConn *ws1 = (KlWsServerConn *)&fake1;
    KlWsServerConn *ws2 = (KlWsServerConn *)&fake2;

    HlWsConn *c1 = hl_ws_registry_add(&test_reg, "/ws", ws1);
    HlWsConn *c2 = hl_ws_registry_add(&test_reg, "/ws", ws2);
    ASSERT_TRUE(c1 != NULL);
    ASSERT_TRUE(c2 != NULL);

    /* Find by kl_ws pointer */
    HlWsConn *found = hl_ws_registry_find(&test_reg, "/ws", ws1);
    ASSERT_TRUE(found == c1);

    found = hl_ws_registry_find(&test_reg, "/ws", ws2);
    ASSERT_TRUE(found == c2);

    /* Non-existent kl_ws */
    int fake3 = 0;
    found = hl_ws_registry_find(&test_reg, "/ws",
                                 (KlWsServerConn *)&fake3);
    ASSERT_TRUE(found == NULL);

    /* Wrong path */
    found = hl_ws_registry_find(&test_reg, "/other", ws1);
    ASSERT_TRUE(found == NULL);

    hl_ws_registry_remove(&test_reg, c1);
    hl_ws_registry_remove(&test_reg, c2);

    teardown_reg();
}

/* Keel frees the KlWsServerConn when on_close returns, while a node whose
 * async on_close handler is parked stays in the list; a new connection may
 * get the same address. find must never return the closed node - whatever
 * order the list holds the two in. */
UTEST(ws_registry, find_never_returns_a_closed_node)
{
    setup_reg();
    int fake = 0;
    KlWsServerConn *ws = (KlWsServerConn *)&fake;

    /* on_close ran (the runtimes mark the node as it starts): */
    HlWsConn *stale = hl_ws_registry_add(&test_reg, "/ws", ws);
    ASSERT_TRUE(stale != NULL);
    hl_ws_registry_mark_closed(stale);
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", ws) == NULL);

    /* The address reused by a new connection: the new node, in either list
     * order (add prepends; the fresh node is added FIRST here so the stale
     * one is ahead of it). */
    hl_ws_registry_remove(&test_reg, stale);
    HlWsConn *fresh = hl_ws_registry_add(&test_reg, "/ws", ws);
    stale = hl_ws_registry_add(&test_reg, "/ws", ws);
    ASSERT_TRUE(fresh != NULL && stale != NULL);
    hl_ws_registry_mark_closed(stale);
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", ws) == fresh);

    /* mark_closed: closed, and its Keel pointer dropped. */
    hl_ws_registry_mark_closed(fresh);
    EXPECT_EQ(1, fresh->closed);
    EXPECT_TRUE(fresh->kl_ws == NULL);
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", ws) == NULL);
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", NULL) == NULL);
    hl_ws_registry_mark_closed(NULL);   /* no crash */

    /* A broadcast skips closed nodes (nothing to send to). */
    EXPECT_EQ(0, hl_ws_broadcast_text(&test_reg, "/ws", "x", 1));

    hl_ws_registry_remove(&test_reg, fresh);
    hl_ws_registry_remove(&test_reg, stale);
    teardown_reg();
}

/* Audit 44 M5: a node the APP closed (conn:close() sets `closed`, Keel still
 * holds the connection) is still found, so Keel's on_close for it runs the
 * app's on_close and removes the node; skipped, it leaked with its conn object
 * and the endpoint count. Once on_close marks it, it is gone. */
UTEST(ws_audit44, app_closed_node_is_found_until_on_close)
{
    setup_reg();
    int fake = 0;
    KlWsServerConn *ws = (KlWsServerConn *)&fake;
    HlWsConn *c = hl_ws_registry_add(&test_reg, "/ws", ws);
    ASSERT_TRUE(c != NULL);
    c->closed = 1;   /* what conn:close() sets */
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", ws) == c);
    EXPECT_EQ(0, hl_ws_broadcast_text(&test_reg, "/ws", "x", 1));   /* no send */
    hl_ws_registry_mark_closed(c);   /* Keel's on_close */
    EXPECT_TRUE(hl_ws_registry_find(&test_reg, "/ws", ws) == NULL);
    hl_ws_registry_remove(&test_reg, c);
    EXPECT_EQ((size_t)0, hl_ws_connections(&test_reg, "/ws"));
    teardown_reg();
}

/* A failed add (the runtimes then close the Keel connection) returns NULL
 * and registers nothing. */
UTEST(ws_registry, add_failure_registers_nothing)
{
    HlAllocator tiny;
    hl_alloc_init(&tiny, 1);   /* every allocation fails */
    HlWsRegistry reg;
    hl_ws_registry_init(&reg, &tiny);
    int fake = 0;
    EXPECT_TRUE(hl_ws_registry_add(&reg, "/ws", (KlWsServerConn *)&fake) == NULL);
    EXPECT_TRUE(hl_ws_registry_find(&reg, "/ws", (KlWsServerConn *)&fake) == NULL);
    EXPECT_EQ((size_t)0, hl_ws_connections(&reg, "/ws"));
    hl_ws_registry_free(&reg);
}

UTEST(ws_registry, null_safety)
{
    /* All functions should handle NULL gracefully */
    hl_ws_registry_free(NULL);  /* should not crash */

    HlWsConn *c = hl_ws_registry_add(NULL, "/ws", NULL);
    ASSERT_TRUE(c == NULL);

    hl_ws_registry_remove(NULL, NULL);

    int sent = hl_ws_broadcast_text(NULL, "/ws", "hello", 5);
    ASSERT_EQ(0, sent);

    ASSERT_EQ((size_t)0, hl_ws_connections(NULL, "/ws"));
}

UTEST_MAIN();

/* db_wait.h - refuse a wait while a shared database connection is inside a
 * transaction (JS side of hl_lua_check_can_wait's transaction check).
 *
 * Every registry connection (default, named, internal) is shared by all
 * requests, SSE events and timers. While a handler is parked on a Hull
 * operation (http.fetch, db.async, hull.sleep, compute / gpu async,
 * worker.dispatch, smtp.send, a multipart body read, tui.poll) another entry
 * runs on the same connection, and the stale-transaction guard rolled the
 * parked handler's transaction back under it; its remaining statements then
 * autocommitted and its COMMIT "succeeded" with half its writes gone (audit 5
 * M1). Each operation that parks calls this before it arms anything, so the
 * pattern fails loudly at the wait instead (see db_registry.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_RUNTIME_JS_DB_WAIT_H
#define HULL_RUNTIME_JS_DB_WAIT_H

#include <quickjs.h>
#include "hull/runtime/js.h"
#include "hull/cap/db_registry.h"

/* 0 to go ahead; -1 with a TypeError pending on @p ctx (return JS_EXCEPTION). */
static inline int hl_js_db_refuse_wait(JSContext *ctx, const char *what)
{
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    const char *txn = js ? hl_db_registry_open_txn(js->base.db_registry) : NULL;
    if (!txn) return 0;
    JS_ThrowTypeError(ctx, "%s cannot wait while a transaction is open on "
                      "database connection '%s': other requests use the same "
                      "connection while this one waits. Commit or roll back "
                      "first (db.batch fn must be synchronous)", what, txn);
    return -1;
}

#endif /* HULL_RUNTIME_JS_DB_WAIT_H */

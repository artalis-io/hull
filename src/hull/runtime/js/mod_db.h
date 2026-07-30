/* mod_db.h — internal seam shared between mod_db.c (base runtime) and the
 * composed SQLite UDF bridge mod_db_udf.c.
 *
 * The udf bindings live in a separate per-runtime bridge so the base runtime
 * archive (libhull_feature-js.a) carries no sqlite3_* references (Phase C.2,
 * docs/sqlite_feature.md). The bridge reuses these two mod_db.c helpers to
 * resolve the bound handle and build the udf sub-object.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_RUNTIME_JS_MOD_DB_H
#define HULL_RUNTIME_JS_MOD_DB_H

#ifdef HL_ENABLE_DB

#include <quickjs.h>
#include "hull/cap/db_backend.h"

/* Resolve the connection a bound method call operates on: the opaque on
 * this_val (db.connect/default sub-objects), an owner box (db.open), else the
 * runtime default. Defined in mod_db.c. */
HlDbHandle *js_call_handle(JSContext *ctx, JSValueConst this_val);

/* Build a connection sub-object (HullDbConnection instance) carrying @p h as
 * its opaque, so js_call_handle resolves it. Defined in mod_db.c. */
JSValue new_bound_subobject(JSContext *ctx, HlDbHandle *h);

/* Attach a `udf` sub-object to the connection object @p conn. Weak no-op in
 * mod_db.c; strong override in the composed bridge mod_db_udf.c. */
void hl_js_db_attach_udf(JSContext *ctx, JSValue conn, HlDbHandle *h);

#endif /* HL_ENABLE_DB */
#endif /* HULL_RUNTIME_JS_MOD_DB_H */

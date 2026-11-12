/* mod_db.h - internal seam shared between mod_db.c (base runtime) and the
 * composed SQLite UDF bridge mod_db_udf.c.
 *
 * The udf bindings live in a separate per-runtime bridge so the base runtime
 * archive (libhull_feature-js.a) carries no sqlite3_* references (
 * docs/sqlite_feature.md). The bridge reuses these two mod_db.c helpers to
 * resolve the bound handle and build the udf sub-object.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_RUNTIME_JS_MOD_DB_H
#define HULL_RUNTIME_JS_MOD_DB_H

#ifdef HL_ENABLE_DB

#include <stdbool.h>
#include <stdint.h>
#include <quickjs.h>
#include "hull/cap/db_backend.h"

/* Resolve the connection a bound method call operates on: the opaque on
 * this_val (db.connect/default sub-objects), an owner box (db.open), else the
 * runtime default. Defined in mod_db.c. */
HlDbHandle *js_call_handle(JSContext *ctx, JSValueConst this_val);

/* True when @p this_val is a db.open() (caller-owned) connection object, open
 * or closed. Defined in mod_db.c (audit 29). */
bool js_call_is_owned(JSValueConst this_val);

/* True when @p v is a connection object or sub-object of either kind. */
bool js_db_is_conn(JSValueConst v);

/* The owner box shared by a db.open() object and its sub-objects, else NULL.
 * Two objects of one connection return the same box. */
void *js_db_owned_box(JSValueConst v);

/* A sub-object sharing @p owned's owner box (conn.udf of a db.open()
 * connection), or JS_EXCEPTION. */
JSValue js_db_owned_subobject(JSContext *ctx, JSValueConst owned);

/* Keep a UDF function alive in the owner box's GC-traced holder under a fresh
 * key (0, or -1 with an exception pending), and drop it again (from the UDF's
 * destructor; a no-op while the box's last finalizer runs). */
int  js_db_udf_hold(JSContext *ctx, void *box, JSValueConst fn, uint32_t *key);
void js_db_udf_release(JSContext *ctx, void *box, uint32_t key);

/* Attach `udf` to the db.open() connection object @p conn. Weak no-op in
 * mod_db.c; strong override in mod_db_udf.c. */
void hl_js_db_attach_owned_udf(JSContext *ctx, JSValue conn);

/* Build a connection sub-object (HullDbConnection instance) carrying @p h as
 * its opaque, so js_call_handle resolves it. Defined in mod_db.c. */
JSValue new_bound_subobject(JSContext *ctx, HlDbHandle *h);

/* Attach a `udf` sub-object to the connection object @p conn. Weak no-op in
 * mod_db.c; strong override in the composed bridge mod_db_udf.c. */
void hl_js_db_attach_udf(JSContext *ctx, JSValue conn, HlDbHandle *h);

#endif /* HL_ENABLE_DB */
#endif /* HULL_RUNTIME_JS_MOD_DB_H */

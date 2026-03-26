/*
 * app_context.h — Reusable application context for commands
 *
 * Consolidates VFS + DB + runtime initialization shared by
 * test, agent, and MCP commands. Each consumer creates an
 * HlAppContext instead of duplicating the init sequence.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_APP_CONTEXT_H
#define HL_APP_CONTEXT_H

#include <stddef.h>

/* Forward declarations */
typedef struct sqlite3 sqlite3;
typedef struct HlAllocator HlAllocator;
typedef struct HlRuntime HlRuntime;
typedef struct HlVfs HlVfs;
typedef struct HlStmtCache HlStmtCache;
typedef struct HlWasmCache HlWasmCache;

typedef struct {
    const char   *app_dir;        /* required */
    const char   *entry_point;    /* NULL = auto-detect (app.lua / app.js) */
    const char   *db_path;        /* NULL = ":memory:" */
    int           no_migrate;     /* 1 = skip migrations */
    int           sandbox;        /* 1 = sandboxed runtime (default) */
    HlAllocator  *alloc;          /* NULL = use raw malloc */
} HlAppContextOpts;

typedef struct HlAppContext HlAppContext;

/*
 * Initialize an app context: open DB, init VFS, create runtime, load app.
 * Returns 0 on success, -1 on error.
 */
int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts);

/*
 * Free all resources. Safe to call on NULL. Idempotent.
 */
void hl_app_context_free(HlAppContext *ctx);

/* Accessors */
sqlite3      *hl_app_context_db(HlAppContext *ctx);
HlRuntime    *hl_app_context_runtime(HlAppContext *ctx);
const HlVfs  *hl_app_context_app_vfs(HlAppContext *ctx);
const HlVfs  *hl_app_context_platform_vfs(HlAppContext *ctx);
HlStmtCache  *hl_app_context_stmt_cache(HlAppContext *ctx);
int           hl_app_context_is_lua(HlAppContext *ctx);

#ifdef HL_ENABLE_LUA
struct HlLua;
struct HlLua *hl_app_context_lua(HlAppContext *ctx);  /* NULL if JS */
#endif
#ifdef HL_ENABLE_JS
struct HlJS;
struct HlJS *hl_app_context_js(HlAppContext *ctx);    /* NULL if Lua */
#endif

#endif /* HL_APP_CONTEXT_H */

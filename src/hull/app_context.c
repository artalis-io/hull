/*
 * app_context.c — Reusable application context for commands
 *
 * Consolidates the VFS + DB + runtime init sequence that was
 * previously duplicated in agent_lib.c, commands/test.c, and main.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/runtime/factory.h"
#include "hull/stdlib_feature.h"
#include "hull/vfs.h"

#include "hull/cap/fs.h"          /* HlFsConfig — test/agent fs sandbox wiring
                                   * (unconditional: fs_cfg is not DB-gated) */
#include "hull/utils/alloc.h"     /* HlAllocator + hl_alloc_init for the fs policy */

#ifdef HL_ENABLE_DB
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"   /* hl_db_sqlite_raw (agent handle) */
#include "hull/cap/db_registry.h"
#include "hull/migrate.h"
#include "hull/worker_db.h"
#include <sqlite3.h>
#endif

/* app_context stays runtime-agnostic: it dispatches through the
 * HlRuntimeFactory vtable and never references a concrete runtime symbol
 * (hl_lua_factory / hl_js_factory). The runtime-typed accessors
 * (hl_app_context_is_lua / _lua / _js) live in the toolchain-only TU
 * app_context_runtime.c so a slim single-runtime app never pulls the
 * other runtime's factory symbol. This TU exposes only the agnostic
 * hl_app_context_factory() getter they build on. */
#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif
#include "hull/cap/gpu.h"  /* base-resident: gpu_ctx propagates for composed feature too */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Opaque struct ─────────────────────────────────────────────────── */

struct HlAppContext {
#ifdef HL_ENABLE_DB
    sqlite3       *db;            /* raw sqlite3* for agent (from the default conn) */
    int            db_open;       /* 1 = db was opened */
    HlDbRegistry  *db_registry;   /* owns all connections, incl. the "default" */
#endif
    HlVfs          app_vfs;
    HlVfs          platform_vfs;
    void          *platform_vfs_owned;  /* opaque sealed-arena handle to free, or NULL */
    HlRuntime     *rt;

    /* Owned app_dir copy backing fs_cfg_storage.base_dir (rt->fs_cfg borrows it,
     * so it must outlive the runtime — opts->app_dir may be a caller stack). */
    char          *app_dir_copy;
    /* fs capability config wired onto rt->fs_cfg when the app declares
     * fs.read/fs.write, so blob.* / fs.* / fs.mmap work under `hull test` /
     * `hull agent` the same way they do under `hull dev` (serve.c does the
     * equivalent wiring in hl_serve_wire_caps). Only base_dir/base_len — the
     * cap layer sandboxes to base_dir + rejects traversal; the per-path
     * allowlist is the kernel sandbox, which the test harness runs without. */
    HlFsConfig     fs_cfg_storage;
    HlFsPolicy     fs_policy_storage; /* compiled fs authorization policy */
    HlAllocator    fs_alloc;          /* owns the policy's memory (opts->alloc may be NULL) */

    /* Resolved module set (opt-in via opts.gate_modules). Lives here so
     * its lifetime matches the runtime — rt->module_set borrows. */
    HlResolvedModuleSet module_set;
    int                 module_set_wired;

    /* Factory that built `rt`, owns the heap storage. NULL until init. */
    const HlRuntimeFactory *factory;

    int            rt_init;   /* 1 = runtime was initialized */
    int            app_loaded; /* 1 = app code was loaded */

    /* Tracks whether context owns the WASM cache (internal init) vs
     * borrowing an external one (server passes its own static cache). */
#ifdef HL_ENABLE_WASM
    HlWasmCache    wasm_cache;
    int            wasm_ok;       /* 1 = wasm_cache was initialized */
    int            wasm_external; /* 1 = external cache, don't destroy */
#endif
};

/* ── Entry point detection ─────────────────────────────────────────── */

static const char *detect_entry(const char *app_dir, const char *ext,
                                char *buf, size_t buf_size)
{
    size_t dir_len = strlen(app_dir);
    while (dir_len > 1 && app_dir[dir_len - 1] == '/')
        dir_len--;

    snprintf(buf, buf_size, "%.*s/app.%s", (int)dir_len, app_dir, ext);
    if (access(buf, F_OK) == 0) return buf;
    return NULL;
}

/* Try each factory's extension against the app dir; on the first hit, set
 * *out_factory and return the entry path (into entry_buf). NULL if none match. */
static const char *discover_entry_in(const HlRuntimeFactory *const *facs,
                                     size_t n, const char *app_dir,
                                     char *entry_buf, size_t buf_size,
                                     const HlRuntimeFactory **out_factory)
{
    for (size_t i = 0; i < n; i++) {
        const HlRuntimeFactory *f = facs ? facs[i] : NULL;
        if (!f || !f->entry_extension) continue;
        /* extension is ".lua" / ".js" — strip leading dot for detect_entry */
        const char *ext = f->entry_extension;
        if (ext[0] == '.') ext++;
        const char *e = detect_entry(app_dir, ext, entry_buf, buf_size);
        if (e) { *out_factory = f; return e; }
    }
    return NULL;
}

/* ── Internal: determine runtime type from entry point ─────────────── */

static int resolve_entry_and_runtime(HlAppContext *ctx,
                                     const HlAppContextOpts *opts,
                                     const char **out_entry,
                                     char *entry_buf, size_t buf_size)
{
    const char *entry = opts->entry_point;

    if (!entry) {
        /* Discover entry by trying each factory's extension in order: the
         * compile-time base factories first, then any runtime composed as a
         * feature. First match wins (Lua then JS in a default build). Two
         * immutable sources, no merged/mutable array. */
        size_t base_count = 0;
        const HlRuntimeFactory *const *base = hl_runtime_factories(&base_count);
        entry = discover_entry_in(base, base_count, opts->app_dir,
                                  entry_buf, buf_size, &ctx->factory);
        if (!entry) {
            size_t feat_count = 0;
            const HlRuntimeFactory *const *feats =
                hl_runtime_feature_factories(&feat_count);
            entry = discover_entry_in(feats, feat_count, opts->app_dir,
                                      entry_buf, buf_size, &ctx->factory);
        }
    } else {
        /* Caller provided entry point — match factory by file extension. */
        ctx->factory = hl_runtime_factory_for_filename(entry);
    }

    // cppcheck-suppress knownConditionTrueFalse
    if (!entry || !ctx->factory)
        return -1;

    *out_entry = entry;
    return 0;
}

/* ── Internal: load app code into an initialized runtime ───────────── */

static int load_app_code(HlAppContext *ctx, const char *entry)
{
    if (!ctx->rt || !ctx->rt->vt || !ctx->rt->vt->load_app) return -1;
    if (ctx->rt->vt->load_app(ctx->rt, entry) != 0)
        return -1;
    ctx->app_loaded = 1;
    return 0;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts)
{
    if (!out || !opts || !opts->app_dir) return -1;

    HlAppContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    /* fds default -1 so hl_app_context_free never close(0)s stdin on a policy
     * that was never compiled (calloc gives base_fd == 0, a live fd). */
    ctx->fs_policy_storage = HL_FS_POLICY_INIT;
    hl_alloc_init(&ctx->fs_alloc, 0);   /* the policy needs a non-NULL allocator */

    /* Own a stable copy of app_dir for fs_cfg.base_dir (see struct comment). */
    ctx->app_dir_copy = strdup(opts->app_dir);
    if (!ctx->app_dir_copy) { free(ctx); return -1; }

    /* Detect entry point and runtime type */
    char entry_buf[4096];
    const char *entry = NULL;
    if (resolve_entry_and_runtime(ctx, opts, &entry, entry_buf, sizeof(entry_buf)) != 0) {
        hl_app_context_free(ctx);
        return -1;
    }

    /* Open database via vtable (skip in pure compute mode) */
#ifdef HL_ENABLE_DB
    if (!opts->no_db) {
        const char *db_path = opts->db_path;
#ifdef HL_ENABLE_SQLITE
        /* SQLite in-memory default when no DSN is given. Only when SQLite is
         * compiled: a postgres-only build has no SQLite to fall back to, so a
         * missing DSN means "no default connection" (see below), not :memory:. */
        if (!db_path) db_path = ":memory:";
#endif
        /* The registry owns every connection, including the default (the -d
         * flag DSN). db.default(), migrations, and the per-request stale-txn
         * guards all resolve it via the registry; there is no separate default
         * handle. The databases map isn't known until app.manifest() runs, so
         * the serve path injects the sealed manifest later via
         * hl_db_registry_set_manifest (named db.connect() then resolves too).
         * The registry is created even without a default DSN so named
         * connections still resolve. */
        ctx->db_registry = hl_db_registry_create(NULL, db_path, opts->alloc);
        if (!ctx->db_registry) {
            fprintf(stderr, "hull: out of memory (db registry)\n");
            free(ctx);
            return -1;
        }
        /* Open the default connection only when a default DSN exists. With none
         * (postgres-only build, no -d), the app runs DB-less: db.default() is
         * lazy and errors only on use, matching a compute-only build. */
        if (db_path) {
            const char *derr = NULL;
            HlDbHandle *def = hl_db_registry_get(ctx->db_registry, "default", &derr);
            if (!def) {
                fprintf(stderr, "hull: %s\n", derr ? derr : "cannot open database");
                hl_db_registry_destroy(ctx->db_registry);
                ctx->db_registry = NULL;
                free(ctx);
                return -1;
            }
            ctx->db_open = 1;
            /* NULL under a non-SQLite backend; raw-pointer consumers guard it. */
            ctx->db = hl_db_sqlite_raw(def);
        }
    }
#endif

    /* Init VFS instances. The platform VFS unions the runtime-agnostic base
     * stdlib with each composed runtime's stdlib (via hl_stdlib_feature_entries);
     * in a base build with no runtime feature it borrows the static base. */
    extern const HlEntry hl_app_entries[];
    hl_vfs_init(&ctx->app_vfs, hl_app_entries, opts->app_dir);
    hl_platform_vfs_init(&ctx->platform_vfs, &ctx->platform_vfs_owned);

#ifdef HL_ENABLE_DB
    /* Run migrations (fail on error to prevent starting with broken schema) */
    if (!opts->no_migrate && ctx->db_open) {
        int migrated = hl_migrate_run(hl_db_registry_default(ctx->db_registry),
                                      &ctx->app_vfs);
        if (migrated == HL_MIGRATE_ERR) {
            hl_app_context_free(ctx);
            return -1;
        }
    }
#endif

    /* WASM cache: use external if provided, else init internal */
#ifdef HL_ENABLE_WASM
    if (opts->wasm_cache) {
        ctx->wasm_external = 1;
        ctx->wasm_ok = 1;
    } else {
        if (hl_cap_wasm_init(&ctx->wasm_cache) == 0)
            ctx->wasm_ok = 1;
    }
#endif

    /* Build base config bundle — wired into rt->base BEFORE init. */
    HlRuntimeBaseConfig base = {0};
#ifdef HL_ENABLE_DB
    if (ctx->db_open) {
        base.db_registry    = ctx->db_registry;
    }
#endif
    base.alloc        = opts->alloc;
    base.app_vfs      = &ctx->app_vfs;
    base.platform_vfs = &ctx->platform_vfs;
    base.thread_pool  = opts->thread_pool;
    base.db_path      = opts->worker_db_path;
    base.compress     = opts->compress;
#ifdef HL_ENABLE_WASM
    if (ctx->wasm_ok)
        base.wasm_cache = opts->wasm_cache ? opts->wasm_cache : &ctx->wasm_cache;
#endif
    /* Base-resident: NULL without a backend (plain base), the live ctx with a
     * monolithic HL_ENABLE_GPU build or a composed --with=gpu feature. The
     * runtime registers hull.gpu only when this is non-NULL, so gpu stays
     * unexposed when there's no GPU. */
    base.gpu_ctx = opts->gpu_ctx;

    /* Init runtime via the factory — table-driven, single-path. */
    if (ctx->factory->create(&ctx->rt, opts, &base) != 0) {
        hl_app_context_free(ctx);
        return -1;
    }
    ctx->rt_init = 1;

    if (!opts->no_load) {
        int load_rc = load_app_code(ctx, entry);
        if (load_rc != 0 && !opts->tolerate_load_error) {
            hl_app_context_free(ctx);
            return -1;
        }
        /* On a tolerated load error the app is only PARTIALLY loaded: skip
         * module gating (no reliable module surface) but keep the context so the
         * caller can still extract whatever manifest app.manifest() stored
         * before the error. load_app_code left app_loaded = 0. */

        /* Module-system gating (opt-in via opts.gate_modules).
         *
         * After the app's top-level code has run (which sets the
         * manifest) and before any handler is invoked, extract the
         * manifest, run the resolver, and wire the resulting set onto
         * the runtime so require/import enforce the declared module
         * surface. This is the same mechanism the server path
         * (main.c::hl_serve_wire_caps) uses, lifted into the shared
         * context so agent/mcp can opt in without re-implementing.
         *
         * The set lives in HlAppContext (lifetime matches the runtime).
         * Resolver failure is fatal — caller sees init failure. */
        if (load_rc == 0 && opts->gate_modules) {
            HlManifest m = {0};
            if (ctx->rt->vt->extract_manifest(ctx->rt, &m) == 0) {
                char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
                int rc = hl_module_resolver_resolve(&m, &ctx->module_set,
                                                     err, sizeof(err));
                /* Wire the fs capability from the manifest, mirroring the serve
                 * path (serve.c::hl_serve_wire_caps). Without this rt->fs_cfg
                 * stays NULL and blob.init / fs.read/write / fs.mmap fail with
                 * "fs config unavailable" under `hull test` / `hull agent`. Only
                 * base_dir is needed (the cap layer sandboxes to base_dir +
                 * rejects traversal; the per-path allowlist is the kernel
                 * sandbox). Keyed on the app declaring fs.read OR fs.write, same
                 * condition as serve. Safe to read m before the free below. */
                if (m.fs_read_count > 0 || m.fs_write_count > 0) {
                    /* Compile the fs.read/fs.write grants into the authorization
                     * policy, mirroring serve.c. Must happen BEFORE
                     * hl_manifest_free(&m) below (compile deep-copies the grant
                     * strings). The cap layer now enforces the grants here even
                     * under `hull test`/`hull agent` (which run without the kernel
                     * sandbox), so a test that reads outside its declared grants is
                     * denied. */
                    const char *perr = NULL;
                    if (hl_fs_policy_compile_manifest(
                            ctx->app_dir_copy, &ctx->fs_alloc,
                            m.fs_read,  (size_t)m.fs_read_count,
                            m.fs_write, (size_t)m.fs_write_count,
                            &ctx->fs_policy_storage, &perr) != 0) {
                        fprintf(stderr, "[app-context] fs policy: %s\n",
                                perr ? perr : "compile failed");
                        hl_manifest_free(&m);
                        hl_app_context_free(ctx);
                        return -1;
                    }
                    ctx->fs_cfg_storage.base_dir = ctx->app_dir_copy;
                    ctx->fs_cfg_storage.base_len = strlen(ctx->app_dir_copy);
                    ctx->fs_cfg_storage.policy   = &ctx->fs_policy_storage;
                    ctx->rt->fs_cfg = &ctx->fs_cfg_storage;
                }
                hl_manifest_free(&m);
                if (rc != 0) {
                    fprintf(stderr, "[app-context] module resolver: %s\n", err);
                    hl_app_context_free(ctx);
                    return -1;
                }
            } else {
                /* No manifest declared → resolver still admits intrinsics. */
                hl_module_resolver_resolve(NULL, &ctx->module_set, NULL, 0);
            }
            ctx->rt->module_set = &ctx->module_set;
            ctx->module_set_wired = 1;
        }
    }

    /* Init worker DB after runtime init (needs db_path). */
#ifdef HL_ENABLE_DB
    if (opts->worker_db_path)
        hl_worker_db_init(opts->worker_db_path);
#endif

    *out = ctx;
    return 0;
}

/* ── Load (deferred) ──────────────────────────────────────────────── */

int hl_app_context_load(HlAppContext *ctx, const char *entry_point)
{
    if (!ctx || !entry_point) return -1;
    if (ctx->app_loaded) return 0; /* already loaded */
    if (!ctx->rt_init) return -1;  /* runtime not initialized */
    /* Note: the gate_modules pass in hl_app_context_init only runs when
     * !opts->no_load. Callers that defer loading via hl_app_context_load
     * must also drive the resolver themselves (main.c does this in
     * hl_serve_wire_caps). Keeping load() narrow so server's existing
     * orchestration is unchanged. */
    return load_app_code(ctx, entry_point);
}

/* ── Free ──────────────────────────────────────────────────────────── */

void hl_app_context_free(HlAppContext *ctx)
{
    if (!ctx) return;

#ifdef HL_ENABLE_DB
    /* Close the DB BEFORE destroying the runtime. The registry owns every
     * connection, including the "default"; destroying it runs sqlite3_close,
     * which fires each registered udf's xDestroy callback. Those callbacks free
     * the runtime-side closure the udf holds (a JS JS_DupValue ref / a Lua
     * registry ref), and they can only do so while the runtime is still alive.
     * Destroying the runtime first would trip QuickJS's JS_FreeRuntime
     * gc_obj_list-empty assertion on the still-referenced udf closure (and
     * silently leak the equivalent Lua ref). Connection-object finalizers run
     * later during the runtime destroy are safe against this ordering: a
     * borrowed conn's finalizer is a no-op (the registry owned the handle), and
     * an owned (db.open) conn's finalizer closes only its own handle, which was
     * never in the registry. ctx->db aliased the default's raw sqlite3*, so it
     * dangles after this. */
    if (ctx->db_registry) {
        hl_db_registry_destroy(ctx->db_registry);
        ctx->db_registry = NULL;
    }
    ctx->db = NULL;
    ctx->db_open = 0;
#endif

    if (ctx->rt_init && ctx->factory && ctx->factory->destroy) {
        ctx->factory->destroy(ctx->rt);
        ctx->rt = NULL;
    }

    /* WASM cache AFTER the runtime destroy: the runtime's GC finalizers
     * (WasmBuffer WASM-kind, WasmInstance) dereference WAMR, so it must outlive
     * them. */
#ifdef HL_ENABLE_WASM
    if (ctx->wasm_ok && !ctx->wasm_external)
        hl_cap_wasm_destroy(&ctx->wasm_cache);
#endif

    ctx->rt = NULL;
    ctx->rt_init = 0;
    ctx->app_loaded = 0;
    hl_platform_vfs_dispose(ctx->platform_vfs_owned);
    ctx->platform_vfs_owned = NULL;
    /* rt->fs_cfg borrowed &ctx->fs_cfg_storage, whose base_dir borrowed this;
     * the runtime is already destroyed above, so no live borrow remains. */
    /* Free the fs authorization policy (closes its held anchor fds; idempotent).
     * The runtime that borrowed rt->fs_cfg->policy is destroyed above. */
    hl_fs_policy_free(&ctx->fs_policy_storage);
    free(ctx->app_dir_copy);
    ctx->app_dir_copy = NULL;
    free(ctx);
}

/* ── Accessors ─────────────────────────────────────────────────────── */

struct sqlite3 *hl_app_context_db(HlAppContext *ctx)
{
#ifdef HL_ENABLE_DB
    return ctx ? ctx->db : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

HlDbHandle *hl_app_context_db_handle(HlAppContext *ctx)
{
#ifdef HL_ENABLE_DB
    return ctx ? hl_db_registry_default(ctx->db_registry) : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

HlRuntime *hl_app_context_runtime(HlAppContext *ctx)
{
    return ctx ? ctx->rt : NULL;
}

const HlVfs *hl_app_context_app_vfs(HlAppContext *ctx)
{
    return ctx ? &ctx->app_vfs : NULL;
}

const HlVfs *hl_app_context_platform_vfs(HlAppContext *ctx)
{
    return ctx ? &ctx->platform_vfs : NULL;
}

HlStmtCache *hl_app_context_stmt_cache(HlAppContext *ctx)
{
#ifdef HL_ENABLE_DB
    return ctx ? hl_db_sqlite_cache(hl_db_registry_default(ctx->db_registry))
               : NULL;
#else
    (void)ctx;
    return NULL;
#endif
}

const char *hl_app_context_app_dir(HlAppContext *ctx)
{
    return ctx ? ctx->app_vfs.root_dir : NULL;
}

/* Agnostic accessor: the runtime factory the context resolved (or NULL before
 * resolution). Lets the toolchain-only runtime-typed accessors
 * (app_context_runtime.c) compare against hl_<rt>_factory without this
 * base TU referencing any concrete runtime symbol. */
const HlRuntimeFactory *hl_app_context_factory(HlAppContext *ctx)
{
    return ctx ? ctx->factory : NULL;
}

/*
 * app_context.c - Reusable application context for commands
 *
 * Consolidates the VFS + DB + runtime init sequence that was
 * previously duplicated in agent_lib.c, commands/test.c, and main.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/app_context.h"
#include "hull/cap/run_watchdog.h"   /* hl_run_watchdog_set_app_limits */
#include "hull/entry.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include "hull/runtime/factory.h"
#include "hull/stdlib_feature.h"
#include "hull/vfs.h"
#include "hull/shared/term_escape.h"   /* hl_eprintf_line (audit 34) */
#include "log.h"

#include "hull/cap/fs_resolve.h"   /* hl_fs_reserve_root_path (audit 42) */
#include "hull/shared/state_dir.h"  /* the @state/ root (gap 3) */
#include <errno.h>
#include <sys/stat.h>
#include "hull/shared/fs_util.h"  /* hl_default_db_path_check */
#include "hull/sandbox.h"        /* hl_sandbox_db_path */
#include <limits.h>
#include "hull/shared/host.h"   /* HL_HOST_PATH_MAX */
#include "hull/utils/path_normalize.h"   /* hl_path_normalize */
#include "hull/cap/fs.h"          /* HlFsConfig - test/agent fs sandbox wiring
                                   * (unconditional: fs_cfg is not DB-gated) */
#include "hull/utils/alloc.h"     /* HlAllocator + hl_alloc_init for the fs policy */

#ifdef HL_ENABLE_DB
#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sqlite.h"   /* hl_db_sqlite_raw (agent handle) */
#include "hull/cap/db_registry.h"
#include "hull/cap/db_internal.h"
#include "hull/cap/db_budget.h"
#include <limits.h>
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
#include "hull/wasm_config.h"
#endif
#include "hull/cap/gpu.h"  /* base-resident: gpu_ctx propagates for composed feature too */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Opaque struct ─────────────────────────────────────────────────── */

struct HlAppContext {
#ifdef HL_ENABLE_DB
    int            db_open;       /* 1 = db was opened */
    HlDbRegistry  *db_registry;   /* owns all connections, incl. the "default" */
#endif
    HlVfs          app_vfs;
    HlVfs          platform_vfs;
    void          *platform_vfs_owned;  /* opaque sealed-arena handle to free, or NULL */
    HlRuntime     *rt;

    /* Owned app_dir copy backing fs_cfg_storage.base_dir (rt->fs_cfg borrows it,
     * so it must outlive the runtime - opts->app_dir may be a caller stack). */
    char          *app_dir_copy;
    int            app_dir_abs;    /* app_dir_copy was made absolute */
    /* fs capability config wired onto rt->fs_cfg when the app declares
     * fs.read/fs.write, so blob.* / fs.* / fs.mmap work under `hull test` /
     * `hull agent` the same way they do under `hull dev` (serve.c does the
     * equivalent wiring in hl_serve_wire_caps). Only base_dir/base_len - the
     * cap layer sandboxes to base_dir + rejects traversal; the per-path
     * allowlist is the kernel sandbox, which the test harness runs without. */
    HlFsConfig     fs_cfg_storage;
    HlFsPolicy     fs_policy_storage; /* compiled fs authorization policy */
    HlAllocator    fs_alloc;          /* owns the policy's memory (opts->alloc may be NULL) */
    /* The app's per-user state directory (shared/state_dir.h), resolved from
     * manifest state.name; "" = none. The CLI runner grants it to the sandbox
     * (hl_app_context_state_dir). */
    char           state_dir[PATH_MAX];
    /* The manifest's default database when it replaced the implicit one
     * (HlAppContextOpts.manifest_default_db); "" = not replaced. And whether
     * the implicit FILE default was absent when init opened it, so the file
     * it made can be removed once the manifest named another. */
    char           default_db_dsn[HL_HOST_PATH_MAX + 16];
    int            implicit_db_absent;
    /* The default internal database worked out from the implicit default,
     * and whether its file was absent before the app's top level ran: a
     * top-level stdlib init() makes it, and the re-point carries its tables
     * over and removes it (audit 44). */
    char           implicit_internal[HL_HOST_PATH_MAX + 32];
    int            implicit_internal_absent;
    int            opt_worker_db;   /* opts->worker_db_path given */
    /* The init options the manifest step (hl_app_context_manifest_db)
     * needs: serve.c defers the app load and runs that step itself. */
    int            opt_no_migrate;
    int            opt_keep_internal;
    int            opt_db_path_explicit;
    int            opt_manifest_default_db;
    int            opt_peek;          /* manifest_default_db_peek */
    int            opt_no_db;
    char           implicit_db_dsn[HL_HOST_PATH_MAX + 16];

    /* Resolved module set (opt-in via opts.gate_modules). Lives here so
     * its lifetime matches the runtime - rt->module_set borrows. */
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
        /* extension is ".lua" / ".js" - strip leading dot for detect_entry */
        const char *ext = f->entry_extension;
        if (ext[0] == '.') ext++;
        const char *e = detect_entry(app_dir, ext, entry_buf, buf_size);
        if (e) { *out_factory = f; return e; }
    }
    return NULL;
}

/* ── Internal: absolute app paths ──────────────────────────────────── */

/* A relative app directory or entry, made absolute against the working
 * directory and normalized, into a malloc'd string (NULL on failure). The
 * JS loader resolves a relative import against the importing module's name
 * and takes a relative result as app-root-relative, so an entry named
 * `app/app.js` (from `hull test app`) made `./lib/a.js` `app/lib/a.js`,
 * opened as `<app>/app/lib/a.js`. Absolute names are matched against the
 * app directory and work, so both are made absolute the same way. */
static char *abs_normalized(const char *path)
{
    char cwd[HL_HOST_PATH_MAX];
    size_t pl = strlen(path);
    if (path[0] == '/') {
        cwd[0] = '\0';
    } else if (!getcwd(cwd, sizeof cwd)) {
        return NULL;
    }
    size_t cl = strlen(cwd);
    if (pl > SIZE_MAX / 2 || cl > SIZE_MAX / 2) return NULL;
    char *out = malloc(cl + 1 + pl + 1);
    if (!out) return NULL;
    if (cl) {
        memcpy(out, cwd, cl);
        out[cl] = '/';
        memcpy(out + cl + 1, path, pl + 1);
    } else {
        memcpy(out, path, pl + 1);
    }
    if (hl_path_normalize(out) != 0) { free(out); return NULL; }
    return out;
}

/* A directory made absolute the way the kernel resolves it - realpath, so a
 * symlink before a `..` (`link/../app`) leads where the kernel would take
 * it, as the serve path names it - else lexically (abs_normalized). NULL
 * when neither works (audit 45: a path past 128 components, or with no
 * working directory, failed init; the caller keeps the path as given). */
static char *abs_dir_path(const char *path)
{
    char *rp = realpath(path, NULL);
    if (rp) return rp;
    return abs_normalized(path);
}

/* An entry file: its directory made absolute as the app directory is, so
 * the loader matches the two; the file name itself is kept (a symlinked
 * entry is the loaders' to judge). NULL on failure. */
static char *abs_entry_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    char *dir;
    const char *base;
    if (slash) {
        size_t dl = (size_t)(slash - path);
        char *d = malloc(dl + 2);
        if (!d) return NULL;
        if (dl == 0) { d[0] = '/'; d[1] = '\0'; }
        else { memcpy(d, path, dl); d[dl] = '\0'; }
        dir = abs_dir_path(d);
        free(d);
        base = slash + 1;
    } else {
        dir = abs_dir_path(".");
        base = path;
    }
    if (!dir) return NULL;
    size_t dl = strlen(dir), bl = strlen(base);
    if (dl > SIZE_MAX / 2 || bl > SIZE_MAX / 2) { free(dir); return NULL; }
    char *out = malloc(dl + 1 + bl + 1);
    if (!out) { free(dir); return NULL; }
    memcpy(out, dir, dl);
    size_t o = dl;
    if (o == 0 || out[o - 1] != '/') out[o++] = '/';
    memcpy(out + o, base, bl + 1);
    free(dir);
    return out;
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
        entry = discover_entry_in(base, base_count, ctx->app_dir_copy,
                                  entry_buf, buf_size, &ctx->factory);
        if (!entry) {
            size_t feat_count = 0;
            const HlRuntimeFactory *const *feats =
                hl_runtime_feature_factories(&feat_count);
            entry = discover_entry_in(feats, feat_count, ctx->app_dir_copy,
                                      entry_buf, buf_size, &ctx->factory);
        }
    } else {
        /* Caller provided entry point - match factory by file extension. */
        ctx->factory = hl_runtime_factory_for_filename(entry);
        /* A disk entry is named absolutely, as the directory is (see
         * abs_normalized); an embedded one is a VFS key and left alone. */
        if (ctx->app_dir_abs && entry[0] != '/') {
            /* Kept as given when it cannot be made absolute (as the
             * directory is, below). */
            char *abs = abs_entry_path(entry);
            if (abs && strlen(abs) < buf_size) {
                memcpy(entry_buf, abs, strlen(abs) + 1);
                entry = entry_buf;
            }
            free(abs);
        }
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

#ifdef HL_ENABLE_DB
/* Remove the file (and SQLite sidecars) the implicit default made at init,
 * now that the manifest named another default: Hull made it seconds ago and
 * only its own migrations touched it. Regular files only, never a link. (A
 * sidecar that was there before the run does not survive the open anyway:
 * SQLite drops a stale journal of an empty database at open and the -wal /
 * -shm at close.) */
static void remove_implicit_db(const char *dsn)
{
    const char *f = hl_sandbox_db_path(dsn);
    if (!f || strcmp(f, ":memory:") == 0) return;
    static const char *const suffixes[] = { "", "-wal", "-shm", "-journal" };
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        char p[PATH_MAX];
        int n = snprintf(p, sizeof p, "%s%s", f, suffixes[i]);
        if (n < 0 || (size_t)n >= sizeof p) return;
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISREG(st.st_mode)) (void)unlink(p);
    }
}

static int any_row(void *ctx, HlColumn *cols, int ncols)
{
    (void)cols; (void)ncols;
    *(int *)ctx = 1;
    return 0;
}

/* The internal connection a top-level stdlib init() opened on the implicit
 * default's internal database (now replaced: the default re-pointed): move
 * the _hull_* tables it made into the new internal database, close it, and
 * remove the file when this run made it (audit 44). Without this the
 * tables (sessions, roles, ...) were left behind, or - in memory - lost. */
static void carry_replaced_internal(HlAppContext *ctx)
{
    HlDbHandle old = {0};
    if (!hl_db_registry_take_replaced_internal(ctx->db_registry, &old)) return;
    const char *e = NULL;
    HlDbHandle *dst = hl_db_registry_internal(ctx->db_registry, &e);
    char err[512] = {0};
    int moved = -1;
    HlDbNativeTag tag = HL_DB_NATIVE_NONE;
    if (dst) (void)hl_db_backend_native_handle(dst, &tag);
    if (dst && tag != HL_DB_NATIVE_SQLITE) {
        /* A network / DuckDB default: the tables cannot be carried over (the
         * move is SQLite to SQLite). An init() at the top level that made
         * any did so before the manifest's default was known. */
        int any = 0;
        HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
        int q = hl_db_query(&old, "SELECT 1 FROM sqlite_master WHERE name LIKE "
                                  "'\\_hull\\_%' ESCAPE '\\' LIMIT 1",
                            NULL, 0, any_row, &any, NULL);
        hl_db_budget_restore(budget);
        if (q >= 0 && !any)
            moved = 0;
        else
            snprintf(err, sizeof err, "the new default database is not SQLite; "
                     "call the stdlib's init() from app.main or a handler, or "
                     "declare databases.internal");
    } else if (dst) {
        moved = hl_db_internal_move(&old, dst, "from the internal database the "
                                    "app's top level used", err, sizeof err);
    }
    if (moved < 0)
        log_warn("[hull:c] the stdlib tables an init() at the app's top level "
                 "made before the manifest's default database was known were "
                 "not carried over to the new internal database: %s",
                 dst ? err : (e ? e : "it cannot be opened"));
    old.backend->close(&old);
    if (moved >= 0 && ctx->implicit_internal_absent)
        remove_implicit_db(ctx->implicit_internal);
}

/* Make @p dsn (confined) the app's default database: close the implicit
 * default, open this one, check a file default the way the implicit one is
 * checked (hl_default_db_path_check), run the migrations on it, and give it
 * its internal database. 0 / -1 (reported). */
static int app_context_repoint_default(HlAppContext *ctx, const char *dsn)
{
    const char *f = hl_sandbox_db_path(dsn);
    if (f && strcmp(f, ":memory:") != 0) {
        char why[PATH_MAX + 160];
        if (hl_default_db_path_check(f, why, sizeof why) != 0) {
            hl_eprintf_line("[app-context] databases.named.default: %s", why);
            return -1;
        }
    }
    const char *implicit = ctx->implicit_db_dsn[0] ? ctx->implicit_db_dsn : ":memory:";
    /* Everything in an implicit default this run created (or an in-memory
     * one) was made by the app's top level and the migrations: it is kept
     * open across the re-point so the tables the top level made (jobs,
     * outbox, inbox, kv, ...) are carried into the new default (audit 44).
     * An implicit file that was there before the run is not read. */
    int carry = ctx->implicit_db_absent || strcmp(implicit, ":memory:") == 0;
    HlDbHandle old = {0};
    if (hl_db_registry_repoint_default_ex(ctx->db_registry, dsn,
                                          carry ? &old : NULL) != 0) {
        hl_eprintf_line("[app-context] cannot make the manifest's default database the default");
        return -1;
    }
    const char *derr = NULL;
    HlDbHandle *def = hl_db_registry_get(ctx->db_registry, "default", &derr);
    if (!def) {
        if (old.backend) old.backend->close(&old);
        hl_eprintf_line("[app-context] databases.named.default: %s",
                        derr ? derr : "cannot open the database");
        return -1;
    }
    ctx->db_open = 1;
    if (!ctx->opt_no_migrate &&
        hl_migrate_run(def, &ctx->app_vfs) == HL_MIGRATE_ERR) {
        if (old.backend) old.backend->close(&old);
        hl_eprintf_line("[app-context] migrations failed on the manifest's default database");
        return -1;
    }
    /* The manifest may name the implicit file itself: nothing to carry, and
     * it is not removed. */
    int same = 0;
    {
        const char *fi = hl_sandbox_db_path(implicit);
        struct stat a, b;
        same = fi && f && strcmp(fi, ":memory:") != 0 &&
               stat(fi, &a) == 0 && stat(f, &b) == 0 &&
               a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    }
    if (old.backend) {
        if (!same) {
            char lost[512] = {0}, err[512] = {0};
            int carried = hl_db_carry_new_tables(&old, def, lost, sizeof lost,
                                                 err, sizeof err);
            if (carried < 0) {
                old.backend->close(&old);
                hl_eprintf_line("[app-context] cannot carry the tables the app's top "
                                "level made into the manifest's default database: %s",
                                err);
                return -1;
            }
            if (lost[0])
                log_warn("[hull:c] the app's top level made table(s) %s in the "
                         "implicit default database before the manifest's default "
                         "was known, and they cannot be carried into it (a view, a "
                         "virtual table, or a default that is not SQLite): they are "
                         "lost. Create them in app.main or a handler, or in a "
                         "migration.", lost);
        }
        old.backend->close(&old);
    }
    if (ctx->implicit_db_absent && !same) remove_implicit_db(implicit);
    if (hl_db_internal_configure_ex(ctx->db_registry, ctx->app_dir_copy,
                                    ctx->state_dir[0] ? ctx->state_dir : NULL,
                                    dsn, ctx->opt_keep_internal) != 0) {
        hl_eprintf_line("[app-context] cannot work out the internal database path");
        return -1;
    }
    carry_replaced_internal(ctx);
    size_t n = strlen(dsn);
    if (n >= sizeof ctx->default_db_dsn) return -1;
    memcpy(ctx->default_db_dsn, dsn, n + 1);
    /* worker.dispatch's `db` and db.async on the default follow it (audit
     * 44): serve.c runs this step after the context's init, where the
     * worker DSN was set to the implicit default this re-point removed. No
     * worker thread runs yet - the pool is held until the phase-2 sandbox. */
    if (ctx->opt_worker_db) hl_worker_db_init(ctx->default_db_dsn);
    return 0;
}
#endif

int hl_app_context_manifest_db(HlAppContext *ctx, const HlManifest *m,
                               const char *state_dir)
{
#ifdef HL_ENABLE_DB
    if (!ctx || !m || !ctx->db_registry) return 0;
    /* The registry learns the state root first, so a "@state/" DSN
     * (databases.named / .internal / the default) confines to it. The copy
     * is the context's: serve.c resolved the directory itself. */
    if (state_dir && state_dir[0]) {
        if (!ctx->state_dir[0]) {
            size_t n = strlen(state_dir);
            if (n < sizeof ctx->state_dir) memcpy(ctx->state_dir, state_dir, n + 1);
        }
        if (hl_db_registry_set_state_dir(ctx->db_registry, state_dir) != 0) {
            hl_eprintf_line("[app-context] cannot resolve the state directory for "
                            "the database registry");
            return -1;
        }
    }
    /* The manifest's own default database (platform gap 3, PR 2):
     * `databases.named.default` names the app's default when the operator
     * gave no -d - the way an app.main tool keeps its database under
     * "@state/". The implicit default was opened and migrated at init,
     * before the manifest could be read: it is closed here, the file it
     * made removed, and the manifest's opened, migrated and given its
     * internal database. */
    if (!ctx->opt_manifest_default_db || ctx->opt_db_path_explicit || ctx->opt_no_db)
        return 0;
    hl_db_registry_set_manifest(ctx->db_registry, m);
    const char *derr = NULL;
    char dbuf[HL_HOST_PATH_MAX + 16];
    const char *d = hl_db_registry_manifest_default_dsn(ctx->db_registry, dbuf,
                                                        sizeof dbuf, &derr);
    hl_db_registry_set_manifest(ctx->db_registry, NULL);
    if (ctx->opt_peek) {
        /* Look only: nothing is opened, so nothing is created (audit 44).
         * A default that cannot be resolved (a state directory not made
         * yet) is no default. */
        size_t n = d ? strlen(d) : 0;
        if (d && n < sizeof ctx->default_db_dsn)
            memcpy(ctx->default_db_dsn, d, n + 1);
        return 0;
    }
    if (!d && derr) {
        hl_eprintf_line("[app-context] databases.named.default: %s", derr);
        return -1;
    }
    if (d && app_context_repoint_default(ctx, d) != 0) return -1;
    return 0;
#else
    (void)ctx; (void)m; (void)state_dir;
    return 0;
#endif
}

int hl_app_context_init(HlAppContext **out, const HlAppContextOpts *opts)
{
    if (!out || !opts || !opts->app_dir) return -1;

    HlAppContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    /* fds default -1 so hl_app_context_free never close(0)s stdin on a policy
     * that was never compiled (calloc gives base_fd == 0, a live fd). */
    ctx->fs_policy_storage = HL_FS_POLICY_INIT;
    hl_alloc_init(&ctx->fs_alloc, 0);   /* the policy needs a non-NULL allocator */

    /* Own a stable copy of app_dir for fs_cfg.base_dir (see struct comment).
     * In development (no embedded app files) it is made absolute, and so is a
     * relative entry (abs_normalized); a built binary's paths are kept. */
    extern const HlEntry hl_app_entries[];
    HlVfs probe;
    hl_vfs_init(&probe, hl_app_entries, NULL);
    if (!hl_vfs_embedded(&probe)) {
        ctx->app_dir_copy = abs_dir_path(opts->app_dir);
        ctx->app_dir_abs = ctx->app_dir_copy != NULL;
    }
    if (!ctx->app_dir_copy)
        ctx->app_dir_copy = strdup(opts->app_dir);
    if (!ctx->app_dir_copy) { free(ctx); return -1; }

    /* `.hull` is reserved from the disk loaders whether or not a policy
     * compiles (audit 42: only a compile registered the root, so an app with
     * no fs grant let a planted in-root symlink reach .hull). */
    (void)hl_fs_reserve_root_path(ctx->app_dir_copy);

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
            hl_app_context_free(ctx);   /* app_dir_copy too (audit 15) */
            return -1;
        }
        /* A databases.named / .internal file DSN must stay in the app tree
         * (audit 34): every entry point (serve, the app.main runner, hull
         * test / agent) builds its registry here, and the worker pool opens
         * the DSN the registry hands it. */
        if (hl_db_registry_set_app_dir(ctx->db_registry, ctx->app_dir_copy) != 0) {
            fprintf(stderr, "hull: cannot resolve the app directory for the "
                            "database registry\n");
            hl_db_registry_destroy(ctx->db_registry);
            ctx->db_registry = NULL;
            hl_app_context_free(ctx);
            return -1;
        }
        /* Whether the implicit file default is there yet: a manifest default
         * (below) may replace it, and the file this open makes is then
         * removed. The operator's -d is never touched. */
        if (db_path && !opts->db_path_explicit) {
            const char *f = hl_sandbox_db_path(db_path);
            struct stat st;
            ctx->implicit_db_absent = f && strcmp(f, ":memory:") != 0 &&
                                      lstat(f, &st) != 0 && errno == ENOENT;
        }
        ctx->opt_worker_db = opts->worker_db_path != NULL;
        ctx->opt_no_migrate          = opts->no_migrate;
        ctx->opt_keep_internal       = opts->keep_internal_in_app_db;
        ctx->opt_db_path_explicit    = opts->db_path_explicit;
        ctx->opt_manifest_default_db = opts->manifest_default_db;
        ctx->opt_peek                = opts->manifest_default_db && opts->manifest_default_db_peek;
        ctx->opt_no_db               = opts->no_db;
        if (db_path && strlen(db_path) < sizeof ctx->implicit_db_dsn)
            memcpy(ctx->implicit_db_dsn, db_path, strlen(db_path) + 1);
        /* Open the default connection only when a default DSN exists. With none
         * (postgres-only build, no -d), the app runs DB-less: db.default() is
         * lazy and errors only on use, matching a compute-only build. */
        if (db_path) {
            const char *derr = NULL;
            HlDbHandle *def = hl_db_registry_get(ctx->db_registry, "default", &derr);
            if (!def) {
                hl_eprintf_line("hull: %s", derr ? derr : "cannot open database");
                hl_db_registry_destroy(ctx->db_registry);
                ctx->db_registry = NULL;
                hl_app_context_free(ctx);
                return -1;
            }
            ctx->db_open = 1;
        }
        /* The Hull-owned internal database (audit convergence §7.2): the
         * stdlib's _hull_* tables live there, not in the app's database
         * file, unless the manifest declares databases.internal. Decided
         * from the default DSN: <app>/.hull/internal.db for a SQLite file
         * in the app tree, a file beside a -d database outside it, a
         * separate :memory: for an in-memory default (hull test / agent),
         * none for a network or DuckDB default. Moved into by
         * hl_db_internal_startup once the manifest is wired. */
        if (db_path) {
            if (hl_db_internal_configure(ctx->db_registry, ctx->app_dir_copy,
                                         db_path,
                                         opts->keep_internal_in_app_db) != 0) {
                fprintf(stderr, "hull: cannot work out the internal database "
                                "path\n");
                hl_db_registry_destroy(ctx->db_registry);
                ctx->db_registry = NULL;
                hl_app_context_free(ctx);
                return -1;
            }
            /* What of the implicit default's internal file is there before
             * the app's top level runs, for a re-point (audit 44). */
            const char *ip = !opts->db_path_explicit
                ? hl_db_registry_default_internal_path(ctx->db_registry) : NULL;
            if (ip && ip[0] == '/' && strlen(ip) < sizeof ctx->implicit_internal) {
                struct stat st;
                memcpy(ctx->implicit_internal, ip, strlen(ip) + 1);
                ctx->implicit_internal_absent = lstat(ip, &st) != 0 && errno == ENOENT;
            }
        }
    }
#endif

    /* Init VFS instances. The platform VFS unions the runtime-agnostic base
     * stdlib with each composed runtime's stdlib (via hl_stdlib_feature_entries);
     * in a base build with no runtime feature it borrows the static base. */
    hl_vfs_init(&ctx->app_vfs, hl_app_entries, ctx->app_dir_copy);
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

    /* Build base config bundle - wired into rt->base BEFORE init. */
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

    /* Init runtime via the factory - table-driven, single-path. */
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
         * Resolver failure is fatal - caller sees init failure. */
        if (load_rc == 0 && opts->gate_modules) {
            HlManifest m = {0};
            int manifest_tui = 0;   /* m is freed before the TUI gate opens */
            if (ctx->rt->vt->extract_manifest(ctx->rt, &m) == 0) {
                manifest_tui = m.tui;
                /* The app's run deadlines (cap/run_watchdog.h). */
                hl_run_watchdog_set_app_limits(m.limits_request_ms,
                                               m.limits_callback_ms);
                /* Every "$VAR" reference must name a declared variable, as the
                 * serve path and the app.main runner check (audit 17: `hull
                 * test` / `hull agent` resolved databases.internal and the
                 * other references against any variable). */
                char ref_err[256] = {0};
                if (hl_manifest_check_env_refs(&m, ref_err, sizeof ref_err) != 0) {
                    hl_eprintf_line("[app-context] manifest: %s", ref_err);
                    hl_manifest_free(&m);
                    hl_app_context_free(ctx);
                    return -1;
                }
                char err[HL_MODULE_RESOLVER_ERR_MAX] = {0};
                int rc = hl_module_resolver_resolve(&m, &ctx->module_set,
                                                     err, sizeof(err));
                /* The per-user state directory (shared/state_dir.h, gap 3),
                 * as serve.c resolves it: before the policy compiles against
                 * it. An app that declared one and cannot have it does not
                 * start. */
                ctx->state_dir[0] = '\0';
                if (m.state_invalid) {
                    hl_eprintf_line("[app-context] the manifest's `state` is not "
                                    "usable (see above); refusing to start");
                    hl_manifest_free(&m);
                    hl_app_context_free(ctx);
                    return -1;
                }
                if (m.state_name) {
                    /* A read-only look makes no state directory (audit 44). */
                    int sr = (opts->manifest_default_db && opts->manifest_default_db_peek)
                             ? (hl_state_dir_peek(m.state_name, ctx->state_dir,
                                                  sizeof ctx->state_dir) < 0 ? -1 : 0)
                             : hl_state_dir_resolve(m.state_name, ctx->state_dir,
                                                    sizeof ctx->state_dir);
                    if (sr != 0) {
                        hl_eprintf_line("[app-context] the state directory for "
                                        "state.name %s is not usable", m.state_name);
                        hl_manifest_free(&m);
                        hl_app_context_free(ctx);
                        return -1;
                    }
                    if (ctx->state_dir[0])
                        (void)hl_fs_reserve_root_path(ctx->state_dir);
                }
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
                    if (hl_fs_policy_compile_manifest_ex(
                            ctx->app_dir_copy,
                            ctx->state_dir[0] ? ctx->state_dir : NULL, &ctx->fs_alloc,
                            m.fs_read,  (size_t)m.fs_read_count,
                            m.fs_write, (size_t)m.fs_write_count,
                            &ctx->fs_policy_storage, &perr) != 0) {
                        hl_eprintf_line("[app-context] fs policy: %s",
                                perr ? perr : "compile failed");
                        hl_manifest_free(&m);
                        hl_app_context_free(ctx);
                        return -1;
                    }
                    ctx->fs_cfg_storage.base_dir = ctx->app_dir_copy;
                    ctx->fs_cfg_storage.base_len = strlen(ctx->app_dir_copy);
                    ctx->fs_cfg_storage.policy   = &ctx->fs_policy_storage;
                    if (ctx->state_dir[0]) {
                        ctx->fs_cfg_storage.state_dir = ctx->state_dir;
                        ctx->fs_cfg_storage.state_len = strlen(ctx->state_dir);
                    }
                    ctx->rt->fs_cfg = &ctx->fs_cfg_storage;
                }
#ifdef HL_ENABLE_DB
                /* The manifest's databases under the state root, and its own
                 * default database (gap 3, PR 2). serve.c, which defers the
                 * load, calls this itself once it has the manifest. */
                if (hl_app_context_manifest_db(ctx, &m,
                                               ctx->state_dir[0] ? ctx->state_dir : NULL) != 0) {
                    hl_manifest_free(&m);
                    hl_app_context_free(ctx);
                    return -1;
                }
#endif
#ifdef HL_ENABLE_WASM
                /* The manifest's `wasm` ceilings (and the operator's flags),
                 * as serve.c resolves them for the server: without this an
                 * app.main app, `hull test` and `hull agent` ran every
                 * compute call uncapped (round-6 M3). */
                hl_wasm_config_resolve(ctx->rt, &m, opts->wasm_cli);
#endif
                /* The input ceilings (manifest `limits`), as serve.c does. */
                hl_runtime_apply_input_limits(ctx->rt, &m);
                hl_manifest_free(&m);
                if (rc != 0) {
                    hl_eprintf_line("[app-context] module resolver: %s", err);
                    hl_app_context_free(ctx);
                    return -1;
                }
            } else {
                /* No manifest declared → resolver still admits intrinsics. */
                hl_module_resolver_resolve(NULL, &ctx->module_set, NULL, 0);
                hl_runtime_apply_input_limits(ctx->rt, NULL);
#ifdef HL_ENABLE_WASM
                hl_wasm_config_resolve(ctx->rt, NULL, opts->wasm_cli);
#endif
            }
            ctx->rt->module_set = &ctx->module_set;
            ctx->module_set_wired = 1;

            /* What the app's top level required / imported before this set
             * existed was let through unchecked; check it now, as serve.c
             * does. Without this an app.main app (the CLI runner, which
             * comes here) could import an undeclared module at top level. */
            char itrack_err[256] = {0};
            if (hl_import_tracker_validate(ctx->rt, ctx->rt->module_set,
                                           itrack_err, sizeof(itrack_err)) != 0) {
                hl_eprintf_line("[app-context] %s", itrack_err);
                hl_app_context_free(ctx);
                return -1;
            }
            /* Manifest applied: the TUI gate opens only for an app that
             * declared `tui = true` and hull/tui (audit 31). */
            {
                HlManifest tm = {0};
                tm.tui = manifest_tui;
                hl_tui_admit_resolved(&tm, ctx->rt->module_set);
            }
        }
    }

    /* Init worker DB after runtime init (needs db_path). */
#ifdef HL_ENABLE_DB
    if (ctx->default_db_dsn[0])
        hl_worker_db_init(ctx->default_db_dsn);   /* the manifest's default */
    else if (opts->worker_db_path)
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
     * never in the registry.
     *
     * Only the CONNECTIONS go here; the registry's memory is freed after the
     * runtime (audit 17). lua_close runs app __gc finalizers, and a borrowed
     * connection object holds a pointer into the registry (a handle slot), and
     * the runtime keeps the registry pointer itself: a finalizer calling
     * conn:query or db.default() read the unmapped page. Closed, the registry
     * refuses every lookup and each handle has no backend, so the finalizer
     * gets an error. */
    if (ctx->db_registry)
        hl_db_registry_close_all(ctx->db_registry);
    ctx->db_open = 0;
#endif

    if (ctx->rt_init && ctx->factory && ctx->factory->destroy) {
        ctx->factory->destroy(ctx->rt);
        ctx->rt = NULL;
    }

#ifdef HL_ENABLE_DB
    if (ctx->db_registry) {
        hl_db_registry_destroy(ctx->db_registry);
        ctx->db_registry = NULL;
    }
#endif

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
    /* Resolved on every call, never cached: the stale-transaction guard may
     * replace the default connection's sqlite3* (or withhold it, broken), and
     * a copy taken at open dangled after that (audit 12). NULL under a
     * non-SQLite backend; raw-pointer consumers guard it. */
    return ctx ? hl_db_sqlite_raw(hl_db_registry_default(ctx->db_registry))
               : NULL;
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

const char *hl_app_context_state_dir(HlAppContext *ctx)
{
    return (ctx && ctx->state_dir[0]) ? ctx->state_dir : NULL;
}

const char *hl_app_context_default_db_path(HlAppContext *ctx)
{
    return (ctx && ctx->default_db_dsn[0]) ? ctx->default_db_dsn : NULL;
}

/* Agnostic accessor: the runtime factory the context resolved (or NULL before
 * resolution). Lets the toolchain-only runtime-typed accessors
 * (app_context_runtime.c) compare against hl_<rt>_factory without this
 * base TU referencing any concrete runtime symbol. */
const HlRuntimeFactory *hl_app_context_factory(HlAppContext *ctx)
{
    return ctx ? ctx->factory : NULL;
}

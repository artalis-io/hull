/*
 * worker_db.c — Runtime-agnostic DB worker capability
 *
 * Per-worker SQLite connections via pthread TLS, and KlWorkItem callbacks
 * for db.async.query/exec operations. Zero Lua/JS knowledge.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/worker_db.h"
#include "hull/cap/db.h"
#include "hull/shared/async.h"
#include "hull/shared/thread_affinity.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/utils/alloc.h"

#include <keel/thread_pool.h>
#include <keel/async.h>

#include <pthread.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "log.h"

/* ── TLS for per-worker DB connections ─────────────────────────────── */

/* Each worker thread caches one connection per distinct database DSN it has
 * been asked for, in a small singly-linked list keyed by the resolved DSN,
 * most-recently-used at the head. The common case is a single node (the
 * default database); an app that runs db.async against several named
 * connections grows the list by one node each. All thread-local, so no locking
 * and no cross-thread connection sharing.
 *
 * The list is a bounded LRU: on a hit the node moves to the head, and a miss
 * that would exceed HL_WORKER_DB_MAX_CONNS first closes + evicts the tail. This
 * bounds a thread's connection set for an app that churns through many distinct
 * dynamic DSNs (db.open per-tenant shards); the cache stays purely a cache and
 * knows nothing about named vs dynamic. The cap is generous, so the small
 * manifest-bounded named/default set (<= HL_DB_REGISTRY_MAX) never evicts in
 * practice. Eviction between work items is safe: async ops are atomic (one
 * query/exec each, no transaction spans pooled ops). */
#define HL_WORKER_DB_MAX_CONNS 32
typedef struct WorkerConnNode {
    char                  *dsn;   /* owned; resolved DSN this connection opened from */
    HlWorkerDb             wdb;   /* embedded backend handle */
    struct WorkerConnNode *next;
} WorkerConnNode;

static pthread_key_t  worker_db_key;
static pthread_once_t worker_db_once = PTHREAD_ONCE_INIT;
static const char    *worker_db_dsn;   /* set once, read-only after: the default */

static void worker_db_destructor(void *ptr)
{
    WorkerConnNode *n = (WorkerConnNode *)ptr;
    while (n) {
        WorkerConnNode *next = n->next;
        if (n->wdb.handle.backend)
            n->wdb.handle.backend->close(&n->wdb.handle);
        free(n->dsn);
        free(n);
        n = next;
    }
}

static void worker_db_key_create(void)
{
    pthread_key_create(&worker_db_key, worker_db_destructor);
}

void hl_worker_db_init(const char *dsn)
{
    /* A SQLite file path is resolved to an absolute path so every worker
     * thread opens the same path the sandbox registered (realpath maps
     * /var → /private/var on macOS). A postgres:// DSN is not a filesystem
     * path, so only realpath when the selected backend is SQLite. */
    const HlDbBackend *be = hl_db_backend_select(dsn, NULL);
    /* native_tag identifies the backend without depending on the concrete
     * hl_db_backend_sqlite symbol, so no HL_ENABLE_SQLITE guard is needed. */
    int is_sqlite = (be && be->native_tag == HL_DB_NATIVE_SQLITE);
    if (is_sqlite) {
        static char resolved[PATH_MAX];
        if (realpath(dsn, resolved))
            worker_db_dsn = resolved;
        else
            worker_db_dsn = dsn;
    } else {
        worker_db_dsn = dsn;
    }
    pthread_once(&worker_db_once, worker_db_key_create);
}

/* Resolve @p dsn to the key this thread caches / opens against: a SQLite file
 * path is realpath'd (so the worker's open hits the exact sandbox-allowed
 * path); a postgres:// DSN passes through unchanged. @p out must be PATH_MAX. */
static const char *worker_db_resolve_key(const char *dsn, const HlDbBackend *be,
                                         char *out)
{
    /* native_tag identifies the backend without depending on the concrete
     * hl_db_backend_sqlite symbol, so no HL_ENABLE_SQLITE guard is needed. */
    int is_sqlite = (be && be->native_tag == HL_DB_NATIVE_SQLITE);
    if (is_sqlite && realpath(dsn, out))
        return out;
    return dsn;
}

HlWorkerDb *hl_worker_db_get_for(const char *dsn)
{
    if (!dsn) dsn = worker_db_dsn;
    if (!dsn) return NULL;

    const char *sel_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(dsn, &sel_err);
    if (!be) {
        log_error("[hull:worker_db] backend select failed: %s",
                  sel_err ? sel_err : "unknown");
        return NULL;
    }

    char keybuf[PATH_MAX];
    const char *key = worker_db_resolve_key(dsn, be, keybuf);

    WorkerConnNode *head = (WorkerConnNode *)pthread_getspecific(worker_db_key);

    /* Cache hit: unlink + move to the head (mark MRU) so the tail stays the LRU
     * eviction target. Count nodes on the way for the cap check below. */
    WorkerConnNode *prev = NULL;
    int count = 0;
    for (WorkerConnNode *n = head; n; prev = n, n = n->next) {
        count++;
        if (n->dsn && strcmp(n->dsn, key) == 0 && n->wdb.handle.backend) {
            if (prev) {
                prev->next = n->next;
                n->next = head;
                head = n;
                pthread_setspecific(worker_db_key, head);
            }
            return &n->wdb;
        }
    }

    /* Miss. Evict the LRU (tail) before opening if this thread is at the cap,
     * so churning through many dynamic DSNs does not grow the set without
     * bound. */
    if (count >= HL_WORKER_DB_MAX_CONNS && head) {
        WorkerConnNode *p = NULL, *t = head;
        while (t->next) { p = t; t = t->next; }
        if (p) p->next = NULL; else head = NULL;
        if (t->wdb.handle.backend)
            t->wdb.handle.backend->close(&t->wdb.handle);
        free(t->dsn);
        free(t);
    }

    WorkerConnNode *node = calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->dsn = strdup(key);
    if (!node->dsn) { free(node); return NULL; }

    node->wdb.handle.backend = be;
    if (be->open(&node->wdb.handle.ctx, key, NULL) != 0) {
        log_error("[hull:worker_db] %s open failed: dsn=%s errno=%d",
                  be->name, key, errno);
        free(node->dsn);
        free(node);
        return NULL;
    }

    node->next = head;
    pthread_setspecific(worker_db_key, node);
    return &node->wdb;
}

HlWorkerDb *hl_worker_db_get(void)
{
    return hl_worker_db_get_for(NULL);
}

/* ── Shared "get + namespace check" for the runtime bindings ──────── */

HlDbHandle *hl_worker_db_handle_checked(const char *sql, const char **out_err)
{
    static const char *e_no_db     = "database not available in worker";
    static const char *e_namespace = "access denied: _hull_* tables are reserved";

    HlWorkerDb *wdb = hl_worker_db_get();
    if (!wdb || !wdb->handle.backend) {
        if (out_err) *out_err = e_no_db;
        return NULL;
    }

    if (hl_cap_db_check_namespace(sql) != 0) {
        if (out_err) *out_err = e_namespace;
        return NULL;
    }

    return &wdb->handle;
}

/* ── HlDbResult helpers ────────────────────────────────────────────── */

void hl_db_result_free(HlDbResult *r)
{
    if (!r) return;
    if (r->col_names) {
        for (int i = 0; i < r->ncols; i++)
            free(r->col_names[i]);
        free(r->col_names);
    }
    if (r->values) {
        int total = r->nrows * r->ncols;
        for (int i = 0; i < total; i++) {
            if (r->values[i].s)
                free(r->values[i].s);
        }
        free(r->values);
    }
    memset(r, 0, sizeof(*r));
}

/* Grow result to hold at least one more row */
static int db_result_grow(HlDbResult *r)
{
    if (r->nrows < r->capacity)
        return 0;
    int new_cap = r->capacity ? r->capacity * 2 : 16;
    if (new_cap > 100000) return -1; /* sanity cap */
    if (r->ncols > 0 &&
        (size_t)new_cap > SIZE_MAX / ((size_t)r->ncols * sizeof(HlDbValue)))
        return -1; /* overflow guard */
    HlDbValue *nv = realloc(r->values,
                             (size_t)new_cap * (size_t)r->ncols * sizeof(HlDbValue));
    if (!nv) return -1;
    r->values = nv;
    r->capacity = new_cap;
    return 0;
}

/* Deep-copy one HlValue (a transient row cell from the backend) into the
 * owned HlDbValue that crosses the worker->event-loop thread boundary. */
static void materialize_value(const HlValue *in, HlDbValue *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->type) {
    case HL_TYPE_INT:
        out->type = HL_TYPE_INT;
        out->i    = in->i;
        break;
    case HL_TYPE_DOUBLE:
        out->type = HL_TYPE_DOUBLE;
        out->d    = in->d;
        break;
    case HL_TYPE_BOOL:
        /* HlDbValue has no bool field; carry the flag in `i` and let the
         * per-runtime converter push it as a boolean on HL_TYPE_BOOL. */
        out->type = HL_TYPE_BOOL;
        out->i    = in->b ? 1 : 0;
        break;
    case HL_TYPE_TEXT:
    case HL_TYPE_BLOB: {
        if (!in->s) { out->type = HL_TYPE_NIL; break; }
        out->type = in->type;
        size_t len = in->len;
        out->s = malloc(len + 1);
        if (out->s) {
            memcpy(out->s, in->s, len);
            out->s[len] = '\0';
            out->len = len;
        } else {
            out->type = HL_TYPE_NIL;
        }
        break;
    }
    case HL_TYPE_NIL:
    default:
        out->type = HL_TYPE_NIL;
        break;
    }
}

/* ── Materializing row callback (backend-agnostic) ─────────────────── */

typedef struct {
    HlDbResult *r;
    const char *err;   /* static message, set on failure */
} MaterializeCtx;

static int db_materialize_row_cb(void *ctx, HlColumn *cols, int ncols)
{
    MaterializeCtx *mc = (MaterializeCtx *)ctx;
    HlDbResult *r = mc->r;

    if (r->col_names == NULL) {   /* first row: capture the column names */
        r->ncols = ncols;
        r->col_names = calloc((size_t)ncols, sizeof(char *));
        if (!r->col_names) { mc->err = "out of memory"; r->ncols = 0; return -1; }
        for (int i = 0; i < ncols; i++) {
            const char *name = cols[i].name;
            r->col_names[i] = strdup(name ? name : "?");
            if (!r->col_names[i]) { mc->err = "out of memory"; return -1; }
        }
    }

    if (db_result_grow(r) != 0) { mc->err = "too many rows"; return -1; }

    HlDbValue *row = &r->values[r->nrows * r->ncols];
    for (int i = 0; i < r->ncols; i++)
        materialize_value(&cols[i].value, &row[i]);
    r->nrows++;
    return 0;
}

/* ── KlWorkItem callbacks ──────────────────────────────────────────── */

static void db_work_fn(void *ud)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    HlWorkerDb *wdb = hl_worker_db_get_for(op->dsn);
    if (!wdb) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "failed to open worker DB connection");
        return;
    }
    HlDbHandle *h = &wdb->handle;

    if (op->kind == HL_WORK_DB_EXEC) {
        int changes = hl_db_exec(h, op->sql, op->params, op->nparams);
        if (changes < 0) {
            op->error = 1;
            snprintf(op->error_msg, sizeof(op->error_msg),
                     "exec: %s", hl_db_errmsg(h));
        } else {
            op->exec_changes = changes;
            op->last_id = hl_db_last_id(h);
        }
        return;
    }

    /* HL_WORK_DB_QUERY: materialize all rows through the vtable. */
    HlDbResult *r = &op->result;
    memset(r, 0, sizeof(*r));
    MaterializeCtx mc = { .r = r, .err = NULL };
    int rc = hl_db_query(h, op->sql, op->params, op->nparams,
                         db_materialize_row_cb, &mc, NULL);
    if (mc.err) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg), "%s", mc.err);
    } else if (rc < 0) {
        op->error = 1;
        snprintf(op->error_msg, sizeof(op->error_msg),
                 "query: %s", hl_db_errmsg(h));
    }
}

static void db_done_fn(void *ud)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    if (op->cancelled) {
        /* Connection closed while work was in flight — clean up.
         * The cont was already freed by hl_worker_db_async_cancel. */
        HlAsyncCtx *ctx = op->async_ctx;
        hl_worker_db_op_free(op);
        free(op);
        if (ctx) hl_async_ctx_free(ctx);
        return;
    }
    HlAsyncCtx *ctx = op->async_ctx;
    if (ctx->detached) {
        /* No connection to revive — fire the cont directly on the
         * event-loop thread. resume_detached owns the teardown
         * (frees driver, cont, ctx). */
        hl_async_ctx_resume_detached(ctx);
    } else {
        /* Attached: wake the suspended FD; on_resume schedules the
         * cont fire on the event-loop thread + tears down. */
        hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
    }
}

static void db_cancel_fn(void *ud)
{
    /* Called by kl_thread_pool_free for items that never ran.
     * The async cancel handler already freed the cont (if it ran). */
    HlWorkerDbOp *op = (HlWorkerDbOp *)ud;
    HlAsyncCtx *ctx = op->async_ctx;
    hl_worker_db_op_free(op);
    free(op);
    if (ctx) hl_async_ctx_free(ctx);
}

/* ── Submit / free ─────────────────────────────────────────────────── */

int hl_worker_db_submit(HlAsyncBackendPool *pool, HlWorkerDbOp *op)
{
    if (!pool || !op) return -1;
    /* db.async submit is event-loop-only; workers use their own per-thread
     * sqlite connection on deep-copied inputs. */
    hl_assert_on_event_loop("hl_worker_db_submit (db.async)");
    const HlAsyncBackend *be = hl_async_backend();
    return be->pool_submit(pool, db_work_fn, db_done_fn, db_cancel_fn, op);
}

/* Deep-copy helper for HlValue params */
HlValue *hl_deep_copy_params(const HlValue *src, int n)
{
    if (n <= 0 || !src) return NULL;
    HlValue *dst = calloc((size_t)n, sizeof(HlValue));
    if (!dst) return NULL;
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
        if ((src[i].type == HL_TYPE_TEXT || src[i].type == HL_TYPE_BLOB) &&
            src[i].s && src[i].len > 0) {
            char *buf = malloc(src[i].len + 1);
            if (!buf) {
                /* Clean up already-copied strings */
                for (int j = 0; j < i; j++) {
                    if ((dst[j].type == HL_TYPE_TEXT || dst[j].type == HL_TYPE_BLOB) && dst[j].s)
                        hl_free_const(dst[j].s);
                }
                free(dst);
                return NULL;
            }
            memcpy(buf, src[i].s, src[i].len);
            buf[src[i].len] = '\0';
            dst[i].s = buf;
        }
    }
    return dst;
}

void hl_worker_db_op_free(HlWorkerDbOp *op)
{
    if (!op) return;
    free(op->sql);
    free(op->dsn);
    if (op->params) {
        for (int i = 0; i < op->nparams; i++) {
            if ((op->params[i].type == HL_TYPE_TEXT ||
                 op->params[i].type == HL_TYPE_BLOB) && op->params[i].s)
                hl_free_const(op->params[i].s);
        }
        free(op->params);
    }
    hl_db_result_free(&op->result);
}

void hl_worker_db_op_free_all(void *ptr)
{
    HlWorkerDbOp *op = (HlWorkerDbOp *)ptr;
    hl_worker_db_op_free(op);
    free(op);
}

void hl_worker_db_async_cancel(KlAsyncOp *kl_op, void *user_data)
{
    (void)kl_op;
    HlAsyncCtx *ctx = (HlAsyncCtx *)user_data;
    HlWorkerDbOp *op = (HlWorkerDbOp *)ctx->driver;

    /* Mark as cancelled — done_fn will handle cleanup */
    op->cancelled = 1;

    /* Cancel and destroy the runtime continuation */
    if (ctx->cont) {
        ctx->cont->cancel(ctx->cont);
        ctx->cont->destroy(ctx->cont);
        ctx->cont = NULL;
    }
    /* Don't free ctx or op — done_fn needs them */
}

#endif /* HL_ENABLE_DB */

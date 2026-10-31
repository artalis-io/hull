/*
 * cap/db_internal.c: the Hull-owned internal database (audit convergence
 * §7.2). See db_internal.h and docs/design/internal_db_separation.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_internal.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_budget.h"     /* hl_db_budget_swap */
#include "hull/cap/db.h"            /* hl_cap_db_ns_trust */
#include "hull/cap/db_registry.h"
#include "hull/shared/host.h"       /* HL_HOST_PATH_MAX */
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Referenced tables first (rbac's join tables name _hull_roles and
 * _hull_permissions); the drop runs this list backwards. Every table a module
 * reaching the internal connection creates: session, audit-log, rbac, totp,
 * idempotency, auth-flows (auth-health only reads them). Not attachment: its
 * metadata must commit with the app's own db.batch, so it keeps it on the app
 * connection unless databases.internal is declared (app_connection). */
static const char *const INTERNAL_TABLES[] = {
    "_hull_sessions",
    "_hull_audit_log",
    "_hull_roles",
    "_hull_permissions",
    "_hull_role_permissions",
    "_hull_user_roles",
    "_hull_totp",
    "_hull_totp_pending",
    "_hull_totp_pending_recovery",
    "_hull_totp_recovery",
    "_hull_totp_attempts",
    "_hull_totp_attempts_by_ip",
    "_hull_totp_attempts_by_ip_user",
    "_hull_idempotency_keys",
    "_hull_auth_used_tokens",
    "_hull_auth_pending_email_changes",
    "_hull_auth_login_attempts",
    NULL
};

const char *const *hl_db_internal_tables(void)
{
    return INTERNAL_TABLES;
}

/* ── Default path ─────────────────────────────────────────────────── */

/* 1 when @p path is @p dir itself or below it. */
static int at_or_under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/') return path[0] == '/';
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* The absolute, canonical spelling of @p path (which may not exist yet):
 * realpath of its deepest existing ancestor plus the rest. */
static int canonical(const char *path, char *out, size_t outsz)
{
    char abs[HL_HOST_PATH_MAX];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof abs) return -1;
        strcpy(abs, path);
    } else {
        char cwd[HL_HOST_PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return -1;
        int n = snprintf(abs, sizeof abs, "%s/%s", cwd, path);
        if (n < 0 || (size_t)n >= sizeof abs) return -1;
    }
    char real[PATH_MAX];
    if (realpath(abs, real)) {
        if (strlen(real) >= outsz) return -1;
        strcpy(out, real);
        return 0;
    }
    /* Not there yet: the deepest existing ancestor, then the rest. */
    char tmp[HL_HOST_PATH_MAX];
    strcpy(tmp, abs);
    size_t cut = strlen(tmp);
    while (cut > 0) {
        while (cut > 0 && tmp[cut] != '/') cut--;
        if (cut == 0) break;
        tmp[cut] = '\0';
        if (realpath(tmp, real)) {
            int n = snprintf(out, outsz, "%s%s",
                             strcmp(real, "/") == 0 ? "" : real, abs + cut);
            return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
        }
    }
    if (strlen(abs) >= outsz) return -1;
    strcpy(out, abs);
    return 0;
}

int hl_db_internal_default_dsn(const char *app_dir_real,
                               const char *default_dsn,
                               char *out, size_t outsz,
                               size_t *base_len, int *in_app_hull)
{
    if (base_len) *base_len = 0;
    if (in_app_hull) *in_app_hull = 0;
    if (!out || outsz == 0) return -1;
    out[0] = '\0';
    if (!default_dsn || !default_dsn[0] || default_dsn[0] == '$') return 0;

    /* Only a SQLite default: scheme-less, sqlite://, file://. */
    const char *path = default_dsn;
    const char *sep = strstr(default_dsn, "://");
    if (sep) {
        size_t n = (size_t)(sep - default_dsn);
        if ((n == 6 && strncasecmp(default_dsn, "sqlite", 6) == 0) ||
            (n == 4 && strncasecmp(default_dsn, "file", 4) == 0))
            path = sep + 3;
        else
            return 0;   /* network, DuckDB, or a scheme no backend takes */
    }
    if (!path[0] || strcmp(path, ":memory:") == 0) {
        if (outsz < sizeof ":memory:") return -1;
        strcpy(out, ":memory:");
        return 2;
    }
    if (!app_dir_real || !app_dir_real[0]) return 0;

    char db[HL_HOST_PATH_MAX];
    if (canonical(path, db, sizeof db) != 0) return -1;

    char hull_db[HL_HOST_PATH_MAX];
    int hn = snprintf(hull_db, sizeof hull_db, "%s/.hull/" HL_DB_INTERNAL_FILE,
                      strcmp(app_dir_real, "/") == 0 ? "" : app_dir_real);
    if (hn < 0 || (size_t)hn >= sizeof hull_db) return -1;

    int n;
    if (at_or_under(db, app_dir_real)) {
        /* The default database IS the internal file (-d .hull/internal.db):
         * no separate internal database to move anything into. */
        if (strcmp(db, hull_db) == 0) return 0;
        n = snprintf(out, outsz, "%s", hull_db);
        if (base_len) *base_len = strlen(app_dir_real);
        if (in_app_hull) *in_app_hull = 1;
    } else {
        n = snprintf(out, outsz, "%s" HL_DB_INTERNAL_BESIDE_SUFFIX, db);
        const char *sl = strrchr(out, '/');
        if (base_len) *base_len = sl ? (size_t)(sl - out) : 0;
    }
    if (n < 0 || (size_t)n >= outsz) { out[0] = '\0'; return -1; }
    return 1;
}

/* ── The move ─────────────────────────────────────────────────────── */

static int is_sqlite(HlDbHandle *h)
{
    HlDbNativeTag tag = HL_DB_NATIVE_NONE;
    (void)hl_db_backend_native_handle(h, &tag);
    return tag == HL_DB_NATIVE_SQLITE;
}

static int flag_row(void *ctx, HlColumn *cols, int ncols)
{
    (void)cols; (void)ncols;
    *(int *)ctx = 1;
    return 0;
}

/* 1 when @p h has table @p name, 0 when not, -1 on error. */
static int table_exists(HlDbHandle *h, const char *name)
{
    int found = 0;
    HlValue p = { .type = HL_TYPE_TEXT, .s = name, .len = strlen(name) };
    if (hl_db_query(h, "SELECT 1 FROM sqlite_master WHERE type = 'table' "
                       "AND name = ?", &p, 1, flag_row, &found, NULL) < 0)
        return -1;
    return found;
}

typedef struct {
    char **sql;
    int    n, cap;
} StrList;

static void strlist_free(StrList *l)
{
    for (int i = 0; i < l->n; i++) free(l->sql[i]);
    free(l->sql);
    memset(l, 0, sizeof *l);
}

static int strlist_push(StrList *l, const char *s, size_t len)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        if (cap > 4096) return -1;
        char **p = realloc(l->sql, (size_t)cap * sizeof *p);
        if (!p) return -1;
        l->sql = p;
        l->cap = cap;
    }
    char *c = malloc(len + 1);
    if (!c) return -1;
    memcpy(c, s, len);
    c[len] = '\0';
    l->sql[l->n++] = c;
    return 0;
}

static int ddl_row(void *ctx, HlColumn *cols, int ncols)
{
    if (ncols < 1 || cols[0].value.type != HL_TYPE_TEXT || !cols[0].value.s)
        return 0;
    return strlist_push(ctx, cols[0].value.s, cols[0].value.len) == 0 ? 0 : 1;
}

static void column_cb(void *ctx, const char *name)
{
    StrList *l = ctx;
    (void)strlist_push(l, name, strlen(name));
}

static int has_name(const StrList *l, const char *name)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->sql[i], name) == 0) return 1;
    return 0;
}

typedef struct {
    HlDbHandle *dst;
    const char *insert;
    int         ncols;
    long long   rows;
    int         failed;
} CopyCtx;

static int copy_row(void *ctx, HlColumn *cols, int ncols)
{
    CopyCtx *c = ctx;
    if (ncols != c->ncols) { c->failed = 1; return 1; }
    HlValue vals[256];
    if (ncols > (int)(sizeof vals / sizeof vals[0])) { c->failed = 1; return 1; }
    for (int i = 0; i < ncols; i++) vals[i] = cols[i].value;
    if (hl_db_exec(c->dst, c->insert, vals, ncols) < 0) {
        c->failed = 1;
        return 1;
    }
    c->rows++;
    return 0;
}

/* Append @p s to @p buf at *pos. */
static int append(char *buf, size_t sz, size_t *pos, const char *s)
{
    size_t n = strlen(s);
    if (*pos + n >= sz) return -1;
    memcpy(buf + *pos, s, n + 1);
    *pos += n;
    return 0;
}

/* 1 when @p sql starts with CREATE TABLE, CREATE INDEX or CREATE UNIQUE
 * INDEX (any case, blanks between): the only statements the move replays
 * from the source's schema. A planted file can hold any text under any type
 * in sqlite_master (writable_schema); a trigger replayed into the internal
 * database would keep firing on Hull's own writes there. */
static int ddl_allowed(const char *sql)
{
    static const char *const forms[3][3] = {
        { "CREATE", "TABLE", NULL },
        { "CREATE", "INDEX", NULL },
        { "CREATE", "UNIQUE", "INDEX" },
    };
    for (int f = 0; f < 3; f++) {
        const char *p = sql;
        int ok = 1;
        for (int w = 0; w < 3 && forms[f][w] && ok; w++) {
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            size_t n = strlen(forms[f][w]);
            ok = strncasecmp(p, forms[f][w], n) == 0 &&
                 (p[n] == ' ' || p[n] == '\t' || p[n] == '\n' || p[n] == '\r');
            p += n;
        }
        if (ok) return 1;
    }
    return 0;
}

/* Copy table @p t from @p src into @p dst inside one transaction on @p dst.
 * Returns the rows copied, or -1 with @p err set. */
static long long copy_table(HlDbHandle *src, HlDbHandle *dst, const char *t,
                            char *err, size_t errsz)
{
    long long result = -1;
    StrList ddl = {0}, scols = {0}, dcols = {0};
    char *select_sql = NULL, *insert_sql = NULL;
    int in_txn = 0;

    HlValue p = { .type = HL_TYPE_TEXT, .s = t, .len = strlen(t) };
    /* The table first, then its indexes (autoindexes have no SQL). Triggers
     * and views are not copied: none of Hull's own tables has one, and the
     * open-time check refuses a file holding one that names a _hull_ table. */
    if (hl_db_query(src, "SELECT sql FROM sqlite_master WHERE tbl_name = ? "
                         "AND type IN ('table', 'index') AND sql IS NOT NULL "
                         "ORDER BY type = 'index', rowid",
                    &p, 1, ddl_row, &ddl, NULL) < 0 || ddl.n == 0) {
        snprintf(err, errsz, "cannot read the definition of %s: %s", t,
                 hl_db_errmsg(src));
        goto out;
    }

    if (hl_db_begin(dst) != 0) {
        snprintf(err, errsz, "cannot begin a transaction on the target: %s",
                 hl_db_errmsg(dst));
        goto out;
    }
    in_txn = 1;

    int exists = table_exists(dst, t);
    if (exists < 0) {
        snprintf(err, errsz, "cannot inspect the target: %s", hl_db_errmsg(dst));
        goto out;
    }
    if (!exists) {
        for (int i = 0; i < ddl.n; i++) {
            if (!ddl_allowed(ddl.sql[i])) {
                snprintf(err, errsz, "the app database's schema for %s holds "
                         "a statement that is not CREATE TABLE / INDEX", t);
                goto out;
            }
            if (hl_db_exec(dst, ddl.sql[i], NULL, 0) < 0) {
                snprintf(err, errsz, "cannot create %s in the target: %s", t,
                         hl_db_errmsg(dst));
                goto out;
            }
        }
    }

    /* The columns both sides have (the target may be a newer schema). */
    if (hl_db_table_columns(src, t, column_cb, &scols) < 0 ||
        hl_db_table_columns(dst, t, column_cb, &dcols) < 0 || scols.n == 0) {
        snprintf(err, errsz, "cannot read the columns of %s", t);
        goto out;
    }
    size_t sz = 256 + (size_t)scols.n * 2 * 140;
    select_sql = malloc(sz);
    insert_sql = malloc(sz);
    if (!select_sql || !insert_sql) {
        snprintf(err, errsz, "out of memory");
        goto out;
    }
    char qt[140];
    if (hl_db_quote_ident(dst, t, qt, sizeof qt) < 0) {
        snprintf(err, errsz, "table name too long");
        goto out;
    }
    size_t sp = 0, ip = 0;
    int ncols = 0, fail = 0;
    fail |= append(select_sql, sz, &sp, "SELECT ");
    fail |= append(insert_sql, sz, &ip, "INSERT OR IGNORE INTO ");
    fail |= append(insert_sql, sz, &ip, qt);
    fail |= append(insert_sql, sz, &ip, " (");
    for (int i = 0; i < scols.n && !fail; i++) {
        if (!has_name(&dcols, scols.sql[i])) continue;
        char qc[140];
        if (hl_db_quote_ident(dst, scols.sql[i], qc, sizeof qc) < 0) { fail = 1; break; }
        if (ncols > 0) {
            fail |= append(select_sql, sz, &sp, ", ");
            fail |= append(insert_sql, sz, &ip, ", ");
        }
        fail |= append(select_sql, sz, &sp, qc);
        fail |= append(insert_sql, sz, &ip, qc);
        ncols++;
    }
    fail |= append(select_sql, sz, &sp, " FROM ");
    fail |= append(select_sql, sz, &sp, qt);
    fail |= append(insert_sql, sz, &ip, ") VALUES (");
    for (int i = 0; i < ncols && !fail; i++)
        fail |= append(insert_sql, sz, &ip, i ? ", ?" : "?");
    fail |= append(insert_sql, sz, &ip, ")");
    if (fail || ncols == 0) {
        snprintf(err, errsz, "cannot build the copy of %s", t);
        goto out;
    }

    CopyCtx cc = { .dst = dst, .insert = insert_sql, .ncols = ncols };
    if (hl_db_query(src, select_sql, NULL, 0, copy_row, &cc, NULL) < 0 ||
        cc.failed) {
        snprintf(err, errsz, "cannot copy the rows of %s: %s", t,
                 cc.failed ? hl_db_errmsg(dst) : hl_db_errmsg(src));
        goto out;
    }
    if (hl_db_commit(dst) != 0) {
        snprintf(err, errsz, "cannot commit the copy of %s: %s", t,
                 hl_db_errmsg(dst));
        goto out;
    }
    in_txn = 0;
    result = cc.rows;

out:
    if (in_txn) (void)hl_db_rollback(dst);
    free(select_sql);
    free(insert_sql);
    strlist_free(&ddl);
    strlist_free(&scols);
    strlist_free(&dcols);
    return result;
}

int hl_db_internal_move(HlDbHandle *src, HlDbHandle *dst, const char *what,
                        char *err, size_t errsz)
{
    if (err && errsz) err[0] = '\0';
    if (!src || !dst || !is_sqlite(src) || !is_sqlite(dst)) return 0;

    /* Startup work, not an app run: unbudgeted, and Hull's own statements on
     * _hull_* tables are trusted by the SQLite authorizer. */
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
    int ns_prev = hl_cap_db_ns_trust(1);
    int moved = 0, rc = 0;
    char ebuf[512];
    if (!err || errsz == 0) { err = ebuf; errsz = sizeof ebuf; }

    int ntab = 0;
    while (INTERNAL_TABLES[ntab]) ntab++;
    unsigned char present[64] = {0};

    for (int i = 0; i < ntab; i++) {
        int e = table_exists(src, INTERNAL_TABLES[i]);
        if (e < 0) {
            snprintf(err, errsz, "cannot inspect the source: %s",
                     hl_db_errmsg(src));
            rc = -1;
            goto out;
        }
        if (!e) continue;
        long long rows = copy_table(src, dst, INTERNAL_TABLES[i], err, errsz);
        if (rows < 0) { rc = -1; goto out; }
        present[i] = 1;
        log_info("[hull:c] internal database: copied %s (%lld rows) %s",
                 INTERNAL_TABLES[i], rows, what);
    }
    /* Every copy committed: drop from the source, referrers first. A crash
     * here leaves a table on both sides; the next start copies it again
     * (INSERT OR IGNORE: no row lost, none duplicated) and drops it. */
    for (int i = ntab - 1; i >= 0; i--) {
        if (!present[i]) continue;
        char sql[192];
        snprintf(sql, sizeof sql, "DROP TABLE \"%s\"", INTERNAL_TABLES[i]);
        if (hl_db_exec(src, sql, NULL, 0) < 0) {
            snprintf(err, errsz, "copied %s but cannot drop it from the "
                     "source: %s", INTERNAL_TABLES[i], hl_db_errmsg(src));
            rc = -1;
            goto out;
        }
        moved++;
    }
    rc = moved;

out:
    hl_cap_db_ns_restore(ns_prev);
    hl_db_budget_restore(budget);
    return rc;
}

/* ── Startup ──────────────────────────────────────────────────────── */

/* The internal database records, once the app database's internal tables
 * were moved into it (or there were none), that the move is done: from then
 * on internal tables that turn up in the app database again (a planted or
 * restored data.db) are ignored with a WARN instead of being merged into the
 * live internal data. Kept in the internal database itself, so it goes with
 * the data it describes. */
#define META_DDL "CREATE TABLE IF NOT EXISTS _hull_internal_meta " \
                 "(key VARCHAR(255) PRIMARY KEY, value TEXT)"
#define META_KEY "app_db_moved"

/* Run @p sql with the _hull_* authorizer trusted and no budget bound. */
static int trusted_exec(HlDbHandle *h, const char *sql, const HlValue *p, int n)
{
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
    int ns_prev = hl_cap_db_ns_trust(1);
    int rc = hl_db_exec(h, sql, p, n);
    hl_cap_db_ns_restore(ns_prev);
    hl_db_budget_restore(budget);
    return rc;
}

/* 1 when @p h records the move as done, 0 when not, -1 on error. */
static int moved_marker(HlDbHandle *h)
{
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
    int ns_prev = hl_cap_db_ns_trust(1);
    int found = 0, rc = table_exists(h, "_hull_internal_meta");
    if (rc > 0) {
        HlValue k = { .type = HL_TYPE_TEXT, .s = META_KEY, .len = strlen(META_KEY) };
        rc = hl_db_query(h, "SELECT 1 FROM _hull_internal_meta WHERE key = ?",
                         &k, 1, flag_row, &found, NULL) < 0 ? -1 : found;
    }
    hl_cap_db_ns_restore(ns_prev);
    hl_db_budget_restore(budget);
    return rc;
}

static int set_moved_marker(HlDbHandle *h, int on)
{
    if (trusted_exec(h, META_DDL, NULL, 0) < 0) return -1;
    HlValue k = { .type = HL_TYPE_TEXT, .s = META_KEY, .len = strlen(META_KEY) };
    if (!on)
        return trusted_exec(h, "DELETE FROM _hull_internal_meta WHERE key = ?",
                            &k, 1) < 0 ? -1 : 0;
    return trusted_exec(h, "INSERT OR REPLACE INTO _hull_internal_meta "
                           "(key, value) VALUES (?, datetime('now'))",
                        &k, 1) < 0 ? -1 : 0;
}

/* How many internal tables @p h holds (-1 on error). */
static int count_internal_tables(HlDbHandle *h)
{
    HlDbBudgetBinding budget = hl_db_budget_swap(NULL, NULL);
    int ns_prev = hl_cap_db_ns_trust(1);
    int n = 0;
    for (int i = 0; INTERNAL_TABLES[i]; i++) {
        int e = table_exists(h, INTERNAL_TABLES[i]);
        if (e < 0) { n = -1; break; }
        n += e;
    }
    hl_cap_db_ns_restore(ns_prev);
    hl_db_budget_restore(budget);
    return n;
}

const char *hl_db_internal_sandbox_path(const HlDbRegistry *reg)
{
    if (!hl_db_registry_internal_is_default(reg)) return NULL;
    const char *p = hl_db_registry_default_internal_path(reg);
    return (p && p[0] == '/') ? p : NULL;
}

int hl_db_internal_startup(HlDbRegistry *reg, int keep, int needed)
{
    if (!reg) return 0;
    HlDbHandle *def = hl_db_registry_default(reg);
    if (!def || !is_sqlite(def)) return 0;

    char err[512] = {0};
    if (!keep) {
        if (!hl_db_registry_internal_is_default(reg)) return 0;
        /* No file for an app that needs none (no internal module, nothing to
         * move, nothing made before). */
        const char *ipath = hl_db_registry_default_internal_path(reg);
        struct stat ist;
        int exists = !ipath || strcmp(ipath, ":memory:") == 0 ||
                     lstat(ipath, &ist) == 0;
        if (!exists && !needed) {
            int held = count_internal_tables(def);
            if (held == 0) return 0;
        }
        const char *oerr = NULL;
        /* Opened now, before the kernel sandbox narrows file access. */
        HlDbHandle *in = hl_db_registry_internal(reg, &oerr);
        if (!in) {
            log_error("[hull:c] cannot open the internal database: %s",
                      oerr ? oerr : "unknown error");
            return -1;
        }
        int done = moved_marker(in);
        if (done < 0) {
            log_error("[hull:c] cannot read the internal database: %s",
                      hl_db_errmsg(in));
            return -1;
        }
        if (done) {
            int stray = count_internal_tables(def);
            if (stray > 0)
                log_warn("[hull:c] the app database holds %d stdlib _hull_* "
                         "table(s) Hull no longer reads (they moved to the "
                         "internal database %s earlier): ignored. A data.db "
                         "restored from an old backup, or planted; drop them, "
                         "or run once with --keep-internal-in-app-db to move "
                         "the internal ones back first.", stray,
                         hl_db_registry_default_internal_path(reg));
            return 0;
        }
        int moved = hl_db_internal_move(def, in, "from the app database",
                                        err, sizeof err);
        if (moved < 0) {
            log_error("[hull:c] moving the stdlib's _hull_* tables into the "
                      "internal database failed: %s. Nothing was dropped "
                      "that was not copied; fix the cause and restart, or "
                      "run with --keep-internal-in-app-db for now.", err);
            return -1;
        }
        if (set_moved_marker(in, 1) != 0) {
            log_error("[hull:c] cannot record the internal-table move: %s",
                      hl_db_errmsg(in));
            return -1;
        }
        if (moved > 0)
            log_warn("[hull:c] moved %d stdlib table(s) (sessions, roles, "
                     "TOTP, audit log, ...) out of the app database into "
                     "the Hull-owned internal database %s - one-time; see "
                     "the CHANGELOG. --keep-internal-in-app-db moves them "
                     "back.", moved, hl_db_registry_default_internal_path(reg));
        return 0;
    }

    /* --keep-internal-in-app-db: the old layout. Move back whatever an
     * earlier start moved into the default internal file. */
    const char *path = hl_db_registry_default_internal_path(reg);
    if (!path || strcmp(path, ":memory:") == 0) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;   /* nothing was ever moved */
    if (!S_ISREG(st.st_mode)) {
        log_error("[hull:c] the internal database %s is not a regular file",
                  path);
        return -1;
    }
    const char *oerr = NULL;
    HlDbHandle tmp = {0};
    if (hl_db_registry_open_default_internal(reg, &tmp, &oerr) != 0) {
        log_error("[hull:c] cannot open the internal database: %s",
                  oerr ? oerr : "unknown error");
        return -1;
    }
    int moved = hl_db_internal_move(&tmp, def, "back into the app database",
                                    err, sizeof err);
    /* The next start without the flag moves the app database's tables
     * (written under the old layout meanwhile) in again. */
    if (moved >= 0 && set_moved_marker(&tmp, 0) != 0) {
        snprintf(err, sizeof err, "cannot clear the move record: %s",
                 hl_db_errmsg(&tmp));
        moved = -1;
    }
    tmp.backend->close(&tmp);
    if (moved < 0) {
        log_error("[hull:c] moving the stdlib's _hull_* tables back into the "
                  "app database failed: %s", err);
        return -1;
    }
    if (moved > 0)
        log_warn("[hull:c] --keep-internal-in-app-db: moved %d stdlib "
                 "table(s) back into the app database (the old layout)",
                 moved);
    return 0;
}

#endif /* HL_ENABLE_DB */

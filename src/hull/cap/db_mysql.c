/*
 * cap/db_mysql.c - MySQL / MariaDB backend (HlDbBackend vtable adapter)
 *
 * Pure-C wire client, no libmysql/libmariadb (mirrors the PostgreSQL backend:
 * cap/mysqlwire.c codec + cap/mysql_conn.c connection + this vtable adapter).
 * One backend serves `mysql://` and `mariadb://` (shared protocol; MariaDB is a
 * MySQL fork). Auth targets mysql_native_password + caching_sha2_password
 * (MySQL 8 default, full-auth over TLS) + client_ed25519 (MariaDB, TweetNaCl).
 *
 * PHASE 1 (skeleton): scheme routing + dialect + link-clean registration. The
 * wire codec, handshake/auth, and query protocols land in later phases; open()
 * fails cleanly until then.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_MYSQL

#include "hull/cap/db_backend.h"
#include "hull/cap/db_mysql.h"

#include "log.h"

/* ── Vtable methods (Phase 1 stubs) ───────────────────────────────────
 * Typed to the HlDbBackend signatures so the const vtable satisfies
 * -fsanitize=cfi-icall. Only open() is reachable in Phase 1; it fails, so the
 * query/exec/txn methods are never dispatched until the protocol lands. */

static int mysql_open(void **out_ctx, const char *dsn, HlAllocator *alloc)
{
    (void)out_ctx; (void)dsn; (void)alloc;
    log_error("[mysql] backend is a Phase 1 skeleton: the wire protocol is not "
              "implemented yet (mysql:// selects the backend but cannot connect)");
    return -1;
}

static void mysql_close(HlDbHandle *h) { (void)h; }

static int mysql_query(HlDbHandle *h, const char *sql,
                       const HlValue *params, int nparams,
                       HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)h; (void)sql; (void)params; (void)nparams;
    (void)cb; (void)cb_ctx; (void)alloc;
    return -1;
}

static int mysql_exec(HlDbHandle *h, const char *sql,
                      const HlValue *params, int nparams)
{
    (void)h; (void)sql; (void)params; (void)nparams;
    return -1;
}

static int mysql_exec_script(HlDbHandle *h, const char *sql)
{
    (void)h; (void)sql;
    return -1;
}

static int mysql_begin(HlDbHandle *h)    { (void)h; return -1; }
static int mysql_commit(HlDbHandle *h)   { (void)h; return -1; }
static int mysql_rollback(HlDbHandle *h) { (void)h; return -1; }

static int64_t mysql_last_id(HlDbHandle *h) { (void)h; return -1; }

static const char *mysql_errmsg(HlDbHandle *h)
{
    (void)h;
    return "mysql: backend not implemented (Phase 1 skeleton)";
}

static int mysql_insert_if_absent(HlDbHandle *h, const char *table,
                                  const char *const *conflict_cols,
                                  int n_conflict,
                                  const char *const *cols,
                                  const HlValue *values, int n_cols)
{
    (void)h; (void)table; (void)conflict_cols; (void)n_conflict;
    (void)cols; (void)values; (void)n_cols;
    return -1;
}

static int mysql_upsert(HlDbHandle *h, const char *table,
                        const char *const *conflict_cols, int n_conflict,
                        const char *const *cols,
                        const HlValue *values, int n_cols)
{
    (void)h; (void)table; (void)conflict_cols; (void)n_conflict;
    (void)cols; (void)values; (void)n_cols;
    return -1;
}

static int mysql_table_columns(HlDbHandle *h, const char *table,
                               HlDbColumnCallback cb, void *cb_ctx)
{
    (void)h; (void)table; (void)cb; (void)cb_ctx;
    return -1;
}

/* Both schemes route here; MariaDB shares the MySQL protocol. */
static const char *const mysql_schemes[] = { "mysql", "mariadb", NULL };

const HlDbBackend hl_db_backend_mysql = {
    .name                  = "mysql",
    .schemes               = mysql_schemes,
    /* MySQL / MariaDB dialect: `AUTO_INCREMENT` surrogate key, backtick
     * identifier quoting (the first consumer of §2.4's non-double-quote path),
     * no in-process UDF (§2.5). */
    .autoincrement_id_ddl  = "BIGINT AUTO_INCREMENT PRIMARY KEY",
    .identifier_quote      = '`',
    .native_tag            = HL_DB_NATIVE_MYSQL,
    .supports_udf          = 0,
    .open                  = mysql_open,
    .close                 = mysql_close,
    .query                 = mysql_query,
    .exec                  = mysql_exec,
    .exec_script           = mysql_exec_script,
    .begin                 = mysql_begin,
    .commit                = mysql_commit,
    .rollback              = mysql_rollback,
    .last_id               = mysql_last_id,
    .errmsg                = mysql_errmsg,
    .insert_if_absent      = mysql_insert_if_absent,
    .upsert                = mysql_upsert,
    .table_columns         = mysql_table_columns,
    /* guard_stale_txn + native_handle: NULL until the connection lands. */
};

#endif /* HL_ENABLE_MYSQL */

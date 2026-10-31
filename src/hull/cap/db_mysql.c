/*
 * cap/db_mysql.c - MySQL / MariaDB backend (HlDbBackend vtable adapter)
 *
 * Pure-C wire client, no libmysql/libmariadb (mirrors the PostgreSQL backend:
 * cap/mysqlwire.c codec + cap/mysql_conn.c connection + this vtable adapter).
 * One backend serves `mysql://` and `mariadb://` (shared protocol; MariaDB is a
 * MySQL fork).
 *
 * Provides connect, the COM_QUERY text protocol (param-less), and parameterized
 * query / exec via the binary prepared-statement protocol (COM_STMT_PREPARE /
 * EXECUTE / CLOSE). The insert_if_absent / upsert dialect helpers use MySQL's
 * INSERT + ER_DUP_ENTRY / ON DUPLICATE KEY syntax and the information_schema-backed
 * table_columns, plus multi-statement exec_script (migrations).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_MYSQL

#include "hull/cap/db_backend.h"
#include "hull/cap/db_mysql.h"
#include "hull/cap/mysql_conn.h"
#include "hull/cap/mysqlwire.h"
#include "hull/cap/db_sql_kw.h"

#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

typedef struct HlDbMyCtx {
    HlMyConn conn;
    char    *dsn;       /* to reconnect (secret: scrubbed on close) */
    /* InnoDB rolled the open transaction back under a failed statement (a
     * deadlock, or a lock-wait timeout under innodb_rollback_on_timeout):
     * later statements would each autocommit and the eventual
     * COMMIT would succeed, so calls refuse until a ROLLBACK (my_ready). */
    int      txn_aborted;
    /* Session state a later entry on this shared connection must not inherit,
     * read from the statements run on it (my_track_session; audit 39): a LOCK
     * TABLES (or FLUSH TABLES WITH READ LOCK) not yet released, a LOCK
     * INSTANCE FOR BACKUP not yet released (audit 40), an XA transaction
     * begun and not ended, and GET_LOCK calls not yet matched by
     * RELEASE_LOCK. Cleared when the session is reset or replaced. */
    int      lock_tables;
    int      lock_instance;
    int      xa_open;
    int      named_locks;
    /* MariaDB BACKUP LOCK / BACKUP STAGE not yet ended (audit 41). FLUSH
     * TABLES ... FOR EXPORT counts as lock_tables (UNLOCK TABLES ends it). */
    int      backup_lock;
    int      backup_stage;
    /* The connection was lost with autocommit off, and the app has since
     * rolled back (my_ready, audit 41): the next connection is opened with
     * autocommit off again, the session the app believes it has. */
    int      ac_off_acked;
} HlDbMyCtx;

/* A new or reset session holds none of the tracked state. */
static void my_session_clear(HlDbMyCtx *s)
{
    s->lock_tables = s->lock_instance = s->xa_open = s->named_locks = 0;
    s->backup_lock = s->backup_stage = 0;
    s->ac_off_acked = 0;
}

/* Forget everything the lost session held (the server ended it): the next
 * call reconnects into a fresh autocommit session. Only at an entry
 * boundary (the stale-transaction guard) or a reset abandoned. */
static void my_lost_forget(HlDbMyCtx *s)
{
    s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
    s->conn.server_status |= HL_MY_SERVER_STATUS_AUTOCOMMIT;
    s->txn_aborted = 0;
    my_session_clear(s);
}

/* A ROLLBACK on a lost connection, answered without being sent: the server
 * rolled the transaction back with the session. With autocommit off the app
 * still believes it has that session, so the next connection is opened with
 * autocommit off (my_ready, audit 41). */
static void my_lost_rolled_back(HlDbMyCtx *s)
{
    s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
    if (!(s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT))
        s->ac_off_acked = 1;
}

/* @p autocommit is what the new session runs with: always set explicitly,
 * since a server whose global default is off (SET GLOBAL autocommit = 0,
 * init_connect) still reports SERVER_STATUS_AUTOCOMMIT in the handshake, and
 * every write not committed by hand was then rolled back by the stale guard
 * (audit 41). One round trip per connect. */
static int my_connect(HlMyConn *conn, const char *dsn, int autocommit)
{
    HlMyDsn parsed;
    char err[128];
    if (hl_my_dsn_parse(dsn, &parsed, err, sizeof err) != 0) {
        /* A parse that failed part-way (a bad port after the password) has
         * already copied the password into @p parsed. */
        hl_my_dsn_scrub(&parsed);
        return -1;
    }
    int rc = hl_my_conn_open(conn, &parsed, 10000 /* 10s connect */);
    hl_my_dsn_scrub(&parsed);   /* password is secret material */
    if (rc != 0) return rc;
    if (hl_my_conn_query(conn, autocommit ? "SET autocommit = 1"
                                          : "SET autocommit = 0",
                         NULL, NULL, NULL, NULL) != 0) {
        conn->broken = 1;   /* first: the close sends no close_notify */
        char saved[sizeof conn->errmsg];
        snprintf(saved, sizeof saved, "%s", conn->errmsg);
        hl_my_conn_close(conn);
        snprintf(conn->errmsg, sizeof conn->errmsg, "%s", saved);
        return -1;
    }
    return 0;
}

/* 1 when @p sql has text MySQL runs or skips differently from the shared
 * reader (cap/db_sql_kw.h): an executable comment (slash-star-bang, and
 * MariaDB's slash-star-M-bang), which MySQL runs though the reader skips it, or a
 * '#' line comment, which the reader does not skip (audit 9 L1). A '#' in a
 * string literal counts too: misreading such a statement as unrecognised
 * only refuses a resume, never makes one. */
static int my_sql_unreadable(const char *sql)
{
    return sql && (strstr(sql, "/*!") || strstr(sql, "/*M!") ||
                   strchr(sql, '#'));
}

/* hl_sql_txn_kind for MySQL: a statement the reader cannot read reliably is
 * HL_SQL_TXN_OTHER - no implicit-commit resume after it, so a transaction it
 * ended is reported lost by the batch. */
static HlSqlTxnKind my_sql_txn_kind(const char *sql)
{
    /* MySQL's block comments do not nest (audit 10). */
    return my_sql_unreadable(sql) ? HL_SQL_TXN_OTHER : hl_sql_txn_kind_ex(sql, 0);
}

/* 1 when @p sql is ONE statement (a trailing ';', whitespace and comments
 * allowed). The connection runs with CLIENT_MULTI_STATEMENTS, and every
 * classifier here reads the first statement only: "CREATE TABLE t (x INT);
 * COMMIT" ended the batch's transaction by its COMMIT, and the resume opened a
 * new one under the DDL's name, so the batch never saw its transaction go
 * (audit 10). Anything this scanner cannot read the way MySQL does - a
 * backslash in a quoted string (its meaning follows NO_BACKSLASH_ESCAPES), a
 * '#' or executable comment, a "--" MySQL would not take as a comment, an
 * unterminated quote or comment - is not one statement either. */
static int my_sql_single_statement(const char *sql)
{
    if (!sql || my_sql_unreadable(sql)) return 0;
    int ended = 0;
    const char *p = sql;
    while (*p) {
        char c = *p;
        if (c == '\'' || c == '"' || c == '`') {
            if (ended) return 0;
            p++;
            for (;;) {
                if (!*p) return 0;
                if (*p == '\\' && c != '`') return 0;
                if (*p == c) {
                    if (p[1] == c) { p += 2; continue; }   /* doubled quote */
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }
        if (c == '/' && p[1] == '*') {          /* MySQL: no nesting */
            const char *e = strstr(p + 2, "*/");
            if (!e) return 0;
            p = e + 2;
            continue;
        }
        if (c == '-' && p[1] == '-') {
            if (!(p[2] == ' ' || p[2] == '\t' || p[2] == '\n' || p[2] == '\r'))
                return 0;
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == ';') { ended = 1; p++; continue; }
        if (ended && !isspace((unsigned char)c)) return 0;
        p++;
    }
    return 1;
}

/* 1 when @p sql is a ROLLBACK of the whole transaction (comments and
 * ROLLBACK WORK included; not ROLLBACK TO SAVEPOINT). A connection lost inside
 * a transaction lost the transaction with it - the server rolled it back - so
 * a ROLLBACK for it has already happened, and the backend answers it without
 * sending anything. So it must be the WHOLE text, read the way MySQL reads it
 * (my_sql_single_statement): anything after it would be answered "done"
 * without having run (audit 12). The reader already sends trailing text to
 * HL_SQL_TXN_OTHER; this also refuses the texts the two read differently (a
 * "--" MySQL takes for minus signs, which the reader skipped as a comment). */
static int sql_is_rollback(const char *sql)
{
    return my_sql_single_statement(sql) &&
           my_sql_txn_kind(sql) == HL_SQL_TXN_ROLLBACK;
}

/* 1 when @p sql is ONE COMMIT statement, read the way MySQL reads it: once
 * it is wholly sent the server commits whatever the client does, so a run
 * stopped while its OK is awaited finishes the read (HlMyConn.commit_next,
 * audit 31) rather than dropping the connection and reporting the commit
 * lost. */
static int sql_is_commit(const char *sql)
{
    return my_sql_single_statement(sql) &&
           my_sql_txn_kind(sql) == HL_SQL_TXN_COMMIT;
}

static void scrub_free(char *s)
{
    if (!s) return;
    volatile char *p = s;
    while (*p) *p++ = 0;
    free(s);
}

/* A statement that ends a piece of session state a lost connection held
 * (audit 41). Only ONE statement (my_sql_single_statement), read by its
 * leading words. */
typedef enum {
    MY_RESTORE_NONE = 0,
    MY_RESTORE_AUTOCOMMIT,      /* SET [SESSION|LOCAL|@@[session.]]autocommit = 1|ON|TRUE */
    MY_RESTORE_UNLOCK_TABLES,   /* UNLOCK TABLE[S] */
    MY_RESTORE_UNLOCK_INSTANCE, /* UNLOCK INSTANCE */
    MY_RESTORE_BACKUP_UNLOCK,   /* BACKUP UNLOCK (MariaDB) */
    MY_RESTORE_BACKUP_END,      /* BACKUP STAGE END (MariaDB) */
    MY_RESTORE_XA_END           /* XA COMMIT / XA ROLLBACK */
} MyRestore;

static MyRestore my_restore_kind(const char *sql)
{
    if (!my_sql_single_statement(sql)) return MY_RESTORE_NONE;
    char w[16], w2[16], w3[16];
    const char *p = hl_sql_next_word_ex(sql, w, sizeof w, 0);
    if (strcmp(w, "set") == 0) {
        p = hl_sql_skip_space_ex(p, 0);
        if (p[0] == '@' && p[1] == '@') {
            p += 2;
            if (strncasecmp(p, "session.", 8) == 0) p += 8;
            else if (strncasecmp(p, "local.", 6) == 0) p += 6;
            p = hl_sql_next_word_ex(p, w2, sizeof w2, 0);
        } else {
            p = hl_sql_next_word_ex(p, w2, sizeof w2, 0);
            if (strcmp(w2, "session") == 0 || strcmp(w2, "local") == 0)
                p = hl_sql_next_word_ex(p, w2, sizeof w2, 0);
        }
        if (strcmp(w2, "autocommit") != 0) return MY_RESTORE_NONE;
        p = hl_sql_skip_space_ex(p, 0);
        if (p[0] == ':' && p[1] == '=') p += 2;
        else if (p[0] == '=') p++;
        else return MY_RESTORE_NONE;
        p = hl_sql_next_word_ex(p, w3, sizeof w3, 0);
        if (strcmp(w3, "1") != 0 && strcmp(w3, "on") != 0 &&
            strcmp(w3, "true") != 0)
            return MY_RESTORE_NONE;
        p = hl_sql_skip_space_ex(p, 0);
        while (*p == ';') p = hl_sql_skip_space_ex(p + 1, 0);
        return *p ? MY_RESTORE_NONE : MY_RESTORE_AUTOCOMMIT;
    }
    p = hl_sql_next_word_ex(p, w2, sizeof w2, 0);
    if (strcmp(w, "unlock") == 0) {
        if (strcmp(w2, "tables") == 0 || strcmp(w2, "table") == 0)
            return MY_RESTORE_UNLOCK_TABLES;
        if (strcmp(w2, "instance") == 0) return MY_RESTORE_UNLOCK_INSTANCE;
        return MY_RESTORE_NONE;
    }
    if (strcmp(w, "backup") == 0) {
        if (strcmp(w2, "unlock") == 0) return MY_RESTORE_BACKUP_UNLOCK;
        (void)hl_sql_next_word_ex(p, w3, sizeof w3, 0);
        if (strcmp(w2, "stage") == 0 && strcmp(w3, "end") == 0)
            return MY_RESTORE_BACKUP_END;
        return MY_RESTORE_NONE;
    }
    if (strcmp(w, "xa") == 0 &&
        (strcmp(w2, "commit") == 0 || strcmp(w2, "rollback") == 0))
        return MY_RESTORE_XA_END;
    return MY_RESTORE_NONE;
}

/* What a lost connection's session held that a new connection would not
 * (audit 41), or NULL: autocommit off (not yet acknowledged by a ROLLBACK),
 * a LOCK TABLES / FLUSH TABLES ... WITH READ LOCK / FOR EXPORT, a LOCK
 * INSTANCE FOR BACKUP, an XA transaction, a MariaDB BACKUP LOCK / STAGE. */
static const char *my_lost_held(const HlDbMyCtx *s)
{
    if (!(s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT) &&
        !s->ac_off_acked)
        return "autocommit off (roll back, or SET autocommit = 1)";
    if (s->lock_tables)   return "LOCK TABLES (UNLOCK TABLES)";
    if (s->lock_instance) return "LOCK INSTANCE FOR BACKUP (UNLOCK INSTANCE)";
    if (s->backup_lock)   return "BACKUP LOCK (BACKUP UNLOCK)";
    if (s->backup_stage)  return "BACKUP STAGE (BACKUP STAGE END)";
    if (s->xa_open)       return "an XA transaction (XA ROLLBACK / XA COMMIT)";
    return NULL;
}

/* Before any use: replace a connection left out of step (HlMyConn.broken), as
 * db_postgres.c's pg_ready does - outside a transaction only. Inside one the
 * server rolled the transaction back when the connection went, and statements
 * run on a new connection would each commit alone, so every call refuses until
 * a ROLLBACK (@p sql), which has in effect already happened and returns 1.
 *
 * The same holds for session state the lost session held with no transaction
 * open (audit 41): autocommit off, a table / instance / backup lock, an XA
 * transaction. A new connection runs in autocommit with no lock, so the app's
 * next writes would commit one by one and its ROLLBACK lie, and other sessions
 * would read the "locked" tables. Calls refuse until the app ends each piece:
 * a ROLLBACK (autocommit off: the next connection is opened with autocommit
 * off again), SET autocommit = 1, UNLOCK TABLES, UNLOCK INSTANCE, BACKUP
 * UNLOCK, BACKUP STAGE END - each answered done without being sent, since the
 * server ended that state with the session - or XA COMMIT / ROLLBACK, which is
 * sent on the new connection (a prepared XA transaction outlives the session),
 * or until the entry ends (the stale-transaction guard forgets it all). Until
 * then mysql_session_held still reports the state, so waits stay refused.
 *
 * Returns 0 to go ahead, 1 done, -1 refused. A @p pinned handle (it holds a
 * session-scoped lock: hl_migrate_run) is not reconnected either: the lock
 * went with the session, and the run would go on without it (audit 6 L5). */
static int my_ready(HlDbMyCtx *s, const char *sql, int pinned)
{
    /* A call starts: an error code left now is this call's server ERR
     * (my_note_failure), not an earlier one's. */
    s->conn.last_err_code = 0;
    if (s->txn_aborted) {
        if (sql_is_rollback(sql)) {   /* the server already rolled back */
            s->txn_aborted = 0;
            s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
            return 1;
        }
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the server rolled this transaction back (a deadlock or a "
                 "lock-wait timeout), so none of its statements were "
                 "applied; roll back and retry");
        return -1;
    }
    if (!s->conn.broken)
        return 0;
    int in_trans = (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS) != 0;
    if ((in_trans || my_lost_held(s)) && sql_is_rollback(sql)) {
        my_lost_rolled_back(s);
        return 1;
    }
    if (in_trans) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost inside a transaction, which the "
                 "server rolled back; roll back and retry");
        return -1;
    }
    if (my_lost_held(s)) {
        switch (my_restore_kind(sql)) {
        case MY_RESTORE_AUTOCOMMIT:
            if (s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT) break;
            s->conn.server_status |= HL_MY_SERVER_STATUS_AUTOCOMMIT;
            s->ac_off_acked = 0;
            return 1;
        case MY_RESTORE_UNLOCK_TABLES:
            if (!s->lock_tables) break;
            s->lock_tables = 0;
            return 1;
        case MY_RESTORE_UNLOCK_INSTANCE:
            if (!s->lock_instance) break;
            s->lock_instance = 0;
            return 1;
        case MY_RESTORE_BACKUP_UNLOCK:
            if (!s->backup_lock) break;
            s->backup_lock = 0;
            return 1;
        case MY_RESTORE_BACKUP_END:
            if (!s->backup_stage) break;
            s->backup_stage = 0;
            return 1;
        case MY_RESTORE_XA_END:
            s->xa_open = 0;   /* sent on the new connection, if nothing else */
            break;
        case MY_RESTORE_NONE:
            break;
        }
        const char *held = my_lost_held(s);
        if (held) {
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                     "the connection was lost while its session held %s; the "
                     "server ended that with the session and a new one would "
                     "not hold it, so calls are refused until it is ended or "
                     "the entry ends", held);
            return -1;
        }
    }
    if (pinned) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost while it held a session lock (the "
                 "migration lock), which went with it; not reconnecting");
        return -1;
    }
    int autocommit = !(s->ac_off_acked &&
                       !(s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT));
    HlMyConn fresh;
    memset(&fresh, 0, sizeof fresh);
    if (my_connect(&fresh, s->dsn, autocommit) != 0) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "the connection was lost, and reconnecting failed: %.150s",
                 fresh.errmsg[0] ? fresh.errmsg : "unknown error");
        return -1;
    }
    hl_my_conn_close(&s->conn);
    s->conn = fresh;
    my_session_clear(s);   /* a new session */
    return 0;
}

/* -- Session state an entry may leave behind (audit 39) ------------- */

typedef struct {
    HlDbMyCtx *s;
    int        ok;        /* the statement succeeded: releasing words count */
    int        take_only; /* only words that take state count (audit 40) */
} MyTrack;

static void my_track_word(void *ud, const char *w, const char *prev,
                          const char *prev2)
{
    MyTrack *t = ud;
    HlDbMyCtx *s = t->s;
    int rel = t->ok && !t->take_only;
    if (strcmp(w, "get_lock") == 0) {
        if (s->named_locks < 1000000) s->named_locks++;
    } else if (strcmp(w, "release_lock") == 0) {
        if (rel && s->named_locks > 0) s->named_locks--;
    } else if (strcmp(w, "release_all_locks") == 0) {
        if (rel) s->named_locks = 0;
    } else if (strcmp(w, "table") == 0 || strcmp(w, "tables") == 0) {
        if (strcmp(prev, "lock") == 0) s->lock_tables = 1;
        else if (strcmp(prev, "unlock") == 0 && rel) s->lock_tables = 0;
    } else if (strcmp(w, "lock") == 0 && strcmp(prev, "read") == 0 &&
               strcmp(prev2, "with") == 0) {
        s->lock_tables = 1;   /* FLUSH TABLES ... WITH READ LOCK (audit 40) */
    } else if (strcmp(w, "export") == 0 && strcmp(prev, "for") == 0) {
        s->lock_tables = 1;   /* FLUSH TABLES ... FOR EXPORT (audit 41) */
    } else if (strcmp(prev, "backup") == 0 &&
               (strcmp(w, "lock") == 0 || strcmp(w, "unlock") == 0 ||
                strcmp(w, "stage") == 0)) {
        /* MariaDB BACKUP LOCK t / BACKUP UNLOCK, BACKUP STAGE x (audit 41) */
        if (strcmp(w, "lock") == 0) s->backup_lock = 1;
        else if (strcmp(w, "unlock") == 0) { if (rel) s->backup_lock = 0; }
        else s->backup_stage = 1;
    } else if (strcmp(w, "end") == 0 && strcmp(prev, "stage") == 0 &&
               strcmp(prev2, "backup") == 0) {
        if (rel) s->backup_stage = 0;   /* BACKUP STAGE END */
    } else if (strcmp(w, "instance") == 0) {
        /* LOCK INSTANCE FOR BACKUP / UNLOCK INSTANCE (audit 40). */
        if (strcmp(prev, "lock") == 0) s->lock_instance = 1;
        else if (strcmp(prev, "unlock") == 0 && rel) s->lock_instance = 0;
    } else if (strcmp(prev, "xa") == 0) {
        if (strcmp(w, "start") == 0 || strcmp(w, "begin") == 0) s->xa_open = 1;
        else if ((strcmp(w, "commit") == 0 || strcmp(w, "rollback") == 0) &&
                 rel)
            s->xa_open = 0;
    }
}

/* Every word of @p sql, read as code: quotes and comments are not skipped
 * (the take-only pass over a text the scanner may misread). */
static void my_scan_raw_words(const char *sql, HlSqlWordFn fn, void *ud)
{
    char w[HL_SQL_SCAN_WORD_MAX];
    char prev[HL_SQL_SCAN_WORD_MAX] = "", prev2[HL_SQL_SCAN_WORD_MAX] = "";
    const char *p = sql;
    while (*p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '_') {
            size_t n = 0;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_') {
                char ch = *p++;
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                if (n + 1 < sizeof w) w[n++] = ch;
                else n = sizeof w;
            }
            w[n < sizeof w ? n : 0] = '\0';
            fn(ud, w, prev, prev2);
            memcpy(prev2, prev, sizeof prev2);
            memcpy(prev, w, sizeof prev);
            continue;
        }
        p++;
    }
}

/* A text the session tracker's word scanner may misread (audit 40): what
 * my_sql_unreadable names (an executable comment - which the scanner also
 * skips when it follows another comment - or a `#` comment), or a
 * backslash, which ends no string under NO_BACKSLASH_ESCAPES. */
static int my_sql_track_unreadable(const char *sql)
{
    return my_sql_unreadable(sql) || strchr(sql, '\\') != NULL ||
           strstr(sql, "/*m!") != NULL;
}

/* Note what @p sql (run with result @p rc) did to the session. Words that
 * take state (LOCK TABLES, FLUSH TABLES WITH READ LOCK, LOCK INSTANCE, XA
 * START, GET_LOCK) count whether or not the statement succeeded; words that
 * release it only when it did. START TRANSACTION / BEGIN releases LOCK
 * TABLES. Conservative where the text cannot be read reliably (audit 40):
 * every word of it counts as taking state, quotes and comments included,
 * and none as releasing it. A misread that way costs a refused wait in the
 * entry and a session reset at its end, no more. */
static void my_track_session(HlDbHandle *h, const char *sql, int rc)
{
    HlDbMyCtx *s = h ? h->ctx : NULL;
    if (!s || !sql) return;
    if (my_sql_track_unreadable(sql)) {
        MyTrack t = { s, rc >= 0, 1 };
        my_scan_raw_words(sql, my_track_word, &t);
        return;
    }
    MyTrack t = { s, rc >= 0, 0 };
    hl_sql_scan_words(sql, 0, 1, my_track_word, &t);
    if (t.ok && my_sql_txn_kind(sql) == HL_SQL_TXN_BEGIN)
        s->lock_tables = 0;
}

/* State no entry can be holding legitimately at an entry boundary: every
 * later entry on the shared connection would run in it. Autocommit off
 * (every statement opens a transaction the next guard rolls back), a LOCK
 * TABLES (every other table refused), an XA transaction (ROLLBACK refused
 * with XAER_RMFAIL; IN_TRANS stays set). */
static int my_session_leftover(const HlDbMyCtx *s)
{
    return !(s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT) ||
           s->lock_tables || s->lock_instance || s->xa_open ||
           s->backup_lock || s->backup_stage;
}

/* hl_db_session_held (audit 40): the same state counts as an open
 * transaction for the wait gates, so an entry holding it cannot park and
 * have the guard that runs before it resumes reset the session under it.
 * Hence, too, every leftover the guard finds belongs to a finished entry.
 * On a lost connection too (audit 41): the state it last held is what the
 * app still believes it has, and my_ready refuses calls until it is ended,
 * so a wait must stay refused as well. */
static int mysql_session_held(HlDbHandle *h)
{
    HlDbMyCtx *s = h ? h->ctx : NULL;
    if (!s) return 0;
    return my_session_leftover(s);
}

/* hl_db_session_dirty: a pooled worker connection holding any of it - or a
 * GET_LOCK, which a later op on another thread could never release - is
 * dropped after the op rather than reused. */
static int mysql_session_dirty(HlDbHandle *h)
{
    HlDbMyCtx *s = h ? h->ctx : NULL;
    if (!s) return 0;
    return my_session_leftover(s) || s->named_locks > 0;
}

/* Reset the session (COM_RESET_CONNECTION): rolls back, ends XA, drops LOCK
 * TABLES / GET_LOCK / temporary tables / user variables and restores every
 * session variable, autocommit included. When the server refuses it (MySQL
 * before 5.7.3, MariaDB before 10.2.4) or it fails, the connection is
 * abandoned instead - the server ends the session's state with it - and
 * the next call reconnects (my_ready). Not on a pinned handle, whose session
 * lock (the migration run) would go with it. */
static void my_reset_session(HlDbHandle *h, const char *why)
{
    HlDbMyCtx *s = h->ctx;
    if (h->session_pinned) return;
    if (!s->conn.broken) {
        log_warn("[hull:c] mysql: resetting the session, which a finished "
                 "entry left with %s; session settings, user variables, "
                 "temporary tables and named locks on it are dropped", why);
        if (hl_my_conn_reset(&s->conn) == 0) {
            my_session_clear(s);
            s->txn_aborted = 0;
            return;
        }
        log_warn("[hull:c] mysql: session reset failed (%s); closing the "
                 "connection instead", s->conn.errmsg);
        s->conn.broken = 1;        /* first: the close sends no close_notify */
        hl_my_conn_close(&s->conn);
    }
    /* Broken: nothing of the session survives; the next call reconnects. */
    my_lost_forget(s);
}

/* Case-insensitive substring search (strcasestr is not standard C). */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return hay;
    return NULL;
}

/*
 * MySQL 8 has no `CREATE INDEX ... IF NOT EXISTS` (MariaDB, SQLite, and
 * Postgres all do). The DB-backed stdlib writes idempotent index DDL in that
 * portable form, so on a MySQL connection rewrite it to a plain CREATE INDEX
 * and treat a later duplicate-index error as success. Sets *handled when the
 * statement was a CREATE INDEX ... IF NOT EXISTS. Returns 0 / -1.
 *
 * Every no-parameter exec reaches here, app SQL included, so the match is
 * exact (audit 14): ONE statement (my_sql_single_statement), whose leading
 * tokens are CREATE [UNIQUE|FULLTEXT|SPATIAL] INDEX IF NOT EXISTS, read the
 * way MySQL reads them. A loose substring scan rewrote a procedure body or a
 * string literal that held the words, and swallowed a duplicate-index error
 * from one statement of a multi-statement text, reporting the rest as run.
 */
static int mysql_create_index_shim(HlDbMyCtx *s, const char *sql, int *handled)
{
    *handled = 0;
    if (!my_sql_single_statement(sql)) return 0;
    char w[16];
    const char *p = hl_sql_next_word_ex(sql, w, sizeof w, 0);
    if (strcmp(w, "create") != 0) return 0;
    p = hl_sql_next_word_ex(p, w, sizeof w, 0);
    if (strcmp(w, "unique") == 0 || strcmp(w, "fulltext") == 0 ||
        strcmp(w, "spatial") == 0)
        p = hl_sql_next_word_ex(p, w, sizeof w, 0);
    if (strcmp(w, "index") != 0) return 0;
    const char *ine = hl_sql_skip_space_ex(p, 0);   /* start of IF */
    static const char *const kw[] = { "if", "not", "exists" };
    for (size_t i = 0; i < sizeof kw / sizeof kw[0]; i++) {
        p = hl_sql_next_word_ex(p, w, sizeof w, 0);
        if (strcmp(w, kw[i]) != 0) return 0;
    }
    const char *rest = p;
    while (isspace((unsigned char)*rest)) rest++;

    *handled = 1;
    size_t pre = (size_t)(ine - sql);
    char *rw = malloc(pre + strlen(rest) + 1);
    if (!rw) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
    memcpy(rw, sql, pre);
    memcpy(rw + pre, rest, strlen(rest) + 1);

    int rc = hl_my_conn_query(&s->conn, rw, NULL, NULL, NULL, NULL);
    free(rw);
    if (rc == 0) return 0;
    /* By the server's error code, not its message (audit 9 L2): the message
     * follows lc_messages, and an app-chosen index name can put the words in
     * another error's text. */
    if (s->conn.last_err_code == HL_MY_ER_DUP_KEYNAME) {
        /* Already present. The DDL still committed the open transaction
         * before it failed, which the ERR does not say (audit 6 L1). */
        if (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS)
            (void)hl_my_conn_ping(&s->conn);
        return 0;
    }
    return -1;
}

/*
 * Hull's own tables (`_hull_*`) are created with a binary collation. The
 * stdlib keys them on VARCHAR columns (role names, inbox ids, dedup keys,
 * job / cron / subscription names) whose comparisons must be exact; under
 * MySQL 8's default utf8mb4_0900_ai_ci, 'Admin' = 'admin' and 'josé' =
 * 'jose', so rbac.has_role matched a role differing in case and two inbox
 * ids differing in case were one duplicate. Every `CREATE TABLE [IF NOT
 * EXISTS] _hull_...(...)` in @p sql gets `DEFAULT CHARSET=utf8mb4
 * COLLATE=<collation>` after its column list, unless it names a collation
 * already: @p collation is the server's NO PAD binary one
 * (hl_my_bin_collation) - utf8mb4_bin is PAD SPACE, so 'alice' and 'alice '
 * were one key (audit 22). Tables created before keep theirs. Returns a
 * malloc'd rewrite, or NULL when nothing changed (or out of memory: the SQL
 * then runs as given).
 */
static char *mysql_bin_collate_hull_tables(const char *sql,
                                           const char *collation)
{
    char opt[96];
    snprintf(opt, sizeof opt, " DEFAULT CHARSET=utf8mb4 COLLATE=%s",
             collation ? collation : "utf8mb4_bin");
    static int warned;
    if ((!collation || strcmp(collation, "utf8mb4_bin") == 0) &&
        ci_strstr(sql, "_hull_") &&
        !__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
        log_warn("[db] this MySQL / MariaDB server has no NO PAD binary "
                 "collation (MySQL 8.0.17+, MariaDB 10.2.2+): Hull's _hull_* "
                 "tables use utf8mb4_bin, where 'alice' = 'alice ' - keys "
                 "that differ only in trailing spaces collide");
    size_t optlen = strlen(opt);
    size_t len = strlen(sql);
    char *out = NULL;
    size_t olen = 0, ocap = 0, copied = 0;
    const char *p = sql;
    while ((p = ci_strstr(p, "create table")) != NULL) {
        const char *q = p + strlen("create table");
        p = q;
        while (isspace((unsigned char)*q)) q++;
        if (strncasecmp(q, "if not exists", 13) == 0) {
            q += 13;
            while (isspace((unsigned char)*q)) q++;
        }
        if (*q == '`' || *q == '"') q++;
        if (strncasecmp(q, "_hull_", 6) != 0) continue;
        const char *lp = strchr(q, '(');
        if (!lp) break;
        /* The column list's closing parenthesis, past quoted text. */
        int depth = 0;
        char quote = 0;
        const char *r = lp;
        for (; *r; r++) {
            if (quote) { if (*r == quote) quote = 0; continue; }
            if (*r == '\'' || *r == '"' || *r == '`') { quote = *r; continue; }
            if (*r == '(') depth++;
            else if (*r == ')' && --depth == 0) break;
        }
        if (*r != ')') break;
        const char *stmt_end = strchr(r, ';');
        size_t tail = stmt_end ? (size_t)(stmt_end - r) : strlen(r);
        int named = 0;
        for (size_t i = 0; i + 7 <= tail; i++)
            if (strncasecmp(r + i, "collate", 7) == 0) { named = 1; break; }
        p = r + 1;
        if (named) continue;
        size_t chunk = (size_t)(r + 1 - (sql + copied));
        size_t need = olen + chunk + optlen + 1;
        if (need > ocap) {
            size_t ncap = ocap ? ocap * 2 : len + 256;
            while (ncap < need) ncap *= 2;
            char *nb = realloc(out, ncap);
            if (!nb) { free(out); return NULL; }
            out = nb; ocap = ncap;
        }
        memcpy(out + olen, sql + copied, chunk);
        olen += chunk;
        memcpy(out + olen, opt, optlen);
        olen += optlen;
        copied = (size_t)(r + 1 - sql);
    }
    if (!out) return NULL;
    size_t rest = len - copied;
    if (olen + rest + 1 > ocap) {
        char *nb = realloc(out, olen + rest + 1);
        if (!nb) { free(out); return NULL; }
        out = nb;
    }
    memcpy(out + olen, sql + copied, rest + 1);
    return out;
}

/* ── Text value -> HlValue by column type ─────────────────────────── */

static void decode_my_value(uint8_t type, const char *text, size_t len,
                            HlValue *out)
{
    if (!text) { out->type = HL_TYPE_NIL; return; }
    switch (type) {
    case HL_MY_TYPE_TINY:  case HL_MY_TYPE_SHORT: case HL_MY_TYPE_LONG:
    case HL_MY_TYPE_LONGLONG: case HL_MY_TYPE_INT24: case HL_MY_TYPE_YEAR: {
        char buf[32];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, text, n); buf[n] = '\0';
        errno = 0;
        long long v = strtoll(buf, NULL, 10);
        if (errno == ERANGE) {   /* BIGINT UNSIGNED above INT64_MAX: its text */
            out->type = HL_TYPE_TEXT;
            out->s = text;
            out->len = len;
            return;
        }
        out->type = HL_TYPE_INT;
        out->i = (int64_t)v;
        return;
    }
    case HL_MY_TYPE_FLOAT: case HL_MY_TYPE_DOUBLE: {
        char buf[64];
        size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, text, n); buf[n] = '\0';
        out->type = HL_TYPE_DOUBLE;
        out->d = strtod(buf, NULL);
        return;
    }
    default:   /* strings / blobs / decimals / dates: keep as borrowed text */
        out->type = HL_TYPE_TEXT;
        out->s = text;   /* points into the frame; valid for the row cb */
        out->len = len;
        return;
    }
}

/* ── Adapter: hl_my_conn_query callbacks -> HlRowCallback ──────────── */

typedef struct {
    HlRowCallback user_cb;
    void         *user_ctx;
    char        **names;   /* copied field names */
    uint8_t      *types;
    int           nfields;
    int           nnames;   /* entries allocated in names (what free walks) */
    int           bad_row;  /* a row did not match its column definitions */
    int           oom;      /* an allocation failed: the query fails (audit 23) */
} MyAdapter;

static void free_names(char **names, int n)
{
    if (names)
        for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* A result set's column definitions. A multi-statement query sends one per
 * result set: the next replaced the arrays without freeing them, and the
 * cleanup freed names[0..nfields) of the LAST count over whichever array
 * was left - out of bounds when it grew. On an allocation failure nfields
 * is -1 and every row is refused. */
static void adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyAdapter *a = ctx;
    free_names(a->names, a->nnames);
    free(a->types);
    a->names = NULL; a->types = NULL; a->nnames = 0;
    a->nfields = 0;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    a->types = calloc((size_t)nf, sizeof *a->types);
    if (!a->names || !a->types) {
        free(a->names); free(a->types);
        a->names = NULL; a->types = NULL;
        a->nfields = -1;
        a->oom = 1;
        return;
    }
    a->nnames = nf;
    a->nfields = nf;
    for (int i = 0; i < nf; i++) {
        a->names[i] = strdup(fields[i].name ? fields[i].name : "");
        a->types[i] = fields[i].type;
        /* Not copied: the column was handed over named "" (audit 23). */
        if (!a->names[i]) a->oom = 1;
    }
}

static int adapter_row(void *ctx, const char *const *vals,
                       const size_t *lens, int nc)
{
    MyAdapter *a = ctx;
    if (!a->user_cb) return 0;
    /* Every row carries its result set's column count. Clamped only from
     * above, a short row reached a consumer that sized its rows from the
     * first (db.async's collector), which read past it. */
    if (a->oom) return 1;
    if (nc != a->nfields) { a->bad_row = 1; return 1; }

    /* A failed allocation stopped the rows and the query succeeded with the
     * rest of the result missing (audit 23): it fails now. */
    HlColumn *cols = calloc((size_t)(nc > 0 ? nc : 1), sizeof *cols);
    if (!cols) { a->oom = 1; return 1; }
    for (int i = 0; i < nc; i++) {
        cols[i].name = (a->names && a->names[i]) ? a->names[i] : "";
        uint8_t type = a->types ? a->types[i] : HL_MY_TYPE_STRING;
        decode_my_value(type, vals[i], vals[i] ? lens[i] : 0, &cols[i].value);
    }
    int rc = a->user_cb(a->user_ctx, cols, nc);
    free(cols);
    return rc ? 1 : 0;
}

/* ── Adapter: prepared-statement binary rows -> HlRowCallback ──────── */

typedef struct {
    HlRowCallback user_cb;
    void         *user_ctx;
    char        **names;
    int           nfields;
    int           nnames;
    int           bad_row;
    int           oom;      /* see MyAdapter */
} MyBinAdapter;

static void bin_adapter_desc(void *ctx, const HlMyField *fields, int nf)
{
    MyBinAdapter *a = ctx;
    free_names(a->names, a->nnames);          /* see adapter_desc */
    a->names = NULL; a->nnames = 0;
    a->nfields = 0;
    if (nf <= 0) return;
    a->names = calloc((size_t)nf, sizeof *a->names);
    if (!a->names) { a->nfields = -1; a->oom = 1; return; }
    a->nnames = nf;
    a->nfields = nf;
    for (int i = 0; i < nf; i++) {
        a->names[i] = strdup(fields[i].name ? fields[i].name : "");
        if (!a->names[i]) a->oom = 1;   /* see adapter_desc */
    }
}

static int bin_adapter_row(void *ctx, const HlMyVal *vals, int nc)
{
    MyBinAdapter *a = ctx;
    if (!a->user_cb) return 0;
    if (a->oom) return 1;
    if (nc != a->nfields) { a->bad_row = 1; return 1; }   /* see adapter_row */

    HlColumn *cols = calloc((size_t)(nc > 0 ? nc : 1), sizeof *cols);
    if (!cols) { a->oom = 1; return 1; }                  /* see adapter_row */
    for (int i = 0; i < nc; i++) {
        cols[i].name = (a->names && a->names[i]) ? a->names[i] : "";
        switch (vals[i].kind) {
        case HL_MY_VAL_INT:
            cols[i].value.type = HL_TYPE_INT;    cols[i].value.i = vals[i].v.i; break;
        case HL_MY_VAL_DOUBLE:
            cols[i].value.type = HL_TYPE_DOUBLE; cols[i].value.d = vals[i].v.d; break;
        case HL_MY_VAL_STR:
            cols[i].value.type = HL_TYPE_TEXT;
            cols[i].value.s = vals[i].v.s.ptr;   cols[i].value.len = vals[i].v.s.len; break;
        case HL_MY_VAL_NULL:
        default:
            cols[i].value.type = HL_TYPE_NIL; break;
        }
    }
    int rc = a->user_cb(a->user_ctx, cols, nc);
    free(cols);
    return rc ? 1 : 0;
}

/* Convert Hull's HlValue params to wire-level HlMyParam. Strings/blobs borrow
 * the HlValue bytes, so the returned array is valid only while @p params is. */
static HlMyParam *encode_my_params(const HlValue *params, int n)
{
    if (n <= 0) return NULL;
    HlMyParam *p = calloc((size_t)n, sizeof *p);
    if (!p) return NULL;
    for (int i = 0; i < n; i++) {
        switch (params[i].type) {
        case HL_TYPE_INT:
            p[i].type = HL_MY_TYPE_LONGLONG;  p[i].v.i = params[i].i; break;
        case HL_TYPE_BOOL:
            p[i].type = HL_MY_TYPE_TINY;      p[i].v.i = params[i].b ? 1 : 0; break;
        case HL_TYPE_DOUBLE:
            p[i].type = HL_MY_TYPE_DOUBLE;    p[i].v.d = params[i].d; break;
        case HL_TYPE_TEXT:
            p[i].type = HL_MY_TYPE_VAR_STRING;
            p[i].v.s.ptr = params[i].s;       p[i].v.s.len = params[i].len; break;
        case HL_TYPE_BLOB:
            p[i].type = HL_MY_TYPE_BLOB;
            p[i].v.s.ptr = params[i].s;       p[i].v.s.len = params[i].len; break;
        case HL_TYPE_NIL:
        default:
            p[i].is_null = 1; p[i].type = HL_MY_TYPE_NULL; break;
        }
    }
    return p;
}

/* ── Vtable methods ───────────────────────────────────────────────── */

static int mysql_open(void **out_ctx, const char *dsn, HlAllocator *alloc)
{
    (void)alloc;
    HlDbMyCtx *s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->dsn = strdup(dsn);
    if (!s->dsn || my_connect(&s->conn, dsn, 1) != 0) {
        scrub_free(s->dsn);
        free(s);
        return -1;
    }
    *out_ctx = s;
    return 0;
}

static void mysql_close(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbMyCtx *s = h->ctx;
    hl_my_conn_close(&s->conn);
    scrub_free(s->dsn);
    free(s);
    h->ctx = NULL;
}

static int mysql_query_raw(HlDbHandle *h, const char *sql,
                           const HlValue *params, int nparams,
                           HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    (void)params; (void)alloc;
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;

    /* Parameterized: bind through the binary prepared-statement protocol so the
     * values never touch the SQL text (injection-safe). */
    if (nparams > 0) {
        HlMyParam *pp = encode_my_params(params, nparams);
        if (!pp) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
        MyBinAdapter ba;
        memset(&ba, 0, sizeof ba);
        ba.user_cb = cb;
        ba.user_ctx = cb_ctx;
        int rc = hl_my_conn_query_prepared(&s->conn, sql, pp, nparams,
                                           cb ? bin_adapter_desc : NULL,
                                           cb ? bin_adapter_row : NULL, &ba, NULL);
        free_names(ba.names, ba.nnames);
        free(pp);
        if (rc == 0 && ba.oom) {
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory");
            rc = -1;
        } else if (rc == 0 && ba.bad_row) {
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                     "server sent a row whose column count does not match "
                     "its column definitions");
            rc = -1;
        }
        return rc;
    }

    /* No params: the simpler COM_QUERY text protocol. */
    MyAdapter a;
    memset(&a, 0, sizeof a);
    a.user_cb = cb;
    a.user_ctx = cb_ctx;
    s->conn.commit_next = sql_is_commit(sql);
    int rc = hl_my_conn_query(&s->conn, sql,
                              cb ? adapter_desc : NULL,
                              cb ? adapter_row : NULL, &a, NULL);
    free_names(a.names, a.nnames);
    free(a.types);
    if (rc == 0 && a.oom) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory");
        rc = -1;
    } else if (rc == 0 && a.bad_row) {
        snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                 "server sent a row whose column count does not match its "
                 "column definitions");
        rc = -1;
    }
    return rc;
}

static int mysql_exec_raw(HlDbHandle *h, const char *sql,
                          const HlValue *params, int nparams)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    int64_t affected = 0;

    char *collated = NULL;
    if (nparams == 0) {
        int handled = 0;
        int shim = mysql_create_index_shim(s, sql, &handled);
        if (handled) return shim == 0 ? 0 : -1;   /* DDL: 0 rows affected */
        collated = mysql_bin_collate_hull_tables(sql, s->conn.bin_collation);
    }

    if (nparams > 0) {
        HlMyParam *pp = encode_my_params(params, nparams);
        if (!pp) { snprintf(s->conn.errmsg, sizeof s->conn.errmsg, "out of memory"); return -1; }
        int rc = hl_my_conn_query_prepared(&s->conn, sql, pp, nparams,
                                           NULL, NULL, NULL, &affected);
        free(pp);
        if (rc != 0) return -1;
        return (int)(affected < 0 ? 0 : affected);
    }

    s->conn.commit_next = sql_is_commit(sql);
    int qrc = hl_my_conn_query(&s->conn, collated ? collated : sql,
                               NULL, NULL, NULL, &affected);
    free(collated);
    if (qrc != 0) {
        if (s->conn.broken && sql_is_rollback(sql)) {   /* see mysql_txn */
            my_lost_rolled_back(s);
            return 0;
        }
        return -1;
    }
    return (int)(affected < 0 ? 0 : affected);
}

/* A statement that failed inside a transaction with ER_LOCK_DEADLOCK took
 * the whole transaction with it (InnoDB rolls back the victim entirely, not
 * only the statement). The server status still read "in a transaction" from
 * the last OK, and nothing told the app: its next statements autocommitted
 * and COMMIT succeeded with the earlier writes gone. Remember it instead. */
/* 1 when @p sql starts with a statement MySQL commits the open transaction
 * before running - so before it fails, too (MySQL 8 "Statements That Cause
 * an Implicit Commit"): DDL, account management, LOCK / UNLOCK TABLES, the
 * table-maintenance and administration statements, replication control.
 * A failure of anything else that ends the transaction was a rollback, so
 * an unrecognised statement is refused (txn_aborted), never resumed: the
 * list errs on the short side. Not CREATE / DROP TEMPORARY TABLE nor LOAD
 * DATA / LOAD XML (audit 8 c_db L2): they commit nothing, and a lock-wait
 * timeout in one under innodb_rollback_on_timeout was resumed as DDL. A
 * multi-statement text is never one (audit 10): refused, not resumed. */
static int my_sql_commits_implicitly(const char *sql)
{
    static const char *const any[] = {   /* every form of the statement */
        "alter", "cache", "check", "flush", "grant", "optimize",
        "rename", "repair", "revoke", "truncate",
    };
    /* Unreadable, or more than one statement (audit 10): refused, never
     * resumed. */
    if (!my_sql_single_statement(sql)) return 0;
    char w[16], w2[24], w3[16];
    const char *p = hl_sql_next_word_ex(sql, w, sizeof w, 0);
    p = hl_sql_next_word_ex(p, w2, sizeof w2, 0);
    for (size_t i = 0; i < sizeof any / sizeof any[0]; i++)
        if (strcmp(w, any[i]) == 0) return 1;
    /* ANALYZE [NO_WRITE_TO_BINLOG | LOCAL] TABLE commits; MariaDB's
     * ANALYZE SELECT / UPDATE / DELETE / FORMAT=JSON (an EXPLAIN that runs the
     * statement) commits nothing (audit 29). */
    if (strcmp(w, "analyze") == 0) {
        if (strcmp(w2, "no_write_to_binlog") == 0 || strcmp(w2, "local") == 0)
            (void)hl_sql_next_word_ex(p, w2, sizeof w2, 0);
        return strcmp(w2, "table") == 0 || strcmp(w2, "tables") == 0;
    }
    /* MariaDB's CREATE OR REPLACE TEMPORARY TABLE is still temporary: look
     * past OR REPLACE (audit 29). */
    if (strcmp(w, "create") == 0 && strcmp(w2, "or") == 0) {
        p = hl_sql_next_word_ex(p, w3, sizeof w3, 0);
        if (strcmp(w3, "replace") != 0) return 1;
        (void)hl_sql_next_word_ex(p, w2, sizeof w2, 0);
    }
    if (strcmp(w, "create") == 0 || strcmp(w, "drop") == 0)
        return strcmp(w2, "temporary") != 0;
    if (strcmp(w, "lock") == 0 || strcmp(w, "unlock") == 0)
        return strcmp(w2, "tables") == 0 || strcmp(w2, "table") == 0;
    if (strcmp(w, "load") == 0)           /* LOAD INDEX INTO CACHE */
        return strcmp(w2, "index") == 0;
    if (strcmp(w, "install") == 0 || strcmp(w, "uninstall") == 0)
        return strcmp(w2, "plugin") == 0;
    if (strcmp(w, "reset") == 0)          /* all but RESET PERSIST */
        return w2[0] != '\0' && strcmp(w2, "persist") != 0;
    if (strcmp(w, "start") == 0 || strcmp(w, "stop") == 0)
        return strcmp(w2, "replica") == 0 || strcmp(w2, "slave") == 0;
    if (strcmp(w, "change") == 0) {       /* CHANGE MASTER / REPLICATION SOURCE TO */
        if (strcmp(w2, "master") == 0) return 1;
        (void)hl_sql_next_word_ex(p, w3, sizeof w3, 0);
        return strcmp(w2, "replication") == 0 && strcmp(w3, "source") == 0;
    }
    if (strcmp(w, "set") == 0)            /* SET PASSWORD (mysql.user) */
        return strcmp(w2, "password") == 0;
    return 0;
}

static void my_note_failure(HlDbMyCtx *s, const char *sql, int was_in_trans,
                            int rc)
{
    /* A statement the server failed outside a transaction may still have
     * changed the session before it failed (audit 41): a CALL whose
     * procedure ran START TRANSACTION or SET autocommit = 0 and then raised,
     * or an earlier statement of a multi-statement text. An ERR carries no
     * status flags, so the status read before the call stayed - in_txn and
     * session_held read 0, the wait gates let the entry park, and the next
     * entry's acknowledged write was rolled back by the guard. Ask. */
    if (rc < 0 && !was_in_trans && !s->conn.broken &&
        s->conn.last_err_code != 0) {
        (void)hl_my_conn_ping(&s->conn);
        return;
    }
    if (rc < 0 && was_in_trans && !s->conn.broken &&
        s->conn.last_err_code == HL_MY_ER_LOCK_DEADLOCK) {
        s->txn_aborted = 1;
        s->conn.server_status &= (uint16_t)~HL_MY_SERVER_STATUS_IN_TRANS;
        return;
    }
    /* Any other failure inside a transaction: an ERR carries no status, and a
     * failed DDL statement had already committed the transaction - ask
     * (audit 6 L1), so the implicit-commit resume and the batch see it. */
    if (rc < 0 && was_in_trans && !s->conn.broken && !s->txn_aborted) {
        if (hl_my_conn_ping(&s->conn) != 0 ||
            (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS))
            return;
        /* Gone, and not committed by a DDL statement: the server rolled the
         * whole transaction back under the failed statement (a lock-wait
         * timeout under innodb_rollback_on_timeout, ...). The resume would
         * have opened a new one and let the batch commit what followed
         * without what came before (audit 7 L1): refuse until a ROLLBACK, as
         * for a deadlock. A COMMIT / ROLLBACK / savepoint statement ends or
         * keeps the transaction on its own terms. */
        if (my_sql_unreadable(sql) ||   /* unrecognised: refused (audit 9 L1) */
            (my_sql_txn_kind(sql) == HL_SQL_TXN_NONE &&
             !my_sql_commits_implicitly(sql)))
            s->txn_aborted = 1;
    }
}

static int my_in_trans(const HlDbHandle *h)
{
    const HlDbMyCtx *s = h ? h->ctx : NULL;
    return s && (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS);
}

static int mysql_txn_raw(HlDbHandle *h, const char *sql);

/* MySQL commits implicitly before and after DDL (CREATE / ALTER / DROP ...),
 * and before a DDL statement that then fails. Inside a db.batch that ended the
 * batch's transaction, and every later statement autocommitted - then the
 * batch reported its transaction lost (hl_db_batch_lost_), failing the
 * stdlib's own idempotent schema batches. When a statement that is not itself
 * COMMIT / ROLLBACK ends the transaction a batch holds, open a new one: what
 * ran before the DDL is committed (MySQL semantics, which no client can
 * change), and the rest of the batch is transactional again. A raw COMMIT /
 * ROLLBACK is still reported as lost.
 *
 * Not inside a NESTED batch (audit 6 L3): the commit also dropped the outer
 * batches' savepoints, so the inner batch could no longer roll back on its
 * own - its writes after the DDL committed with the outer batch even when it
 * failed. The transaction stays ended instead, and every enclosing batch's
 * leave reports it lost.
 *
 * Only after ONE statement that commits implicitly (audit 10): the resume
 * used to follow anything that was not itself a COMMIT / ROLLBACK, read by
 * its first word - so "SELECT 1; COMMIT" (the connection runs multi-statement
 * texts) ended the batch's transaction and had a new one opened under it,
 * and the batch committed the rest as if nothing had happened. */
static void my_resume_after_implicit_commit(HlDbHandle *h, const char *sql,
                                            int was)
{
    if (!was || !h || h->batch_depth != 1 || my_in_trans(h)) return;
    HlDbMyCtx *s = h->ctx;
    if (!s || s->txn_aborted || s->conn.broken) return;
    if (!my_sql_commits_implicitly(sql)) return;
    /* A new transaction under the batch: the transaction generation must
     * move, which the caller's note cannot see (in a transaction before and
     * after the statement, with DDL text; audit 39). */
    HlDbTxnSnap before = hl_db_txn_snap(h);
    (void)mysql_txn_raw(h, "START TRANSACTION");
    hl_db_txn_note(h, before, NULL);
}

static int mysql_query(HlDbHandle *h, const char *sql,
                       const HlValue *params, int nparams,
                       HlRowCallback cb, void *cb_ctx, HlAllocator *alloc)
{
    int was = my_in_trans(h);
    int rc = mysql_query_raw(h, sql, params, nparams, cb, cb_ctx, alloc);
    my_track_session(h, sql, rc);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    my_resume_after_implicit_commit(h, sql, was);
    return rc;
}

static int mysql_exec(HlDbHandle *h, const char *sql,
                      const HlValue *params, int nparams)
{
    int was = my_in_trans(h);
    int rc = mysql_exec_raw(h, sql, params, nparams);
    my_track_session(h, sql, rc);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    my_resume_after_implicit_commit(h, sql, was);
    return rc;
}

static int mysql_exec_script(HlDbHandle *h, const char *sql)
{
    /* Migrations are multi-statement; the connection advertises
     * CLIENT_MULTI_STATEMENTS so the whole script runs as one COM_QUERY. */
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    char *collated = mysql_bin_collate_hull_tables(sql, s->conn.bin_collation);
    int rc = hl_my_conn_exec_multi(&s->conn, collated ? collated : sql);
    free(collated);
    my_track_session(h, sql, rc);
    return rc;
}

static int mysql_txn_raw(HlDbHandle *h, const char *sql)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    int ready = my_ready(s, sql, h->session_pinned);
    if (ready != 0) return ready > 0 ? 0 : -1;
    s->conn.commit_next = sql_is_commit(sql);
    int rc = hl_my_conn_query(&s->conn, sql, NULL, NULL, NULL, NULL);
    /* A ROLLBACK that failed because the connection went: the server rolled
     * the transaction back with it. */
    if (rc != 0 && s->conn.broken && sql_is_rollback(sql)) {
        my_lost_rolled_back(s);
        rc = 0;
    }
    return rc;
}

static int mysql_txn(HlDbHandle *h, const char *sql)
{
    int was = my_in_trans(h);
    int rc = mysql_txn_raw(h, sql);
    my_track_session(h, sql, rc);
    if (h && h->ctx) my_note_failure(h->ctx, sql, was, rc);
    return rc;
}

static int mysql_begin(HlDbHandle *h)    { return mysql_txn(h, "START TRANSACTION"); }
static int mysql_commit(HlDbHandle *h)   { return mysql_txn(h, "COMMIT"); }
static int mysql_rollback(HlDbHandle *h) { return mysql_txn(h, "ROLLBACK"); }

/* See pg_guard_stale_txn: a transaction a request left open is rolled back
 * before the next request runs. */
static int mysql_in_txn(HlDbHandle *h)
{
    if (!h || !h->ctx) return 0;
    HlDbMyCtx *s = h->ctx;
    return (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS) ||
           s->txn_aborted;
}

/* A ROLLBACK alone did not clear what a failed entry can leave on the
 * session (audit 39): after XA START it fails (XAER_RMFAIL) with IN_TRANS
 * still set, and after SET autocommit = 0 every later statement opens a
 * transaction the next guard rolls back - one failed request and every
 * later write was acknowledged and never committed, every wait refused,
 * until a restart. So the guard also resets the session when the rollback
 * failed or left the transaction open, when autocommit is off, when a LOCK
 * TABLES or an XA transaction is outstanding, and - after a stale
 * transaction, the sign of a failed entry - when a GET_LOCK may be held. A
 * GET_LOCK an entry left with no transaction open is not seen. */
static void mysql_guard_stale_txn(HlDbHandle *h)
{
    if (!h || !h->ctx) return;
    HlDbMyCtx *s = h->ctx;
    const char *why = NULL;
    if (mysql_in_txn(h)) {
        fprintf(stderr, "[hull:c] rolling back stale transaction from previous request\n");
        int rc = mysql_rollback(h);
        if (s->conn.broken) {
            /* Lost: the server ended the session; my_ready reconnects. */
            if (!h->session_pinned) my_lost_forget(s);
            return;
        }
        if (rc != 0 || (s->conn.server_status & HL_MY_SERVER_STATUS_IN_TRANS))
            why = "a transaction its ROLLBACK cannot end (XA)";
        else if (s->named_locks > 0)
            why = "a GET_LOCK";
    }
    if (s->conn.broken) {
        /* Lost with session state the entry never ended (audit 41): the
         * server ended it with the session, and my_ready refused every call
         * meanwhile. The entry is over, so the next call reconnects into a
         * fresh autocommit session. */
        if (!h->session_pinned) my_lost_forget(s);
        return;
    }
    if (!why && my_session_leftover(s))
        why = !(s->conn.server_status & HL_MY_SERVER_STATUS_AUTOCOMMIT)
              ? "autocommit off" : s->xa_open ? "an XA transaction"
              : s->lock_instance ? "LOCK INSTANCE FOR BACKUP"
              : (s->backup_lock || s->backup_stage) ? "a BACKUP LOCK / STAGE"
              : "LOCK TABLES (or a global read lock)";
    if (why && (s->backup_lock || s->backup_stage) && !h->session_pinned) {
        /* Whether COM_RESET_CONNECTION ends a MariaDB backup lock or stage
         * is not documented; ending the session does. */
        log_warn("[hull:c] mysql: closing the connection, whose session a "
                 "finished entry left with %s", why);
        s->conn.broken = 1;        /* first: the close sends no close_notify */
        hl_my_conn_close(&s->conn);
        my_lost_forget(s);
        return;
    }
    if (why) my_reset_session(h, why);
}

static int64_t mysql_last_id(HlDbHandle *h)
{
    if (!h || !h->ctx) return -1;
    HlDbMyCtx *s = h->ctx;
    return (int64_t)s->conn.last_insert_id;
}

static const char *mysql_errmsg(HlDbHandle *h)
{
    if (!h || !h->ctx) return "no database";
    HlDbMyCtx *s = h->ctx;
    return s->conn.errmsg[0] ? s->conn.errmsg : "no error";
}

/* ── Dialect helpers ──────────────────────────────────────────────── */

/* Append to a bounded buffer; latch overflow via *off >= cap (checked once). */
static void ap(char *buf, size_t cap, size_t *off, const char *str)
{
    size_t n = strlen(str);
    if (*off + n < cap) memcpy(buf + *off, str, n);
    *off += n;
}

/*
 * Build and run `INSERT INTO t (cols) VALUES (?,...) [ON DUPLICATE KEY
 * UPDATE c = VALUES(c), ...]`, binding @p values through the prepared-statement
 * path. MySQL keys the conflict off any UNIQUE / PRIMARY index, so
 * @p conflict_cols is unused (kept for the vtable signature parity).
 *
 * insert_if_absent is a plain INSERT whose ER_DUP_ENTRY is taken as "already
 * there" (0 rows), not INSERT IGNORE (audit 31): IGNORE downgrades EVERY
 * error to a warning, so under strict mode an invalid utf8mb4 value (1366)
 * or one too long for its column (1406) was stored TRUNCATED and the call
 * succeeded - `'evt_1\xff\xfe'` stored as 'evt_1', and the genuine 'evt_1'
 * then read as a duplicate (inbox dedup poisoning); a jobs payload over a
 * TEXT column's 64 KB kept its first 64 KB. A plain INSERT fails atomically
 * on those (strict mode rolls the statement back), so no truncated row is
 * ever written, and a failed duplicate rolls back only the statement - the
 * transaction a batch holds goes on, as it did under IGNORE. A foreign-key
 * or NOT NULL violation, which IGNORE also skipped, now fails the call. An
 * upsert (ON DUPLICATE KEY UPDATE, no IGNORE) already surfaced strict-mode
 * errors. Both rely on the server's strict sql_mode (the MySQL 5.7+ /
 * MariaDB 10.2.4+ default): without it MySQL truncates any INSERT.
 */
static int build_and_run_insert(HlDbHandle *h, const char *table,
                                const char *const *cols,
                                const HlValue *values, int n_cols, int do_update)
{
    /* Each column can appear up to three times (insert list + both sides of
     * `c = VALUES(c)`); budget 3x + overhead so a long name never truncates. */
    size_t cap = 64 + strlen(table);
    for (int i = 0; i < n_cols; i++) cap += strlen(cols[i]) * 3 + 24;
    char *sql = malloc(cap);
    if (!sql) return -1;
    size_t off = 0;

    ap(sql, cap, &off, "INSERT INTO ");
    ap(sql, cap, &off, table);
    ap(sql, cap, &off, " (");
    for (int i = 0; i < n_cols; i++) {
        if (i) ap(sql, cap, &off, ", ");
        ap(sql, cap, &off, cols[i]);
    }
    ap(sql, cap, &off, ") VALUES (");
    for (int i = 0; i < n_cols; i++)
        ap(sql, cap, &off, i ? ", ?" : "?");
    ap(sql, cap, &off, ")");
    if (do_update) {
        ap(sql, cap, &off, " ON DUPLICATE KEY UPDATE ");
        for (int i = 0; i < n_cols; i++) {
            if (i) ap(sql, cap, &off, ", ");
            ap(sql, cap, &off, cols[i]);
            ap(sql, cap, &off, " = VALUES(");
            ap(sql, cap, &off, cols[i]);
            ap(sql, cap, &off, ")");
        }
    }

    if (off >= cap) {                            /* estimate was too small */
        free(sql);
        HlDbMyCtx *s = h ? h->ctx : NULL;
        if (s)
            snprintf(s->conn.errmsg, sizeof s->conn.errmsg,
                     "insert/upsert SQL exceeded its estimated buffer");
        return -1;
    }
    sql[off] = '\0';

    if (do_update) {
        int rc = mysql_exec(h, sql, values, n_cols);
        free(sql);
        return rc;
    }
    /* insert_if_absent: a duplicate is a clean outcome, not a failure, so it
     * never reaches my_note_failure (whose COM_PING after a failed statement
     * inside a transaction would be a round trip for nothing). */
    int was = my_in_trans(h);
    int rc = mysql_exec_raw(h, sql, values, n_cols);
    HlDbMyCtx *s = h ? h->ctx : NULL;
    if (rc < 0 && s && !s->conn.broken && !s->txn_aborted &&
        s->conn.last_err_code == HL_MY_ER_DUP_ENTRY) {
        s->conn.errmsg[0] = '\0';
        s->conn.last_err_code = 0;
        rc = 0;                                  /* already there: 0 rows */
    } else if (s) {
        my_note_failure(s, sql, was, rc);
    }
    free(sql);
    return rc;
}

static int mysql_insert_if_absent(HlDbHandle *h, const char *table,
                                  const char *const *conflict_cols,
                                  int n_conflict, const char *const *cols,
                                  const HlValue *values, int n_cols)
{
    (void)conflict_cols; (void)n_conflict;
    return build_and_run_insert(h, table, cols, values, n_cols, 0);
}

static int mysql_upsert(HlDbHandle *h, const char *table,
                        const char *const *conflict_cols, int n_conflict,
                        const char *const *cols,
                        const HlValue *values, int n_cols)
{
    (void)conflict_cols; (void)n_conflict;
    return build_and_run_insert(h, table, cols, values, n_cols, 1);
}

typedef struct {
    HlDbColumnCallback cb;
    void              *cb_ctx;
} MyColsFwd;

static int mysql_table_columns_row(void *ctx, HlColumn *cols, int ncols)
{
    MyColsFwd *fwd = ctx;
    for (int i = 0; i < ncols; i++) {
        if (cols[i].value.type != HL_TYPE_TEXT || !cols[i].value.s)
            continue;
        /* The text value borrows non-NUL-terminated wire bytes; copy + terminate
         * with the tracked length before handing a C string to the callback. */
        size_t len = cols[i].value.len;
        char stackbuf[128];
        char *name = (len < sizeof stackbuf) ? stackbuf : malloc(len + 1);
        if (!name) continue;
        memcpy(name, cols[i].value.s, len);
        name[len] = '\0';
        fwd->cb(fwd->cb_ctx, name);
        if (name != stackbuf) free(name);
    }
    return 0;
}

static int mysql_table_columns(HlDbHandle *h, const char *table,
                               HlDbColumnCallback cb, void *cb_ctx)
{
    MyColsFwd fwd = { cb, cb_ctx };
    HlValue p;
    memset(&p, 0, sizeof p);
    p.type = HL_TYPE_TEXT;
    p.s = table;
    p.len = strlen(table);
    /* Scope to the connection's own schema so a same-named table in another
     * database doesn't leak columns. */
    return mysql_query(h,
        "SELECT column_name FROM information_schema.columns "
        "WHERE table_name = ? AND table_schema = DATABASE() "
        "ORDER BY ordinal_position",
        &p, 1, mysql_table_columns_row, &fwd, NULL);
}

/* Both schemes route here; MariaDB shares the MySQL protocol. */
static const char *const mysql_schemes[] = { "mysql", "mariadb", NULL };

const HlDbBackend hl_db_backend_mysql = {
    .name                  = "mysql",
    .schemes               = mysql_schemes,
    .dialect = {
        .identifier_quote             = '`',
        .placeholder                  = "?",
        .upsert_style                 = "on_duplicate_key",
        .supports_returning           = 0,   /* MySQL 8; MariaDB is 1 (see design doc) */
        .supports_index_if_not_exists = 0,   /* rewritten by the backend shim */
        .supports_skip_locked         = 1,   /* MySQL 8+ / MariaDB 10.6+ */
        .identity_column              = "BIGINT AUTO_INCREMENT PRIMARY KEY",
        .identity_sequence            = NULL,
    },
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
    .guard_stale_txn       = mysql_guard_stale_txn,
    .in_txn                = mysql_in_txn,
    .session_dirty         = mysql_session_dirty,
    .session_held          = mysql_session_held,
    .last_id               = mysql_last_id,
    .errmsg                = mysql_errmsg,
    .insert_if_absent      = mysql_insert_if_absent,
    .upsert                = mysql_upsert,
    .table_columns         = mysql_table_columns,
    /* native_handle: no consumer (udf / agent introspection are SQLite-only). */
};

#endif /* HL_ENABLE_MYSQL */

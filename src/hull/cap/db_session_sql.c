/*
 * cap/db_session_sql.c: refuse session-altering app SQL on MySQL / MariaDB /
 * Postgres (audit convergence phase 3, §7.1). See db_session_sql.h.
 *
 * One pass over the text, a word at a time, with the two words before each
 * one. A word "anchors" a statement when it is the statement's first word (a
 * ';' starts a new statement; in a MySQL compound body, the word after BEGIN /
 * THEN / ELSE / DO / LOOP / REPEAT too). Read raw - quotes and comments as
 * code, every word an anchor - when the text cannot be read the way the
 * server reads it, and for a Postgres dollar-quoted body and the statement a
 * MySQL PREPARE / EXECUTE IMMEDIATE runs from a string.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_session_sql.h"

#include <stdio.h>
#include <string.h>

/* Longest word kept; a longer one matches nothing (every name below is
 * shorter). */
#define W_MAX 48

/* ── Classes (the refusal message names one) ────────────────────────── */

static const char C_AUTOCOMMIT[]  = "SET autocommit";
static const char C_MY_TXN[]      = "a session-scoped SET TRANSACTION";
static const char C_CHARSET[]     = "SET NAMES / CHARACTER SET";
static const char C_ROLE[]        = "SET ROLE / RESET ROLE";
static const char C_LOCK_TABLES[] = "LOCK TABLES / UNLOCK TABLES";
static const char C_LOCK_INST[]   = "LOCK INSTANCE FOR BACKUP / UNLOCK INSTANCE";
static const char C_XA[]          = "an XA statement";
static const char C_NAMED_LOCK[]  = "a named lock (GET_LOCK / RELEASE_LOCK)";
static const char C_FTWRL[]       = "FLUSH TABLES ... WITH READ LOCK";
static const char C_EXPORT[]      = "FLUSH TABLES ... FOR EXPORT";
static const char C_BACKUP[]      = "BACKUP LOCK / BACKUP STAGE";
static const char C_ADVISORY[]    = "a session-level advisory lock (pg_advisory_xact_lock is allowed)";
static const char C_SESS_AUTH[]   = "SET SESSION AUTHORIZATION / RESET SESSION AUTHORIZATION";
static const char C_SESS_CHAR[]   = "SET SESSION CHARACTERISTICS (or default_transaction_*)";
static const char C_LISTEN[]      = "LISTEN / UNLISTEN (use conn.wait_notify)";
static const char C_PREP_TXN[]    = "PREPARE TRANSACTION";
static const char C_SET_CONFIG[]  = "set_config() of a role or session characteristic";
static const char C_UESCAPE[]     = "a Unicode-escape identifier (U&\"...\")";

/* ── Scanner state ──────────────────────────────────────────────────── */

typedef struct {
    HlDbSqlDialect d;
    int            raw;          /* quotes / comments as code, every word anchors */
    const char    *refused;
    int            unterminated; /* a quote / comment / dollar body ran off the end */
    /* the current statement */
    const char    *stmt_start;
    int            k;            /* words seen in it */
    char           prev[W_MAX], prev2[W_MAX];
    int            set_mode;     /* 0, or 1 + words since an anchoring SET */
    int            set_local;    /* Postgres SET LOCAL: transaction-scoped */
    int            reset_mode;   /* Postgres RESET: 1 + words since it */
    int            rescan;       /* MySQL PREPARE / EXECUTE: read its string raw */
} Scan;

static void scan(Scan *s, const char *p, const char *end);

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static int in(const char *w, const char *const *list)
{
    for (; *list; list++)
        if (eq(w, *list)) return 1;
    return 0;
}

static void refuse(Scan *s, const char *cls)
{
    if (!s->refused) s->refused = cls;
}

static int is_word_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static int is_word_start(const Scan *s, unsigned char c)
{
    if (c == '$') return s->d == HL_DB_SQL_MYSQL;  /* Postgres: a dollar quote / $n */
    return is_word_char(c);
}

/* Scan [b, e) raw with a child scanner; its refusal is ours. */
static void scan_raw(Scan *s, const char *b, const char *e)
{
    if (s->refused || b >= e) return;
    Scan child;
    memset(&child, 0, sizeof child);
    child.d = s->d;
    child.raw = 1;
    scan(&child, b, e);
    if (child.refused) refuse(s, child.refused);
}

static void end_statement(Scan *s, const char *at)
{
    if (s->rescan && !s->raw) scan_raw(s, s->stmt_start, at);
    s->k = 0;
    s->prev[0] = s->prev2[0] = '\0';
    s->set_mode = s->set_local = s->reset_mode = s->rescan = 0;
}

/* ── Rules ──────────────────────────────────────────────────────────── */

static void my_word(Scan *s, const char *w, int uservar, int anchor)
{
    static const char *const named_locks[] = {
        "get_lock", "release_lock", "release_all_locks", NULL };
    static const char *const xa_verbs[] = {
        "start", "begin", "end", "prepare", "commit", "rollback", "recover", NULL };
    static const char *const backup_verbs[] = { "lock", "unlock", "stage", NULL };
    static const char *const txn_vars[] = {
        "transaction_isolation", "tx_isolation", "transaction_read_only",
        "tx_read_only", NULL };
    static const char *const charset_vars[] = {
        "names", "charset", "character_set_client", "character_set_connection",
        "character_set_results", "collation_connection", NULL };
    static const char *const scopes[] = {
        "session", "local", "global", "persist", "persist_only", NULL };
    /* A user variable (@name) is the app's own; raw, nothing is trusted. */
    int user = uservar && !s->raw;

    if (!user && in(w, named_locks)) refuse(s, C_NAMED_LOCK);
    if ((eq(s->prev, "lock") || eq(s->prev, "unlock")) &&
        (eq(w, "table") || eq(w, "tables")))
        refuse(s, C_LOCK_TABLES);
    if ((eq(s->prev, "lock") || eq(s->prev, "unlock")) && eq(w, "instance"))
        refuse(s, C_LOCK_INST);
    if ((anchor && eq(w, "xa")) || (eq(s->prev, "xa") && in(w, xa_verbs)))
        refuse(s, C_XA);
    if (eq(s->prev2, "with") && eq(s->prev, "read") && eq(w, "lock"))
        refuse(s, C_FTWRL);
    if (eq(s->prev, "for") && eq(w, "export")) refuse(s, C_EXPORT);
    if (eq(s->prev, "backup") && in(w, backup_verbs)) refuse(s, C_BACKUP);
    if (anchor && (eq(w, "prepare") || eq(w, "execute"))) s->rescan = 1;

    if (s->set_mode) {
        s->set_mode++;               /* 2 = the first word after SET */
        if (!user) {
            if (eq(w, "autocommit")) refuse(s, C_AUTOCOMMIT);
            if (in(w, txn_vars)) refuse(s, C_MY_TXN);
            if (in(w, charset_vars)) refuse(s, C_CHARSET);
            if (eq(s->prev, "character") && eq(w, "set")) refuse(s, C_CHARSET);
            if (s->set_mode == 2 && eq(w, "role")) refuse(s, C_ROLE);
            /* SET SESSION / GLOBAL TRANSACTION ...; a bare SET TRANSACTION
             * applies to the next transaction only and is allowed. */
            if (s->set_mode == 3 && in(s->prev, scopes) && eq(w, "transaction"))
                refuse(s, C_MY_TXN);
        }
    }
    if (anchor && eq(w, "set") && !eq(s->prev, "character")) s->set_mode = 1;
}

/* Past whitespace and comments (Postgres rules: nested block comments). */
static const char *pg_skip_ws(const char *p, const char *end)
{
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                           *p == '\f' || *p == '\v'))
            p++;
        if (p + 1 < end && p[0] == '-' && p[1] == '-') {
            while (p < end && *p != '\n' && *p != '\r') p++;
        } else if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            int depth = 1;
            p += 2;
            while (p < end && depth > 0) {
                if (p + 1 < end && p[0] == '/' && p[1] == '*') { depth++; p += 2; }
                else if (p + 1 < end && p[0] == '*' && p[1] == '/') { depth--; p += 2; }
                else p++;
            }
        } else {
            return p;
        }
    }
}

/* set_config('<name>', ...): refused for a session role or session
 * transaction characteristic, or when the first argument is not a plain
 * literal (it could name one). Not followed by '(': a column of that name. */
static void pg_set_config(Scan *s, const char *after, const char *end)
{
    static const char *const names[] = {
        "role", "session_authorization", "default_transaction_isolation",
        "default_transaction_read_only", "default_transaction_deferrable", NULL };
    const char *p = pg_skip_ws(after, end);
    if (p >= end || *p != '(') return;
    p = pg_skip_ws(p + 1, end);
    if (p >= end || *p != '\'') { refuse(s, C_SET_CONFIG); return; }
    p++;
    char name[W_MAX];
    size_t n = 0;
    while (p < end && *p != '\'') {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (n + 1 < sizeof name) name[n++] = c;
        else { n = 0; break; }
    }
    name[n] = '\0';
    if (in(name, names)) refuse(s, C_SET_CONFIG);
}

static void pg_word(Scan *s, const char *w, int anchor, const char *after,
                    const char *end)
{
    static const char *const advisory[] = {
        "pg_advisory_lock", "pg_advisory_lock_shared", "pg_try_advisory_lock",
        "pg_try_advisory_lock_shared", "pg_advisory_unlock",
        "pg_advisory_unlock_shared", "pg_advisory_unlock_all", NULL };
    static const char *const default_txn[] = {
        "default_transaction_isolation", "default_transaction_read_only",
        "default_transaction_deferrable", NULL };

    if (in(w, advisory)) refuse(s, C_ADVISORY);
    if (eq(w, "set_config")) pg_set_config(s, after, end);
    if (anchor && (eq(w, "listen") || eq(w, "unlisten"))) refuse(s, C_LISTEN);
    if (eq(s->prev, "prepare") && eq(w, "transaction") && (s->raw || s->k == 1))
        refuse(s, C_PREP_TXN);

    if (s->reset_mode) {
        s->reset_mode++;
        if (s->reset_mode == 2 && eq(w, "role")) refuse(s, C_ROLE);
        if (s->reset_mode == 2 && eq(w, "session_authorization")) refuse(s, C_SESS_AUTH);
        if (s->reset_mode == 3 && eq(s->prev, "session") && eq(w, "authorization"))
            refuse(s, C_SESS_AUTH);
    }
    if (s->set_mode) {
        s->set_mode++;               /* 2 = the first word after SET */
        if (s->set_mode == 2 && eq(w, "local")) s->set_local = 1;
        if (!s->set_local) {
            if (eq(w, "role") &&
                (s->set_mode == 2 || (s->set_mode == 3 && eq(s->prev, "session"))))
                refuse(s, C_ROLE);
            if (eq(w, "session_authorization")) refuse(s, C_SESS_AUTH);
            if (eq(s->prev, "session") && eq(w, "authorization")) refuse(s, C_SESS_AUTH);
            if (s->set_mode == 3 && eq(s->prev, "session") && eq(w, "characteristics"))
                refuse(s, C_SESS_CHAR);
            if (in(w, default_txn)) refuse(s, C_SESS_CHAR);
        }
    }
    if (anchor && eq(w, "set")) { s->set_mode = 1; s->set_local = 0; }
    if (anchor && eq(w, "reset")) s->reset_mode = 1;
}

static void on_word(Scan *s, const char *w, int uservar, const char *after,
                    const char *end)
{
    static const char *const compound[] = {
        "begin", "then", "else", "do", "loop", "repeat", NULL };
    int anchor = s->raw || s->k == 0 ||
                 (s->d == HL_DB_SQL_MYSQL && in(s->prev, compound));
    if (s->d == HL_DB_SQL_MYSQL) my_word(s, w, uservar, anchor);
    else                         pg_word(s, w, anchor, after, end);
    memcpy(s->prev2, s->prev, sizeof s->prev2);
    memcpy(s->prev, w, sizeof s->prev);
    s->k++;
}

/* ── Tokenizer ──────────────────────────────────────────────────────── */

/* Past a '...' literal ('' doubles a quote), or @p end when unterminated. */
static const char *skip_quote(Scan *s, const char *p, const char *end, char q)
{
    p++;
    while (p < end) {
        if (*p == q) {
            if (p + 1 < end && p[1] == q) { p += 2; continue; }
            return p + 1;
        }
        p++;
    }
    s->unterminated = 1;
    return end;
}

/* A Postgres dollar quote at @p p ($$ or $tag$): 1 with the opening tag's
 * length in *taglen, or 0 when @p p opens none ($1, a lone '$'). */
static int pg_dollar_tag(const char *p, const char *end, size_t *taglen)
{
    const char *q = p + 1;
    if (q < end && *q != '$') {
        unsigned char c = (unsigned char)*q;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80))
            return 0;
        while (q < end && is_word_char((unsigned char)*q) && *q != '$') q++;
    }
    if (q >= end || *q != '$') return 0;
    *taglen = (size_t)(q - p) + 1;
    return 1;
}

static void scan(Scan *s, const char *p, const char *end)
{
    int mysql = s->d == HL_DB_SQL_MYSQL;
    int uservar = 0;
    s->stmt_start = p;
    while (p < end && !s->refused) {
        unsigned char c = (unsigned char)*p;
        if (!s->raw) {
            if (c == '\'') { p = skip_quote(s, p, end, '\''); uservar = 0; continue; }
            if (c == '"' || c == '`') {
                /* A quoted name (or MySQL "string"): one word, so a quoted
                 * function name still matches. */
                const char *b = p + 1, *e = skip_quote(s, p, end, (char)c);
                const char *ce = (e > b && e[-1] == (char)c) ? e - 1 : e;
                char w[W_MAX];
                size_t n = 0;
                int ok = 1;
                for (const char *q = b; q < ce; q++) {
                    char ch = *q;
                    if (!is_word_char((unsigned char)ch)) { ok = 0; break; }
                    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                    if (n + 1 < sizeof w) w[n++] = ch;
                    else { ok = 0; break; }
                }
                w[ok ? n : 0] = '\0';
                on_word(s, w, uservar, e, end);
                uservar = 0;
                p = e;
                continue;
            }
            if (c == '-' && p + 1 < end && p[1] == '-' &&
                (!mysql || p + 2 >= end || (unsigned char)p[2] <= 0x20 ||
                 p[2] == 0x7f)) {
                /* Ended at CR as well as LF: ending early shows more code. */
                while (p < end && *p != '\n' && *p != '\r') p++;
                uservar = 0;
                continue;
            }
            if (mysql && c == '#') {
                while (p < end && *p != '\n' && *p != '\r') p++;
                uservar = 0;
                continue;
            }
            if (c == '/' && p + 1 < end && p[1] == '*') {
                int depth = 1;
                p += 2;
                while (p < end && depth > 0) {
                    if (!mysql && p + 1 < end && p[0] == '/' && p[1] == '*') { depth++; p += 2; }
                    else if (p + 1 < end && p[0] == '*' && p[1] == '/') { depth--; p += 2; }
                    else p++;
                }
                if (depth > 0) s->unterminated = 1;
                uservar = 0;
                continue;
            }
            if (!mysql && c == '$') {
                size_t tl = 0;
                if (pg_dollar_tag(p, end, &tl)) {
                    const char *b = p + tl, *e = b;
                    while (e + tl <= end && memcmp(e, p, tl) != 0) e++;
                    if (e + tl > end) {
                        s->unterminated = 1;
                        scan_raw(s, b, end);
                        p = end;
                    } else {
                        scan_raw(s, b, e);   /* a DO / function body runs it */
                        p = e + tl;
                    }
                    uservar = 0;
                    continue;
                }
                p++;                         /* $1, a lone '$' */
                while (p < end && *p >= '0' && *p <= '9') p++;
                uservar = 0;
                continue;
            }
        }
        if (c == ';') {
            end_statement(s, p);
            p++;
            s->stmt_start = p;
            uservar = 0;
            continue;
        }
        if (c == '@') {
            if (p + 1 < end && p[1] == '@') { p += 2; uservar = 0; continue; }
            p++;
            uservar = 1;
            continue;
        }
        if (is_word_start(s, c)) {
            char w[W_MAX];
            size_t n = 0;
            int ok = 1;
            while (p < end && is_word_char((unsigned char)*p)) {
                char ch = *p++;
                if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
                if (n + 1 < sizeof w) w[n++] = ch;
                else ok = 0;
            }
            w[ok ? n : 0] = '\0';
            on_word(s, w, uservar, p, end);
            uservar = 0;
            continue;
        }
        uservar = 0;
        p++;
    }
    if (!s->refused) end_statement(s, end);
}

/* 1 when @p sql must be read raw from the start: a backslash (an escape or
 * not, by the server's mode), a MySQL executable comment (MySQL runs it). */
static int needs_raw(const char *sql, HlDbSqlDialect d)
{
    if (strchr(sql, '\\')) return 1;
    if (d == HL_DB_SQL_MYSQL &&
        (strstr(sql, "/*!") || strstr(sql, "/*M!") || strstr(sql, "/*m!")))
        return 1;
    return 0;
}

/* Postgres U&"..." (any case): its escapes can spell any name. */
static int pg_has_uescape_ident(const char *sql)
{
    for (const char *p = sql; p[0] && p[1] && p[2]; p++)
        if ((p[0] == 'u' || p[0] == 'U') && p[1] == '&' && p[2] == '"') return 1;
    return 0;
}

const char *hl_db_session_sql_refused(const char *sql, HlDbSqlDialect d)
{
    if (!sql) return NULL;
    if (d == HL_DB_SQL_POSTGRES && pg_has_uescape_ident(sql)) return C_UESCAPE;
    const char *end = sql + strlen(sql);
    Scan s;
    memset(&s, 0, sizeof s);
    s.d = d;
    s.raw = needs_raw(sql, d);
    scan(&s, sql, end);
    if (!s.refused && !s.raw && s.unterminated) {
        /* The server rejects such a text, but read it raw anyway: what it
         * ran before the bad token is not ours to guess. */
        Scan r;
        memset(&r, 0, sizeof r);
        r.d = d;
        r.raw = 1;
        scan(&r, sql, end);
        return r.refused;
    }
    return s.refused;
}

/* ── Hull's own session statements ──────────────────────────────────── */

static _Thread_local int tl_trusted;

int hl_db_session_sql_trust(int on)
{
    int prev = tl_trusted;
    tl_trusted = on ? 1 : 0;
    return prev;
}

int hl_db_session_sql_trusted(void)
{
    return tl_trusted;
}

void hl_db_session_sql_message(char *buf, size_t bufsz, const char *cls)
{
    if (!buf || bufsz == 0) return;
    snprintf(buf, bufsz,
             "%s is refused on a shared connection (its session is reset when "
             "each entry ends); use db.batch or BEGIN/COMMIT, or a "
             "databases.named connection with session_control = true",
             cls ? cls : "a session-altering statement");
}

#endif /* HL_ENABLE_DB */

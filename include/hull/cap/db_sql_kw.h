/*
 * cap/db_sql_kw.h: what kind of transaction-control statement a SQL text is
 *
 * The backends that track transaction state from the statement text (DuckDB,
 * which reports none) or that must tell a COMMIT from a ROLLBACK by text
 * (Postgres, MySQL) share this one reader. Leading whitespace and SQL comments
 * (line comments from "--", and block comments) are skipped, so a BEGIN behind
 * a comment is still a BEGIN; a longer word such as COMMITTED is not a
 * keyword. It reads at most the first four words, so it costs nothing on
 * ordinary statements (audit 6 L6).
 *
 * Block comments nest in Postgres (and DuckDB, whose parser is Postgres's),
 * not in MySQL or SQLite: there the first star-slash ends the comment, so
 * slash-star slash-star star-slash BEGIN ... reads as a BEGIN to the server and,
 * nested, as nothing at all to this reader. The *_ex forms take the dialect's
 * rule; the plain ones nest (audit 10).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_SQL_KW_H
#define HL_CAP_DB_SQL_KW_H

#include <stddef.h>

typedef enum {
    HL_SQL_TXN_NONE = 0,  /* not a transaction-control statement */
    HL_SQL_TXN_BEGIN,     /* BEGIN [WORK|TRANSACTION|...], START TRANSACTION */
    HL_SQL_TXN_COMMIT,    /* COMMIT / END [WORK|TRANSACTION] [AND [NO] CHAIN] */
    HL_SQL_TXN_ROLLBACK,  /* ROLLBACK / ABORT [WORK|TRANSACTION] (not TO ...) */
    HL_SQL_TXN_SAVEPOINT, /* SAVEPOINT, RELEASE, ROLLBACK [WORK] TO ... */
    HL_SQL_TXN_OTHER      /* a transaction word in a form not listed above
                           * (COMMIT PREPARED, ...): callers decide safely */
} HlSqlTxnKind;

/* Skip whitespace and SQL comments; block comments nest when @p nest. An
 * unterminated block comment ends the text. */
static inline const char *hl_sql_skip_space_ex(const char *p, int nest)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f' || *p == '\v')
            p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
        } else if (p[0] == '/' && p[1] == '*') {
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (nest && p[0] == '/' && p[1] == '*') { depth++; p += 2; }
                else if (p[0] == '*' && p[1] == '/')    { depth--; p += 2; }
                else p++;
            }
        } else {
            return p;
        }
    }
}

static inline const char *hl_sql_skip_space(const char *p)
{
    return hl_sql_skip_space_ex(p, 1);
}

/* Read the next word (letters, digits, '_') lowercased into @p buf; "" when
 * the next character starts no word. Returns the text after the word. */
static inline const char *hl_sql_next_word_ex(const char *p, char *buf,
                                              size_t sz, int nest)
{
    p = hl_sql_skip_space_ex(p, nest);
    size_t n = 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '_') {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (n + 1 < sz) buf[n++] = c;
        else n = sz;                     /* too long to be a keyword */
    }
    buf[n < sz ? n : 0] = '\0';
    return p;
}

static inline const char *hl_sql_next_word(const char *p, char *buf, size_t sz)
{
    return hl_sql_next_word_ex(p, buf, sz, 1);
}

static inline int hl_sql_word_is_(const char *w, const char *kw)
{
    while (*w && *w == *kw) { w++; kw++; }
    return *w == '\0' && *kw == '\0';
}

static inline HlSqlTxnKind hl_sql_txn_kind_ex(const char *sql, int nest)
{
    if (!sql) return HL_SQL_TXN_NONE;
    char w[16], w2[16];
    const char *p = hl_sql_next_word_ex(sql, w, sizeof w, nest);
    if (hl_sql_word_is_(w, "begin"))
        return HL_SQL_TXN_BEGIN;
    if (hl_sql_word_is_(w, "start")) {
        (void)hl_sql_next_word_ex(p, w2, sizeof w2, nest);
        return hl_sql_word_is_(w2, "transaction") ? HL_SQL_TXN_BEGIN
                                                  : HL_SQL_TXN_NONE;
    }
    if (hl_sql_word_is_(w, "savepoint") || hl_sql_word_is_(w, "release"))
        return HL_SQL_TXN_SAVEPOINT;
    int commit = hl_sql_word_is_(w, "commit") || hl_sql_word_is_(w, "end");
    int rollback = hl_sql_word_is_(w, "rollback") || hl_sql_word_is_(w, "abort");
    if (!commit && !rollback) return HL_SQL_TXN_NONE;
    p = hl_sql_next_word_ex(p, w2, sizeof w2, nest);
    if (hl_sql_word_is_(w2, "work") || hl_sql_word_is_(w2, "transaction"))
        p = hl_sql_next_word_ex(p, w2, sizeof w2, nest);
    if (rollback && hl_sql_word_is_(w2, "to"))
        return HL_SQL_TXN_SAVEPOINT;
    if (hl_sql_word_is_(w2, "and")) {     /* AND [NO] CHAIN */
        p = hl_sql_next_word_ex(p, w2, sizeof w2, nest);
        if (!hl_sql_word_is_(w2, "no"))
            return HL_SQL_TXN_OTHER;      /* AND CHAIN opens the next one */
        p = hl_sql_next_word_ex(p, w2, sizeof w2, nest);
        if (!hl_sql_word_is_(w2, "chain")) return HL_SQL_TXN_OTHER;
        w2[0] = '\0';
    }
    if (w2[0])
        return HL_SQL_TXN_OTHER;          /* COMMIT PREPARED 'x', ... */
    p = hl_sql_skip_space_ex(p, nest);
    while (*p == ';') p = hl_sql_skip_space_ex(p + 1, nest);
    if (*p) return HL_SQL_TXN_OTHER;      /* trailing text we do not read */
    return commit ? HL_SQL_TXN_COMMIT : HL_SQL_TXN_ROLLBACK;
}

/* Postgres / DuckDB comment rules (nested block comments). */
static inline HlSqlTxnKind hl_sql_txn_kind(const char *sql)
{
    return hl_sql_txn_kind_ex(sql, 1);
}

#endif /* HL_CAP_DB_SQL_KW_H */

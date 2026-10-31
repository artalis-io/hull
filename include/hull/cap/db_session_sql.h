/*
 * cap/db_session_sql.h: app SQL that would change a shared MySQL / MariaDB /
 * Postgres session beyond the statement (audit convergence phase 3, §7.1)
 *
 * Hull no longer infers server session state from SQL text: every connection
 * an entry used is reset at the entry boundary (COM_RESET_CONNECTION /
 * DISCARD ALL), and the statements whose whole point is session state that
 * outlives the statement - autocommit, table / instance / backup locks, XA,
 * named and advisory locks, a session role or session transaction
 * characteristics, LISTEN - are refused on every connection that has not
 * opted in with `databases.named.<x>.session_control = true`.
 *
 * The classifier is lexical and conservative: a text it cannot read the way
 * the server does (a backslash, which ends no string under
 * NO_BACKSLASH_ESCAPES; a MySQL executable comment; an unterminated quote or
 * comment) is read "raw" - quotes and comments count as code and every word
 * may start a statement - and a Postgres dollar-quoted body (a DO block or a
 * function body runs it) and the text a MySQL PREPARE / EXECUTE IMMEDIATE
 * takes are read raw too. Dynamic SQL that assembles a name at run time
 * (CONCAT, EXECUTE format(...)) is not seen: the reset at the entry boundary
 * is the guarantee, the refusal makes the ordinary spellings fail loudly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_SESSION_SQL_H
#define HL_CAP_DB_SESSION_SQL_H

#include <stddef.h>

typedef enum {
    HL_DB_SQL_MYSQL = 0,      /* MySQL and MariaDB */
    HL_DB_SQL_POSTGRES = 1
} HlDbSqlDialect;

/* The class of session-altering statement @p sql contains (a static string
 * such as "LOCK TABLES / UNLOCK TABLES"), or NULL when it contains none. Any
 * statement of a multi-statement text counts. NULL @p sql is NULL. */
const char *hl_db_session_sql_refused(const char *sql, HlDbSqlDialect dialect);

/* Hull's own session statements (the migration lock: GET_LOCK /
 * pg_advisory_lock in hl_migrate_run) run with this thread's flag set, so the
 * backend does not refuse them. Returns the previous value. App SQL - the
 * migration files included - never runs with it set. */
int hl_db_session_sql_trust(int on);
int hl_db_session_sql_trusted(void);

/* Write the refusal message for @p cls into @p buf. */
void hl_db_session_sql_message(char *buf, size_t bufsz, const char *cls);

#endif /* HL_CAP_DB_SESSION_SQL_H */

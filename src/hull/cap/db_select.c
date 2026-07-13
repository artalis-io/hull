/*
 * cap/db_select.c: Database backend selection by DSN scheme
 *
 * Chooses the HlDbBackend for a DSN so the three call sites that open a
 * database (app_context, tool_orchestration, migrate) stay backend-neutral.
 * Deliberately free of any backend header (no sqlite3.h) so it compiles in
 * every flag combination the umbrella permits: SQLite-only, PostgreSQL-only,
 * or both.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_backend.h"
#include <string.h>

/* Case-sensitive prefix test. DSN schemes are lowercase by convention. */
static int dsn_has_scheme(const char *dsn, const char *scheme)
{
    size_t n = strlen(scheme);
    return strncmp(dsn, scheme, n) == 0;
}

const HlDbBackend *hl_db_backend_select(const char *dsn, const char **err)
{
    if (err) *err = NULL;

    /* PostgreSQL DSNs are explicit by scheme; everything else is SQLite
     * (a file path, ":memory:", or an sqlite "file:" URI). */
    if (dsn && (dsn_has_scheme(dsn, "postgres://") ||
                dsn_has_scheme(dsn, "postgresql://"))) {
#ifdef HL_ENABLE_POSTGRES
        return &hl_db_backend_postgres;
#else
        if (err)
            *err = "this hull was built without HL_ENABLE_POSTGRES; a "
                   "postgres:// DSN requires it";
        return NULL;
#endif
    }

#ifdef HL_ENABLE_SQLITE
    return &hl_db_backend_sqlite;
#else
    if (err)
        *err = "this hull was built without HL_ENABLE_SQLITE; a file or "
               ":memory: DSN requires it";
    return NULL;
#endif
}

#endif /* HL_ENABLE_DB */

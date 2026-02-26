/*
 * cap/db.h — Database capability (SQLite)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_H
#define HL_CAP_DB_H

#include "hull/cap/types.h"

typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

int hl_cap_db_query(sqlite3 *db, const char *sql,
                      const HlValue *params, int nparams,
                      HlRowCallback cb, void *ctx,
                      HlAllocator *alloc);

int hl_cap_db_exec(sqlite3 *db, const char *sql,
                     const HlValue *params, int nparams);

int64_t hl_cap_db_last_id(sqlite3 *db);

#endif /* HL_CAP_DB_H */

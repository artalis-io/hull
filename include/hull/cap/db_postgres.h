/*
 * cap/db_postgres.h - PostgreSQL backend registration
 *
 * Exposes the Postgres backend const for the DSN-scheme registry (db_select.c).
 * The abstract interface (db_backend.h) stays free of concrete-backend symbols.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_POSTGRES_H
#define HL_CAP_DB_POSTGRES_H

#include "hull/cap/db_backend.h"

#ifdef HL_ENABLE_POSTGRES
extern const HlDbBackend hl_db_backend_postgres;
#endif

#endif /* HL_CAP_DB_POSTGRES_H */

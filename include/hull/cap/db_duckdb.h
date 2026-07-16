/*
 * cap/db_duckdb.h - DuckDB backend registration
 *
 * Exposes the DuckDB backend const for the DSN-scheme registry (db_select.c).
 * The abstract interface (db_backend.h) stays free of concrete-backend symbols.
 * DuckDB is a side-loaded, statically-linked embedded OLAP backend; the archive
 * set is fetched via `make fetch-duckdb` and the flavor built with
 * HL_ENABLE_DUCKDB=1. See docs/duckdb_backend_design.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_DUCKDB_H
#define HL_CAP_DB_DUCKDB_H

#include "hull/cap/db_backend.h"

#ifdef HL_ENABLE_DUCKDB
extern const HlDbBackend hl_db_backend_duckdb;
#endif

#endif /* HL_CAP_DB_DUCKDB_H */

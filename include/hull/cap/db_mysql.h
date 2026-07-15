/*
 * cap/db_mysql.h - MySQL / MariaDB backend registration
 *
 * Exposes the MySQL backend const for the DSN-scheme registry (db_select.c).
 * The abstract interface (db_backend.h) stays free of concrete-backend symbols.
 * One backend serves both `mysql://` and `mariadb://` (shared wire protocol).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_MYSQL_H
#define HL_CAP_DB_MYSQL_H

#include "hull/cap/db_backend.h"

#ifdef HL_ENABLE_MYSQL
extern const HlDbBackend hl_db_backend_mysql;
#endif

#endif /* HL_CAP_DB_MYSQL_H */

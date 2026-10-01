/*
 * cap/db_dynamic.h: db.open(dsn) dynamic connection open with allowlist
 *
 * Validates a runtime-computed DSN against the manifest's databases.dynamic
 * policy (roadmap §2.2) and opens a caller-owned connection. A network scheme
 * is gated by the host allowlist (glob/CIDR via host_match, "$VAR" entries
 * resolved from the environment); a file scheme by manifest.fs. A process-wide
 * cap bounds concurrent dynamic connections. Credentials ride in the DSN.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_DYNAMIC_H
#define HL_CAP_DB_DYNAMIC_H

#include "hull/cap/db_backend.h"

#include <stddef.h>

typedef struct HlManifestDbDynamic HlManifestDbDynamic;

/* Longest DSN db.open will open (the app's, or the absolute file path it
 * resolves to). */
#define HL_DB_DYNAMIC_DSN_MAX 4096
typedef struct HlFsConfig HlFsConfig;

/*
 * Validate @p dsn against @p policy (+ @p fs_cfg for file backends) and open a
 * caller-owned connection. Returns a malloc'd HlDbHandle (close with
 * hl_db_dynamic_close) on success, or NULL with *err (a static message) set on
 * any rejection: no policy, scheme not allowlisted, host/path not allowed,
 * backend not compiled, concurrent-cap reached, or connect failure.
 */
HlDbHandle *hl_db_dynamic_open(const char *dsn,
                               const HlManifestDbDynamic *policy,
                               const HlFsConfig *fs_cfg,
                               const char **err);

/*
 * hl_db_dynamic_open, also writing the DSN the connection was actually opened
 * with into @p opened (@p opened_size bytes) - the one a second connection to
 * the same database must use, e.g. conn.async's worker pool. For a file
 * backend that is the absolute path under the app directory, not the
 * app-relative DSN the app passed (which the backend would open against the
 * process's working directory). Every other DSN is returned as given.
 * @p opened may be NULL.
 */
HlDbHandle *hl_db_dynamic_open_ex(const char *dsn,
                                  const HlManifestDbDynamic *policy,
                                  const HlFsConfig *fs_cfg,
                                  char *opened, size_t opened_size,
                                  const char **err);

/* Close + free a handle from hl_db_dynamic_open. NULL-safe; call exactly once
 * per successful open (the caller guards against double-close). */
void hl_db_dynamic_close(HlDbHandle *h);

/* Current count of open dynamic connections (tests / diagnostics). */
int hl_db_dynamic_open_count(void);

#endif /* HL_CAP_DB_DYNAMIC_H */

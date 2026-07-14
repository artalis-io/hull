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

typedef struct HlManifestDbDynamic HlManifestDbDynamic;
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

/* Close + free a handle from hl_db_dynamic_open. NULL-safe; call exactly once
 * per successful open (the caller guards against double-close). */
void hl_db_dynamic_close(HlDbHandle *h);

/* Current count of open dynamic connections (tests / diagnostics). */
int hl_db_dynamic_open_count(void);

#endif /* HL_CAP_DB_DYNAMIC_H */

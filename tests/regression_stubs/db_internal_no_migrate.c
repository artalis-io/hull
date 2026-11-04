/*
 * Pre-fix stand-ins for hl_db_internal_check_unmoved and
 * hl_db_internal_configure (src/hull/cap/db_internal.c), linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before the fix --no-migrate checked nothing: the internal database was
 * activated whether or not the app database's tables had been moved into it.
 * configure is the derivation app_context.c did inline.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifdef HL_ENABLE_DB

#include "hull/cap/db_internal.h"
#include "hull/cap/db_registry.h"
#include "hull/shared/host.h"

#include <limits.h>
#include <stdlib.h>

int hl_db_internal_check_unmoved(HlDbRegistry *reg, int keep)
{
    (void)reg; (void)keep;
    return 0;
}

int hl_db_internal_configure(HlDbRegistry *reg, const char *app_dir,
                             const char *default_dsn, int keep)
{
    char app_real[PATH_MAX], idsn[HL_HOST_PATH_MAX + 32];
    size_t ibase = 0;
    int in_hull = 0;
    int k = realpath(app_dir, app_real)
            ? hl_db_internal_default_dsn(app_real, default_dsn, idsn,
                                         sizeof idsn, &ibase, &in_hull)
            : 0;
    if (k < 0) return -1;
    if (k == 0) return 0;
    return hl_db_registry_set_default_internal(reg, idsn, ibase, in_hull,
                                               !keep) != 0 ? -1 : 0;
}

#endif /* HL_ENABLE_DB */

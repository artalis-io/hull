/*
 * Pre-fix stand-in for hl_db_internal_fail_column_push_for_test
 * (src/hull/cap/db_internal.c), linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before C audit L14 there was no seam, and column_cb ignored a failed push:
 * the stand-in injects nothing, so the move copies the table as the pre-fix
 * code did and the L14 test sees it succeed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifdef HL_ENABLE_DB

#include "hull/cap/db_internal.h"

void hl_db_internal_fail_column_push_for_test(int after)
{
    (void)after;
}

#endif

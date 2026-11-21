/*
 * Pre-fix stand-in for hl_generation_take_ex (src/hull/shared/generation.c),
 * linked only by scripts/check_regression_test.sh. Before the fix the
 * runners called hl_generation_take with no --verify-sig argument, so the
 * pipeline ran under --verify-sig too, before its generation was refused.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/generation.h"

int hl_generation_take_ex(int embedded, const char *app_dir, int verify_sig)
{
    (void)verify_sig;
    return hl_generation_take(embedded, app_dir);
}

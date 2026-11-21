/*
 * Pre-fix stand-in for hl_stdin_secret_test_set_hook
 * (src/hull/shared/stdin_secret.c, test builds only), linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before audit 51 the read had no test hook: setting one does nothing, so
 * the stdin_secret_audit51 cases see the pre-fix read drop the signal.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define HL_STDIN_SECRET_TEST_HOOKS 1   /* the declaration lives under it */
#include "hull/shared/stdin_secret.h"

void hl_stdin_secret_test_set_hook(int (*fn)(int point)) { (void)fn; }

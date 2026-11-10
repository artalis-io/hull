/*
 * Pre-fix stand-ins for hl_host_test_force_windows and
 * hl_host_path_unsafe_on_windows (src/hull/shared/host.c), linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before the fix host.c had no test override - hl_host_is_windows() was the
 * real host's answer - and no shared Windows path predicate: the fs policy
 * kept its own copy and confine_manifest_dsn had none, so a backslash, drive
 * or short-name spelling of a manifest file DSN was accepted.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/host.h"

void hl_host_test_force_windows(int v)
{
    (void)v;
}

int hl_host_path_unsafe_on_windows(const char *path)
{
    (void)path;
    return 0;
}

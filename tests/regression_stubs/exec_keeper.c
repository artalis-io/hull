/*
 * Pre-fix stand-ins for the macOS keeper (#1218 phase 2,
 * docs/design/exec_broker_macos_isolation.md §4.6): hl_exec_plat_keeper and
 * hl_exec_plat_keeper_run (src/hull/cap/exec_broker_*.c). Linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before the fix the broker forked the job itself: nothing waited on the
 * job for it, and nothing ended it when the broker was killed with SIGKILL.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/exec.h"

#ifdef HL_EXEC_BROKER_SUPPORTED
#include "hull/cap/exec_platform.h"
#include <unistd.h>

int hl_exec_plat_keeper(void) { return 0; }

void hl_exec_plat_keeper_run(pid_t broker, pid_t job, int kill_r)
{
    (void)broker; (void)job; (void)kill_r;
    _exit(0);   /* no keeper: the job is left to itself */
}
#endif

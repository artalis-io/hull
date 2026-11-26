/*
 * Pre-fix stand-ins for the macOS command router (#1218,
 * docs/design/exec_broker_macos_isolation.md): hl_exec_policy_slice
 * (src/hull/cap/exec.c), hl_exec_plat_router / hl_exec_plat_router_lock and
 * hl_exec_macos_router_profile (src/hull/cap/exec_broker_*.c). Linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before the fix no host routed: one broker held every command, and on macOS
 * its one Seatbelt profile was the union of every command's grants. There
 * was no way to take one command's entry out of a policy, and no router
 * profile.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/exec.h"

int hl_exec_policy_slice(const uint8_t *data, size_t len, int i,
                         uint8_t **out, size_t *out_len)
{
    (void)data; (void)len; (void)i; (void)out; (void)out_len;
    return -1;
}

#ifdef HL_EXEC_BROKER_SUPPORTED
#include "hull/cap/exec_platform.h"

int hl_exec_plat_router(void) { return 0; }

int hl_exec_plat_router_lock(HlExecReport *r)
{
    hl_exec_report_set(r, "exec: no router");
    return -1;
}

#ifdef __APPLE__
const char hl_exec_macos_router_profile[] = "";
#endif
#endif

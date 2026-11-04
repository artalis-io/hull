/*
 * Pre-fix stand-in for hl_ws_registry_mark_closed (src/hull/cap/ws.c), linked
 * only by scripts/check_regression_test.sh (docs/design/audit_convergence.md
 * 5.1). Before the fix on_close only set `closed`, kept the Keel pointer Keel
 * was about to free, and hl_ws_registry_find still returned the node.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/ws.h"

void hl_ws_registry_mark_closed(HlWsConn *conn)
{
    if (conn) conn->closed = 1;
}

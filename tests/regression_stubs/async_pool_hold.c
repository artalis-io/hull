/*
 * Pre-fix stand-ins for hl_async_pool_hold / _release / _held
 * (src/hull/async/poll.c), linked only by scripts/check_regression_test.sh
 * (docs/design/audit_convergence.md 5.1). Before C audit L4 nothing held the
 * worker pool before the phase-2 sandbox: a hold is a no-op and the pool is
 * never held, so the L4 test sees a submit start the workers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/async_backend.h"

void hl_async_pool_hold(void) {}
void hl_async_pool_release(void) {}
int  hl_async_pool_held(void) { return 0; }

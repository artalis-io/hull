/*
 * hull/limits.h - Umbrella for all subsystem limit headers
 *
 * Constants have been split per subsystem so that touching a WASM or GPU
 * limit doesn't force a recompile of unrelated translation units. New code
 * should include the specific sub-header it needs:
 *
 *   hull/limits/core.h       - module system, HTTP body, server defaults,
 *                              crypto, SMTP, thread pool, worker, compression
 *   hull/limits/runtime.h    - Lua + JS runtime memory + instruction limits
 *                              (transitively pulls in core.h)
 *   hull/limits/wasm.h       - WAMR / compute plugin limits
 *   hull/limits/gpu.h        - wgpu-native GPU compute limits
 *
 * This umbrella stays for back-compat: existing consumers that include
 * "hull/limits.h" continue to see every constant.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_H
#define HL_LIMITS_H

#include "hull/limits/core.h"
#include "hull/limits/runtime.h"
#include "hull/limits/wasm.h"
#include "hull/limits/gpu.h"

#endif /* HL_LIMITS_H */

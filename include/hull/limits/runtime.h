/*
 * hull/limits/runtime.h - Lua + QuickJS runtime memory / instruction limits
 *
 * Used by runtime/lua and runtime/js and by code that constructs
 * HlLuaConfig / HlJsConfig defaults (mainly main.c).
 *
 * Depends on core.h for HL_MODULE_MAX_SIZE (used in HL_SCRATCH_SIZE).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_RUNTIME_H
#define HL_LIMITS_RUNTIME_H

#include "hull/limits/core.h"

/* ── Runtime memory ────────────────────────────────────────────────── */

#define HL_SCRATCH_SIZE       (HL_MODULE_MAX_SIZE + 256 * 1024) /* module load + request scratch */
#define HL_LUA_DEFAULT_HEAP   (64 * 1024 * 1024) /* 64 MB */
#define HL_JS_DEFAULT_HEAP    (64 * 1024 * 1024) /* 64 MB */
#define HL_JS_DEFAULT_STACK   (1 * 1024 * 1024)  /* 1 MB */
#define HL_JS_GC_THRESHOLD    (256 * 1024)       /* 256 KB */

/* ── Instruction limits ────────────────────────────────────────────── */

#define HL_DEFAULT_INSTRUCTIONS (100 * 1000 * 1000) /* 100M per handler */

#endif /* HL_LIMITS_RUNTIME_H */

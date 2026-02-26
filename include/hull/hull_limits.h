/*
 * hull_limits.h — Named constants for all configurable limits
 *
 * Centralizes magic numbers scattered across the codebase into a single
 * header. Each constant documents its purpose and default value.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_H
#define HL_LIMITS_H

/* ── Module system ──────────────────────────────────────────────────── */

#define HL_MODULE_PATH_MAX    4096              /* Max resolved module path length */
#define HL_MODULE_MAX_SIZE    (10 * 1024 * 1024) /* 10 MB max module file */

/* ── HTTP / body ────────────────────────────────────────────────────── */

#define HL_BODY_MAX_SIZE      (1024 * 1024)     /* 1 MB request body */
#define HL_QUERY_BUF_SIZE     4096              /* Query string parse buffer */
#define HL_PARAM_NAME_MAX     256               /* Route param name buffer */

/* ── Server defaults ────────────────────────────────────────────────── */

#define HL_MAX_ROUTES         256               /* Maximum route count */
#define HL_DEFAULT_PORT       3000
#define HL_DEFAULT_MAX_CONN   64
#define HL_DEFAULT_READ_TIMEOUT_MS 30000

/* ── Crypto ─────────────────────────────────────────────────────────── */

#define HL_RANDOM_MAX_BYTES   65536             /* crypto.random() max */
#define HL_PBKDF2_ITERATIONS  100000

/* ── Runtime memory ─────────────────────────────────────────────────── */

#define HL_LUA_DEFAULT_HEAP   (64 * 1024 * 1024) /* 64 MB */
#define HL_JS_DEFAULT_HEAP    (64 * 1024 * 1024)  /* 64 MB */
#define HL_JS_DEFAULT_STACK   (1 * 1024 * 1024)   /* 1 MB */
#define HL_JS_GC_THRESHOLD    (256 * 1024)         /* 256 KB */

#endif /* HL_LIMITS_H */

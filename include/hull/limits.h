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
#define HL_DEFAULT_MAX_CONN   256
#define HL_DEFAULT_READ_TIMEOUT_MS 30000
#define HL_DEFAULT_DRAIN_TIMEOUT_MS 5000        /* 5s graceful shutdown */

/* ── Crypto ─────────────────────────────────────────────────────────── */

#define HL_RANDOM_MAX_BYTES   65536             /* crypto.random() max */
#define HL_PBKDF2_ITERATIONS  100000

/* ── SMTP client ───────────────────────────────────────────────────── */

#define HL_SMTP_RECV_BUF_SIZE      1024                /* SMTP response line buffer */
#define HL_SMTP_SEND_BUF_SIZE      1024                /* SMTP command buffer */
#define HL_SMTP_DEFAULT_TIMEOUT_MS 30000               /* Connect/send/recv timeout */
#define HL_SMTP_MAX_MSG_SIZE       (10 * 1024 * 1024)  /* 10 MB max formatted message */

/* ── Thread pool ───────────────────────────────────────────────────── */

#define HL_THREAD_POOL_WORKERS     4                   /* Default worker thread count */
#define HL_THREAD_POOL_CAPACITY    64                  /* Default work queue capacity */

/* ── Worker ─────────────────────────────────────────────────────────── */

#define HL_WORKER_ERR_SIZE         256                  /* Worker error message buffer */

/* ── Compression ───────────────────────────────────────────────────── */

#define HL_COMPRESS_MIN_SIZE  860                  /* Gzip break-even threshold (bytes) */

/* ── Runtime memory ─────────────────────────────────────────────────── */

#define HL_SCRATCH_SIZE       (HL_MODULE_MAX_SIZE + 256 * 1024) /* module load + request scratch */
#define HL_LUA_DEFAULT_HEAP   (64 * 1024 * 1024) /* 64 MB */
#define HL_JS_DEFAULT_HEAP    (64 * 1024 * 1024)  /* 64 MB */
#define HL_JS_DEFAULT_STACK   (1 * 1024 * 1024)   /* 1 MB */
#define HL_JS_GC_THRESHOLD    (256 * 1024)         /* 256 KB */

/* ── Instruction limits ────────────────────────────────────────────── */

#define HL_DEFAULT_INSTRUCTIONS (100 * 1000 * 1000) /* 100M per handler */

/* ── WASM compute limits ───────────────────────────────────────────── */
/* All maximums are #ifndef-guarded so Makefile -D can override them.  */

#define HL_WASM_DEFAULT_HEAP       (2 * 1024 * 1024)       /* 2 MB */
#ifndef HL_WASM_MAX_HEAP
#define HL_WASM_MAX_HEAP           (4U * 1024 * 1024 * 1024 - 1) /* ~4 GB (uint32_t max) */
#endif
#define HL_WASM_DEFAULT_STACK      (64 * 1024)             /* 64 KB */
#ifndef HL_WASM_MAX_STACK
#define HL_WASM_MAX_STACK          (8 * 1024 * 1024)       /* 8 MB */
#endif
#ifndef HL_WASM_DEFAULT_MAX_INPUT
#define HL_WASM_DEFAULT_MAX_INPUT  (1 * 1024 * 1024)       /* 1 MB */
#endif
#ifndef HL_WASM_DEFAULT_MAX_OUTPUT
#define HL_WASM_DEFAULT_MAX_OUTPUT (1 * 1024 * 1024)       /* 1 MB */
#endif
#ifndef HL_WASM_MAX_IO_SIZE
#define HL_WASM_MAX_IO_SIZE        (256 * 1024 * 1024)     /* 256 MB */
#endif
#define HL_WASM_DEFAULT_GAS        (10 * 1000 * 1000)      /* 10M instructions */
#ifndef HL_WASM_MAX_GAS
#define HL_WASM_MAX_GAS            (100LL * 1000 * 1000 * 1000) /* 100B instructions */
#endif

/* ── WASM host_call buffer ────────────────────────────────────────── */
#ifndef HL_WASM_CALLBACK_BUF_SIZE
#define HL_WASM_CALLBACK_BUF_SIZE  4096  /* host_call CALLBACK output (stack) */
#endif

/* ── WASM instance pool ───────────────────────────────────────────── */
#ifndef HL_WASM_POOL_MAX
#define HL_WASM_POOL_MAX            8
#endif
#ifndef HL_WASM_POOL_HEAP_THRESHOLD
#define HL_WASM_POOL_HEAP_THRESHOLD (4 * 1024 * 1024)   /* 4 MB */
#endif

#endif /* HL_LIMITS_H */

/*
 * hull/limits/core.h — Cross-cutting limits used everywhere
 *
 * Module system, HTTP body, server defaults, crypto, SMTP, threads,
 * workers, compression. Anything not specific to a single subsystem.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_CORE_H
#define HL_LIMITS_CORE_H

/* ── Module system ──────────────────────────────────────────────────── */

#define HL_MODULE_PATH_MAX    4096              /* Max resolved module path length */
#define HL_MODULE_MAX_SIZE    (10 * 1024 * 1024) /* 10 MB max module file */

/* Module registry / resolver (capability-aware declaration system). */
#define HL_MODULE_NAME_MAX           64    /* Max bytes for "hull/<x>" + NUL */
#define HL_MODULE_MAX_DEPS           10    /* Max internal deps per spec. History: 4 →
                                            * 8 for middleware/auth's chain (db, crypto,
                                            * cookie, jwt, session + slack); 8 → 10
                                            * for round-8's auth-flows hull/log dep
                                            * (MEDIUM-9 pcall'd audit-log warning).
                                            * Each unit costs ~16 bytes per registry
                                            * entry — cheap. */
#define HL_MODULE_BITSET_WORDS       2     /* 128 bits → registry headroom */
#define HL_MODULE_RESOLVER_ERR_MAX   256   /* Resolver error message buffer */

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

#endif /* HL_LIMITS_CORE_H */

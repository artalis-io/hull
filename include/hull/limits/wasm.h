/*
 * hull/limits/wasm.h - WASM compute plugin limits (WAMR)
 *
 * Used only by cap/wasm sources, worker_wasm, runtime/{lua,js}/mod_compute,
 * and any other consumer of HlWasmConfig. Changes here should not force
 * a rebuild of non-WASM TUs.
 *
 * All maximums are #ifndef-guarded so Makefile -D can override them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_WASM_H
#define HL_LIMITS_WASM_H

#include <stdint.h>

/* ── WASM compute heaps / stacks ───────────────────────────────────── */

#define HL_WASM_DEFAULT_HEAP       (2 * 1024 * 1024)       /* 2 MB */
#ifndef HL_WASM_MAX_HEAP
#define HL_WASM_MAX_HEAP           ((uint32_t)0xFFFFFFFF) /* ~4 GB (uint32_t max) */
#endif
#define HL_WASM_DEFAULT_STACK      (64 * 1024)             /* 64 KB */
#ifndef HL_WASM_MAX_STACK
#define HL_WASM_MAX_STACK          (8 * 1024 * 1024)       /* 8 MB */
#endif

/* ── WASM I/O ──────────────────────────────────────────────────────── */

#ifndef HL_WASM_DEFAULT_MAX_INPUT
#define HL_WASM_DEFAULT_MAX_INPUT  (1 * 1024 * 1024)       /* 1 MB */
#endif
#ifndef HL_WASM_DEFAULT_MAX_OUTPUT
#define HL_WASM_DEFAULT_MAX_OUTPUT (1 * 1024 * 1024)       /* 1 MB */
#endif
#ifndef HL_WASM_MAX_IO_SIZE
#define HL_WASM_MAX_IO_SIZE        ((uint64_t)256 * 1024 * 1024)  /* 256 MB (WASM32) */
#endif
#ifndef HL_WASM64_MAX_IO_SIZE
#define HL_WASM64_MAX_IO_SIZE      ((uint64_t)16 * 1024 * 1024 * 1024)  /* 16 GB (Memory64) */
#endif

/* ── WASM gas ──────────────────────────────────────────────────────── */

#define HL_WASM_DEFAULT_GAS        (10 * 1000 * 1000)      /* 10M instructions */
#ifndef HL_WASM_MAX_GAS
#define HL_WASM_MAX_GAS            (100LL * 1000 * 1000 * 1000) /* 100B instructions */
#endif

/* ── WASM streaming I/O ────────────────────────────────────────────── */

#ifndef HL_WASM_STREAM_MAX_CHUNK
#define HL_WASM_STREAM_MAX_CHUNK    (16 * 1024 * 1024)  /* 16 MB */
#endif
#ifndef HL_WASM_STREAM_MIN_CHUNK
#define HL_WASM_STREAM_MIN_CHUNK    256
#endif
#ifndef HL_WASM_STREAM_DEFAULT_CHUNK
#define HL_WASM_STREAM_DEFAULT_CHUNK 65536  /* 64 KB */
#endif

/* ── WASM host_call buffer ─────────────────────────────────────────── */

#ifndef HL_WASM_CALLBACK_BUF_SIZE
#define HL_WASM_CALLBACK_BUF_SIZE  4096  /* host_call CALLBACK output (stack) */
#endif

/* ── WASM instance pool ────────────────────────────────────────────── */

#ifndef HL_WASM_POOL_MAX
#define HL_WASM_POOL_MAX            8
#endif
#ifndef HL_WASM_POOL_HEAP_THRESHOLD
#define HL_WASM_POOL_HEAP_THRESHOLD (4 * 1024 * 1024)   /* 4 MB */
#endif

/* ── WASM shared data ──────────────────────────────────────────────── */

#ifndef HL_WASM_MAX_SHARED_DATA
#define HL_WASM_MAX_SHARED_DATA  ((uint32_t)(3UL * 1024 * 1024 * 1024))  /* 3 GB total */
#endif
#ifndef HL_WASM_MAX_DATA_SEGMENTS
#define HL_WASM_MAX_DATA_SEGMENTS 16
#endif

#endif /* HL_LIMITS_WASM_H */

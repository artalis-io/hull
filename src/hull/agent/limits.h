/*
 * agent/limits.h - Named constants for the hull-agent surface.
 *
 * Internal to src/hull/agent/. Replaces every numeric literal that
 * would otherwise read as a magic number.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_AGENT_LIMITS_H
#define HL_AGENT_LIMITS_H

/* ── Buffer sizes (bytes) ──────────────────────────────────────────── */

/* Path buffer for "<dir>/<file>" composition. PATH_MAX on most systems
 * is 4096; the agent never accepts longer paths via CLI. */
#define HL_AGENT_PATH_MAX             1024
/* Same plus a small extension headroom for joined paths like
 * "<HL_AGENT_PATH_MAX-dir>/<file>". */
#define HL_AGENT_PATH_PLUS            1280
/* Longer buffer when recursing into subdirectories. */
#define HL_AGENT_PATH_DEEP            2048

/* Short identifier buffer (module names, table names, arches). */
#define HL_AGENT_IDENT_SHORT          32
/* Medium identifier buffer (file basenames, route patterns). */
#define HL_AGENT_IDENT_MED            128

/* ── File-size caps (bytes) ────────────────────────────────────────── */

#define HL_AGENT_LOG_TAIL_BYTES       (4 * 1024 * 1024)   /* 4 MiB tail of dev.log */
#define HL_AGENT_VALIDATE_MAX_FILE    (4 * 1024 * 1024)   /* 4 MiB per-file source */
#define HL_AGENT_COMPUTE_MAX_INPUT    (16 * 1024 * 1024)  /* 16 MiB compute input */
#define HL_AGENT_TEMPLATE_MAX_DATA    (1 * 1024 * 1024)   /* 1 MiB JSON data */
#define HL_AGENT_QUERIES_MAX_FILE     (1 * 1024 * 1024)   /* 1 MiB queries.json */
#define HL_AGENT_CAPS_MAX_PER_FILE    (1 * 1024 * 1024)   /* 1 MiB per source file in caps walk */

/* ── Iteration / line caps ─────────────────────────────────────────── */

#define HL_AGENT_LOGS_DEFAULT_TAIL    100
#define HL_AGENT_LOGS_MAX_TAIL        10000

/* Max recursion depth for source-walk in capabilities analysis. */
#define HL_AGENT_WALK_MAX_DEPTH       8

/* Initial / max NL-offset buffers for log tailing. */
#define HL_AGENT_NL_CAP_INITIAL       1024

/* Aggregate buffer growth - initial size; doubles thereafter. */
#define HL_AGENT_AGG_INITIAL_CAP      (64 * 1024)

/* sh_arena slack: arena size = input_size * factor + slack. The slack
 * covers fixed per-arena bookkeeping that grows sub-linearly. */
#define HL_AGENT_ARENA_SLOPPY_FACTOR  4
#define HL_AGENT_ARENA_SLOPPY_SLACK   4096

/* Snippet-builder buffer: holds a small Lua/JS wrapper to invoke a
 * stdlib function (e.g. template.render). Bounded because the snippet
 * is template-driven, not user-supplied. */
#define HL_AGENT_SNIPPET_BUF          512

/* Stringified BLOB cell, e.g. "<blob:1234>". */
#define HL_AGENT_BLOB_LABEL           64

/* Eval wrapper headroom past the user-supplied code length. */
#define HL_AGENT_EVAL_WRAP_HEADROOM   64

#endif /* HL_AGENT_LIMITS_H */

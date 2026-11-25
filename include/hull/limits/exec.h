/*
 * hull/limits/exec.h - limits of the manifest's `exec` section and of one
 * exec.run call (docs/design/exec_broker.md §3, §4, §5.2).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_EXEC_H
#define HL_LIMITS_EXEC_H

/* The policy (one encoded `exec` section). */
#define HL_EXEC_MAX_COMMANDS        32
#define HL_EXEC_NAME_MAX            64          /* [a-z0-9][a-z0-9-]{0,63} */
#define HL_EXEC_PATH_MAX            4096
#define HL_EXEC_MAX_ARGV            64          /* fixed entries + slots */
#define HL_EXEC_MAX_FIXED_BYTES     16384       /* all fixed argv text of a command */
#define HL_EXEC_MAX_ENV             16
#define HL_EXEC_ENV_VALUE_MAX       4096
#define HL_EXEC_MAX_FS              16          /* fs.read, and fs.write, each */
#define HL_EXEC_MAX_PROTECT         16          /* database files a write grant may not reach */
#define HL_EXEC_MAX_ENUM            64
#define HL_EXEC_POLICY_MAX_BYTES    (1u << 20)  /* the whole encoding */
#define HL_EXEC_PIN_MAX_BYTES       (512u * 1024u * 1024u)  /* a pinned binary is hashed whole */

/* A slot. */
#define HL_EXEC_SLOT_DEFAULT_MIN    1
#define HL_EXEC_SLOT_DEFAULT_MAX    256
#define HL_EXEC_SLOT_LEN_CEIL       4096
#define HL_EXEC_MAX_REPEAT          64

/* A command's limits: default, floor and ceiling. */
#define HL_EXEC_TIMEOUT_DEFAULT_MS  5000
#define HL_EXEC_TIMEOUT_MIN_MS      100
#define HL_EXEC_TIMEOUT_MAX_MS      60000
#define HL_EXEC_STDOUT_DEFAULT      (64u * 1024u)
#define HL_EXEC_STDOUT_CEIL         (16u * 1024u * 1024u)
#define HL_EXEC_STDERR_DEFAULT      (16u * 1024u)
#define HL_EXEC_STDERR_CEIL         (1024u * 1024u)
#define HL_EXEC_CONCURRENCY_DEFAULT 2
#define HL_EXEC_CONCURRENCY_MAX     8
/* Bytes a call may write to the command's stdin: none unless the command
 * declares stdin_max (1..ceiling). */
#define HL_EXEC_STDIN_CEIL          (1024u * 1024u)

/* One call: every slot value of it together. */
#define HL_EXEC_MAX_ARG_BYTES       (64u * 1024u)
/* The arguments a call may pass (every slot, a repeat slot at its max). */
#define HL_EXEC_MAX_CALL_ARGS       (HL_EXEC_MAX_ARGV + HL_EXEC_MAX_REPEAT)

/* Admission, held by the broker (PR 2) and mirrored by the binding. */
#define HL_EXEC_MAX_INFLIGHT        8
#define HL_EXEC_MAX_BUFFERED        (32u * 1024u * 1024u)

#endif /* HL_LIMITS_EXEC_H */

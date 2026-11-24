/*
 * hull/cap/exec.h - the declared-command policy (`exec` manifest section)
 * and the one argument matcher (docs/design/exec_broker.md).
 *
 * The policy is ONE byte encoding, used everywhere it travels:
 *   - built by both manifest extractors through the hl_exec_b_* builder,
 *     straight into the manifest's own buffer (so a Lua raise part-way is
 *     freed with the manifest);
 *   - checked once, in full, by hl_exec_policy_validate - every semantic
 *     rule of §3 / §4 lives there, so the extractors and (PR 2) the broker
 *     read the same bytes the same way;
 *   - sealed with the manifest as one memdup (position independent: no
 *     pointers inside);
 *   - read in place by hl_exec_policy_find / hl_exec_check_call, which
 *     assume a policy hl_exec_policy_validate accepted.
 *
 * In PR 1 nothing executes a command: exec.run answers only from the stubs
 * a `hull test` registers, after the full match. This header is pure C with
 * no Hull dependencies beyond the allocator, so the fuzzers link it alone.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_EXEC_H
#define HL_CAP_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "hull/limits/exec.h"

typedef struct HlAllocator HlAllocator;

/* The encoding, built in place. `data` is allocator-owned (NULL = none).
 * `failed` is sticky: the first builder error is kept in `err`. */
typedef struct HlExecBuf {
    uint8_t     *data;
    size_t       len;
    size_t       cap;
    HlAllocator *alloc;
    uint16_t     ncmds;
    int          failed;
    char         err[192];
} HlExecBuf;

/* ── Builder (the manifest extractors) ─────────────────────────────── */

/* Every integer argument takes -1 for "not given" (the default applies).
 * A value out of range is kept out of range, so validation names it. */

void   hl_exec_b_init(HlExecBuf *b, HlAllocator *alloc);
/* Record a builder error (the extractor's: an unknown key, a wrong type).
 * The first one wins. printf-style. */
void   hl_exec_b_fail(HlExecBuf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void   hl_exec_b_cmd(HlExecBuf *b, const char *name, size_t name_len,
                     const char *path, size_t path_len,
                     int64_t timeout_ms, int64_t stdout_max, int64_t stderr_max,
                     int64_t concurrency, int unix_sockets);
/* A count of what follows (env pairs, fs paths, argv entries, enum values),
 * written as a placeholder and filled in by hl_exec_b_count_end. */
size_t hl_exec_b_count_begin(HlExecBuf *b);
void   hl_exec_b_count_end(HlExecBuf *b, size_t mark, size_t n);
/* One string: an env name or value, an fs path, an enum value. */
void   hl_exec_b_str(HlExecBuf *b, const char *s, size_t n);
/* One fixed argv entry. */
void   hl_exec_b_fixed(HlExecBuf *b, const char *s, size_t n);
/* One slot; its enum values follow (count_begin, str..., count_end).
 * `chars` NULL = the default class; otherwise a class spec (§4). */
void   hl_exec_b_slot(HlExecBuf *b, int allow_dash,
                      int64_t min, int64_t max,
                      int repeat, int64_t repeat_min, int64_t repeat_max,
                      const char *prefix, size_t prefix_len,
                      const char *suffix, size_t suffix_len,
                      const char *chars, size_t chars_len);
/* Finish: write the command count and validate the whole encoding.
 * 0 = a valid policy in b->data; -1 = b->err says why (b->data freed). */
int    hl_exec_b_finish(HlExecBuf *b);
/* Free b->data (safe on a zeroed or failed buffer). */
void   hl_exec_buf_free(HlExecBuf *b);

/* ── Validation (untrusted bytes) ──────────────────────────────────── */

/* 0 when @p data is a well-formed policy that obeys every rule of §3 / §4;
 * -1 otherwise, with a reason in @p err. Never reads past @p len. */
int hl_exec_policy_validate(const uint8_t *data, size_t len,
                            char *err, size_t err_size);

/* ── Reading a validated policy ────────────────────────────────────── */

#define HL_EXEC_CMD_UNIX_SOCKETS 0x01u

#define HL_EXEC_SLOT_ALLOW_DASH  0x01u
#define HL_EXEC_SLOT_HAS_CHARS   0x02u
#define HL_EXEC_SLOT_REPEAT      0x04u

/* One declared command, pointing into the policy. Strings are
 * NUL-terminated in place. The list pointers are opaque cursors for the
 * iterators below. */
typedef struct HlExecCmd {
    const char    *name;
    const char    *path;
    uint32_t       timeout_ms;
    uint32_t       stdout_max;
    uint32_t       stderr_max;
    uint8_t        concurrency;
    uint8_t        flags;            /* HL_EXEC_CMD_* */
    uint16_t       n_env;
    uint16_t       n_fs_read;
    uint16_t       n_fs_write;
    uint16_t       n_argv;
    const uint8_t *env;              /* n_env (name, value) pairs */
    const uint8_t *fs_read;
    const uint8_t *fs_write;
    const uint8_t *argv;
    const uint8_t *end;              /* the policy's end: bounds hl_exec_next_arg */
} HlExecCmd;

/* One slot of a command's argv. */
typedef struct HlExecSlot {
    uint8_t        flags;            /* HL_EXEC_SLOT_* */
    uint16_t       min, max;         /* byte length of the whole value */
    uint8_t        repeat_min, repeat_max;   /* 1 / 1 without REPEAT */
    const char    *prefix;  size_t prefix_len;
    const char    *suffix;  size_t suffix_len;
    const uint8_t *chars;            /* 32-byte bitmap, NULL = default class */
    uint16_t       n_enum;
    const uint8_t *enums;            /* n_enum strings */
} HlExecSlot;

/* One argv entry: a fixed string, or a slot. */
typedef struct HlExecArg {
    int            is_slot;
    const char    *fixed;            /* !is_slot */
    size_t         fixed_len;
    HlExecSlot     slot;             /* is_slot */
} HlExecArg;

/* Everything below reads a policy hl_exec_policy_validate accepted; on any
 * other bytes the result is undefined. Number of commands, or -1 when the
 * header is not a policy's. */
int hl_exec_policy_count(const uint8_t *data, size_t len);
/* The @p i th command (0-based). 0 / -1. */
int hl_exec_policy_at(const uint8_t *data, size_t len, int i, HlExecCmd *out);
/* The command named @p name. 0 / -1 (no such command). */
int hl_exec_policy_find(const uint8_t *data, size_t len,
                        const char *name, size_t name_len, HlExecCmd *out);

/* Iterate a string list (env pairs come as name, value, name, value...).
 * *cursor starts at the list pointer; returns the string or NULL at the
 * end of the @p n strings already read. */
const char *hl_exec_next_str(const uint8_t **cursor, size_t *len_out);
/* Iterate argv: fills @p out and advances, reading no further than @p end
 * (the command's `end`); call exactly n_argv times. */
void hl_exec_next_arg(const uint8_t **cursor, const uint8_t *end, HlExecArg *out);

/* ── Matching ──────────────────────────────────────────────────────── */

/* THE slot matcher (INV-17, PR 2): the binding and the broker both call it.
 * 0 when @p v (n bytes) is a value @p slot admits; -1 with the rule it
 * broke in @p why. */
int hl_exec_slot_match(const HlExecSlot *slot, const char *v, size_t n,
                       char *why, size_t why_size);

/* Result of hl_exec_check_call. */
#define HL_EXEC_OK          0
#define HL_EXEC_E_UNKNOWN  -1   /* no command of that name */
#define HL_EXEC_E_ARGS     -2   /* the arguments do not fit its slots */

/* Check one call: @p name must be declared and @p args (n values, lengths
 * in @p lens) must fill its slots in order - one value per slot, M..N for
 * the repeat slot - each admitted by hl_exec_slot_match, all together
 * under HL_EXEC_MAX_ARG_BYTES. @p cmd_out (may be NULL) gets the command. */
int hl_exec_check_call(const uint8_t *policy, size_t policy_len,
                       const char *name, size_t name_len,
                       const char *const *args, const size_t *lens, size_t n,
                       HlExecCmd *cmd_out, char *err, size_t err_size);

/* Is @p name (n bytes) a valid command name? 1 / 0. */
int hl_exec_name_valid(const char *name, size_t n);

#endif /* HL_CAP_EXEC_H */

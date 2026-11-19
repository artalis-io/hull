/*
 * type_check.h - running the TypeScript compiler the app or the user
 * installed (`hull check --types`, `hull dev`).
 *
 * Hull ships no compiler and depends on as little of one as it can: the
 * compiler's path (the app's node_modules/.bin, tsgo before tsc, else tsgo,
 * then tsc, on PATH), its arguments (`--noEmit -p DIR`, and `--pretty false`
 * where Hull reads the output), its exit status, and the diagnostic line
 * both tsc and tsgo print - `FILE(LINE,COL): error TSNNNN: MESSAGE`. A line
 * of any other shape is kept as text, never dropped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_TYPE_CHECK_H
#define HL_SHARED_TYPE_CHECK_H

#include <stddef.h>

/* 1 when <app_dir>/tsconfig.json exists (`hull types` writes it). */
int hl_type_check_configured(const char *app_dir);

/* The compiler to run for the app at @p app_dir: 0 with its path in @p out,
 * -1 when none is installed. */
int hl_type_check_find(const char *app_dir, char *out, size_t outsz);

/* One diagnostic. `parsed` is 0 for a line of another shape, kept whole in
 * `message` (file empty, line / col 0). */
typedef struct {
    char file[512];
    long line, col;
    char severity[16];      /* "error", "warning", ... */
    char code[16];          /* "TS2322" */
    char message[1024];
    int  parsed;
} HlTypeDiag;

/* Parse one output line (no newline). 1 with @p d filled for a non-empty
 * line, 0 for an empty one. Fields are cut, never overrun. */
int hl_type_check_parse_line(const char *line, size_t n, HlTypeDiag *d);

#endif /* HL_SHARED_TYPE_CHECK_H */

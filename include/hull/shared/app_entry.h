/*
 * app_entry.h - a TypeScript app entry (docs/typescript_design.md §4.3).
 *
 * An app's entry is `app.lua`, `app.js`, `app.ts` or
 * `app.mts`: a TypeScript entry runs on the JavaScript runtime, its erased
 * bytes coming from the pipeline generation (shared/generation.h). Every
 * place that looks for an app's entry asks hl_app_entry_ts first, so a
 * TypeScript entry beside another entry is refused the same way everywhere.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_APP_ENTRY_H
#define HL_SHARED_APP_ENTRY_H

#include <stddef.h>

#include "hull/entry.h"

/* The TypeScript entry of the app at `app_dir` ("." when NULL or empty):
 * 1 with "<app_dir>/app.ts" (or app.mts) in `buf`; 0 when it has none; -1
 * with a message in `err` when one sits beside app.js, app.lua or the other
 * TypeScript name - which file is the entry is then the developer's to say. */
int hl_app_entry_ts(const char *app_dir, char *buf, size_t bufsz,
                    char *err, size_t errsz);

/* 1 when `name` is a TypeScript entry name that runs on the JavaScript
 * runtime: it ends in ".ts" or ".mts". */
int hl_app_entry_is_ts(const char *name);

/* 1 when `name` is any TypeScript source name (".ts", ".mts", ".cts",
 * ".tsx"): a module the JavaScript runtime may run only as the pipeline's
 * erased output, never as the bytes on disk. */
int hl_app_name_is_ts(const char *name);

/* The entry a built binary embeds, by its entry-point name ("app.js",
 * "app.ts", "app.mts", "app.lua"), or NULL when `entries` (a
 * NULL-name-terminated array, hl_app_entries) holds none: hull build embeds
 * a JS / TypeScript entry as "./app.js" / "./app.ts" / "./app.mts" and a Lua
 * one as "./app". */
const char *hl_app_entry_embedded(const HlEntry *entries);

/* The JavaScript runtime's test file globs, NULL-terminated: its own and
 * the TypeScript names (HlRuntimeVtable.test_file_patterns, hull test). */
extern const char *const hl_js_test_file_patterns[];

#endif /* HL_SHARED_APP_ENTRY_H */

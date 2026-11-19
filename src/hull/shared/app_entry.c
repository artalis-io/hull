/*
 * app_entry.c - a TypeScript app entry (shared/app_entry.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/app_entry.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

const char *const hl_js_test_file_patterns[] = {
    "test_*.js", "test_*.ts", "test_*.mts", NULL
};

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n > k && strcmp(s + n - k, suffix) == 0;
}

int hl_app_entry_is_ts(const char *name)
{
    return name && (ends_with(name, ".ts") || ends_with(name, ".mts"));
}

int hl_app_name_is_ts(const char *name)
{
    return name && (hl_app_entry_is_ts(name) || ends_with(name, ".cts") ||
                    ends_with(name, ".tsx"));
}

/* "<dir>/<file>" into buf; 1 when it names a file that exists. */
static int present(const char *dir, size_t dlen, const char *file,
                   char *buf, size_t bufsz)
{
    struct stat st;
    int n = snprintf(buf, bufsz, "%.*s/%s", (int)dlen, dir, file);
    return n > 0 && (size_t)n < bufsz && stat(buf, &st) == 0;
}

int hl_app_entry_ts(const char *app_dir, char *buf, size_t bufsz,
                    char *err, size_t errsz)
{
    static const char *const ts_names[] = { "app.ts", "app.mts" };
    static const char *const others[] = { "app.js", "app.lua" };
    const char *dir = app_dir && app_dir[0] ? app_dir : ".";
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen - 1] == '/')
        dlen--;
    char probe[4096];
    const char *found = NULL;
    for (size_t i = 0; i < sizeof ts_names / sizeof *ts_names; i++) {
        if (!present(dir, dlen, ts_names[i], probe, sizeof probe))
            continue;
        if (found) {
            if (err && errsz)
                snprintf(err, errsz, "%.*s has both %s and %s: keep one entry",
                         (int)dlen, dir, found, ts_names[i]);
            return -1;
        }
        found = ts_names[i];
    }
    if (!found)
        return 0;
    for (size_t i = 0; i < sizeof others / sizeof *others; i++) {
        if (present(dir, dlen, others[i], probe, sizeof probe)) {
            if (err && errsz)
                snprintf(err, errsz, "%.*s has both %s and %s: which one is "
                         "the app's entry is ambiguous, keep one",
                         (int)dlen, dir, found, others[i]);
            return -1;
        }
    }
    int n = snprintf(buf, bufsz, "%.*s/%s", (int)dlen, dir, found);
    if (n < 0 || (size_t)n >= bufsz) {
        if (err && errsz)
            snprintf(err, errsz, "the app directory path is too long");
        return -1;
    }
    return 1;
}

const char *hl_app_entry_embedded(const HlEntry *entries)
{
    static const struct { const char *key, *name; } map[] = {
        { "./app.js", "app.js" }, { "./app.ts", "app.ts" },
        { "./app.mts", "app.mts" }, { "./app", "app.lua" },
    };
    for (int i = 0; entries && entries[i].name; i++)
        for (size_t k = 0; k < sizeof map / sizeof *map; k++)
            if (strcmp(entries[i].name, map[k].key) == 0)
                return map[k].name;
    return NULL;
}

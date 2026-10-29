/*
 * cap/db_live_files.c - the database files SQLite holds open (audit 33)
 *
 * See include/hull/cap/db_live_files.h. Built into the base whatever the
 * database flags (the fs capability asks it); with no SQLite backend nothing
 * registers and every match is 0.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db_live_files.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *path;
    int   refs;
} LiveFile;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static LiveFile       *g_files;
static int             g_count;
static int             g_cap;

int hl_db_live_file_add(const char *path)
{
    if (!path || !path[0]) return 0;
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_files[i].path, path) == 0) {
            g_files[i].refs++;
            goto out;
        }
    }
    if (g_count == g_cap) {
        int ncap = g_cap ? g_cap * 2 : 8;
        LiveFile *n = realloc(g_files, (size_t)ncap * sizeof(*n));
        if (!n) { rc = -1; goto out; }
        g_files = n;
        g_cap = ncap;
    }
    char *copy = strdup(path);
    if (!copy) { rc = -1; goto out; }
    g_files[g_count].path = copy;
    g_files[g_count].refs = 1;
    g_count++;
out:
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void hl_db_live_file_remove(const char *path)
{
    if (!path || !path[0]) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_files[i].path, path) != 0) continue;
        if (--g_files[i].refs == 0) {
            free(g_files[i].path);
            g_files[i] = g_files[g_count - 1];
            g_count--;
        }
        break;
    }
    pthread_mutex_unlock(&g_mu);
}

static int same_file(const char *path, const struct stat *st)
{
    struct stat s;
    return stat(path, &s) == 0 && s.st_dev == st->st_dev &&
           s.st_ino == st->st_ino;
}

void hl_db_live_files_lock(void)   { pthread_mutex_lock(&g_mu); }
void hl_db_live_files_unlock(void) { pthread_mutex_unlock(&g_mu); }

int hl_db_live_file_match_locked(const struct stat *st)
{
    static const char *const sfx[] = { "", "-wal", "-shm", "-journal" };
    if (!st) return 0;
    for (int i = 0; i < g_count; i++) {
        for (size_t k = 0; k < sizeof sfx / sizeof sfx[0]; k++) {
            char p[4096];
            int n = snprintf(p, sizeof p, "%s%s", g_files[i].path, sfx[k]);
            if (n > 0 && (size_t)n < sizeof p && same_file(p, st))
                return 1;
        }
    }
    return 0;
}

int hl_db_live_file_match(const struct stat *st)
{
    pthread_mutex_lock(&g_mu);
    int hit = hl_db_live_file_match_locked(st);
    pthread_mutex_unlock(&g_mu);
    return hit;
}

static int ends_with(const char *s, size_t n, const char *suffix)
{
    size_t k = strlen(suffix);
    /* Without case: a case-insensitive filesystem (macOS, Windows) opens
     * "APP.DB-SHM" as the same file. */
    return n >= k && strncasecmp(s + n - k, suffix, k) == 0;
}

int hl_db_sidecar_name(const char *path)
{
    if (!path) return 0;
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    size_t n = strlen(leaf);
    return ends_with(leaf, n, "-wal") || ends_with(leaf, n, "-shm") ||
           ends_with(leaf, n, "-journal");
}

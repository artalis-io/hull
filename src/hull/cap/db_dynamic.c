/*
 * cap/db_dynamic.c: db.open(dsn) validation + caller-owned open. See the header.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_dynamic.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/manifest.h"
#include "hull/host_match.h"
#include "hull/shared/fs_util.h"   /* hl_default_db_path_check: the leaf */
#include "hull/cap/fs_resolve.h"   /* hl_fs_charge (audit 39) */
#include "hull/shared/work_charge.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>   /* lstat: hl_db_path_has_symlink */

/* Process-wide cap on concurrent dynamic connections (fd-exhaustion backstop).
 * db.open / close run on the event-loop thread, so the handle table needs no
 * lock. Each open handle also has an id, published in g_dynamic_id for the
 * db.async workers (hl_db_dynamic_id_live); 0 marks a free slot. */
#define HL_DB_DYNAMIC_MAX 16
static HlDbHandle      *g_dynamic[HL_DB_DYNAMIC_MAX];
static _Atomic uint64_t g_dynamic_id[HL_DB_DYNAMIC_MAX];
static uint64_t         g_dynamic_next_id;

int hl_db_dynamic_open_count(void)
{
    int n = 0;
    for (int i = 0; i < HL_DB_DYNAMIC_MAX; i++)
        if (g_dynamic[i]) n++;
    return n;
}

static int dynamic_slot(const HlDbHandle *h)
{
    for (int i = 0; h && i < HL_DB_DYNAMIC_MAX; i++)
        if (g_dynamic[i] == h) return i;
    return -1;
}

uint64_t hl_db_dynamic_id(const HlDbHandle *h)
{
    int i = dynamic_slot(h);
    return i < 0 ? 0 : atomic_load(&g_dynamic_id[i]);
}

int hl_db_dynamic_id_live(uint64_t id)
{
    if (id == 0) return 0;
    for (int i = 0; i < HL_DB_DYNAMIC_MAX; i++)
        if (atomic_load(&g_dynamic_id[i]) == id) return 1;
    return 0;
}

/* An app may keep a db.open handle at module level and use it from every
 * request, so it is shared exactly like a registry connection: the same
 * stale-transaction guard and wait refusal apply (audit 6 L7). */
void hl_db_dynamic_guard_stale_txns(void)
{
    for (int i = 0; i < HL_DB_DYNAMIC_MAX; i++)
        if (g_dynamic[i]) hl_db_guard_stale_txn(g_dynamic[i]);
}

int hl_db_dynamic_in_txn(void)
{
    for (int i = 0; i < HL_DB_DYNAMIC_MAX; i++)
        if (g_dynamic[i] && hl_db_holds_txn(g_dynamic[i])) return 1;
    return 0;
}

/* Lowercased scheme (the text before "://") into @p buf; "" when the DSN has no
 * "://" (a bare path or ":memory:"). */
static void dsn_scheme(const char *dsn, char *buf, size_t bufsz)
{
    buf[0] = '\0';
    const char *sep = strstr(dsn, "://");
    if (!sep) return;
    size_t n = (size_t)(sep - dsn);
    if (n == 0 || n >= bufsz) return;
    for (size_t i = 0; i < n; i++) {
        char c = dsn[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    buf[n] = '\0';
}

/* File-based backends key off the local filesystem, so they are gated by the fs
 * allowlist, not the host allowlist. A scheme-less DSN implies SQLite. */
static int scheme_is_file(const char *scheme)
{
    return scheme[0] == '\0' ||
           strcmp(scheme, "sqlite") == 0 ||
           strcmp(scheme, "file")   == 0 ||
           strcmp(scheme, "duckdb") == 0;
}

/* Extract the host from a network DSN authority
 * (scheme://[user[:pass]@]host[:port]/...). IPv6 literals "[::1]" are returned
 * without the brackets (so host_match's inet_pton sees a bare address). Returns
 * 1 + host in @p buf, or 0 if no host is present. */
/* Match @p host against the policy's host patterns (exact / glob / CIDR),
 * resolving "$VAR" entries from the environment. Same matcher http/smtp use. */
static int host_allowed(const HlManifestDbDynamic *policy, const char *host)
{
    return hl_host_match_any_env(policy->hosts, policy->host_count, host);
}

/* 1 when any component of @p abs below its first @p base_len bytes (the app
 * directory) is a symbolic link, or the leaf or one of its SQLite sidecars
 * (-wal / -shm / -journal) is a link or not a regular file. hl_cap_fs_validate
 * only checks that the realpath stays inside the app directory and the fs.write
 * policy is matched lexically, but SQLite opens the absolute path FOLLOWING
 * links, so a planted `data/x.db -> ../private/ref.db` opened a file fs.write
 * would have re-rooted or refused (audit 29). A component that does not exist
 * yet ends the walk: SQLite creates it. */
int hl_db_path_has_symlink(const char *abs, size_t base_len)
{
    char prefix[HL_DB_DYNAMIC_DSN_MAX];
    size_t len = strlen(abs);
    if (len >= sizeof prefix || base_len >= len) return 1;
    memcpy(prefix, abs, len + 1);
    uint64_t comps = 0;
    for (size_t i = 0; i <= base_len; i++)
        if (prefix[i] == '/') comps++;
    for (size_t i = base_len + 1; i < len; i++) {
        if (prefix[i] != '/') continue;
        /* Each lstat walks the whole prefix: charged before it to the
         * thread's charge scope (audit 39); a trip refuses the path. */
        if (hl_fs_charge(HL_WORK_FS_LOOKUP_UNITS +
                         ++comps * HL_WORK_FS_NAME_UNITS))
            return 1;
        prefix[i] = '\0';
        struct stat st;
        int r = lstat(prefix, &st);
        prefix[i] = '/';
        if (r != 0) return 0;              /* the rest does not exist yet */
        if (S_ISLNK(st.st_mode)) return 1;
    }
    return hl_default_db_path_check(abs, NULL, 0) != 0;
}

HlDbHandle *hl_db_dynamic_open(const char *dsn,
                               const HlManifestDbDynamic *policy,
                               const HlFsConfig *fs_cfg,
                               const char **err)
{
    return hl_db_dynamic_open_ex(dsn, policy, fs_cfg, NULL, 0, err);
}

HlDbHandle *hl_db_dynamic_open_ex(const char *dsn,
                                  const HlManifestDbDynamic *policy,
                                  const HlFsConfig *fs_cfg,
                                  char *opened, size_t opened_size,
                                  const char **err)
{
    if (err) *err = NULL;
    if (opened && opened_size) opened[0] = '\0';
    /* The DSN the backend opens: the app's, or (file backends) the same file
     * named by its absolute path under the app directory. */
    char abs_dsn[HL_DB_DYNAMIC_DSN_MAX];
    const char *open_dsn = dsn;
    if (!dsn || !dsn[0]) {
        if (err) *err = "db.open: empty DSN";
        return NULL;
    }
    if (!policy || !policy->declared) {
        if (err) *err = "db.open requires a databases.dynamic policy in the manifest";
        return NULL;
    }
    int slot = -1;
    for (int i = 0; slot < 0 && i < HL_DB_DYNAMIC_MAX; i++)
        if (!g_dynamic[i]) slot = i;
    if (slot < 0) {
        if (err) *err = "db.open: too many open dynamic connections (close some first)";
        return NULL;
    }

    char scheme[24];
    dsn_scheme(dsn, scheme, sizeof scheme);

    /* Scheme must be allowlisted. A bare path (no scheme) implies "sqlite". */
    const char *want = scheme[0] ? scheme : "sqlite";
    int scheme_ok = 0;
    for (int i = 0; i < policy->scheme_count; i++)
        if (strcmp(policy->schemes[i], want) == 0) { scheme_ok = 1; break; }
    if (!scheme_ok) {
        if (err) *err = "db.open: DSN scheme not in databases.dynamic.schemes";
        return NULL;
    }

    /* Backend must be compiled + selectable for this DSN. */
    const char *sel_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(dsn, &sel_err);
    if (!be) {
        if (err) *err = sel_err ? sel_err : "db.open: no backend for DSN";
        return NULL;
    }

    if (scheme_is_file(scheme)) {
        /* File backend: reduce the DSN to a filesystem path by stripping the
         * "<scheme>://" prefix (sqlite://, duckdb://, ...) so fs-sandbox
         * validation sees the bare path. A scheme-less DSN passes through
         * unchanged. ":memory:" (possibly after stripping) opens no file. */
        const char *path = dsn;
        if (scheme[0]) {
            size_t slen = strlen(scheme);
            if (strncmp(dsn + slen, "://", 3) == 0) path = dsn + slen + 3;
        }
        if (strcmp(path, ":memory:") != 0) {
            if (!fs_cfg) {
                if (err) *err = "db.open: a file DSN needs a filesystem-scoped app";
                return NULL;
            }
            const char *fe = NULL;
            if (hl_cap_fs_validate(fs_cfg, path, &fe) != 0) {
                if (err) *err = fe ? fe : "db.open: file path not allowed by manifest.fs";
                return NULL;
            }
            /* Containment is not authorization. SQLite opens the file read-
             * write and creates it (and its -wal / -journal siblings) if it is
             * missing, so the path needs an fs.write grant, the same one
             * fs.write would. Without it any SQLite file under the app dir
             * could be read or created with no fs grant at all. */
            char scratch[HL_DB_DYNAMIC_DSN_MAX];
            HlFsSelection sel = hl_fs_policy_select(fs_cfg->policy, path,
                                                    HL_FS_OPEN_WRITE,
                                                    scratch, sizeof scratch);
            if (!fs_cfg->policy || !sel.entry) {
                if (err) *err = "db.open: a file DSN needs an fs.write grant for its path";
                return NULL;
            }
            /* Open it where it was checked. The backend resolves a relative
             * path against the process's working directory, which is not the
             * app directory when hull runs an app elsewhere - the check and the
             * open would name two different files. */
            int n = scheme[0]
                ? snprintf(abs_dsn, sizeof abs_dsn, "%s://%s/%s", scheme,
                           fs_cfg->base_dir, path)
                : snprintf(abs_dsn, sizeof abs_dsn, "%s/%s", fs_cfg->base_dir, path);
            if (n < 0 || (size_t)n >= sizeof abs_dsn) {
                if (err) *err = "db.open: file path too long";
                return NULL;
            }
            /* No symbolic link on the way to the file, nor at it (audit 29). */
            const char *abs_path = abs_dsn + (scheme[0] ? strlen(scheme) + 3 : 0);
            if (hl_db_path_has_symlink(abs_path, strlen(fs_cfg->base_dir))) {
                if (err) *err = "db.open: the file path runs through a symbolic "
                                "link, or names something that is not a "
                                "regular file";
                return NULL;
            }
            open_dsn = abs_dsn;
        }
    } else {
        /* Network backend: host must match databases.dynamic.hosts. */
        char host[256];
        if (!hl_dsn_host(dsn, host, sizeof host)) {
            if (err) *err = "db.open: DSN has no host, or one a backend could parse differently";
            return NULL;
        }
        if (!host_allowed(policy, host)) {
            if (err) *err = "db.open: host not allowed by databases.dynamic.hosts";
            return NULL;
        }
    }

    /* The open's fixed work, charged before it to the thread's charge scope
     * (audit 39): a file database is sqlite3_open, the PRAGMAs and the WAL
     * setup, and its close a checkpoint and the sidecar unlinks - ~1.3 ms
     * an open + close, which a db.open / close loop got for the ~2400
     * units SQLite's allocations charge (106 s per budget). */
    if (hl_fs_charge(open_dsn == abs_dsn ? HL_WORK_DB_OPEN_FILE_UNITS
                                         : HL_WORK_DB_OPEN_UNITS)) {
        if (err) *err = "budget_exhausted";
        return NULL;
    }
    HlDbHandle *h = calloc(1, sizeof *h);
    if (!h) {
        if (err) *err = "db.open: out of memory";
        return NULL;
    }
    h->backend = be;
    if (be->open(&h->ctx, open_dsn, NULL) != 0) {
        if (err) *err = "db.open: connection failed";
        free(h);
        return NULL;
    }
    g_dynamic[slot] = h;
    atomic_store(&g_dynamic_id[slot], ++g_dynamic_next_id);
    if (opened && opened_size) {
        if (strlen(open_dsn) >= opened_size) {
            /* The caller could not name this database again; do not hand it
             * a connection whose async twin would open something else. */
            hl_db_dynamic_close(h);
            if (err) *err = "db.open: DSN too long";
            return NULL;
        }
        memcpy(opened, open_dsn, strlen(open_dsn) + 1);
    }
    return h;
}

void hl_db_dynamic_close(HlDbHandle *h)
{
    if (!h) return;
    int i = dynamic_slot(h);
    if (i >= 0) {
        g_dynamic[i] = NULL;
        atomic_store(&g_dynamic_id[i], 0);
    }
    /* Closing a handle inside a transaction ends that transaction: the
     * generation changes, as for every other end (audit 37). Noted after
     * the close with the backend cleared, so hl_db_in_txn reads 0 instead
     * of asking a closed connection. */
    HlDbTxnSnap was = hl_db_txn_snap(h);
    if (h->backend)
        h->backend->close(h);
    h->backend = NULL;
    hl_db_txn_note(h, was, NULL);
    free(h);
}

#endif /* HL_ENABLE_DB */

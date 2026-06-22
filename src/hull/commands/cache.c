/**
 * @file commands/cache.c
 * @brief `hull cache list|prune|clear` implementation.
 *
 * Manages the runtime cache pool (Lua bytecode, compute AOT,
 * template render functions) plus the tools store, via the
 * compile-time registry in `cache_registry.{c,h}`. All operations
 * route through `hl_blob_store_*` so the storage layer stays in
 * one place.
 *
 *   list                List every registered cache: path, entry
 *                       count, total size, runtime/system flag.
 *                       `--json` for machine output.
 *
 *   prune               Run blob_store cleanup over runtime caches.
 *                       Flags:
 *                         --kind=K       restrict to one cache
 *                         --max-size=N   evict until total ≤ N bytes
 *                         --max-age=N    evict entries older than N s
 *                         --strategy=lru|fifo  (default: lru)
 *                         --dry-run      report without unlinking
 *
 *   clear [--kind=K] [--yes]
 *                       Delete every entry from the named cache
 *                       (all runtime caches if --kind omitted).
 *                       Requires --yes to skip interactive prompt.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/cache.h"
#include "hull/shared/blob_store.h"
#include "hull/shared/cache_dir.h"
#include "hull/shared/cache_registry.h"
#include "hull/cap/crypto.h"
#include "hull/runtime/cache_common.h"  /* hex_encode shared helper */
#include "sh_json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Human-readable byte counter. Identical formatter as `cmd_tools.c`
 * so cache output and tool list match. */
static void format_size(uint64_t bytes, char *out, size_t out_sz)
{
    if (bytes >= (1ULL << 30))
        snprintf(out, out_sz, "%.1f GB", bytes / (double)(1ULL << 30));
    else if (bytes >= (1ULL << 20))
        snprintf(out, out_sz, "%.1f MB", bytes / (double)(1ULL << 20));
    else if (bytes >= (1ULL << 10))
        snprintf(out, out_sz, "%.1f KB", bytes / (double)(1ULL << 10));
    else
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
}

/* Generic FILE* writer for ShJsonWriter: used by every JSON-emitting
 * command below. Matches the audit.c pattern but parameterises on the
 * FILE so the same helper serves both stdout (results) and stderr
 * (audit log). Failed writes flip the writer's error flag, which
 * downstream sh_json_write_* calls observe and turn into no-ops; we
 * report once at the end via sh_json_writer_error(). */
static int stdio_write_fn(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/* Parse a duration string with an optional unit suffix:
 *   "30"  → 30 seconds (bare number; legacy behaviour)
 *   "30s" → 30 seconds
 *   "5m"  → 300 seconds
 *   "2h"  → 7 200 seconds
 *   "7d"  → 604 800 seconds
 *   "2w"  → 1 209 600 seconds
 *   "1y"  → 31 536 000 seconds (365 days, calendar-agnostic)
 *
 * Returns the duration in seconds, or 0 + sets `*err` on failure.
 * 0 is also the "no bound" sentinel for cleanup opts, so callers
 * distinguish "unset" from "invalid" via the error pointer.
 *
 * Single unit only (no "1d12h" composites) — keeps the parser
 * trivial and the CLI predictable. */
static uint64_t parse_duration(const char *s, const char **err)
{
    *err = NULL;
    if (!s || !*s) { *err = "empty duration"; return 0; }

    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &endp, 10);
    if (errno || endp == s) { *err = "not a number"; return 0; }

    uint64_t mult;
    if (*endp == '\0' || *endp == 's')      mult = 1;
    else if (*endp == 'm' && endp[1] == '\0') mult = 60;
    else if (*endp == 'h' && endp[1] == '\0') mult = 3600;
    else if (*endp == 'd' && endp[1] == '\0') mult = 86400;
    else if (*endp == 'w' && endp[1] == '\0') mult = 604800;
    else if (*endp == 'y' && endp[1] == '\0') mult = 31536000ULL;
    else { *err = "bad unit (use s/m/h/d/w/y)"; return 0; }

    /* Overflow guard. */
    if (mult > 1 && v > UINT64_MAX / mult) { *err = "overflow"; return 0; }
    return (uint64_t)v * mult;
}

/* Parse a size string with an optional unit suffix (binary,
 * 1024-based, matching how `format_size` displays them):
 *   "1048576" → 1 048 576 bytes (bare number; legacy behaviour)
 *   "1024"    → 1 024 bytes
 *   "1K"      → 1 024 bytes
 *   "10M"     → 10 485 760 bytes
 *   "1G"      → 1 073 741 824 bytes
 *
 * Lowercase suffixes (`k`/`m`/`g`) accepted. Trailing `B` ignored
 * (so `10MB` is also valid). 0 + non-NULL `*err` on failure. */
static uint64_t parse_size(const char *s, const char **err)
{
    *err = NULL;
    if (!s || !*s) { *err = "empty size"; return 0; }

    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &endp, 10);
    if (errno || endp == s) { *err = "not a number"; return 0; }

    /* Strip an optional trailing 'B'/'b' so "10MB" and "10M" both
     * work. Check unit-letter first since we then look at what
     * follows for the B. */
    char unit = *endp;
    uint64_t mult;
    if (unit == '\0' || unit == 'B' || unit == 'b') mult = 1;
    else if (unit == 'K' || unit == 'k')            mult = 1024ULL;
    else if (unit == 'M' || unit == 'm')            mult = 1024ULL * 1024;
    else if (unit == 'G' || unit == 'g')            mult = 1024ULL * 1024 * 1024;
    else { *err = "bad unit (use K/M/G; B optional)"; return 0; }

    /* If we matched a multiplier, allow an optional trailing 'B'. */
    if (mult > 1) {
        endp++;
        if (*endp == 'B' || *endp == 'b') endp++;
    } else if (unit == 'B' || unit == 'b') {
        endp++;
    }
    if (*endp != '\0') { *err = "trailing garbage"; return 0; }

    if (mult > 1 && v > UINT64_MAX / mult) { *err = "overflow"; return 0; }
    return (uint64_t)v * mult;
}

/* Build the HULL_NO_<KIND>_CACHE env-var name for a registry entry.
 * Returns an empty string for system stores (env_kind == NULL — no
 * per-cache opt-out applies). Forwarding wrapper around the
 * canonical composer in cache_dir.h. */
static void env_var_for(const HlCacheKind *kind, char *out, size_t out_sz)
{
    if (!kind->env_kind || out_sz == 0) {
        if (out_sz > 0) out[0] = '\0';
        return;
    }
    if (hl_hull_cache_env_name(kind->env_kind, out, out_sz) != 0) {
        out[0] = '\0';
    }
}

/* Try to open the blob store for `kind`. Returns NULL silently if
 * the store doesn't exist on disk yet (a kind that has never been
 * populated). Caller is responsible for closing. */
static HlBlobStore *open_kind(const HlCacheKind *kind)
{
    char path[PATH_MAX];
    if (hl_cache_resolve_path(kind, path, sizeof(path)) != 0) return NULL;
    HlBlobStore *s = NULL;
    /* shard_depth=1 matches what all four consumers use. */
    if (hl_blob_store_open(&s, NULL, path, /*shard_depth=*/1, 0) != 0)
        return NULL;
    return s;
}

/* ── list ────────────────────────────────────────────────────────── */

static int cache_list_text(void)
{
    /* Status column shows the per-cache opt-out state:
     *   ok          — cache active
     *   off (env)   — HULL_NO_<KIND>_CACHE truthy in this process
     *   off (all)   — HULL_NO_CACHE truthy (overrides everything)
     *   n/a         — system store; no opt-out concept
     */
    int global_off = hl_hull_cache_disabled(NULL);

    fprintf(stdout,
        "Cache        Kind     Status     Path                                            "
        "Entries  Size\n"
        "──────────── ──────── ────────── "
        "─────────────────────────────────────────────── "
        "─────── ──────────\n");

    uint64_t runtime_count = 0, runtime_bytes = 0;
    uint64_t total_count = 0,   total_bytes = 0;

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        char path[PATH_MAX];
        if (hl_cache_resolve_path(k, path, sizeof(path)) != 0) {
            fprintf(stdout, "  %-10s %-8s  (path unavailable: %s)\n",
                    k->name, k->is_runtime ? "runtime" : "system",
                    strerror(errno));
            continue;
        }

        HlBlobStore *s = open_kind(k);
        uint64_t cnt = 0, size = 0;
        if (s) {
            cnt  = hl_blob_store_count(s);
            size = hl_blob_store_total_size(s);
            hl_blob_store_close(s);
        }

        const char *status;
        if (!k->env_kind) {
            status = "n/a";  /* system store — not a cache, no opt-out */
        } else if (global_off) {
            status = "off (all)";
        } else if (hl_hull_cache_disabled(k->env_kind)) {
            status = "off (env)";
        } else {
            status = "ok";
        }

        char size_str[32];
        format_size(size, size_str, sizeof(size_str));
        fprintf(stdout, "%-12s %-8s %-10s %-47s %7llu  %s\n",
                k->name,
                k->is_runtime ? "runtime" : "system",
                status,
                path,
                (unsigned long long)cnt, size_str);

        total_count += cnt; total_bytes += size;
        if (k->is_runtime) {
            runtime_count += cnt;
            runtime_bytes += size;
        }
    }

    char total_str[32], runtime_str[32];
    format_size(total_bytes, total_str, sizeof(total_str));
    format_size(runtime_bytes, runtime_str, sizeof(runtime_str));
    fprintf(stdout,
        "\nRuntime: %llu entries, %s   "
        "Total (incl system): %llu entries, %s\n",
        (unsigned long long)runtime_count, runtime_str,
        (unsigned long long)total_count,   total_str);

    /* Footer: surface the active opt-outs explicitly so users see
     * WHY a cache shows `off`. Only emit when something is set.
     * `need_leading_newline` is set only on the first emission
     * inside the loop; subsequent assignments would be dead stores
     * (scan-build flags those with deadcode.DeadStores). */
    if (global_off) {
        fprintf(stdout, "\nHULL_NO_CACHE=1 active - all runtime caches disabled.\n");
    } else {
        int need_leading_newline = 1;
        for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
            if (!k->env_kind) continue;
            if (!hl_hull_cache_disabled(k->env_kind)) continue;
            char env_name[64];
            env_var_for(k, env_name, sizeof(env_name));
            if (need_leading_newline) {
                fputc('\n', stdout);
                need_leading_newline = 0;
            }
            fprintf(stdout, "%s=1 active - %s cache disabled.\n",
                    env_name, k->name);
        }
    }

    const char *override = getenv("HULL_CACHE_DIR");
    if (override && *override) {
        fprintf(stdout, "\nHULL_CACHE_DIR override active: %s\n", override);
        fprintf(stdout,
            "(Runtime caches are rooted here for per-app isolation. "
            "System stores (tools) are unaffected.)\n");
    }
    return 0;
}

static int cache_list_json(void)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write_fn, stdout);
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "caches");
    sh_json_write_array_start(&w);

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        char path[PATH_MAX];
        int resolved = (hl_cache_resolve_path(k, path, sizeof(path)) == 0);

        HlBlobStore *s = resolved ? open_kind(k) : NULL;
        uint64_t cnt = 0, size = 0;
        if (s) {
            cnt  = hl_blob_store_count(s);
            size = hl_blob_store_total_size(s);
            hl_blob_store_close(s);
        }

        char env_name[64];
        env_var_for(k, env_name, sizeof(env_name));
        int disabled = (hl_hull_cache_disabled(NULL) != 0) ||
                       (k->env_kind &&
                        hl_hull_cache_disabled(k->env_kind) != 0);

        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name",        k->name);
        sh_json_write_kv_string(&w, "description", k->description);
        sh_json_write_kv_string(&w, "path",        resolved ? path : "");
        sh_json_write_kv_bool  (&w, "is_runtime",  k->is_runtime != 0);
        sh_json_write_kv_string(&w, "env_var",     env_name);
        sh_json_write_kv_bool  (&w, "disabled",    disabled != 0);
        sh_json_write_kv_int   (&w, "count",       (int64_t)cnt);
        sh_json_write_kv_int   (&w, "size_bytes",  (int64_t)size);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

    const char *override = getenv("HULL_CACHE_DIR");
    sh_json_write_kv_string(&w, "hull_cache_dir",
                            (override && *override) ? override : "");
    sh_json_write_object_end(&w);
    fputc('\n', stdout);
    return sh_json_writer_error(&w) ? 1 : 0;
}

static int cmd_list(int argc, char **argv)
{
    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) json = 1;
        else {
            fprintf(stderr, "hull cache list: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }
    return json ? cache_list_json() : cache_list_text();
}

/* ── prune ───────────────────────────────────────────────────────── */

static int cmd_prune(int argc, char **argv)
{
    const char *kind_name = NULL;
    uint64_t max_size = 0, max_age = 0;
    HlBlobStoreCleanupStrategy strategy = HL_BLOB_STORE_LRU;
    int dry_run = 0;
    int json    = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *parse_err = NULL;
        if (strcmp(a, "--json") == 0)             json = 1;
        else if (strncmp(a, "--kind=", 7) == 0)   kind_name = a + 7;
        else if (strncmp(a, "--max-size=", 11) == 0) {
            max_size = parse_size(a + 11, &parse_err);
            if (parse_err) {
                fprintf(stderr,
                    "hull cache prune: bad --max-size value '%s' (%s)\n"
                    "  examples: --max-size=100M  --max-size=2G  --max-size=1048576\n",
                    a + 11, parse_err);
                return 2;
            }
        }
        else if (strncmp(a, "--max-age=", 10) == 0) {
            max_age = parse_duration(a + 10, &parse_err);
            if (parse_err) {
                fprintf(stderr,
                    "hull cache prune: bad --max-age value '%s' (%s)\n"
                    "  examples: --max-age=30d  --max-age=24h  --max-age=2592000\n",
                    a + 10, parse_err);
                return 2;
            }
        }
        else if (strcmp(a, "--strategy=lru") == 0)  strategy = HL_BLOB_STORE_LRU;
        else if (strcmp(a, "--strategy=fifo") == 0) strategy = HL_BLOB_STORE_FIFO;
        else if (strcmp(a, "--dry-run") == 0)       dry_run = 1;
        else {
            fprintf(stderr, "hull cache prune: unknown flag '%s'\n", a);
            return 2;
        }
    }

    /* Need at least one bound — pruning with no max-size and no
     * max-age would be a no-op, which is confusing. Make the user
     * pass `clear` if they want to wipe everything. */
    if (max_size == 0 && max_age == 0) {
        fprintf(stderr,
            "hull cache prune: need --max-size=N or --max-age=N "
            "(use `hull cache clear` to wipe entirely)\n");
        return 2;
    }

    const HlCacheKind *only = NULL;
    if (kind_name) {
        only = hl_cache_find(kind_name);
        if (!only) {
            fprintf(stderr,
                "hull cache prune: unknown kind '%s' "
                "(see `hull cache list`)\n", kind_name);
            return 2;
        }
    }

    HlBlobStoreCleanupOpts opts = {
        .max_total_size = max_size,
        .max_age_sec    = max_age,
        .strategy       = strategy,
        .dry_run        = dry_run,
    };

    uint64_t total_removed = 0, total_freed = 0;
    int rc_overall = 0;

    ShJsonWriter w;
    if (json) {
        sh_json_writer_init(&w, stdio_write_fn, stdout);
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "results");
        sh_json_write_array_start(&w);
    }

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (only && k != only) continue;
        /* By default `prune` only touches runtime caches; system
         * stores (tools) are signed durable downloads and shouldn't
         * be swept opportunistically. Explicit --kind=tools bypass. */
        if (!only && !k->is_runtime) continue;

        HlBlobStore *s = open_kind(k);
        if (!s) {
            if (json) {
                sh_json_write_object_start(&w);
                sh_json_write_kv_string(&w, "name",         k->name);
                sh_json_write_kv_int   (&w, "removed",      0);
                sh_json_write_kv_int   (&w, "freed_bytes",  0);
                sh_json_write_kv_bool  (&w, "no_store",     true);
                sh_json_write_object_end(&w);
            } else {
                fprintf(stdout, "  %-12s (no store)\n", k->name);
            }
            continue;
        }

        uint64_t removed = 0, freed = 0;
        int rc = hl_blob_store_cleanup(s, &opts, &removed, &freed);
        hl_blob_store_close(s);
        if (rc != 0) rc_overall = 1;

        if (json) {
            sh_json_write_object_start(&w);
            sh_json_write_kv_string(&w, "name",        k->name);
            sh_json_write_kv_int   (&w, "removed",     (int64_t)removed);
            sh_json_write_kv_int   (&w, "freed_bytes", (int64_t)freed);
            sh_json_write_kv_int   (&w, "rc",          rc);
            sh_json_write_object_end(&w);
        } else {
            char size_str[32];
            format_size(freed, size_str, sizeof(size_str));
            fprintf(stdout, "  %-12s %s %llu entries, %s\n",
                    k->name,
                    dry_run ? "would remove" : "removed",
                    (unsigned long long)removed, size_str);
        }
        total_removed += removed;
        total_freed   += freed;
    }

    if (json) {
        sh_json_write_array_end(&w);
        sh_json_write_kv_int   (&w, "total_removed",     (int64_t)total_removed);
        sh_json_write_kv_int   (&w, "total_freed_bytes", (int64_t)total_freed);
        sh_json_write_kv_bool  (&w, "dry_run",           dry_run != 0);
        sh_json_write_object_end(&w);
        fputc('\n', stdout);
        if (sh_json_writer_error(&w)) rc_overall = 1;
    } else {
        char total_str[32];
        format_size(total_freed, total_str, sizeof(total_str));
        fprintf(stdout,
            "\nTotal %s: %llu entries, %s\n",
            dry_run ? "would-be removed" : "removed",
            (unsigned long long)total_removed, total_str);
    }
    return rc_overall;
}

/* ── clear ───────────────────────────────────────────────────────── */

/* Snapshot every id in the store so we can delete them in a second
 * pass. hl_blob_store_iter takes a callback; we accumulate ids into
 * a small grow buffer that we own. Snapshot semantics already, so
 * deleting during the second pass doesn't race with the walk. */

typedef struct {
    char    (*ids)[HL_BLOB_STORE_ID_BUF_SIZE];
    size_t    count;
    size_t    cap;
    uint64_t  bytes;
} ClearCollector;

static int collect_ids(const char *id, size_t size, void *user)
{
    ClearCollector *c = (ClearCollector *)user;
    if (c->count == c->cap) {
        size_t new_cap = c->cap ? c->cap * 2 : 64;
        void *grown = realloc(c->ids, new_cap * sizeof(c->ids[0]));
        if (!grown) return 1;  /* abort iter */
        c->ids = grown;
        c->cap = new_cap;
    }
    memcpy(c->ids[c->count], id, HL_BLOB_STORE_ID_BUF_SIZE);
    c->count++;
    c->bytes += size;
    return 0;
}

/* Returns: removed count via out_removed, bytes freed via
 * out_bytes, blob-store rc as the return value. Renders nothing —
 * caller picks human vs JSON output shape. */
static int clear_one(const HlCacheKind *k,
                     uint64_t *out_removed, uint64_t *out_bytes,
                     int *out_no_store)
{
    *out_removed = 0;
    *out_bytes   = 0;
    *out_no_store = 0;

    HlBlobStore *s = open_kind(k);
    if (!s) { *out_no_store = 1; return 0; }

    /* "Clear" means "remove every entry", not "evict by policy" —
     * so iter + delete rather than blob_store_cleanup. The
     * cleanup-based approach would miss files less than 1 second
     * old (max_age_sec=0 means "no age limit", and a smaller
     * value rejects fresh files). */
    ClearCollector c = { NULL, 0, 0, 0 };
    int rc = hl_blob_store_iter(s, collect_ids, &c);

    uint64_t removed = 0;
    for (size_t i = 0; i < c.count; i++) {
        if (hl_blob_store_delete(s, c.ids[i]) > 0) removed++;
    }
    free(c.ids);
    hl_blob_store_close(s);

    *out_removed = removed;
    *out_bytes   = c.bytes;
    return rc;
}

static int cmd_clear(int argc, char **argv)
{
    const char *kind_name = NULL;
    int yes  = 0;
    int json = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0)            json = 1;
        else if (strncmp(a, "--kind=", 7) == 0)  kind_name = a + 7;
        else if (strcmp(a, "--yes") == 0)        yes = 1;
        else {
            fprintf(stderr, "hull cache clear: unknown flag '%s'\n", a);
            return 2;
        }
    }

    const HlCacheKind *only = NULL;
    if (kind_name) {
        only = hl_cache_find(kind_name);
        if (!only) {
            fprintf(stderr,
                "hull cache clear: unknown kind '%s' "
                "(see `hull cache list`)\n", kind_name);
            return 2;
        }
    }

    if (!yes) {
        /* Use stderr even in JSON mode — refusal is a process
         * error, not a structured result. Mirrors the prune
         * behaviour for invalid flags. */
        fprintf(stderr,
            "hull cache clear: refusing to wipe without --yes\n"
            "  target: %s\n"
            "  re-run with --yes to confirm.\n",
            only ? only->name : "all runtime caches");
        return 2;
    }

    uint64_t total_removed = 0, total_bytes = 0;
    int rc_overall = 0;

    ShJsonWriter w;
    if (json) {
        sh_json_writer_init(&w, stdio_write_fn, stdout);
        sh_json_write_object_start(&w);
        sh_json_write_key(&w, "results");
        sh_json_write_array_start(&w);
    }

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (only && k != only) continue;
        /* Without --kind, only wipe runtime caches. Explicit
         * --kind=tools still works for the rare "really nuke the
         * signed binaries too" case. */
        if (!only && !k->is_runtime) continue;

        uint64_t removed = 0, bytes = 0;
        int no_store = 0;
        int rc = clear_one(k, &removed, &bytes, &no_store);
        if (rc != 0) rc_overall = 1;

        if (json) {
            sh_json_write_object_start(&w);
            sh_json_write_kv_string(&w, "name",        k->name);
            sh_json_write_kv_int   (&w, "removed",     (int64_t)removed);
            sh_json_write_kv_int   (&w, "freed_bytes", (int64_t)bytes);
            if (no_store)
                sh_json_write_kv_bool(&w, "no_store",  true);
            sh_json_write_kv_int   (&w, "rc",          rc);
            sh_json_write_object_end(&w);
        } else {
            if (no_store) {
                fprintf(stdout, "  %-12s (no store)\n", k->name);
            } else {
                char size_str[32];
                format_size(bytes, size_str, sizeof(size_str));
                fprintf(stdout, "  %-12s cleared %llu entries, %s\n",
                        k->name, (unsigned long long)removed, size_str);
            }
        }
        total_removed += removed;
        total_bytes   += bytes;
    }

    if (json) {
        sh_json_write_array_end(&w);
        sh_json_write_kv_int(&w, "total_removed",     (int64_t)total_removed);
        sh_json_write_kv_int(&w, "total_freed_bytes", (int64_t)total_bytes);
        sh_json_write_object_end(&w);
        fputc('\n', stdout);
        if (sh_json_writer_error(&w)) rc_overall = 1;
    }
    return rc_overall;
}

/* ── verify [--repair] ────────────────────────────────────────────
 *
 * Walks every entry of every kind (or just --kind=K), checks
 * structural integrity, and for CAS-mode kinds also recomputes
 * sha256(contents) and compares to the filename.
 *
 * Checks per entry:
 *   - filename is 64 lowercase hex (already enforced by the
 *     store's iter walker, but we re-check defensively)
 *   - file is readable + non-empty (zero-byte files are the
 *     valid SHA-256("") entry only — separate check)
 *   - is_cas kinds: sha256(contents) == filename
 *
 * On failure:
 *   - default: report only, exit 1
 *   - --repair: unlink the bad entry, exit 0 if all repairs OK
 *
 * Output: human table by default, --json for structured.
 */

/* Per-kind verify result. */
typedef struct {
    uint64_t checked;
    uint64_t ok;
    uint64_t corrupt;
    uint64_t repaired;
} VerifyStats;

/* Iter state — passed through hl_blob_store_iter via the
 * callback's `user` pointer. `w` is non-NULL in JSON mode; the
 * writer manages comma-separation automatically so we no longer
 * need a json_first flag here. */
typedef struct {
    const HlCacheKind *kind;
    HlBlobStore       *store;
    int                repair;
    ShJsonWriter      *w;       /* NULL = text mode */
    VerifyStats        stats;
} VerifyCtx;

/* Read a file fully into a malloc'd buffer. Returns 0 + (*out,
 * *out_len) on success, -1 on failure. Used for sha re-check. */
static int verify_read_all(const char *path,
                           uint8_t **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd); return -1;
    }
    if ((size_t)st.st_size > HL_BLOB_STORE_MAX_IN_MEMORY_BYTES) {
        /* Shared cap with `hl_blob_store_get`. A legit cache blob
         * is orders of magnitude smaller — see the constant's
         * docstring in include/hull/blob_store.h. */
        close(fd); return -1;
    }
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = (uint8_t *)malloc(sz > 0 ? sz : 1);
    if (!buf) { close(fd); return -1; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, buf + got, sz - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return -1;
        }
        if (n == 0) { free(buf); close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    *out = buf;
    *out_len = sz;
    return 0;
}

/* Per-entry verify callback. */
static int verify_visit(const char *id, size_t size, void *user)
{
    VerifyCtx *v = (VerifyCtx *)user;
    v->stats.checked++;

    /* Hex-validity already enforced by iter. We rely on it. */

    char path[PATH_MAX];
    if (hl_blob_store_compose_path(v->store, id, path, sizeof(path)) != 0) {
        /* Can't even compose the path — count as corrupt but no
         * repair possible. */
        v->stats.corrupt++;
        if (!v->w) {
            fprintf(stdout, "    %s  path-overflow\n", id);
        }
        return 0;
    }

    int bad = 0;
    const char *why = "ok";

    if (v->kind->is_cas) {
        /* Recompute sha256(contents); compare to filename. */
        uint8_t *bytes = NULL;
        size_t   len   = 0;
        if (verify_read_all(path, &bytes, &len) != 0) {
            bad = 1; why = "read-failed";
        } else {
            uint8_t digest[32];
            if (hl_cap_crypto_sha256(bytes, len, digest) != 0) {
                bad = 1; why = "sha-failed";
            } else {
                char digest_hex[HL_BLOB_STORE_ID_BUF_SIZE];
                hl_runtime_cache_hex_encode(digest, 32, digest_hex);
                if (memcmp(digest_hex, id, 64) != 0) {
                    bad = 1; why = "sha-mismatch";
                }
            }
            free(bytes);
        }
    } else {
        /* Keyed-mode: structural check only. Per-kind
         * deserializer-load checks would couple cache.c to
         * lua/quickjs/wamr; we let the runtime catch semantic
         * corruption lazily (the cache modules already
         * unlink+recompile on load failure).
         *
         * A zero-byte runtime cache entry is always corrupt —
         * none of the bytecode/template caches write empty
         * blobs. Read-failure also flags. */
        struct stat st;
        if (stat(path, &st) != 0) {
            bad = 1; why = "stat-failed";
        } else if (!S_ISREG(st.st_mode)) {
            bad = 1; why = "not-regular-file";
        } else if (st.st_size == 0) {
            bad = 1; why = "zero-size";
        }
        (void)size;  /* iter-time size unused for keyed mode */
    }

    if (bad) {
        v->stats.corrupt++;
        if (v->repair) {
            if (hl_blob_store_delete(v->store, id) > 0) {
                v->stats.repaired++;
                why = "unlinked";
            }
        }
    } else {
        v->stats.ok++;
    }

    if (v->w) {
        sh_json_write_object_start(v->w);
        sh_json_write_kv_string(v->w, "id",         id);
        sh_json_write_kv_string(v->w, "status",     why);
        sh_json_write_kv_int   (v->w, "size_bytes", (int64_t)size);
        sh_json_write_object_end(v->w);
    } else if (bad) {
        fprintf(stdout, "    %s  %s%s\n",
                id, why,
                v->repair ? "" : "  (re-run with --repair to evict)");
    }
    return 0;
}

/* Verify one cache kind. Emits the per-kind text or JSON section
 * (header + per-entry results + footer), accumulates into totals,
 * and returns 1 if anything was unrecoverable (open_failed or
 * un-repaired corruption). Extracted to keep cmd_verify scoped to
 * arg parsing + loop control + the grand-total footer. */
typedef struct {
    uint64_t checked, ok, corrupt, repaired;
} VerifyTotals;

static int verify_one_kind(const HlCacheKind *k, int repair,
                           ShJsonWriter *w, VerifyTotals *totals)
{
    /* Path resolution can fail when the cache subdir path resolves
     * to something other than a directory (e.g. a regular file
     * planted there by a misconfigured deployment, or stale state
     * from a different Hull version). Treat that identically to
     * a blob_store_open failure — emit `open_failed`, bump rc.
     * Silent skip misleads CI gates consuming `verify --json`. */
    char root[PATH_MAX];
    HlBlobStore *s = NULL;
    int open_ok = (hl_cache_resolve_path(k, root, sizeof(root)) == 0 &&
                   hl_blob_store_open(&s, NULL, root,
                                      /*shard_depth=*/1, 0) == 0);
    if (!open_ok) {
        if (w) {
            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "name",         k->name);
            sh_json_write_kv_bool  (w, "open_failed",  true);
            sh_json_write_key      (w, "entries");
            sh_json_write_array_start(w);
            sh_json_write_array_end(w);
            sh_json_write_object_end(w);
        } else {
            fprintf(stdout, "  %-12s (cannot open store)\n", k->name);
        }
        return 1;
    }

    if (w) {
        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "name",   k->name);
        sh_json_write_kv_bool  (w, "is_cas", k->is_cas != 0);
        sh_json_write_key      (w, "entries");
        sh_json_write_array_start(w);
    } else {
        fprintf(stdout, "  %-12s (%s mode)\n", k->name,
                k->is_cas ? "CAS — sha-checked" : "keyed — structural only");
    }

    VerifyCtx v = {
        .kind = k, .store = s, .repair = repair, .w = w,
        .stats = {0, 0, 0, 0},
    };
    (void)hl_blob_store_iter(s, verify_visit, &v);
    hl_blob_store_close(s);

    if (w) {
        sh_json_write_array_end(w);
        sh_json_write_kv_int(w, "checked",  (int64_t)v.stats.checked);
        sh_json_write_kv_int(w, "ok",       (int64_t)v.stats.ok);
        sh_json_write_kv_int(w, "corrupt",  (int64_t)v.stats.corrupt);
        sh_json_write_kv_int(w, "repaired", (int64_t)v.stats.repaired);
        sh_json_write_object_end(w);
    } else {
        fprintf(stdout, "    %llu checked, %llu ok, %llu corrupt%s\n",
                (unsigned long long)v.stats.checked,
                (unsigned long long)v.stats.ok,
                (unsigned long long)v.stats.corrupt,
                v.stats.repaired ? " (repaired)" : "");
    }

    totals->checked  += v.stats.checked;
    totals->ok       += v.stats.ok;
    totals->corrupt  += v.stats.corrupt;
    totals->repaired += v.stats.repaired;
    return 0;
}

static int cmd_verify(int argc, char **argv)
{
    const char *kind_name = NULL;
    int repair = 0;
    int json   = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--kind=", 7) == 0)  kind_name = a + 7;
        else if (strcmp(a, "--repair") == 0) repair = 1;
        else if (strcmp(a, "--json") == 0)   json = 1;
        else {
            fprintf(stderr, "hull cache verify: unknown flag '%s'\n", a);
            return 2;
        }
    }

    const HlCacheKind *only = NULL;
    if (kind_name) {
        only = hl_cache_find(kind_name);
        if (!only) {
            fprintf(stderr,
                "hull cache verify: unknown kind '%s' "
                "(see `hull cache list`)\n", kind_name);
            return 2;
        }
    }

    VerifyTotals totals = {0, 0, 0, 0};
    int rc_overall = 0;

    ShJsonWriter w;
    ShJsonWriter *wp = NULL;
    if (json) {
        sh_json_writer_init(&w, stdio_write_fn, stdout);
        wp = &w;
        sh_json_write_object_start(wp);
        sh_json_write_key(wp, "results");
        sh_json_write_array_start(wp);
    }

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (only && k != only) continue;
        if (verify_one_kind(k, repair, wp, &totals) != 0)
            rc_overall = 1;
    }

    if (wp) {
        sh_json_write_array_end(wp);
        sh_json_write_kv_int(wp, "total_checked",  (int64_t)totals.checked);
        sh_json_write_kv_int(wp, "total_ok",       (int64_t)totals.ok);
        sh_json_write_kv_int(wp, "total_corrupt",  (int64_t)totals.corrupt);
        sh_json_write_kv_int(wp, "total_repaired", (int64_t)totals.repaired);
        sh_json_write_object_end(wp);
        fputc('\n', stdout);
        if (sh_json_writer_error(wp)) rc_overall = 1;
    } else {
        fprintf(stdout, "\nTotal: %llu checked, %llu ok, %llu corrupt",
                (unsigned long long)totals.checked,
                (unsigned long long)totals.ok,
                (unsigned long long)totals.corrupt);
        if (totals.repaired)
            fprintf(stdout, " (%llu repaired)",
                    (unsigned long long)totals.repaired);
        fputc('\n', stdout);
    }

    /* Exit 1 if anything is corrupt AND we didn't repair (or
     * repair couldn't fix every issue). Exit 0 if --repair
     * brought everything to OK. */
    if (totals.corrupt > totals.repaired) rc_overall = 1;
    return rc_overall;
}

/* ── Dispatch ────────────────────────────────────────────────────── */

static void usage(FILE *f)
{
    fprintf(f,
"Usage: hull cache <verb> [flags]\n"
"\n"
"Verbs:\n"
"  list [--json]                  Show every registered cache: name, path,\n"
"                                 entries, size.\n"
"  prune [--kind=K] [--max-size=N] [--max-age=N]\n"
"        [--strategy=lru|fifo] [--dry-run] [--json]\n"
"                                 Evict from runtime caches by age / total\n"
"                                 size. System stores (tools) are skipped\n"
"                                 unless --kind=tools is set.\n"
"                                 --max-size accepts K / M / G suffixes\n"
"                                   (binary, 1024-based; B optional).\n"
"                                 --max-age accepts s / m / h / d / w / y\n"
"                                   suffixes (bare numbers = seconds).\n"
"                                 --json emits one result object per kind\n"
"                                   plus total_removed + total_freed_bytes.\n"
"  clear [--kind=K] --yes [--json]\n"
"                                 Wipe runtime caches entirely. With\n"
"                                 --kind=K, restrict to one cache.\n"
"                                 --json emits the same shape as prune.\n"
"  verify [--kind=K] [--repair] [--json]\n"
"                                 Walk every entry: structural check\n"
"                                 (filename shape, readability); for\n"
"                                 CAS-mode kinds (tools) also recompute\n"
"                                 sha256 and compare to the filename.\n"
"                                 Exits 1 if anything is corrupt and\n"
"                                 --repair wasn't able to fix it.\n"
"                                 --repair unlinks corrupt entries\n"
"                                 (the next compile / lookup repopulates).\n"
"\n"
"Cache layout:\n"
"  Runtime caches live at $HOME/.hull/blobs/runtime/<kind>/.\n"
"  Set HULL_CACHE_DIR=/abs/path to redirect for per-app isolation\n"
"  (useful on multi-tenant boxes or when running under systemd /\n"
"  k8s / Docker). The path must be absolute. Tools storage\n"
"  ($HOME/.hull/blobs/tools/) is unaffected by the override.\n"
"\n"
"Examples:\n"
"  hull cache list\n"
"  hull cache prune --max-size=100M\n"
"  hull cache prune --max-age=30d --strategy=lru\n"
"  hull cache prune --max-age=24h --dry-run\n"
"  hull cache clear --kind=lua-bytecode --yes\n"
"  HULL_CACHE_DIR=/var/lib/myapp/cache hull cache list\n");
}

int hl_cmd_cache(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;
    if (argc < 2 || strcmp(argv[1], "--help") == 0
                 || strcmp(argv[1], "-h")     == 0
                 || strcmp(argv[1], "help")   == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }

    const char *verb = argv[1];
    if (strcmp(verb, "list")   == 0) return cmd_list(argc - 1,   argv + 1);
    if (strcmp(verb, "prune")  == 0) return cmd_prune(argc - 1,  argv + 1);
    if (strcmp(verb, "clear")  == 0) return cmd_clear(argc - 1,  argv + 1);
    if (strcmp(verb, "verify") == 0) return cmd_verify(argc - 1, argv + 1);

    fprintf(stderr, "hull cache: unknown verb '%s'\n\n", verb);
    usage(stderr);
    return 2;
}

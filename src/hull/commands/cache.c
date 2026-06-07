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
#include "hull/blob_store.h"
#include "hull/cache_dir.h"
#include "hull/cache_registry.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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

/* JSON string escape — matches the minimal escaper sh_json uses. */
static void json_emit_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f);  break;
            case '\r': fputs("\\r", f);  break;
            case '\t': fputs("\\t", f);  break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else          fputc(c, f);
        }
    }
    fputc('"', f);
}

/* Build the HULL_NO_<KIND>_CACHE env-var name for a registry
 * entry. Mirrors the composition rule in cache_dir.c's
 * hl_hull_cache_disabled. Returns an empty string for system
 * stores (env_kind == NULL — no per-cache opt-out applies). */
static void env_var_for(const HlCacheKind *kind, char *out, size_t out_sz)
{
    if (!kind->env_kind) { out[0] = '\0'; return; }
    size_t pre = strlen("HULL_NO_");
    size_t suf = strlen("_CACHE");
    size_t kl  = strlen(kind->env_kind);
    if (pre + kl + suf + 1 > out_sz) { out[0] = '\0'; return; }
    memcpy(out, "HULL_NO_", pre);
    for (size_t j = 0; j < kl; j++) {
        char c = kind->env_kind[j];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[pre + j] = c;
    }
    memcpy(out + pre + kl, "_CACHE", suf);
    out[pre + kl + suf] = '\0';
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
     * WHY a cache shows `off`. Only emit when something is set. */
    int any_off_emitted = 0;
    if (global_off) {
        fprintf(stdout, "\nHULL_NO_CACHE=1 active — all runtime caches disabled.\n");
        any_off_emitted = 1;
    } else {
        for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
            if (!k->env_kind) continue;
            if (!hl_hull_cache_disabled(k->env_kind)) continue;
            char env_name[64];
            env_var_for(k, env_name, sizeof(env_name));
            if (!any_off_emitted) fputc('\n', stdout);
            fprintf(stdout, "%s=1 active — %s cache disabled.\n",
                    env_name, k->name);
            any_off_emitted = 1;
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
    fputs("{\"caches\":[", stdout);
    int first = 1;
    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (!first) fputc(',', stdout);
        first = 0;

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

        fputs("{\"name\":", stdout);
        json_emit_str(stdout, k->name);
        fputs(",\"description\":", stdout);
        json_emit_str(stdout, k->description);
        fputs(",\"path\":", stdout);
        json_emit_str(stdout, resolved ? path : "");
        fprintf(stdout, ",\"is_runtime\":%s",
                k->is_runtime ? "true" : "false");
        fputs(",\"env_var\":", stdout);
        json_emit_str(stdout, env_name);
        fprintf(stdout, ",\"disabled\":%s",
                disabled ? "true" : "false");
        fprintf(stdout, ",\"count\":%llu", (unsigned long long)cnt);
        fprintf(stdout, ",\"size_bytes\":%llu", (unsigned long long)size);
        fputc('}', stdout);
    }
    fputs("]", stdout);

    const char *override = getenv("HULL_CACHE_DIR");
    fprintf(stdout, ",\"hull_cache_dir\":");
    json_emit_str(stdout, (override && *override) ? override : "");
    fputs("}\n", stdout);
    return 0;
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

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--kind=", 7) == 0)        kind_name = a + 7;
        else if (strncmp(a, "--max-size=", 11) == 0)
            max_size = strtoull(a + 11, NULL, 10);
        else if (strncmp(a, "--max-age=", 10) == 0)
            max_age  = strtoull(a + 10, NULL, 10);
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

    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (only && k != only) continue;
        /* By default `prune` only touches runtime caches; system
         * stores (tools) are signed durable downloads and shouldn't
         * be swept opportunistically. Explicit --kind=tools bypass. */
        if (!only && !k->is_runtime) continue;

        HlBlobStore *s = open_kind(k);
        if (!s) {
            fprintf(stdout, "  %-12s (no store)\n", k->name);
            continue;
        }

        uint64_t removed = 0, freed = 0;
        int rc = hl_blob_store_cleanup(s, &opts, &removed, &freed);
        hl_blob_store_close(s);
        if (rc != 0) rc_overall = 1;

        char size_str[32];
        format_size(freed, size_str, sizeof(size_str));
        fprintf(stdout, "  %-12s %s %llu entries, %s\n",
                k->name,
                dry_run ? "would remove" : "removed",
                (unsigned long long)removed, size_str);
        total_removed += removed;
        total_freed   += freed;
    }

    char total_str[32];
    format_size(total_freed, total_str, sizeof(total_str));
    fprintf(stdout,
        "\nTotal %s: %llu entries, %s\n",
        dry_run ? "would-be removed" : "removed",
        (unsigned long long)total_removed, total_str);
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

static int clear_one(const HlCacheKind *k)
{
    HlBlobStore *s = open_kind(k);
    if (!s) {
        fprintf(stdout, "  %-12s (no store)\n", k->name);
        return 0;
    }

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

    char size_str[32];
    format_size(c.bytes, size_str, sizeof(size_str));
    fprintf(stdout, "  %-12s cleared %llu entries, %s\n",
            k->name, (unsigned long long)removed, size_str);
    return rc;
}

static int cmd_clear(int argc, char **argv)
{
    const char *kind_name = NULL;
    int yes = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--kind=", 7) == 0) kind_name = a + 7;
        else if (strcmp(a, "--yes") == 0)  yes = 1;
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
        fprintf(stderr,
            "hull cache clear: refusing to wipe without --yes\n"
            "  target: %s\n"
            "  re-run with --yes to confirm.\n",
            only ? only->name : "all runtime caches");
        return 2;
    }

    int rc_overall = 0;
    for (const HlCacheKind *k = hl_cache_registry(); k->name; k++) {
        if (only && k != only) continue;
        /* Without --kind, only wipe runtime caches. Explicit
         * --kind=tools still works for the rare "really nuke the
         * signed binaries too" case. */
        if (!only && !k->is_runtime) continue;
        if (clear_one(k) != 0) rc_overall = 1;
    }
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
"        [--strategy=lru|fifo] [--dry-run]\n"
"                                 Evict from runtime caches by age / total\n"
"                                 size. System stores (tools) are skipped\n"
"                                 unless --kind=tools is set.\n"
"  clear [--kind=K] --yes         Wipe runtime caches entirely. With\n"
"                                 --kind=K, restrict to one cache.\n"
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
"  hull cache prune --max-size=$((100*1024*1024))\n"
"  hull cache prune --max-age=$((30*86400)) --strategy=lru\n"
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
    if (strcmp(verb, "list")  == 0) return cmd_list(argc - 1,  argv + 1);
    if (strcmp(verb, "prune") == 0) return cmd_prune(argc - 1, argv + 1);
    if (strcmp(verb, "clear") == 0) return cmd_clear(argc - 1, argv + 1);

    fprintf(stderr, "hull cache: unknown verb '%s'\n\n", verb);
    usage(stderr);
    return 2;
}

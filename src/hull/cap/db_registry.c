/*
 * cap/db_registry.c: Named database connection registry
 *
 * See db_registry.h. Lazily opens + caches HlDbHandle connections by name,
 * resolving DSNs from the sealed manifest's databases map (with { dsn_env }
 * read from the environment at open time).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db_registry.h"
#include "hull/cap/db_dynamic.h"   /* hl_db_dynamic_guard_stale_txns */
#include "hull/manifest.h"
#include "hull/utils/env_ref.h"   /* hl_env_ref */
#include "hull/cap/policy_seal.h"  /* hl_policy_page_* */
#include "hull/shared/host.h"     /* hl_host_normalize_path, HL_HOST_PATH_MAX */

#include <limits.h>

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */

/* One extra slot beyond the manifest cap for the seeded "default". */
#define HL_DB_REGISTRY_MAX (HL_MANIFEST_MAX_DATABASES + 1)

/* The registry name of the stdlib's internal connection. It starts with a
 * byte no manifest name can, and hl_db_registry_get refuses it, so app code
 * cannot reach the connection by name (db.connect). */
#define INTERNAL_NAME "\x01internal"

/* Longest -d DSN the registry keeps (inline, in the sealed span). */
#define HL_DB_REGISTRY_DSN_MAX 8192

typedef struct {
    char       *name;    /* owned */
    int         dsn_known; /* 0 for a seeded/borrowed connection */
    HlDbHandle  handle;
    int         open;
    int         owned;   /* 1 = registry closes it; 0 = seeded/borrowed */
} RegSlot;

struct HlDbRegistry {
    /* Where each connection's DSN comes from: a leading policy span (see
     * cap/policy_seal.h), sealed by hl_db_registry_seal once wiring is done,
     * so a write cannot point a database at another host. The connection
     * cache after it stays writable. */
    union {
        struct {
            const HlManifest *manifest;    /* borrowed (the sealed manifest) */
            int               has_default_dsn;
            char              default_dsn[HL_DB_REGISTRY_DSN_MAX]; /* the -d DSN */
            /* databases.internal learned before the manifest is wired (a
             * stdlib init() at app top level); the wired manifest's own
             * value takes over once set. */
            int               has_internal_dsn;
            char              internal_dsn[HL_DB_REGISTRY_DSN_MAX];
            /* The app directory a manifest file DSN must stay in (audit
             * 34): its realpath, and the spelling it was given in (an
             * absolute DSN may name the app directory either way). */
            int               has_app_dir;
            char              app_dir_real[HL_HOST_PATH_MAX];
            char              app_dir_raw[HL_HOST_PATH_MAX];
        };
        unsigned char policy_span[HL_POLICY_SPAN];
    };
    int               sealed;
    HlAllocator      *alloc;       /* borrowed */
    RegSlot           slots[HL_DB_REGISTRY_MAX];
    int               nslots;
    /* Set by hl_db_registry_close_all: every connection is closed and nothing
     * opens again. The memory stays until hl_db_registry_destroy (audit 17). */
    int               closed;
};

HlDbRegistry *hl_db_registry_create(const HlManifest *manifest,
                                    const char *default_dsn,
                                    HlAllocator *alloc)
{
    size_t dlen = default_dsn ? strlen(default_dsn) : 0;
    if (dlen >= HL_DB_REGISTRY_DSN_MAX) return NULL;
    HlDbRegistry *r = hl_policy_page_alloc(sizeof *r);   /* zeroed */
    if (!r) return NULL;
    r->manifest = manifest;
    r->alloc = alloc;
    if (dlen > 0) {
        memcpy(r->default_dsn, default_dsn, dlen + 1);
        r->has_default_dsn = 1;
    }
    return r;
}

int hl_db_registry_seal(HlDbRegistry *reg)
{
    if (!reg) return -1;
    if (reg->sealed) return 0;
    if (hl_policy_page_seal(reg) != 0) return -1;
    reg->sealed = 1;
    return 0;
}

/* hl_db_registry_guard_hold's count. Event-loop thread only, as the guard. */
static int g_guard_hold;

void hl_db_registry_guard_hold(int delta)
{
    g_guard_hold += delta;
    if (g_guard_hold < 0) g_guard_hold = 0;
}

int hl_db_registry_guard_held(void)
{
    return g_guard_hold > 0;
}

void hl_db_registry_guard_stale_txns(HlDbRegistry *reg)
{
    if (g_guard_hold > 0) return;   /* a test case's live transaction */
    hl_db_dynamic_guard_stale_txns();
    if (!reg) return;
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open)
            hl_db_guard_stale_txn(&reg->slots[i].handle);
}

const char *hl_db_registry_open_txn(HlDbRegistry *reg)
{
    if (hl_db_dynamic_in_txn()) return "db.open";
    if (!reg) return NULL;
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && hl_db_in_txn(&reg->slots[i].handle))
            return strcmp(reg->slots[i].name, INTERNAL_NAME) == 0
                   ? "internal" : reg->slots[i].name;
    return NULL;
}

/* Fast accessor for the already-open "default" connection (opened at startup
 * for migrations, so it is cached). Returns NULL if absent. No open, no error
 * path. */
HlDbHandle *hl_db_registry_default(HlDbRegistry *reg)
{
    if (!reg || reg->closed) return NULL;
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && strcmp(reg->slots[i].name, "default") == 0)
            return &reg->slots[i].handle;
    return NULL;
}

void hl_db_registry_set_manifest(HlDbRegistry *reg, const HlManifest *manifest)
{
    if (reg && !reg->sealed) reg->manifest = manifest;
}

int hl_db_registry_seed(HlDbRegistry *reg, const char *name,
                        const HlDbHandle *h)
{
    if (!reg || !name || !h || !h->backend) return -1;
    if (reg->nslots >= HL_DB_REGISTRY_MAX) return -1;
    RegSlot *s = &reg->slots[reg->nslots];
    s->name = strdup(name);
    if (!s->name) return -1;
    s->handle = *h;      /* value-copy; shares the underlying ctx */
    s->open = 1;
    s->owned = 0;        /* caller keeps ownership; destroy won't close it */
    reg->nslots++;
    return 0;
}

/* Copy @p dir into @p out without its trailing slashes ("/" stays "/"). */
static int copy_dir(const char *dir, char *out, size_t outsz)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    if (n == 0 || n >= outsz) return -1;
    memcpy(out, dir, n);
    out[n] = '\0';
    return 0;
}

int hl_db_registry_set_app_dir(HlDbRegistry *reg, const char *app_dir)
{
    if (!reg || reg->sealed || !app_dir || !app_dir[0]) return -1;
    char real[PATH_MAX];
    if (!realpath(app_dir, real)) return -1;
    if (copy_dir(real, reg->app_dir_real, sizeof reg->app_dir_real) != 0 ||
        copy_dir(app_dir, reg->app_dir_raw, sizeof reg->app_dir_raw) != 0)
        return -1;
    reg->has_app_dir = 1;
    return 0;
}

/* 1 when @p path starts with directory @p dir followed by '/'. */
static int under_dir(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n == 1 && dir[0] == '/') return path[0] == '/';
    return strncmp(path, dir, n) == 0 && path[n] == '/' && path[n + 1];
}

/* 1 when @p path has a ".." component. */
static int has_dotdot(const char *path)
{
    for (const char *p = path; *p; ) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 2 && p[0] == '.' && p[1] == '.') return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* A manifest (databases.named / databases.internal) DSN, after any "$VAR"
 * reference was resolved, made safe to open (audit 34). A network DSN, an
 * unknown scheme (the selector refuses it) and an in-memory / temporary
 * database pass unchanged. A file DSN (scheme-less, sqlite://, file://,
 * duckdb://) must name a file inside the app directory - a relative path is
 * taken against the app directory, not the working directory - with no ".."
 * component, no symbolic link below the app directory, and a leaf and SQLite
 * sidecars (-wal / -shm / -journal) that are absent or regular files, as
 * db.open requires (audit 29) and the implicit data.db (audit 25). The
 * declaration is the grant, as -d is the operator's: no fs.write entry is
 * needed, but nothing outside the tree is reachable - a cloned app committing
 * cache.db -> ../secret.db opened that database wherever the kernel sandbox
 * is absent. Returns the DSN to open (@p buf for a file DSN, holding its
 * absolute path with the original scheme), or NULL + *err. */
static const char *confine_manifest_dsn(const HlDbRegistry *reg,
                                        const char *dsn, char *buf,
                                        size_t bufsz, const char **err)
{
    size_t plen = 0;   /* length of a file scheme's "<scheme>://" prefix */
    const char *sep = strstr(dsn, "://");
    if (sep) {
        size_t n = (size_t)(sep - dsn);
        int scheme = n > 0 && ((dsn[0] >= 'a' && dsn[0] <= 'z') ||
                               (dsn[0] >= 'A' && dsn[0] <= 'Z'));
        for (size_t i = 1; scheme && i < n; i++) {
            char c = dsn[i];
            scheme = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        }
        if (scheme) {
            if ((n == 6 && strncasecmp(dsn, "sqlite", 6) == 0) ||
                (n == 4 && strncasecmp(dsn, "file", 4) == 0) ||
                (n == 6 && strncasecmp(dsn, "duckdb", 6) == 0))
                plen = n + 3;
            else
                return dsn;   /* a network backend, or one the selector refuses */
        }
    }
    const char *path = dsn + plen;
    if (!path[0] || strcmp(path, ":memory:") == 0) return dsn;

    if (!reg->has_app_dir) {
        *err = "a file database DSN needs the app directory (none is known)";
        return NULL;
    }
    char norm[HL_HOST_PATH_MAX];
    if (hl_host_normalize_path(path, norm, sizeof norm) >= 0) path = norm;
    if (has_dotdot(path)) {
        *err = "a file database DSN may not have a \"..\" component";
        return NULL;
    }

    char abs[HL_HOST_PATH_MAX];
    size_t base_len;
    if (path[0] == '/') {
        size_t n = strlen(path);
        if (n >= sizeof abs) { *err = "database path too long"; return NULL; }
        memcpy(abs, path, n + 1);
        if (under_dir(abs, reg->app_dir_real)) {
            base_len = strlen(reg->app_dir_real);
        } else if (reg->app_dir_raw[0] == '/' && under_dir(abs, reg->app_dir_raw)) {
            base_len = strlen(reg->app_dir_raw);
        } else {
            /* The app directory spelled through a link above it (/var vs
             * /private/var on macOS): canonicalise the deepest existing
             * ancestor and compare again. A link INSIDE the tree that leads
             * out resolves outside and is refused here; one that stays in
             * is no escape, and the leaf is still checked below. */
            char tmp[HL_HOST_PATH_MAX], real[PATH_MAX];
            memcpy(tmp, abs, n + 1);
            size_t cut = n;
            int found = 0;
            while (cut > 0) {
                while (cut > 0 && tmp[cut] != '/') cut--;
                if (cut == 0) break;
                tmp[cut] = '\0';
                if (realpath(tmp, real)) { found = 1; break; }
            }
            int m = found ? snprintf(tmp, sizeof tmp, "%s%s", real, abs + cut) : -1;
            if (m < 0 || (size_t)m >= sizeof tmp ||
                !under_dir(tmp, reg->app_dir_real)) {
                *err = "a file database DSN must name a file inside the app directory";
                return NULL;
            }
            memcpy(abs, tmp, (size_t)m + 1);
            base_len = strlen(reg->app_dir_real);
        }
    } else {
        int n = snprintf(abs, sizeof abs, "%s/%s", reg->app_dir_real, path);
        if (n < 0 || (size_t)n >= sizeof abs) {
            *err = "database path too long";
            return NULL;
        }
        base_len = strlen(reg->app_dir_real);
    }
    if (hl_db_path_has_symlink(abs, base_len)) {
        *err = "a file database DSN runs through a symbolic link, or names "
               "something that is not a regular file";
        return NULL;
    }
    int n = snprintf(buf, bufsz, "%.*s%s", (int)plen, dsn, abs);
    if (n < 0 || (size_t)n >= bufsz) {
        *err = "database path too long";
        return NULL;
    }
    return buf;
}

/* Look up the DSN for @p name: manifest databases first (resolving a "$VAR"
 * env reference from the environment), then any seeded fallback is handled by
 * the cache scan in _get. Returns NULL + *err on failure. */
static const char *resolve_manifest_dsn(HlDbRegistry *reg, const char *name,
                                         const char **err)
{
    if (!reg->manifest) return NULL;
    for (int i = 0; i < reg->manifest->databases.named_count; i++) {
        const HlManifestDbNamed *d = &reg->manifest->databases.named[i];
        if (strcmp(d->name, name) != 0) continue;
        char var[128];
        if (hl_env_ref(d->dsn, var, sizeof var)) {
            const char *v = getenv(var);
            if (!v || !v[0]) {
                *err = "database DSN env var is unset";
                return NULL;
            }
            return v;
        }
        return d->dsn;
    }
    return NULL;
}

/* The DSN for @p name, from the sealed sources only: the manifest, then for
 * "default" the -d DSN when the manifest declares no entry for it (derr stays
 * NULL for a plain miss; a set derr, e.g. an unset env ref, is a real error).
 * A manifest DSN goes through confine_manifest_dsn (@p buf, @p bufsz hold a
 * rewritten one); the operator's -d DSN is taken as given. Worked out afresh
 * on every call rather than kept per connection: a stored copy would be
 * writable heap the worker pool connects to. */
static const char *lookup_dsn(HlDbRegistry *reg, const char *name,
                              const char **err, char *buf, size_t bufsz)
{
    const char *d = NULL;
    if (strcmp(name, INTERNAL_NAME) == 0) {
        d = (reg->manifest && reg->manifest->databases.internal)
            ? reg->manifest->databases.internal
            : (reg->has_internal_dsn ? reg->internal_dsn : NULL);
        char var[128];
        if (d && hl_env_ref(d, var, sizeof var)) {
            const char *v = getenv(var);
            if (!v || !v[0]) {
                *err = "databases.internal env var is unset";
                return NULL;
            }
            d = v;
        }
    } else {
        d = resolve_manifest_dsn(reg, name, err);
        if (!d) {
            if (!*err && reg->has_default_dsn && strcmp(name, "default") == 0)
                return reg->default_dsn;
            return NULL;
        }
    }
    if (!d) return NULL;
    return confine_manifest_dsn(reg, d, buf, bufsz, err);
}

static HlDbHandle *registry_get(HlDbRegistry *reg, const char *name,
                                const char **err);

/* Whether the manifest declares @p name with session_control = true. */
static int manifest_session_control(const HlDbRegistry *reg, const char *name)
{
    if (!reg->manifest) return 0;
    for (int i = 0; i < reg->manifest->databases.named_count; i++) {
        const HlManifestDbNamed *d = &reg->manifest->databases.named[i];
        if (d->name && strcmp(d->name, name) == 0) return d->session_control;
    }
    return 0;
}

HlDbHandle *hl_db_registry_get(HlDbRegistry *reg, const char *name,
                               const char **err)
{
    if (err) *err = NULL;
    if (!reg || !name || !name[0] || name[0] == INTERNAL_NAME[0]) {
        if (err) *err = "invalid database name";
        return NULL;
    }
    return registry_get(reg, name, err);
}

int hl_db_registry_has_internal(const HlDbRegistry *reg)
{
    return reg && ((reg->manifest && reg->manifest->databases.internal)
                   || reg->has_internal_dsn);
}

HlDbHandle *hl_db_registry_internal(HlDbRegistry *reg, const char **err)
{
    if (err) *err = NULL;
    if (!reg) return NULL;
    if (!hl_db_registry_has_internal(reg))
        return registry_get(reg, "default", err);
    return registry_get(reg, INTERNAL_NAME, err);
}

int hl_db_registry_set_internal_dsn(HlDbRegistry *reg, const char *dsn)
{
    if (!reg || reg->sealed) return -1;
    if (!dsn || !dsn[0]) {
        reg->has_internal_dsn = 0;
        return 0;
    }
    size_t n = strlen(dsn);
    if (n >= sizeof reg->internal_dsn) return -1;
    memcpy(reg->internal_dsn, dsn, n + 1);
    reg->has_internal_dsn = 1;
    return 0;
}

int hl_db_registry_default_is_network(const HlDbRegistry *reg)
{
    if (!reg) return 0;
    const char *err = NULL;
    char buf[HL_HOST_PATH_MAX + 16];
    const char *dsn = lookup_dsn((HlDbRegistry *)(uintptr_t)reg, "default", &err,
                                 buf, sizeof buf);
    if (!dsn) return 0;
    if (dsn[0] == '$') return 1;
    /* Without case (audit 14): the backend selector reads the scheme that
     * way, so "PostgreSQL://..." is a network database too. */
    return strncasecmp(dsn, "postgres", 8) == 0 || strncasecmp(dsn, "mysql", 5) == 0
        || strncasecmp(dsn, "mariadb", 7) == 0
        || strncasecmp(dsn, "duckdb", 6) == 0;   /* audit 25 */
}

int hl_db_registry_manifest_wired(const HlDbRegistry *reg)
{
    return reg && reg->manifest;
}

static HlDbHandle *registry_get(HlDbRegistry *reg, const char *name,
                                const char **err)
{
    if (reg->closed) {
        if (err) *err = "database connections are closed (shutting down)";
        return NULL;
    }

    /* Cache hit (includes seeded connections like "default"). */
    for (int i = 0; i < reg->nslots; i++)
        if (reg->slots[i].open && strcmp(reg->slots[i].name, name) == 0)
            return &reg->slots[i].handle;

    if (reg->nslots >= HL_DB_REGISTRY_MAX) {
        if (err) *err = "too many database connections";
        return NULL;
    }

    const char *derr = NULL;
    char dsn_buf[HL_HOST_PATH_MAX + 16];
    const char *dsn = lookup_dsn(reg, name, &derr, dsn_buf, sizeof dsn_buf);
    if (!dsn) {
        if (err) *err = derr ? derr
                             : "unknown database name (not in manifest.databases)";
        return NULL;
    }

    const char *sel_err = NULL;
    const HlDbBackend *be = hl_db_backend_select(dsn, &sel_err);
    if (!be) {
        if (err) *err = sel_err ? sel_err : "no database backend for dsn";
        return NULL;
    }

    RegSlot *s = &reg->slots[reg->nslots];
    memset(s, 0, sizeof *s);
    s->name = strdup(name);
    if (!s->name) { if (err) *err = "out of memory"; return NULL; }
    s->handle.backend = be;
    if (be->open(&s->handle.ctx, dsn, reg->alloc) != 0) {
        free(s->name);
        s->name = NULL;
        if (err) *err = "failed to open database connection";
        return NULL;
    }
    s->dsn_known = 1;
    s->open = 1;
    s->owned = 1;
    /* A session_control connection (never "default": the manifest check
     * refuses it, and never the internal one). */
    s->handle.session_control =
        strcmp(name, INTERNAL_NAME) != 0 && strcmp(name, "default") != 0 &&
        manifest_session_control(reg, name);
    reg->nslots++;
    return &s->handle;
}

const char *hl_db_registry_dsn_for(HlDbRegistry *reg, const HlDbHandle *h)
{
    if (!reg || !h) return NULL;
    for (int i = 0; i < reg->nslots; i++) {
        const RegSlot *s = &reg->slots[i];
        if (s->open && &s->handle == h) {
            if (!s->dsn_known) return NULL;
            /* The worker copies the DSN before this thread calls again. */
            static _Thread_local char buf[HL_HOST_PATH_MAX + 16];
            const char *err = NULL;
            const char *dsn = lookup_dsn(reg, s->name, &err, buf, sizeof buf);
            /* NULL would send the op to the DEFAULT database: a connection
             * whose DSN no longer resolves or passes the confinement check
             * (a symlink planted since it opened, an env var unset) gets a
             * DSN no backend takes, so the op fails instead (audit 34). */
            return dsn ? dsn : "hull-refused://";
        }
    }
    return NULL;
}

const HlManifestDbDynamic *hl_db_registry_dynamic_policy(HlDbRegistry *reg)
{
    if (!reg || reg->closed || !reg->manifest) return NULL;
    return &reg->manifest->databases.dynamic;
}

int hl_db_registry_is_closed(const HlDbRegistry *reg)
{
    return !reg || reg->closed;
}

void hl_db_registry_close_all(HlDbRegistry *reg)
{
    if (!reg) return;
    reg->closed = 1;   /* first: a backend close cannot reopen through _get */
    for (int i = 0; i < reg->nslots; i++) {
        RegSlot *s = &reg->slots[i];
        HlDbTxnSnap was = { 0, 0 };
        if (s->open) was = hl_db_txn_snap(&s->handle);
        if (s->open && s->owned && s->handle.backend)
            s->handle.backend->close(&s->handle);
        /* A seeded slot is a value copy of a handle its owner closes; drop
         * the copy too. A runtime object still holding &s->handle then sees
         * no backend and fails "not available" (audit 17). */
        s->open = 0;
        memset(&s->handle, 0, sizeof s->handle);
        /* A transaction open here ended with the close (audit 37). */
        hl_db_txn_note(&s->handle, was, NULL);
    }
}

void hl_db_registry_destroy(HlDbRegistry *reg)
{
    if (!reg) return;
    hl_db_registry_close_all(reg);
    for (int i = 0; i < reg->nslots; i++)
        free(reg->slots[i].name);
    if (reg->sealed) hl_policy_page_unseal(reg);
    hl_policy_page_free(reg, sizeof *reg);
}

#endif /* HL_ENABLE_DB */

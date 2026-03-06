/*
 * hull_cap_fs.c — Shared filesystem capability
 *
 * All file I/O goes through these functions with path validation.
 * Rejects "..", absolute paths, and paths outside the declared base_dir.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/fs.h"
#include "hull/cap/audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/* ── Path validation ────────────────────────────────────────────────── */

int hl_cap_fs_validate(const HlFsConfig *cfg, const char *path)
{
    if (!cfg || !path || !cfg->base_dir)
        return -1;

    /* Reject empty path */
    if (path[0] == '\0')
        return -1;

    /* Reject absolute paths */
    if (path[0] == '/')
        return -1;

    /* Reject ".." components — walk the path */
    const char *p = path;
    while (*p) {
        /* Check for ".." at start or after "/" */
        if (p[0] == '.' && p[1] == '.') {
            /* Must be followed by '/' or '\0' to be a component */
            if (p[2] == '/' || p[2] == '\0')
                return -1;
        }
        /* Advance to next component */
        const char *slash = strchr(p, '/');
        if (!slash)
            break;
        p = slash + 1;
    }

    /* Resolve the base directory (must exist) */
    char resolved_base[PATH_MAX];
    if (realpath(cfg->base_dir, resolved_base) == NULL)
        return -1; /* base dir must exist */

    /* Build full path */
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", resolved_base, path);
    if (n < 0 || (size_t)n >= sizeof(full))
        return -1;

    /* Walk up the path to find the deepest existing ancestor,
     * resolve it, and verify it's under base_dir. */
    char probe[PATH_MAX];
    strncpy(probe, full, sizeof(probe) - 1);
    probe[sizeof(probe) - 1] = '\0';

    char resolved[PATH_MAX];
    while (realpath(probe, resolved) == NULL) {
        char *slash = strrchr(probe, '/');
        if (!slash || slash == probe)
            return -1; /* exhausted all ancestors */
        *slash = '\0';
    }

    /* Verify the resolved ancestor starts with resolved base */
    size_t base_len = strlen(resolved_base);
    if (strncmp(resolved, resolved_base, base_len) != 0)
        return -1;

    /* Must be followed by '/' or be exactly the base dir */
    if (resolved[base_len] != '/' && resolved[base_len] != '\0')
        return -1;

    return 0;
}

/* ── Internal: build full path ──────────────────────────────────────── */

static int build_path(const HlFsConfig *cfg, const char *path,
                      char *out, size_t out_size)
{
    if (hl_cap_fs_validate(cfg, path) != 0)
        return -1;

    /* Use resolved base_dir to avoid TOCTOU with symlinks.
     * hl_cap_fs_validate already verified base_dir resolves. */
    char resolved_base[PATH_MAX];
    if (realpath(cfg->base_dir, resolved_base) == NULL)
        return -1;

    int n = snprintf(out, out_size, "%s/%s", resolved_base, path);
    if (n < 0 || (size_t)n >= out_size)
        return -1;

    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────── */

int64_t hl_cap_fs_read(const HlFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size)
{
    int64_t result = -1;

    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full)) != 0)
        goto audit;

    FILE *f = fopen(full, "rb");
    if (!f)
        goto audit;

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        goto audit;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        goto audit;
    }

    /* If buf is NULL, just return the size */
    if (!buf) {
        fclose(f);
        result = (int64_t)size;
        goto audit;
    }

    if ((size_t)size > buf_size) {
        fclose(f);
        goto audit; /* buffer too small */
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        goto audit;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    int read_err = ferror(f);
    fclose(f);

    if (read_err || nread != (size_t)size)
        goto audit;

    result = (int64_t)nread;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.read");
        sh_json_write_kv_string(&w, "path", path);
        if (result >= 0)
            sh_json_write_kv_int(&w, "bytes", result);
        sh_json_write_kv_int(&w, "result", result >= 0 ? 0 : -1);
        hl_audit_end(&w);
    }
    return result;
}

int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len)
{
    int result = -1;

    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full)) != 0)
        goto audit;

    /* Create parent directories if needed */
    char tmp[PATH_MAX];
    strncpy(tmp, full, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755); /* ignore errors — may already exist */
            *p = '/';
        }
    }

    {
        FILE *f = fopen(full, "wb");
        if (!f)
            goto audit;

        if (len > 0 && data) {
            size_t written = fwrite(data, 1, len, f);
            if (written != len) {
                fclose(f);
                goto audit;
            }
        }

        fclose(f);
        result = 0;
    }

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.write");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "len", (int64_t)len);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

int hl_cap_fs_exists(const HlFsConfig *cfg, const char *path)
{
    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full)) != 0)
        return -1;

    return access(full, F_OK) == 0 ? 1 : 0;
}

int hl_cap_fs_delete(const HlFsConfig *cfg, const char *path)
{
    int result = -1;

    char full[PATH_MAX];
    if (build_path(cfg, path, full, sizeof(full)) != 0)
        goto audit;

    if (unlink(full) != 0)
        goto audit;

    result = 0;

audit:
    {
        ShJsonWriter w = hl_audit_begin("fs.delete");
        sh_json_write_kv_string(&w, "path", path);
        sh_json_write_kv_int(&w, "result", result);
        hl_audit_end(&w);
    }
    return result;
}

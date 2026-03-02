/*
 * static.c — Static file serving middleware for Hull
 *
 * Serves files from the /static/ prefix. Build mode uses embedded entries;
 * dev mode reads from the filesystem with sendfile (zero-copy).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/static.h"

#include <keel/request.h>
#include <keel/response.h>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── MIME type lookup ─────────────────────────────────────────────── */

typedef struct {
    const char *ext;
    size_t      ext_len;
    const char *mime;
} MimeEntry;

static const MimeEntry mime_table[] = {
    { ".html",  5, "text/html; charset=utf-8" },
    { ".css",   4, "text/css" },
    { ".js",    3, "application/javascript" },
    { ".json",  5, "application/json" },
    { ".txt",   4, "text/plain; charset=utf-8" },
    { ".xml",   4, "application/xml" },
    { ".svg",   4, "image/svg+xml" },
    { ".png",   4, "image/png" },
    { ".jpg",   4, "image/jpeg" },
    { ".jpeg",  5, "image/jpeg" },
    { ".gif",   4, "image/gif" },
    { ".webp",  5, "image/webp" },
    { ".ico",   4, "image/x-icon" },
    { ".woff",  5, "font/woff" },
    { ".woff2", 6, "font/woff2" },
    { ".ttf",   4, "font/ttf" },
    { ".pdf",   4, "application/pdf" },
    { ".mp4",   4, "video/mp4" },
    { ".webm",  5, "video/webm" },
    { ".map",   4, "application/json" },
    { NULL, 0, NULL },
};

const char *hl_static_mime_type(const char *path, size_t path_len)
{
    /* Find last '.' in path */
    const char *dot = NULL;
    for (size_t i = path_len; i > 0; i--) {
        if (path[i - 1] == '.') {
            dot = path + i - 1;
            break;
        }
        if (path[i - 1] == '/')
            break;
    }
    if (!dot)
        return "application/octet-stream";

    size_t ext_len = path_len - (size_t)(dot - path);

    for (const MimeEntry *m = mime_table; m->ext; m++) {
        if (m->ext_len == ext_len &&
            strncasecmp(dot, m->ext, ext_len) == 0)
            return m->mime;
    }
    return "application/octet-stream";
}

/* ── Path safety ──────────────────────────────────────────────────── */

static int path_is_safe(const char *rel, size_t len)
{
    /* Reject null bytes */
    if (memchr(rel, '\0', len) != NULL)
        return 0;

    /* Reject ".." anywhere in path */
    for (size_t i = 0; i + 1 < len; i++) {
        if (rel[i] == '.' && rel[i + 1] == '.') {
            /* Must be bounded by / or at start/end */
            int left  = (i == 0 || rel[i - 1] == '/');
            int right = (i + 2 >= len || rel[i + 2] == '/');
            if (left && right)
                return 0;
        }
    }

    /* Reject leading / (absolute path escape) */
    if (len > 0 && rel[0] == '/')
        return 0;

    return 1;
}

/* ── ETag helpers ─────────────────────────────────────────────────── */

static int format_etag_embedded(char *buf, size_t cap, size_t content_len)
{
    return snprintf(buf, cap, "W/\"%zx\"", content_len);
}

static int format_etag_file(char *buf, size_t cap, time_t mtime, off_t size)
{
    return snprintf(buf, cap, "W/\"%lx-%llx\"",
                    (unsigned long)mtime, (unsigned long long)size);
}

static int etag_matches(const KlRequest *req, const char *etag, size_t etag_len)
{
    size_t inm_len = 0;
    const char *inm = kl_request_header_len(req, "If-None-Match", &inm_len);
    if (!inm || inm_len != etag_len)
        return 0;
    return memcmp(inm, etag, etag_len) == 0;
}

/* ── Middleware entry point ────────────────────────────────────────── */

int hl_static_middleware(KlRequest *req, KlResponse *res, void *user_data)
{
    const HlStaticCtx *ctx = (const HlStaticCtx *)user_data;

    /* Only handle GET (kl_server_use already filters, but be safe) */
    if (req->method_len != 3 || memcmp(req->method, "GET", 3) != 0)
        return 0;

    /* Must start with /static/ (8 chars minimum + at least 1 char filename) */
    if (req->path_len < 9 || memcmp(req->path, "/static/", 8) != 0)
        return 0;

    const char *rel = req->path + 8;
    size_t rel_len = req->path_len - 8;

    /* Reject unsafe paths */
    if (!path_is_safe(rel, rel_len))
        return 0;

    const char *mime = hl_static_mime_type(rel, rel_len);

    /* ── Try embedded entries (build mode) ────────────────────────── */
    if (ctx->entries) {
        for (const HlStaticEntry *e = ctx->entries; e->name; e++) {
            if (strlen(e->name) == rel_len &&
                memcmp(e->name, rel, rel_len) == 0) {
                /* ETag check */
                char etag[64];
                int elen = format_etag_embedded(etag, sizeof(etag), e->len);
                if (elen > 0 && etag_matches(req, etag, (size_t)elen)) {
                    kl_response_status(res, 304);
                    kl_response_header(res, "ETag", etag);
                    kl_response_body(res, NULL, 0);
                    return 1;
                }

                kl_response_status(res, 200);
                kl_response_header(res, "Content-Type", mime);
                kl_response_header(res, "Cache-Control", "public, max-age=86400");
                if (elen > 0)
                    kl_response_header(res, "ETag", etag);
                kl_response_body(res, (const char *)e->data, e->len);
                return 1;
            }
        }
    }

    /* ── Try filesystem (dev mode) ────────────────────────────────── */
    if (ctx->app_dir) {
        char fpath[4096];
        int n = snprintf(fpath, sizeof(fpath), "%s/static/", ctx->app_dir);
        if (n < 0 || (size_t)n + rel_len >= sizeof(fpath))
            return 0;
        memcpy(fpath + n, rel, rel_len);
        fpath[n + (int)rel_len] = '\0';

        int fd = open(fpath, O_RDONLY);
        if (fd < 0)
            return 0;

        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            return 0;
        }

        /* ETag check */
        char etag[64];
        int elen = format_etag_file(etag, sizeof(etag), st.st_mtime, st.st_size);
        if (elen > 0 && etag_matches(req, etag, (size_t)elen)) {
            close(fd);
            kl_response_status(res, 304);
            kl_response_header(res, "ETag", etag);
            kl_response_body(res, NULL, 0);
            return 1;
        }

        kl_response_status(res, 200);
        kl_response_header(res, "Content-Type", mime);
        kl_response_header(res, "Cache-Control", "no-cache");
        if (elen > 0)
            kl_response_header(res, "ETag", etag);
        kl_response_file(res, fd, st.st_size);
        return 1;
    }

    return 0;
}

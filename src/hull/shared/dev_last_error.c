/*
 * dev_last_error.c - the `hull dev --agent` diagnostic sidecars (see
 * include/hull/shared/dev_session.h): <app>/.hull/last_error.json, written by
 * the served child when the app fails to load (serve.c) and by the supervisor
 * when the build pipeline fails before a (re)spawn (dev.c), and
 * <app>/.hull/types.json, the supervisor's type check of the app (dev.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/dev_session.h"

#include "sh_json.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int stdio_write(void *ctx, const char *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)ctx) == len ? 0 : -1;
}

/* <app_dir>/.hull/<name>, created afresh: only in a real .hull directory
 * this user owns, never through a planted link (fopen "w" truncated whatever
 * .hull/last_error.json pointed at). NULL when it cannot be. */
static FILE *sidecar_create(const char *app_dir, const char *name)
{
    char dir[4096], path[4096];
    int nd = snprintf(dir, sizeof(dir), "%s/.hull", app_dir);
    int np = snprintf(path, sizeof(path), "%s/.hull/%s", app_dir, name);
    if (nd <= 0 || (size_t)nd >= sizeof(dir) || np <= 0 || (size_t)np >= sizeof(path))
        return NULL;
    mkdir(dir, 0755);
    /* The app tree may be a cloned repo. */
    struct stat dst;
    if (!(lstat(dir, &dst) == 0 && S_ISDIR(dst.st_mode)
#ifndef _WIN32
          && dst.st_uid == geteuid()
#endif
         ))
        return NULL;
    (void)unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}

void hl_dev_last_error_write(const char *app_dir, const char *nonce,
                             long session_pid, const char *msg)
{
    FILE *ef = sidecar_create(app_dir, "last_error.json");
    if (!ef) return;
    ShJsonWriter w;
    sh_json_writer_init(&w, stdio_write, ef);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "error", msg);
    int64_t ts = (int64_t)time(NULL);
    sh_json_write_kv_int(&w, "timestamp", ts);
    /* `hull agent errors` serves the record only when this MAC verifies
     * under the live session's key (audit 36 / 37). Sized from the message,
     * never a fixed stack buffer a longer message would outgrow and leave
     * the record without its MAC (C audit L11). */
    if (nonce && nonce[0]) {
        char mac[HL_DEV_SIDECAR_MAC_LEN + 1];
        size_t csz = strlen(msg) + 32;
        char *content = malloc(csz);
        int cn = content ? snprintf(content, csz, "%lld\n%s", (long long)ts, msg)
                         : -1;
        if (cn > 0 && (size_t)cn < csz &&
            hl_dev_sidecar_mac(nonce, HL_DEV_SIDECAR_LAST_ERROR, app_dir,
                               session_pid, content, (size_t)cn, mac) == 0)
            sh_json_write_kv_string(&w, "mac", mac);
        free(content);
    }
    sh_json_write_object_end(&w);
    fputc('\n', ef);
    fclose(ef);
}

void hl_dev_types_write(const char *app_dir, const char *nonce, long session_pid,
                        const char *doc, size_t len)
{
    char mac[HL_DEV_SIDECAR_MAC_LEN + 1];
    if (!nonce || !nonce[0] || len < 2 || doc[len - 1] != '}' ||
        hl_dev_sidecar_mac(nonce, HL_DEV_SIDECAR_TYPES, app_dir, session_pid,
                           doc, len, mac) != 0)
        return;
    FILE *f = sidecar_create(app_dir, "types.json");
    if (!f) return;
    /* The document without its closing brace, then the MAC member. */
    int ok = fwrite(doc, 1, len - 1, f) == len - 1 &&
             fprintf(f, "%s%s\"}\n", HL_DEV_DISCOVERY_MAC_KEY, mac) > 0;
    if (fclose(f) != 0 || !ok) hl_dev_types_remove(app_dir);
}

void hl_dev_types_remove(const char *app_dir)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/.hull/types.json", app_dir);
    struct stat st;
    if (n > 0 && (size_t)n < sizeof(path) && lstat(path, &st) == 0 && S_ISREG(st.st_mode))
        (void)unlink(path);
}

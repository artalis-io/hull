/*
 * dev_last_error.c - the `hull dev --agent` load-failure sidecar,
 * <app>/.hull/last_error.json (see include/hull/shared/dev_session.h).
 * Written by the served child when the app fails to load (serve.c) and by the
 * supervisor when the build pipeline fails before a (re)spawn (dev.c).
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

void hl_dev_last_error_write(const char *app_dir, const char *nonce,
                             long session_pid, const char *msg)
{
    char err_dir[4096], err_path[4096];
    int nd = snprintf(err_dir, sizeof(err_dir), "%s/.hull", app_dir);
    int np = snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json",
                      app_dir);
    if (nd <= 0 || (size_t)nd >= sizeof(err_dir) ||
        np <= 0 || (size_t)np >= sizeof(err_path))
        return;
    mkdir(err_dir, 0755);
    /* The app tree may be a cloned repo: write only into a real .hull
     * directory of ours, and never through a planted link (fopen "w"
     * truncated whatever .hull/last_error.json pointed at). */
    struct stat dst;
    FILE *ef = NULL;
    if (lstat(err_dir, &dst) == 0 && S_ISDIR(dst.st_mode)
#ifndef _WIN32
        && dst.st_uid == geteuid()
#endif
       ) {
        (void)unlink(err_path);
        int efd = open(err_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                       O_CLOEXEC, 0644);
        if (efd >= 0) {
            ef = fdopen(efd, "w");
            if (!ef) close(efd);
        }
    }
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

/*
 * agent/session.c - bind <app>/.hull sidecars to a live `hull dev --agent`
 * session.
 *
 * The app tree may be a cloned repo, and a repo (or the app running under
 * the session) can write any file under .hull/. The dev supervisor keeps its
 * session nonce outside the tree ($HOME/.hull/dev-sessions/<pid>, see
 * commands/dev.c); a sidecar is Hull's own output only when it carries a MAC
 * keyed by that nonce (include/hull/shared/dev_session.h; audit 36 / 37).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/shared/dev_session.h"
#include "hull/shared/host.h"   /* hl_host_home, hl_host_is_windows */
#include "hull/utils/secure_zero.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

char *hl_agent_read_sidecar(const char *path, size_t cap, size_t *out_len)
{
    FILE *f = hl_agent_open_sidecar(path);   /* no link, no FIFO (audit 22) */
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > cap) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    int bad = (got != (size_t)sz) || ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

long hl_agent_json_int_field(const char *buf, const char *key)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) return -1;
    const char *p = strstr(buf, needle);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (errno != 0 || end == p) return -1;
    if (*end != ',' && *end != '}' && *end != ' ' && *end != '\t' &&
        *end != '\n' && *end != '\r' && *end != '\0') return -1;
    if (v <= 0 || v > INT_MAX) return -1;
    return v;
}

int hl_agent_dev_session(const char *app_dir, char *nonce_out,
                         long *session_pid, int *port)
{
    const char *dir = app_dir ? app_dir : ".";
    char dev_path[PATH_MAX];
    int b = snprintf(dev_path, sizeof(dev_path), "%s/.hull/dev.json", dir);
    if (b < 0 || (size_t)b >= sizeof(dev_path)) return -1;

    char *dev = hl_agent_read_sidecar(dev_path, 64 * 1024, NULL);
    if (!dev) return -1;
    long sp = hl_agent_json_int_field(dev, "session_pid");
    long pt = hl_agent_json_int_field(dev, "port");
    long cp = hl_agent_json_int_field(dev, "pid");
    long st = hl_agent_json_int_field(dev, "started_at");
    char mac[HL_DEV_SIDECAR_MAC_LEN + 1] = "";
    const char *p = strstr(dev, "\"mac\":\"");
    if (p) {
        p += 7;
        size_t i = 0;
        while (i < HL_DEV_SIDECAR_MAC_LEN &&
               ((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) {
            mac[i] = p[i];
            i++;
        }
        mac[i] = '\0';
        if (i != HL_DEV_SIDECAR_MAC_LEN || p[i] != '"') mac[0] = '\0';
    }
    free(dev);
    if (sp <= 0 || pt <= 0 || cp <= 0 || st <= 0 || !mac[0]) return -1;

    /* The key: the nonce the supervisor keeps under $HOME. It is in no file
     * under the app tree (audit 37: dev.json carried it, so an app granted
     * .hull/ could stamp it into forged sidecars). */
    const char *home = hl_host_home();
    if (!home || !*home) return -1;
    char kp[PATH_MAX];
    int n = snprintf(kp, sizeof kp, "%s/.hull/dev-sessions/%ld", home, sp);
    if (n < 0 || (size_t)n >= sizeof kp) return -1;
    size_t kl = 0;
    char *kept = hl_agent_read_sidecar(kp, 128, &kl);
    char nonce[HL_DEV_SESSION_NONCE_LEN + 1] = "";
    if (kept && kl == HL_DEV_SESSION_NONCE_LEN) {
        memcpy(nonce, kept, HL_DEV_SESSION_NONCE_LEN);
        nonce[HL_DEV_SESSION_NONCE_LEN] = '\0';
    }
    if (kept) { hl_secure_zero(kept, kl); free(kept); }
    if (!hl_dev_session_nonce_valid(nonce)) return -1;

    /* dev.json is this session's, for this app directory: its MAC binds the
     * port / pid / start time to the supervisor pid and the canonical app dir
     * (a dev.json copied from another tree, or naming another live session's
     * pid, does not verify). */
    char content[128];
    n = snprintf(content, sizeof content, "port=%ld pid=%ld started_at=%ld",
                 pt, cp, st);
    if (n < 0 || (size_t)n >= sizeof content ||
        !hl_dev_sidecar_mac_ok(nonce, HL_DEV_SIDECAR_DEV_JSON, dir, sp,
                               content, (size_t)n, mac)) {
        hl_secure_zero(nonce, sizeof nonce);
        return -1;
    }

    /* kill(pid, 0) terminates the process on Windows (cosmo; see
     * agent_inspect_stream_live): there the record alone binds. The
     * supervisor removes its $HOME copy when it exits, and a new session
     * prunes records whose pid is gone. */
    if (!hl_host_is_windows() && kill((pid_t)sp, 0) != 0) {
        hl_secure_zero(nonce, sizeof nonce);
        return -1;
    }

    if (nonce_out) memcpy(nonce_out, nonce, sizeof nonce);
    hl_secure_zero(nonce, sizeof nonce);
    if (session_pid) *session_pid = sp;
    if (port) *port = pt <= 65535 ? (int)pt : 0;
    return 0;
}

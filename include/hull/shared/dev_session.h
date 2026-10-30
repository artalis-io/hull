/*
 * shared/dev_session.h - the `hull dev --agent` session key and the MAC on
 * the sidecars it writes.
 *
 * The dev supervisor makes a random 64-hex nonce per session and keeps it at
 * $HOME/.hull/dev-sessions/<supervisor pid> - outside the app tree, which may
 * be a cloned repo, and outside the app's reach. The nonce is a KEY: it is in
 * no file under the app tree (audit 37: dev.json carried it, and an app
 * granted fs access to .hull/ read it and stamped it into forged sidecars).
 *
 * Every sidecar Hull writes under <app>/.hull carries an HMAC-SHA256 keyed by
 * that nonce (hl_dev_sidecar_mac) over a fixed label, the sidecar kind, the
 * canonical app directory, the supervisor pid and the exact content:
 *   dev.json        - the supervisor (commands/dev.c);
 *   last_error.json - the served child (serve.c), which gets the nonce
 *                     through HL_DEV_SESSION_ENV, scrubs and unsets it before
 *                     any app code runs and keeps it only in C memory until
 *                     the app has loaded;
 *   discovery.json  - the publisher (`hull agent inspect --generation`), which
 *                     takes it the same way before its tool VM starts.
 * `hull agent errors` / `status` / `inspect` read the key from $HOME and
 * verify the MAC in constant time before serving anything.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_DEV_SESSION_H
#define HL_SHARED_DEV_SESSION_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hull/cap/crypto.h"          /* hl_cap_crypto_sha256 / hmac_sha256 */
#include "hull/utils/secure_zero.h"   /* hl_secure_zero */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define HL_DEV_SESSION_ENV "HULL_DEV_SESSION"
#define HL_DEV_SESSION_NONCE_LEN 64
#define HL_DEV_SIDECAR_MAC_LEN 64           /* hex */
#define HL_DEV_SIDECAR_LABEL "hull-dev-sidecar-v1"

/* Sidecar kinds (a field of the MAC'd message). */
#define HL_DEV_SIDECAR_DEV_JSON   "dev.json"
#define HL_DEV_SIDECAR_LAST_ERROR "last_error.json"
#define HL_DEV_SIDECAR_DISCOVERY  "discovery.json"

/* The JSON member that carries a discovery generation's MAC: always the LAST
 * member of the object, so the MAC'd document is the file with
 * `,"session_mac":"<64 hex>"` removed before the closing brace. */
#define HL_DEV_DISCOVERY_MAC_KEY ",\"session_mac\":\""

/* 1 iff @p s is exactly 64 lowercase hex digits followed by NUL. */
static inline int hl_dev_session_nonce_valid(const char *s)
{
    if (!s) return 0;
    for (size_t i = 0; i < HL_DEV_SESSION_NONCE_LEN; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return s[HL_DEV_SESSION_NONCE_LEN] == '\0';
}

/* Take the session nonce out of the environment: copy it to @p out (65
 * bytes, "" when absent or malformed), overwrite the value in place - the
 * string getenv returns is the process's own environment block, which
 * /proc/<pid>/environ and KERN_PROCARGS2 read back even after unsetenv - and
 * unset it. Call before any app code (or tool VM) runs. */
static inline void hl_dev_session_take_env(char out[HL_DEV_SESSION_NONCE_LEN + 1])
{
    out[0] = '\0';
    char *ds = getenv(HL_DEV_SESSION_ENV);
    if (ds) {
        if (hl_dev_session_nonce_valid(ds))
            memcpy(out, ds, HL_DEV_SESSION_NONCE_LEN + 1);
        hl_secure_zero(ds, strlen(ds));
    }
    unsetenv(HL_DEV_SESSION_ENV);
}

/* HMAC-SHA256 keyed by @p nonce over
 *   LABEL \0 kind \0 realpath(app_dir) \0 session_pid \0 SHA256(content)
 * into @p out_hex (65 bytes, lowercase hex). The app directory is
 * canonicalized, so a sidecar copied into another app tree does not verify
 * there. Returns 0, or -1 (bad nonce, app_dir that does not resolve). */
static inline int hl_dev_sidecar_mac_raw(const char *nonce, const char *kind,
                                         const char *app_dir, long session_pid,
                                         const void *content, size_t content_len,
                                         uint8_t out[32])
{
    if (!hl_dev_session_nonce_valid(nonce) || !kind || !app_dir ||
        session_pid <= 0 || (!content && content_len))
        return -1;
    char canon[PATH_MAX];
    if (!realpath(app_dir, canon)) return -1;
    uint8_t digest[32];
    if (hl_cap_crypto_sha256(content ? content : "", content_len, digest) != 0)
        return -1;
    char head[PATH_MAX + 128];
    int n = snprintf(head, sizeof head, "%s%c%s%c%s%c%ld%c",
                     HL_DEV_SIDECAR_LABEL, 0, kind, 0, canon, 0, session_pid, 0);
    if (n < 0 || (size_t)n + sizeof digest > sizeof head) return -1;
    memcpy(head + n, digest, sizeof digest);
    int rc = hl_cap_crypto_hmac_sha256((const uint8_t *)nonce,
                                       HL_DEV_SESSION_NONCE_LEN,
                                       (const uint8_t *)head,
                                       (size_t)n + sizeof digest, out);
    return rc == 0 ? 0 : -1;
}

static inline int hl_dev_sidecar_mac(const char *nonce, const char *kind,
                                     const char *app_dir, long session_pid,
                                     const void *content, size_t content_len,
                                     char out_hex[HL_DEV_SIDECAR_MAC_LEN + 1])
{
    uint8_t mac[32];
    out_hex[0] = '\0';
    if (hl_dev_sidecar_mac_raw(nonce, kind, app_dir, session_pid,
                               content, content_len, mac) != 0)
        return -1;
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[2 * i]     = hx[mac[i] >> 4];
        out_hex[2 * i + 1] = hx[mac[i] & 15];
    }
    out_hex[HL_DEV_SIDECAR_MAC_LEN] = '\0';
    hl_secure_zero(mac, sizeof mac);
    return 0;
}

/* Constant-time check of @p mac_hex (64 lowercase hex chars; need not be
 * NUL-terminated after them) against the MAC of the given fields. 1 = valid,
 * 0 = anything else. */
static inline int hl_dev_sidecar_mac_ok(const char *nonce, const char *kind,
                                        const char *app_dir, long session_pid,
                                        const void *content, size_t content_len,
                                        const char *mac_hex)
{
    if (!mac_hex) return 0;
    uint8_t want[32];
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = mac_hex[2 * i + k];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else return 0;
            v = (v << 4) | d;
        }
        want[i] = (uint8_t)v;
    }
    uint8_t got[32];
    if (hl_dev_sidecar_mac_raw(nonce, kind, app_dir, session_pid,
                               content, content_len, got) != 0)
        return 0;
    unsigned diff = 0;
    for (int i = 0; i < 32; i++) diff |= (unsigned)(got[i] ^ want[i]);
    hl_secure_zero(got, sizeof got);
    return diff == 0;
}

#endif /* HL_SHARED_DEV_SESSION_H */

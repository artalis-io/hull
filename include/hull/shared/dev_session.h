/*
 * shared/dev_session.h - the `hull dev --agent` session nonce.
 *
 * The dev supervisor makes a random 64-hex nonce per session and keeps a copy
 * at $HOME/.hull/dev-sessions/<supervisor pid> - outside the app tree, which
 * may be a cloned repo. dev.json carries it; the served child gets it through
 * HL_DEV_SESSION_ENV (unset before any app code runs) and stamps it into
 * .hull/last_error.json. `hull agent inspect` / `errors` / `status` trust a
 * sidecar only when its nonce matches the $HOME copy (audit 36): a repo that
 * ships its own .hull/ files cannot also plant one under $HOME.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_DEV_SESSION_H
#define HL_SHARED_DEV_SESSION_H

#include <stddef.h>

#define HL_DEV_SESSION_ENV "HULL_DEV_SESSION"
#define HL_DEV_SESSION_NONCE_LEN 64

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

#endif /* HL_SHARED_DEV_SESSION_H */

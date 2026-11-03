/*
 * Pre-fix stand-in for src/hull/cap/conn_limit.c, linked only by
 * scripts/check_regression_test.sh (docs/design/audit_convergence.md 5.1).
 * Before the fix nothing counted open connections and Keel's read timeout
 * was the only deadline: every connection is admitted, none is counted and
 * none expires.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/conn_limit.h"

#include <string.h>

int hl_conn_limit_init(HlConnLimit *l, int max_connections, uint32_t per_ip,
                       uint32_t header_ms, uint32_t idle_ms)
{
    (void)max_connections;
    memset(l, 0, sizeof *l);
    l->per_ip = per_ip;
    l->header_ms = header_ms;
    l->idle_ms = idle_ms;
    return 0;
}
void hl_conn_limit_free(HlConnLimit *l) { (void)l; }
int hl_conn_limit_open(HlConnLimit *l, int fd, const HlReqKey *key, uint64_t now_ms)
{
    (void)l; (void)fd; (void)key; (void)now_ms;
    return HL_CONN_ADMIT;
}
void hl_conn_limit_close(HlConnLimit *l, int fd) { (void)l; (void)fd; }
void hl_conn_limit_bytes_in(HlConnLimit *l, int fd, uint64_t now_ms)
{
    (void)l; (void)fd; (void)now_ms;
}
void hl_conn_limit_request(HlConnLimit *l, int fd) { (void)l; (void)fd; }
void hl_conn_limit_response_done(HlConnLimit *l, int fd, int status, uint64_t now_ms)
{
    (void)l; (void)fd; (void)status; (void)now_ms;
}
int hl_conn_limit_sweep(HlConnLimit *l, uint64_t now_ms, HlConnExpireFn fn, void *user)
{
    (void)l; (void)now_ms; (void)fn; (void)user;
    return 0;
}
uint32_t hl_conn_limit_count(HlConnLimit *l, const HlReqKey *key)
{
    (void)l; (void)key;
    return 0;
}
int hl_conn_limit_phase(HlConnLimit *l, int fd)
{
    (void)l; (void)fd;
    return HL_CONN_NONE;
}

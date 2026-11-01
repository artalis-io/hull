/*
 * vmt_http.h - the HTTP side of --vm-thread (cap/vm_thread.h), shared by the
 * Lua and JS dispatch bridges. Header-only: it touches Keel's request /
 * response structs, which the Keel-free base (cap/vm_thread.c) must not.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_VMT_HTTP_H
#define HL_SHARED_VMT_HTTP_H

#include <keel/http_request.h>
#include <keel/http_response.h>

#include "hull/cap/vm_thread.h"
#include "hull/shared/res_headers.h"   /* hl_res_error_reset */

/* What standby may need about the request, read on the loop thread BEFORE
 * the hand-off: from then until the run is done the response is the VM
 * thread's. A raw 503 may go straight to the socket only on a plaintext
 * HTTP/1.x connection whose response has not started (TLS and HTTP/2 frame
 * their bytes; an SSE entry streams on its socket - handlers and middleware
 * never do). */
static inline void hl_vmt_http_capture(HlVmtHttp *out, const KlHttpRequest *req,
                                       const KlHttpResponse *res)
{
    out->fd = -1;
    out->early_ok = 0;
    if (!res) return;
    out->fd = (int)res->conn_fd;
    out->early_ok = req && req->version_major == 1 && res->tls == NULL &&
                    !res->headers_sent && res->body_mode != KL_HTTP_BODY_STREAM &&
                    out->fd >= 0;
}

/* The entry's run was abandoned and has now finished (on the loop thread,
 * after the runtime's own bookkeeping): answer 503 and close. When the 503
 * already went out raw (@p early), the socket's write side is shut, so what
 * Keel sends now fails and the connection closes; otherwise this is the
 * answer (TLS, HTTP/2). */
static inline void hl_vmt_http_finish_abandoned(KlHttpRequest *req,
                                                KlHttpResponse *res, int early)
{
    if (req) req->keep_alive = 0;   /* Keel closes after this response */
    if (!res) return;
    if (!early && !res->headers_sent) {
        hl_res_error_reset(res, 503, "Service Unavailable\n", 20);
        (void)kl_http_response_header(res, "Retry-After", "1");
    }
    res->keep_alive = 0;
}

#endif /* HL_SHARED_VMT_HTTP_H */

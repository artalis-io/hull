/*
 * cap/ssh.c - open the stream an SSH session runs over (see cap/ssh.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <string.h>

#include "hull/cap/ssh.h"
#include "hull/cap/ssh_policy.h"

#include "log.h"

int hl_cap_ssh_open(const HlSshEnv *env, const HlSshOpen *req,
                    HlNetStream **out, const char **why)
{
    if (out) *out = NULL;
    if (why) *why = NULL;
    if (!env || !req || !out) return HL_NET_E_INVAL;

    /* The grants, before any name resolution and before a socket exists.
     *
     * The DESTINATION first even when a relay is in play: refusing on the
     * machine the app asked to reach is the more informative answer, and it
     * keeps the relay from being probed by an app that may not reach the
     * host behind it anyway. */
    HlSshAuth auth = hl_ssh_check_connect(env->policy, req->host, req->port,
                                          req->user);
    if (auth == HL_SSH_ALLOW && req->via)
        auth = hl_ssh_check_tunnel(env->policy, req->via_host, req->via_port);
    if (auth != HL_SSH_ALLOW) {
        if (why) *why = hl_ssh_auth_reason(auth);
        return HL_NET_E_DENIED;
    }

    HlNetStreamConfig cfg;
    KlAllocator       kalloc;
    memset(&cfg, 0, sizeof cfg);
    cfg.async      = env->async;
    cfg.pool       = env->pool;
    cfg.host       = req->via ? req->via_host : req->host;
    cfg.port       = req->via ? req->via_port : req->port;
    cfg.connect_ms = req->connect_ms;

    if (req->via && req->via_tls) {
        /* Refuse rather than downgrade. A caller that asked for an encrypted
         * relay and quietly got a plaintext one would never find out, and the
         * credentials in a tunnel's headers are exactly what the TLS is
         * protecting. NULL here means this build composed no TLS feature, or
         * this invocation resolved no CA bundle. */
        const HlClientTls *t = env->client_tls;
        if (!t || !t->cfg) {
            if (why)
                *why = "ssh: a TLS tunnel was requested but this build has no "
                       "TLS trust anchor (no CA bundle resolved, or TLS is not "
                       "composed into this binary)";
            return HL_NET_E_TLS;
        }
        if (!t->verifies)
            /* Allowed - --no-ca-bundle is a development switch - but said
             * here, where it matters: the relay's certificate is not checked,
             * and the tunnel's headers carry its credentials to whoever
             * answers. */
            log_warn("[hull:ssh] relay %s:%d: certificate NOT verified "
                     "(--no-ca-bundle); tunnel credentials go to whoever "
                     "answers", req->via_host, req->via_port);
        cfg.tls       = t->cfg;
        /* The runtime's own allocator, bridged. Stack-local is safe because
         * the transport COPIES the KlAllocator by value - and the HlAllocator
         * it points through is the runtime's, which outlives every stream
         * made from it. hl_alloc_kl rather than kl_allocator_default keeps
         * this file free of a libkeel symbol. */
        kalloc        = hl_alloc_kl(env->alloc);
        cfg.tls_alloc = &kalloc;
        /* Default the certificate name to the relay's host, which is what a
         * caller wants unless it dialled an address and expects another
         * name. Never the SSH destination: the relay presents its own. */
        cfg.tls_hostname = req->via_tls_hostname;
    }

    return hl_net_stream_connect(out, &cfg);
}

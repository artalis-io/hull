/*
 * cap/ssh.c - open the stream an SSH session runs over (see cap/ssh.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdatomic.h>
#include <string.h>

#include "hull/cap/ssh.h"
#include "hull/cap/ssh_policy.h"

#include "log.h"

/* Live streams (HL_SSH_MAX_LIVE_STREAMS, audit 35). Each stream holds a
 * socket - up to NET_MAX_ADDRS (8) while the connect races addresses - until
 * it is closed or collected, and a coroutine parked on one anchors its handle
 * so the collector frees nothing: with no count, app code pacing connects to
 * a host that accepts and stalls filled the process's descriptor table, and
 * Keel stopped accepting. Process-wide and atomic like kv.open's count; a
 * slot is taken before anything is dialled and given back by the stream's
 * release hook, once its sockets are closed. */
static atomic_int g_ssh_live;

#define SSH_STR_(x) #x
#define SSH_XSTR(x) SSH_STR_(x)

static int ssh_slot_take(void)
{
    int n = atomic_load(&g_ssh_live);
    do {
        if (n >= HL_SSH_MAX_LIVE_STREAMS) return -1;
    } while (!atomic_compare_exchange_weak(&g_ssh_live, &n, n + 1));
    return 0;
}

static void ssh_slot_give(void *unused)
{
    (void)unused;
    atomic_fetch_sub(&g_ssh_live, 1);
}

int hl_cap_ssh_live_streams(void) { return atomic_load(&g_ssh_live); }

/* A name a relay header or a resolver can take as one host: letters,
 * digits, '-', '.', '_' (a DNS name), or an IP literal (hex digits, ':' and
 * '.'). A grant matched by glob ("*.example.com") admitted
 * "evil,a.example.com", which the stdlib then wrote into the relay's
 * destination header, where a relay splitting on ',' read "evil" (audit 39).
 * NULL (absent) is left to the grant check. */
static int ssh_host_ok(const char *h)
{
    if (!h) return 1;
    size_t n = strlen(h);
    if (n == 0 || n > 253) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)h[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == ':')
            continue;
        return 0;
    }
    return 1;
}

int hl_cap_ssh_open(const HlSshEnv *env, const HlSshOpen *req,
                    HlNetStream **out, const char **why)
{
    if (out) *out = NULL;
    if (why) *why = NULL;
    if (!env || !req || !out) return HL_NET_E_INVAL;

    if (!ssh_host_ok(req->host) ||
        (req->via && (!ssh_host_ok(req->via_host) ||
                      (req->via_tls_hostname && req->via_tls_hostname[0] &&
                       !ssh_host_ok(req->via_tls_hostname))))) {
        if (why) *why = "host must be a host name or an IP address";
        return HL_NET_E_DENIED;
    }

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
    /* The relay's NAME is granted too: it is the TLS SNI, the name its
     * certificate is checked against, and the upgrade's Host - what a CDN or
     * a shared-IP virtual host routes by. Checked only by address, an app
     * granted one relay behind a CDN reached any origin on it (audit 21). */
    if (auth == HL_SSH_ALLOW && req->via && req->via_tls_hostname &&
        req->via_tls_hostname[0])
        auth = hl_ssh_check_tunnel(env->policy, req->via_tls_hostname,
                                   req->via_port);
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

    /* After every refusal above, so a denied or TLS-less open costs no slot,
     * and before the dial, so a refused one opens no socket. */
    if (ssh_slot_take() != 0) {
        if (why)
            *why = "ssh: too many open SSH connections (at most "
                   SSH_XSTR(HL_SSH_MAX_LIVE_STREAMS) "; close some first)";
        return HL_NET_E_LIMIT;
    }
    cfg.on_release  = ssh_slot_give;
    cfg.release_arg = NULL;

    int rc = hl_net_stream_connect(out, &cfg);
    /* A stream handed out gives the slot back through on_release; one that
     * was never made does not, so it is given back here. */
    if (!*out) ssh_slot_give(NULL);
    return rc;
}

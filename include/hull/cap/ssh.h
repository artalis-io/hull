/**
 * @file cap/ssh.h
 * @brief Open the byte stream an SSH session runs over: grant, relay, TLS, dial.
 *
 * The capability half of hull/ssh. A runtime binding parses its arguments
 * into an HlSshOpen, hands over what the host resolved (the manifest's `ssh`
 * grant, the outbound TLS trust, the loop and pool), and gets back either a
 * connecting stream or a refusal. Everything a binding used to decide inline -
 * which grants to check and in what order, when a TLS relay is refused, which
 * name the relay's certificate is checked against - is decided here, once, so
 * a second runtime's binding cannot decide it differently.
 *
 * Denial happens before anything observable from the network: the grants are
 * checked, and a TLS relay's trust anchor looked up, before a name is
 * resolved or a socket exists.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_CAP_SSH_H
#define HULL_CAP_SSH_H

#include "hull/cap/net_stream.h"
#include "hull/manifest.h"
#include "hull/tls_transport.h"   /* HlClientTls */
#include "hull/utils/alloc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the caller asked for. Strings are borrowed for the duration of the
 * call only (the transport copies what it keeps). */
typedef struct HlSshOpen {
    /* The SSH destination, whether or not a relay is used: what the grant,
     * the host key and the login are about. */
    const char *host;
    int         port;
    const char *user;
    int         connect_ms;          /* whole-connect deadline, > 0 */

    /* The relay a socket is actually opened to. `via` says one was asked
     * for, separately from its host: a relay with no host must be refused by
     * the tunnel grant, never read as "connect directly" - the caller's
     * credentials may live in that relay's headers. */
    int         via;
    const char *via_host;
    int         via_port;
    int         via_tls;             /* wrap the relay connection in TLS */
    const char *via_tls_hostname;    /* certificate name; NULL = via_host */
} HlSshOpen;

/* What the host resolved, all borrowed. */
typedef struct HlSshEnv {
    const HlManifestSsh        *policy;      /* NULL denies everything */
    const HlClientTls          *client_tls;  /* NULL: no trust anchor  */
    HlAllocator                *alloc;       /* outlives every stream  */
    struct HlAsyncBackendCtx   *async;
    struct HlAsyncBackendPool  *pool;
} HlSshEnv;

/**
 * Authorize and start one connection.
 *
 * @param out  set to the connecting stream on success; finish it with
 *             hl_net_stream_connect_result. NULL on every refusal.
 * @param why  set to a message for the two refusals below; NULL otherwise
 * @return
 *   - HL_NET_OK or HL_NET_E_AGAIN with *out set: the connect is under way
 *   - HL_NET_E_DENIED: a grant refused it; *why names the rule (the SSH
 *     destination is checked first, then the relay)
 *   - HL_NET_E_TLS with *why set: a TLS relay was asked for and this
 *     invocation has no trust anchor. Refused rather than downgraded -
 *     the tunnel's headers carry credentials
 *   - any other HL_NET_E_* from the transport, *why NULL
 */
int hl_cap_ssh_open(const HlSshEnv *env, const HlSshOpen *req,
                    HlNetStream **out, const char **why);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CAP_SSH_H */

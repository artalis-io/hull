/*
 * ca_trust.h - resolve the outbound TLS trust anchor, once, for every entry point.
 *
 * serve.c (the KlServer loop) and serve_cli.c (the Keel-free app.main runner)
 * both need the same answer to "which CA bundle does this invocation trust",
 * and each used to walk its own copy of the ladder. Two copies drifted: the CLI
 * path once took only the embedded bundle, so --ca-bundle looked accepted and
 * did nothing there. This is the one copy.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CA_TRUST_H
#define HL_CA_TRUST_H

#include "hull/tls_transport.h"   /* KlTlsCtx, KlAllocator */

/**
 * Build the client TLS context an invocation's outbound connections use.
 *
 * The ladder, first match wins:
 *   1. @p skip_verify (--no-ca-bundle)  -> a context that verifies nothing
 *   2. @p override (--ca-bundle PATH)   -> that file, or nothing: an operator
 *                                          who NAMES a file has chosen the
 *                                          anchor, so a bad one fails closed
 *   3. the system CA store              -> tried, not trusted: a readable path
 *                                          whose contents do not parse falls
 *                                          through to 4
 *   4. the embedded Mozilla bundle
 *   5. none, and a warning saying how to get one
 *
 * Each step logs what it chose, as both entry points always did.
 *
 * @param out_source  optional; set to the file used, "(embedded)", or NULL
 *                    (no verification, or no anchor). Borrowed: it points at
 *                    @p override, a static path, or a literal.
 * @return the context (destroy with hl_tls_ctx_destroy), or NULL when none
 *         could be built - including when TLS is not composed into this base.
 */
KlTlsCtx *hl_ca_trust_resolve(int skip_verify, const char *override,
                              KlAllocator *alloc, const char **out_source);

#endif /* HL_CA_TRUST_H */

/*
 * cacert.h - Embedded Mozilla CA bundle (optional)
 *
 * When HL_EMBED_CA_BUNDLE is defined at build time, hull embeds a copy
 * of Mozilla's CA bundle (as distributed by curl.se) so HTTPS works in
 * environments without a system CA store: Cosmopolitan APE on Windows,
 * FROM-scratch containers, alpine, air-gapped servers.
 *
 * The bundle is part of libhull_platform.a, so apps produced by
 * `hull build` inherit it automatically.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CACERT_H
#define HL_CACERT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the embedded CA bundle (PEM, NUL-terminated, suitable for
 * mbedtls_x509_crt_parse). Returns 0 on success, -1 if no bundle is
 * embedded in this build.
 *
 *   *data  → pointer to PEM bytes (do not free - points into .rodata)
 *   *len   → length INCLUDING the trailing NUL byte
 */
int hl_embedded_ca_bundle(const unsigned char **data, size_t *len);

/**
 * @brief Path of the first readable SYSTEM CA store, or NULL if none.
 *
 * The other half of "where does a CA bundle come from", and it lives here
 * rather than in an entry point because BOTH entry points need it and a
 * second copy is how two of them drift. (`hull doctor` and `hull tools list`
 * once disagreed about the same tool on the same box for exactly that
 * reason - see shared/host.c.)
 *
 * @return a borrowed static path, or NULL when no system store is readable
 * (the normal, healthy state on Windows, where the embedded bundle is the
 * designed answer).
 */
const char *hl_ca_bundle_find_system(void);

/**
 * @brief The trust anchor per-connection TLS clients load.
 *
 * The DB / KV wire backends (shared/tls_client.c) and the async SMTP workers
 * build a TLS context per connection, from bytes, and used to take the
 * embedded bundle unconditionally - so `--ca-bundle` and the system store,
 * which the entry points resolve for http.fetch and the SSH tunnel, never
 * reached a database or a mail relay behind a private CA.
 *
 * The entry points now publish the anchor they resolved here
 * (hl_ca_trust_resolve does it), and those clients read it back. Unset, it is
 * the embedded bundle, as before.
 *
 * Set once at startup, before any connection is made; read-only after.
 *
 * @param data  the anchor's bytes (PEM NUL-terminated with len counting the
 *              NUL, or DER), or NULL for "no usable anchor": a named bundle
 *              that failed to load. Verification then fails closed rather
 *              than quietly trusting a different anchor.
 * @param owned non-zero hands @p data (from malloc) to this module, which
 *              frees it on the next set or reset.
 */
void hl_ca_bundle_set_active(const unsigned char *data, size_t len, int owned);

/** Back to the embedded bundle; frees an owned anchor. */
void hl_ca_bundle_reset_active(void);

/**
 * @brief The active anchor: the one published, else the embedded bundle.
 * @return 0 with *data / *len set, or -1 when there is none (no anchor was
 *         usable, or none is embedded and none was published).
 */
int hl_ca_bundle_active(const unsigned char **data, size_t *len);

/**
 * @brief Make the published anchor read-only for the rest of the process.
 *
 * Called once startup has resolved the anchor (both entry points, right after
 * hl_ca_trust_resolve), before any connection reads it. The descriptor, and
 * the bytes when they are a heap copy (--ca-bundle, the system store), move
 * into a sealed arena: a heap write can then neither append a root to the
 * bytes nor repoint the descriptor, so a verify-full database connection or
 * an SMTP STARTTLS cannot be turned into a MITM by one. A later
 * hl_ca_bundle_set_active is refused. Idempotent.
 *
 * @return 0, or -1 if the arena could not be mapped or sealed (startup
 * treats that as fatal, like every other policy seal).
 */
int hl_ca_bundle_seal_active(void);

/* Returns a short identifier for the embedded bundle's update date,
 * or "none" if no bundle is embedded. For doctor / version display. */
const char *hl_embedded_ca_bundle_label(void);

#ifdef __cplusplus
}
#endif

#endif /* HL_CACERT_H */

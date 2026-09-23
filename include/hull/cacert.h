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

/* Returns a short identifier for the embedded bundle's update date,
 * or "none" if no bundle is embedded. For doctor / version display. */
const char *hl_embedded_ca_bundle_label(void);

#ifdef __cplusplus
}
#endif

#endif /* HL_CACERT_H */

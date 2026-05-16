/**
 * @file cap/body.h
 * @brief Request-body reader factory + accessor.
 *
 * Hull plugs a body-reader factory into Keel so the body bytes are
 * collected during the post-body middleware phase and exposed as
 * `req.body` to handlers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_BODY_H
#define HL_CAP_BODY_H

#include <stddef.h>
#include <keel/body_reader.h>

/**
 * @brief Body-reader factory function.
 *
 * Called by Keel for each request. Returns a `KlBodyReader` configured
 * with Hull's max-body-size policy (default 1 MiB, override via
 * `--body-max-size`).
 *
 * @param alloc      Keel's per-request allocator. Pass-through.
 * @param req        Inbound request (read-only).
 * @param user_data  Opaque pointer registered with `kl_server_set_body_factory`.
 *
 * @return A configured `KlBodyReader`. The caller (Keel) owns its lifetime.
 */
KlBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data);

/**
 * @brief Get the collected body bytes from a reader.
 *
 * @param reader    Body reader (post-body phase or later).
 * @param out_data  Out-parameter: pointer to the body bytes. May contain NULs;
 *                  NOT NUL-terminated.
 *
 * @return Byte count. `0` if no body was sent.
 */
size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data);

#endif /* HL_CAP_BODY_H */

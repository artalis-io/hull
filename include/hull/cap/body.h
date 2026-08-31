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
#include <keel/http_body_reader.h>

/**
 * @brief Body-reader factory function.
 *
 * Called by Keel for each request. Returns a `KlHttpBodyReader` configured
 * with Hull's max-body-size policy (default 1 MiB, override via
 * `--body-max-size`).
 *
 * @param alloc      Keel's per-request allocator. Pass-through.
 * @param req        Inbound request (read-only).
 * @param user_data  Opaque pointer registered with `kl_http_server_set_body_factory`.
 *
 * @return A configured `KlHttpBodyReader`. The caller (Keel) owns its lifetime.
 */
KlHttpBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlHttpRequest *req,
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
size_t hl_cap_body_data(const KlHttpBodyReader *reader, const char **out_data);

/* ── Streaming multipart body reader ──────────────────────────────── */

/**
 * @brief Reason for resuming a parked handler.
 */
typedef enum {
    HL_MP_RESUME_DATA = 1,   /**< on_data fired; more bytes available */
    HL_MP_RESUME_DONE,       /**< on_complete fired; no more bytes ever */
    HL_MP_RESUME_ERROR       /**< on_error fired or parser hit ERROR */
} HlMultipartResumeReason;

/**
 * @brief Callback fired when the parked handler should be resumed.
 *
 * Runtime-agnostic: the Lua and JS bindings each register a callback
 * that wakes their respective coroutine / Promise.
 */
typedef void (*HlMultipartResumeFn)(void *ctx, HlMultipartResumeReason reason);

/**
 * @brief Streaming-multipart body-reader factory.
 *
 * Use with kl_http_server_route_streaming when the route's handler will
 * drive kl_http_multipart_next() (via req:multipart() or req.multipart()).
 *
 * The returned reader wraps Keel's kl_http_body_reader_multipart with a
 * Hull-owned slot for a "parked handler" callback. When the handler
 * yields on NEED_DATA, it registers its resume callback via
 * hl_cap_multipart_park(); subsequent on_data / on_complete / on_error
 * callbacks invoke the resume callback so the handler can pump more
 * events.
 *
 * @param alloc      Per-request allocator.
 * @param req        Inbound request (Content-Type must be multipart/form-data).
 * @param user_data  KlHttpMultipartConfig*; caller-owned. NULL → defaults
 *                   (all caps unlimited - set caps for adversarial input).
 *
 * @return Body reader, or NULL on rejection (non-multipart Content-Type,
 *         missing/oversized boundary, allocation failure → 415).
 */
KlHttpBodyReader *hl_cap_multipart_factory(KlAllocator *alloc,
                                       const KlHttpRequest *req,
                                       void *user_data);

/**
 * @brief Get the inner Keel multipart reader for kl_http_multipart_next.
 *
 * The wrapper holds the parked-handler slot but delegates parsing to
 * Keel's kl_http_body_reader_multipart. Pass this pointer to kl_http_multipart_next.
 *
 * @param wrapper  Body reader returned by hl_cap_multipart_factory.
 * @return Inner KlHttpBodyReader*, or NULL if wrapper is not a multipart wrapper.
 */
KlHttpBodyReader *hl_cap_multipart_inner(KlHttpBodyReader *wrapper);

/**
 * @brief Register a callback to fire on the next data/complete/error event.
 *
 * Called by the handler when it yields on NEED_DATA. The callback fires
 * AT MOST ONCE per park; the registration is cleared on fire. If the
 * handler yields again, it re-parks. ctx is opaque, owned by caller.
 *
 * Passing a NULL callback unparks (used when the handler is being
 * cancelled, e.g. on connection close).
 *
 * @param wrapper  Body reader returned by hl_cap_multipart_factory.
 * @param on_resume Callback fired on next event (or NULL to unpark).
 * @param ctx      Opaque caller context passed to on_resume.
 * @return 0 on success, -1 if wrapper is not a multipart wrapper.
 */
int hl_cap_multipart_park(KlHttpBodyReader *wrapper,
                          HlMultipartResumeFn on_resume,
                          void *ctx);

#endif /* HL_CAP_BODY_H */

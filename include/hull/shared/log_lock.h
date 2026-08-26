/*
 * log_lock.h - make vendored log.c thread-safe.
 *
 * vendor/log.c serializes log_log() through an optional lock callback,
 * but does nothing unless one is registered. Hull logs from worker
 * threads (db/wasm/gpu) concurrently with the event loop, so without a
 * lock those calls race on the callback list and the output stream.
 *
 * hl_log_make_threadsafe() registers a process-global mutex lock. It is
 * idempotent and cheap; call it once early on any process that may log
 * from more than one thread (the server does this in init_logging; the
 * multithreaded unit tests call it before spawning threads).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LOG_LOCK_H
#define HL_LOG_LOCK_H

void hl_log_make_threadsafe(void);

#endif /* HL_LOG_LOCK_H */

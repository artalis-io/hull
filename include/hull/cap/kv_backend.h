/*
 * cap/kv_backend.h: the non-SQL key/value backend seam (HlKvBackend).
 *
 * The FIRST non-SQL connection seam in Hull, and a LONG-LIVED ABI boundary
 * (every future non-SQL connection backend - memcached, NATS, ... - reuses it),
 * so the ownership/lifetime/threading contract below is normative, not advisory.
 *
 * RESP (Redis/Valkey) is a command protocol, not a query language, so it does
 * NOT belong behind HlDbBackend and must never be reachable by a db.query. A KV
 * backend fills the base-resident weak hook `hl_kv_feature_backends`
 * (cap/kv_feature.c) with a strong override from a composed `--with=valkey`
 * feature archive - mirroring `hl_db_feature_backends` on its own hook so the
 * two compose side by side. The vtable is a deliberately narrow, byte-oriented
 * store interface matching the milestone-1 hull.kv / hull.cache handle ops.
 * There is NO generic "send any command" escape hatch in this milestone.
 *
 * ── Values are bytes; the CALLER owns namespacing.
 *   Keys and values are arbitrary bytes (ptr+len, may contain NUL). The caller
 *   bakes the namespace prefix into every key BEFORE calling, so a KV and a
 *   cache namespace can never collide; the backend treats keys opaquely.
 *
 * ── Ownership & lifetime of returned bytes (normative).
 *   get():  on hit, *val and *vlen BORROW into the handle's internal receive
 *           buffer. Valid ONLY until the next call on the same handle (any op)
 *           or hl_kv close. The caller MUST copy before the next call. On miss,
 *           *val = NULL, *vlen = 0. Never caller-freed.
 *   scan(): each key is delivered to the callback as a BORROWED (ptr,len) valid
 *           ONLY for the duration of that callback invocation. Copy inside it.
 *   No vtable method transfers heap ownership to the caller; no returned pointer
 *   is ever free()d by the caller.
 *
 * ── Error model.
 *   Ops return 0 on success (including an expected miss), -1 on a transport /
 *   protocol / server-side failure. A human message for the last failure on a
 *   handle is available via last_error(h): a BORROWED string valid until the
 *   next call on that handle. Callers map -1 to a coded stdlib error
 *   (`backend_error`); a miss (found/present == 0) is NOT an error.
 *
 * ── Threading.
 *   A handle is NOT thread-safe and is single-owner: use it only on the thread
 *   that opened it (the event loop). The worker-pool async path (a later epic)
 *   opens its OWN per-thread handle keyed by DSN, exactly like db.async. Backend
 *   vtable instances are immutable and may be shared across threads (see rodata).
 *
 * ── Secrets.
 *   open() MUST scrub the DSN password from its own working memory after the
 *   handshake (hl_valkey_dsn_scrub) and MUST NOT retain plaintext credentials in
 *   the handle past connect. A password sent over a plaintext redis:// (no TLS)
 *   logs a one-time warning pointing at rediss://.
 *
 * ── Immutability (rodata / §5b).
 *   Every concrete HlKvBackend instance is a `static const` dispatch table that
 *   lands in read-only .rodata - there is NO writable dispatch state. Only the
 *   opaque HlKvHandle (per-connection runtime state) is mutable, and it is not a
 *   dispatch table. The weak hook returns a `const`-qualified array of `const`
 *   backend pointers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_KV_BACKEND_H
#define HL_CAP_KV_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "hull/utils/alloc.h"   /* HlAllocator (pluggable allocator) */

/*
 * Capability bits a backend advertises, and their EXACT mapping to the vtable.
 * A method whose gating cap is clear is left NULL and MUST NOT be called; the
 * generic cap layer checks caps and raises `unsupported` instead. The four
 * "declarative" caps gate no method - they describe backend nature and are
 * surfaced verbatim in the handle.caps table.
 *
 *   method-gating caps
 *     HL_KV_CAP_TTL               set()'s ttl_ms is honored (else it must be 0)
 *     HL_KV_CAP_ATOMIC_INCREMENT  incr != NULL
 *     HL_KV_CAP_COMPARE_EXCHANGE  cas  != NULL
 *     HL_KV_CAP_SCAN              scan != NULL   (clear() also relies on it)
 *   declarative caps (gate nothing; describe semantics)
 *     HL_KV_CAP_PERSISTENT  HL_KV_CAP_SHARED  HL_KV_CAP_EVICTION  HL_KV_CAP_TRANSACTIONS
 *
 * ALWAYS non-NULL (core): open, close, last_error, get, set, del, exists, clear.
 * (clear is a namespace-prefix wipe; on a Redis-shaped backend it is SCAN+DEL,
 * so it is bounded and NON-ATOMIC - see scan.)
 */
#define HL_KV_CAP_TTL               (1u << 0)
#define HL_KV_CAP_ATOMIC_INCREMENT  (1u << 1)
#define HL_KV_CAP_COMPARE_EXCHANGE  (1u << 2)
#define HL_KV_CAP_SCAN              (1u << 3)
#define HL_KV_CAP_PERSISTENT        (1u << 4)
#define HL_KV_CAP_SHARED            (1u << 5)
#define HL_KV_CAP_EVICTION          (1u << 6)
#define HL_KV_CAP_TRANSACTIONS      (1u << 7)

/* Opaque, backend-owned, single-thread connection handle (mutable runtime
 * state; NOT a dispatch table). Freed only by close(). */
typedef struct HlKvHandle HlKvHandle;

/* scan() delivers each matching key (namespace prefix already stripped by the
 * backend) as a BORROWED (ptr,len) valid only during the call. Return non-zero
 * to stop iteration early. */
typedef int (*HlKvScanCb)(void *ctx, const uint8_t *key, size_t klen);

/*
 * compare-and-swap outcome - four DISTINCT states (never conflated):
 *   OK        the swap happened (current == expected, new written)
 *   MISMATCH  a definitive comparison failure (current != expected); no write.
 *             This is a normal negative result, NOT an error.
 *   CONFLICT  a retryable optimistic conflict: the key changed concurrently
 *             during the WATCH/MULTI/EXEC window and the backend's internal
 *             retry budget was exhausted. The caller MAY retry. NOT an error.
 *   ERROR     a transport / protocol / server failure. last_error(h) has detail.
 */
typedef enum HlKvCasResult {
    HL_KV_CAS_OK       =  0,
    HL_KV_CAS_MISMATCH =  1,
    HL_KV_CAS_CONFLICT =  2,
    HL_KV_CAS_ERROR    = -1,
} HlKvCasResult;

typedef struct HlKvBackend {
    const char        *name;         /* "valkey" */
    const char *const *schemes;      /* {"valkey","valkeys","redis","rediss",NULL} */
    uint32_t           caps;         /* HL_KV_CAP_* the backend supports */

    /* Open a connection from a DSN. 0 + *out on success; -1 + a message in
     * errbuf (caller-provided; never a borrowed pointer) on failure.
     * timeout_ms bounds the connect + handshake. Scrubs credentials post-open. */
    int  (*open)(HlKvHandle **out, const char *dsn, int timeout_ms,
                 HlAllocator *alloc, char *errbuf, size_t errlen);
    /* Free the handle + its buffers (scrubbing any residual secret material).
     * NULL-safe; idempotent is NOT required - the caller must not reuse or
     * double-close a handle. */
    void (*close)(HlKvHandle *h);
    /* Borrowed message for the last failed op on h; valid until the next call
     * on h. Never NULL for a live handle (may be an empty string). */
    const char *(*last_error)(HlKvHandle *h);

    /* GET. On hit sets *found=1 and *val and *vlen borrowing into h's buffer (valid
     * until the next call on h); on miss *found=0, *val=NULL, *vlen=0. Returns
     * 0 for hit OR miss, -1 on transport error. */
    int (*get)(HlKvHandle *h, const uint8_t *key, size_t klen,
               const uint8_t **val, size_t *vlen, int *found);
    /* SET key=val, ttl_ms <= 0 means no expiry (ttl honored iff HL_KV_CAP_TTL).
     * Returns 0 / -1. */
    int (*set)(HlKvHandle *h, const uint8_t *key, size_t klen,
               const uint8_t *val, size_t vlen, int64_t ttl_ms);
    /* DEL. *deleted set to 1 if the key existed, else 0. Returns 0 / -1. */
    int (*del)(HlKvHandle *h, const uint8_t *key, size_t klen, int *deleted);
    /* EXISTS. *present 1/0. Returns 0 / -1. */
    int (*exists)(HlKvHandle *h, const uint8_t *key, size_t klen, int *present);

    /* INCR by `by`; a fresh key starts at 0. An existing key PRESERVES its TTL;
     * a fresh key gets ttl_ms (iff HL_KV_CAP_TTL, else no expiry). *newval = the
     * post-increment value. Returns 0 / -1 (a non-integer stored value is -1).
     * NULL unless caps & HL_KV_CAP_ATOMIC_INCREMENT. */
    int (*incr)(HlKvHandle *h, const uint8_t *key, size_t klen,
                int64_t by, int64_t ttl_ms, int64_t *newval);

    /* Compare-and-swap. has_expected==0 means "set only if absent". Returns the
     * four-state HlKvCasResult (never -1/errno-style); ERROR detail via
     * last_error(h). NULL unless caps & HL_KV_CAP_COMPARE_EXCHANGE. */
    HlKvCasResult (*cas)(HlKvHandle *h, const uint8_t *key, size_t klen,
                         const uint8_t *expected, size_t elen, int has_expected,
                         const uint8_t *newv, size_t nlen, int64_t ttl_ms);

    /* Namespace-prefix wipe. Bounded + NON-ATOMIC on a Redis-shaped backend
     * (cursor SCAN + batched DEL): concurrent writes under the prefix may or may
     * not be removed. *removed = count deleted (best-effort). Returns 0 / -1. */
    int (*clear)(HlKvHandle *h, const uint8_t *prefix, size_t plen, int64_t *removed);

    /* BOUNDED, BEST-EFFORT prefix scan (a cursor iteration, NOT a snapshot).
     * Emits AT MOST `limit` matching keys (limit==0 means "up to an internal
     * cap", never unbounded) via cb, driving an opaque server cursor internally.
     * Because it is non-atomic it MAY return duplicates and MAY miss keys added
     * or removed mid-scan; it is not a point-in-time collection. cb returning
     * non-zero stops early. Returns 0 / -1. NULL unless caps & HL_KV_CAP_SCAN. */
    int (*scan)(HlKvHandle *h, const uint8_t *prefix, size_t plen, size_t limit,
                HlKvScanCb cb, void *cbctx);
} HlKvBackend;

/*
 * Weak hook: KV feature backends composed at `hull build --with=`. Returns a
 * `const` array of `const` backend pointers and its count. The base ships a
 * WEAK default (cap/kv_feature.c) returning an empty set; a feature archive
 * links a STRONG override (generated feature_registry.c). Plural to match the
 * generated feature collector's per-hook naming (cf. hl_db_feature_backends).
 */
const HlKvBackend *const *hl_kv_feature_backends(size_t *count);

/* Select a composed backend by DSN scheme (case-insensitive), or NULL. */
const HlKvBackend *hl_kv_backend_select(const char *dsn);

#endif /* HL_CAP_KV_BACKEND_H */

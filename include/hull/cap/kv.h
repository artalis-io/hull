/*
 * cap/kv.h: app-facing KV connection capability (base-resident).
 *
 * The thin enforcement + dispatch layer the runtime bindings (mod_kv) call. It
 * resolves a composed backend by DSN scheme (hl_kv_backend_select), opens a
 * handle, and forwards the narrow op set through the HlKvBackend vtable, adding
 * capability checks (an op the backend lacks fails with "unsupported"). This is
 * also where the manifest host-allowlist validation is layered in.
 *
 * Ownership of returned bytes follows the HlKvBackend contract: get() returns a
 * value BORROWED into the connection buffer, valid only until the next op on the
 * same connection - the binding MUST copy it into runtime-owned memory before
 * any further call.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_KV_H
#define HL_CAP_KV_H

#include <stddef.h>
#include <stdint.h>

#include "hull/cap/kv_backend.h"   /* HlKvCasResult, HlKvScanCb, caps, HlAllocator */

struct HlManifestKvDynamic;   /* fwd; the kv.open allowlist policy (manifest.h) */

typedef struct HlKvConn HlKvConn;   /* opaque: backend + handle + allocator */

/*
 * Validate a runtime KV DSN against the manifest kv.dynamic policy (cap/
 * kv_dynamic.c): the scheme must be listed in kv.dynamic.schemes and the host
 * must match kv.dynamic.hosts (exact / "*.suffix" glob / CIDR / "$VAR" env ref,
 * the shared host matcher). Every KV scheme is a network scheme. Fails closed:
 * a NULL / undeclared / empty policy denies. Returns 0 if allowed, -1 with a
 * message in errbuf. The binding calls this BEFORE hl_cap_kv_open.
 */
int hl_cap_kv_check_dsn(const struct HlManifestKvDynamic *policy, const char *dsn,
                        char *errbuf, size_t errlen);

/*
 * Open a KV connection from a DSN. Selects the composed backend by scheme; if
 * none is composed, fails with an actionable hint (compose --with=valkey /
 * `hull feature install valkey`). timeout_ms <= 0 uses the DSN default. All
 * memory comes from `alloc` (NULL -> process default). Returns 0 + *out, or -1
 * with a message in errbuf.
 */
int hl_cap_kv_open(HlKvConn **out, const char *dsn, int timeout_ms,
                   HlAllocator *alloc, char *errbuf, size_t errlen);
void hl_cap_kv_close(HlKvConn *c);

const char *hl_cap_kv_error(HlKvConn *c);   /* borrowed; valid until the next op */
uint32_t    hl_cap_kv_caps(HlKvConn *c);    /* HL_KV_CAP_* the backend supports */
const char *hl_cap_kv_backend_name(HlKvConn *c);

/* get: on a hit, val+vlen BORROW into the connection buffer (valid until the
 * next op); on a miss, found=0. The caller MUST copy before any further op. */
int hl_cap_kv_get(HlKvConn *c, const uint8_t *key, size_t klen,
                  const uint8_t **val, size_t *vlen, int *found);
int hl_cap_kv_set(HlKvConn *c, const uint8_t *key, size_t klen,
                  const uint8_t *val, size_t vlen, int64_t ttl_ms);
int hl_cap_kv_del(HlKvConn *c, const uint8_t *key, size_t klen, int *deleted);
int hl_cap_kv_exists(HlKvConn *c, const uint8_t *key, size_t klen, int *present);
int hl_cap_kv_incr(HlKvConn *c, const uint8_t *key, size_t klen,
                   int64_t by, int64_t ttl_ms, int64_t *newval);
HlKvCasResult hl_cap_kv_cas(HlKvConn *c, const uint8_t *key, size_t klen,
                            const uint8_t *expected, size_t elen, int has_expected,
                            const uint8_t *newv, size_t nlen, int64_t ttl_ms);
int hl_cap_kv_clear(HlKvConn *c, const uint8_t *prefix, size_t plen, int64_t *removed);
int hl_cap_kv_scan(HlKvConn *c, const uint8_t *prefix, size_t plen, size_t limit,
                   HlKvScanCb cb, void *cbctx);

#endif /* HL_CAP_KV_H */

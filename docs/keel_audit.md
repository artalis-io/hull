# Keel HTTP Server — C Code Audit Report

**Date:** 2026-03-01
**Auditor:** Claude Code (`/c-audit`)
**Keel version:** `464fab8` (vendor/keel submodule)
**Files scanned:** 47 (16 src, 17 headers, 1 parser, 13 test suites)
**Total lines of code:** ~13,550
**Test count:** 229 unit tests across 13 suites

## Summary

| Severity | Count |
|----------|------:|
| Critical | 2 |
| High | 5 |
| Medium | 8 |
| Low | 7 |
| Informational | 4 |

---

## Critical Issues

| # | File:Line | Issue | Suggested Fix |
|---|-----------|-------|---------------|
| C-1 | `src/event_kqueue.c:28-49` | **kqueue `kl_event_mod` does not support READ\|WRITE bitmask.** The if/else treats the mask as either READ or WRITE, never both. When `server.c:395` passes `KL_EVENT_READ | KL_EVENT_WRITE` for HTTP/2 connections, the WRITE filter is incorrectly disabled. Stalls HTTP/2 write flushing on macOS. | Add a branch for `(mask & KL_EVENT_READ) && (mask & KL_EVENT_WRITE)` that enables both kevent filters. |
| C-2 | `src/websocket.c:220-226` | **WebSocket `ws_send_frame` does not handle partial writes.** `conn_write()` may return fewer bytes on non-blocking sockets (especially TLS WANT_WRITE). Header short-write causes payload to proceed at wrong offset, corrupting the frame. | Use `writev_all()` from response.c, or implement a write-retry loop for header+payload. |

## High Issues

| # | File:Line | Issue | Suggested Fix |
|---|-----------|-------|---------------|
| H-1 | `src/event_poll.c:49-71` | **`grow_arrays()` inconsistent state on partial failure.** If `kl_realloc` for `udata` fails after `fds` succeeded, `st->fds` points to new allocation but `st->capacity` is stale. Next `kl_free` gets wrong size. | Save old fds pointer; on udata failure, revert fds or update capacity. |
| H-2 | `src/tls_mbedtls.c:281` | **`read_file()` uses raw `malloc`/`free`.** Key material in `key_buf` is freed without zeroing. Private key residue remains in heap. | Add `explicit_bzero(key_buf, key_len)` before `free(key_buf)`. |
| H-3 | `src/h2.c:509-511` | **HTTP/2 101 upgrade `conn_write` does not handle partial writes.** Short write on non-blocking socket corrupts protocol stream. | Use write-all loop or buffer for event loop completion. |
| H-4 | `src/websocket.c:376-377` | **WebSocket 101 handshake same partial-write issue.** | Same fix as H-3. |
| H-5 | `src/response.c:246-273` | **`writev_all` plaintext spin loop on EAGAIN.** `KL_WRITE_SPIN_MAX = 65536` busy-spins when kernel buffer is full, blocking all connections on this event loop tick. | Return partial-write indication and let event loop re-arm for `KL_EVENT_WRITE`, or reduce spin limit significantly. |

## Medium Issues

| # | File:Line | Issue | Suggested Fix |
|---|-----------|-------|---------------|
| M-1 | `src/server.c:180` | **`signal(SIGPIPE, SIG_IGN)` is process-global.** Affects all threads/libraries. | Use `MSG_NOSIGNAL`/`SO_NOSIGPIPE` per-socket, or document requirement. |
| M-2 | `src/response.c:60-73` | **`format_uint` buffer `tmp[20]` has zero margin for 64-bit max (20 digits).** | Use `tmp[21]` or add `_Static_assert(sizeof(size_t) <= 8)`. |
| M-3 | `src/connection.c:339-340` | **`memcpy` of params does not locally validate `num_params` against `KL_MAX_PARAMS`.** Relies on router invariant. | Add `if (c->num_params > KL_MAX_PARAMS) c->num_params = KL_MAX_PARAMS;` before memcpy. |
| M-4 | `src/tls_mbedtls.c:336-338` | **Static RNG personalization `"keel-tls"`.** Identical for all instances, reduces entropy diversity. | Append PID or timestamp to personalization string. |
| M-5 | `src/connection.c:34-39` | **`kl_monotonic_ms()` returns 0 on failure.** Causes immediate timeout for all connections. | Return previous known-good value, or treat 0 as sentinel. |
| M-6 | `src/body_reader_buffer.c:58` | **`(size_t)user_data` pointer-to-integer round-trip.** Implementation-defined on `sizeof(void*) < sizeof(size_t)` platforms. | Use `(uintptr_t)` cast or config struct pointer. |
| M-7 | `src/cors.c:129` | **`snprintf` in hot request path** for CORS `max_age_seconds`. | Use `format_uint` helper or static lookup table. |
| M-8 | `src/response.c:224-242` | **`kl_sendfile_impl` fallback does not handle partial `write()`.** | Wrap in retry loop for short writes / EAGAIN. |

## Low Issues

| # | File:Line | Issue | Suggested Fix |
|---|-----------|-------|---------------|
| L-1 | `src/event_kqueue.c:47` | **`kevent` return value unchecked in `kl_event_mod`.** | Check return value, return -1 on failure. |
| L-2 | `src/event_kqueue.c:55` | **`kevent` return value unchecked in `kl_event_del`.** | Same as L-1. |
| L-3 | `src/router.c:186-195` | **`strlen()` on route method/pattern on every match** (constant strings). | Cache `method_len`/`pattern_len` in `KlRoute` at registration. |
| L-4 | `src/router.c:166-167` | **`strlen()` on middleware pattern on every request.** | Cache `pattern_len` in `KlMiddlewareEntry`. |
| L-5 | `src/server.c:24` | **`kl_signal_server` global atomic supports only one server instance.** | Document limitation or maintain server list. |
| L-6 | `src/response.c:189-192` | **`kl_response_header` silently drops headers on alloc failure.** Partial writes not rolled back. | Check each `hdr_append` return and roll back or set error flag. |
| L-7 | `include/keel/request.h:51-61` | **`kl_request_header` returns non-null-terminated pointer.** Users may pass to `strcmp` causing OOB read. | Rename to `_raw` or deprecate in favor of `kl_request_header_len`. |

## Informational Notes

| # | File | Note |
|---|------|------|
| I-1 | `src/tls_mbedtls.c` | Raw `malloc`/`free` usage is intentional (documented lines 303-308). Startup-only, outside KlAllocator lifecycle. Key scrubbing (H-2) should still be added. |
| I-2 | `Makefile` | Build hardening is solid: `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror -fstack-protector-strong`. Debug with ASan+UBSan. Fuzz with both sanitizers. Cosmo correctly omits `-fstack-protector-strong`. |
| I-3 | `parsers/parser_llhttp.c:121-127` | Request smuggling mitigation present: `Transfer-Encoding: chunked` zeroes `Content-Length` (RFC 7230 §3.3.3). |
| I-4 | `src/response.c:177-180` | Header injection guard present: `contains_crlf()` rejects names/values with `\r`/`\n`. |

## Build Hardening

| Feature | Status |
|---------|--------|
| `-Wall -Wextra -Wpedantic -Werror` | Present |
| `-Wshadow -Wformat=2` | Present |
| `-fstack-protector-strong` | Present (non-Cosmo) |
| ASan + UBSan debug build | `make debug` |
| Fuzz testing (libFuzzer) | 2 targets (parser + multipart) |
| Static analysis | `scan-build` + `cppcheck` |
| Vendor code isolation | Separate `VENDOR_CFLAGS` with `-w` |

## Test Coverage

| Module | Tests | Notes |
|--------|------:|-------|
| allocator | 4 | Basic alloc/free/realloc, custom allocator |
| router | 21 | Params, matching, HEAD fallback, middleware, prefixes |
| parser (llhttp) | 9 | Parsing, headers, body, keep-alive |
| response | 14 | Status lines, headers, body modes, streaming, injection guard |
| body_reader | 30 | Growth, limits, multipart state machine, boundary spanning |
| chunked | 17 | Basic, extensions, trailers, overflow, boundary split |
| cors | 17 | All middleware paths, preflight, credentials |
| connection | 4 | Pool init/acquire/release, pool full |
| timeout | 4 | Timeout sweep, body timeout |
| tls | 20 | Vtable validation, mbedTLS lifecycle, handshake states |
| websocket | 38 | Frame parser, SHA-1, base64, UTF-8, fragmentation, close |
| h2 | 29 | Streams, callbacks, request lifecycle, header extraction |
| integration | 22 | End-to-end with real server |
| **Total** | **229** | |

### Coverage Gaps

1. No unit tests for event loop backends (`event_epoll.c`, `event_kqueue.c`, `event_poll.c`, `event_iouring.c`) — only indirect via integration tests
2. No test for `kl_event_mod` with `READ|WRITE` bitmask (the C-1 kqueue bug)
3. No test for `writev_all` partial write / EAGAIN handling (H-5)
4. No test for `kl_sendfile_impl` fallback partial write (M-8)
5. No test for WebSocket partial write scenarios (C-2)
6. No fuzz target for WebSocket frame parser
7. No test for `kl_monotonic_ms` failure path (M-5)

## Recommendations

### Immediate (Critical/High)

1. Fix kqueue `kl_event_mod` READ|WRITE bitmask (C-1) — actively breaks HTTP/2 on macOS
2. Fix WebSocket `ws_send_frame` partial writes (C-2) — corrupts frames on non-blocking sockets
3. Fix all `conn_write` call sites expecting complete writes (H-3, H-4)
4. Scrub private key material before free (H-2) — `explicit_bzero` in tls_mbedtls.c
5. Fix `grow_arrays` partial failure in event_poll.c (H-1)

### Short-term (Medium)

6. Add WebSocket fuzz target
7. Add event loop unit tests for combined READ|WRITE mask
8. Replace process-global `signal(SIGPIPE, SIG_IGN)` with per-socket suppression (M-1)
9. Add defensive `num_params` bounds check before memcpy (M-3)

### Long-term (Low/Performance)

10. Cache `strlen` results for route patterns at registration time (L-3, L-4)
11. Refactor `writev_all` to return partial progress instead of busy-spinning (H-5)
12. Deprecate `kl_request_header` in favor of `kl_request_header_len` (L-7)

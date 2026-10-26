/*
 * fuzz_http_guard_stubs.c: the Keel client symbols cap/http.c's request paths
 * reference, for fuzz_http_guard. The fuzzed guards never call them; linking
 * Keel's HTTP client would drag in mbedTLS. Kept out of the harness TU because
 * hull/cap/http.h declares the real prototypes (this file includes no Keel
 * header, so the stub signatures cannot conflict). Each aborts if reached.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdlib.h>

void kl_http_client_request(void);
void kl_http_client_request(void) { abort(); }
void kl_http_client_request_pooled(void);
void kl_http_client_request_pooled(void) { abort(); }
void kl_http_redirect_request(void);
void kl_http_redirect_request(void) { abort(); }
void kl_http_redirect_request_pooled(void);
void kl_http_redirect_request_pooled(void) { abort(); }
void kl_monotonic_ms(void);
void kl_monotonic_ms(void) { abort(); }
void kl_strerror(void);
void kl_strerror(void) { abort(); }
void kl_allocator_default(void);
void kl_allocator_default(void) { abort(); }


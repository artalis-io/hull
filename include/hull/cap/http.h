/*
 * cap/http.h — HTTP client capability with host allowlist
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_HTTP_H
#define HL_CAP_HTTP_H

#include <stddef.h>

typedef struct HlHttpConfig {
    const char **allowed_hosts;
    int          count;
} HlHttpConfig;

typedef struct {
    int          status;
    const char  *body;
    size_t       body_len;
    const char  *content_type;
} HlHttpResponse;

int hl_cap_http_request(const HlHttpConfig *cfg,
                          const char *method, const char *url,
                          const char *body, size_t body_len,
                          const char *content_type,
                          HlHttpResponse *resp);

void hl_cap_http_free(HlHttpResponse *resp);

#endif /* HL_CAP_HTTP_H */

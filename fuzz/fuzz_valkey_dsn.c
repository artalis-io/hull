/*
 * fuzz_valkey_dsn.c: Valkey/Redis DSN parser over untrusted input.
 *
 * Feeds arbitrary NUL-terminated strings to hl_valkey_dsn_parse: it must bound
 * every field, decode percent-escapes safely, and never read/write out of
 * bounds. On success every field must be NUL-terminated (checked via strlen).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/valkey_conn.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *s = (char *)malloc(size + 1);
    if (!s) return 0;
    if (size) memcpy(s, data, size);
    s[size] = '\0';

    HlValkeyDsn d;
    char err[128];
    if (hl_valkey_dsn_parse(s, &d, err, sizeof err) == 0) {
        (void)strlen(d.host);
        (void)strlen(d.port);
        (void)strlen(d.username);
        (void)strlen(d.password);
        (void)strlen(d.dbindex);
        hl_valkey_dsn_scrub(&d);
    }
    free(s);
    return 0;
}

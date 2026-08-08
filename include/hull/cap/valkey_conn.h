/*
 * cap/valkey_conn.h: Valkey/Redis DSN + RESP connection (transport).
 *
 * DSN parsing (pure, fuzzable) plus the blocking socket/TLS connection and the
 * HELLO 3 / AUTH handshake, split from the RESP codec (cap/respwire.c) and the
 * KV backend vtable (cap/valkey.c). The transport lives in the composed
 * `--with=valkey` feature archive; only the DSN parser is needed at test time.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_VALKEY_CONN_H
#define HL_CAP_VALKEY_CONN_H

#include <stddef.h>
#include <stdint.h>

/* Parsed DSN. Password is secret - scrub with hl_valkey_dsn_scrub after use. */
typedef struct HlValkeyDsn {
    char host[256];
    char port[16];        /* numeric string; default "6379" */
    char username[128];   /* optional ACL user */
    char password[256];   /* optional secret */
    char dbindex[16];     /* logical DB index; default "0" */
    int  tls;             /* 1 for rediss:// / valkeys:// */
    int  verify;          /* TLS chain+hostname verify (1) unless sslmode=disable/none */
    int  connect_timeout_ms;  /* default 5000 */
} HlValkeyDsn;

/*
 * Parse `scheme://[user[:pass]@]host[:port][/db][?opts]`.
 * Schemes (case-insensitive): redis / rediss / valkey / valkeys (the *s
 * variants select TLS). Query opts: connect_timeout (ms), sslmode
 * (verify-full|require|disable|none). Returns 0 on success; on failure returns
 * -1 with a human message in errbuf. Every field is bounded; oversized input
 * is an error, never a silent truncation.
 */
int hl_valkey_dsn_parse(const char *dsn, HlValkeyDsn *out, char *errbuf, size_t errlen);

/* Zero the password field (call after connecting / on teardown). */
void hl_valkey_dsn_scrub(HlValkeyDsn *dsn);

#endif /* HL_CAP_VALKEY_CONN_H */

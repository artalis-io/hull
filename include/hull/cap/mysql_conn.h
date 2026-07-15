/*
 * cap/mysql_conn.h: MySQL / MariaDB connection (DSN parse now; socket + auth
 * in later phases)
 *
 * Phase 1b exposes only the pure DSN parser (fuzzable, no socket). The
 * handshake, auth plugins, TLS, and query protocols land in later phases and
 * are added to cap/mysql_conn.c behind guards so this parser stays linkable
 * standalone by the fuzzers.
 *
 * The password is secret material: callers scrub the HlMyDsn after connecting.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_MYSQL_CONN_H
#define HL_CAP_MYSQL_CONN_H

#include <stddef.h>
#include <stdint.h>

typedef struct HlMyDsn {
    char host[256];
    char port[8];         /* numeric string; default "3306" */
    char user[128];
    char password[256];   /* secret: scrub after use */
    char dbname[128];
    char sslmode[16];     /* parsed now, enforced in the TLS phase */
} HlMyDsn;

/*
 * Parse a `mysql://` / `mariadb://` DSN into @p out. Percent-decodes user /
 * password / host / dbname; validates the port is numeric and bounded. Returns
 * 0 on success, -1 with a message in @p errbuf on any malformed / oversized
 * field. Every populated field is a NUL-terminated string on success.
 */
int hl_my_dsn_parse(const char *dsn, HlMyDsn *out, char *errbuf, size_t errlen);

/* Zero the password field (volatile) after the connection is established. */
void hl_my_dsn_scrub(HlMyDsn *dsn);

/* ── Auth plugins (crypto; compiled out under HL_MY_NO_AUTH) ───────── */
#ifndef HL_MY_NO_AUTH
/*
 * mysql_native_password auth response:
 *   SHA1(pw) XOR SHA1( scramble || SHA1(SHA1(pw)) )
 * Writes up to 20 bytes into @p out. Returns the response length (0 for an
 * empty password, 20 otherwise) or -1 on a crypto error. The pure fuzzers
 * define HL_MY_NO_AUTH to stay free of the cap/crypto (mbedTLS) dependency.
 */
int hl_my_native_password_scramble(const char *password,
                                   const uint8_t scramble[20],
                                   uint8_t out[20]);
#endif

#endif /* HL_CAP_MYSQL_CONN_H */

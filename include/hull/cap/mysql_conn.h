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

#endif /* HL_CAP_MYSQL_CONN_H */

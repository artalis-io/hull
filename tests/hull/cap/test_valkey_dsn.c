/*
 * test_valkey_dsn.c: Valkey/Redis DSN parser tests.
 *
 * Every field bounded, percent-escapes decoded, TLS/verify derived from the
 * scheme + sslmode, and hostile / oversized input rejected (never a silent
 * truncation). Fuzzed by fuzz/fuzz_valkey_dsn.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/valkey_conn.h"
#include <string.h>

#define OK(dsn) \
    HlValkeyDsn d; char e[128]; \
    ASSERT_EQ(0, hl_valkey_dsn_parse((dsn), &d, e, sizeof e))

UTEST(valkey_dsn, host_only_defaults) {
    OK("redis://cache.local");
    ASSERT_STREQ(d.host, "cache.local");
    ASSERT_STREQ(d.port, "6379");
    ASSERT_STREQ(d.dbindex, "0");
    ASSERT_EQ(d.tls, 0);
    ASSERT_EQ(d.username[0], '\0');
    ASSERT_EQ(d.password[0], '\0');
}

UTEST(valkey_dsn, host_port) {
    OK("valkey://10.0.0.5:6400");
    ASSERT_STREQ(d.host, "10.0.0.5");
    ASSERT_STREQ(d.port, "6400");
    ASSERT_EQ(d.tls, 0);
}

UTEST(valkey_dsn, full_userinfo_db) {
    OK("redis://alice:s3cr3t@db.example.com:6380/2");
    ASSERT_STREQ(d.host, "db.example.com");
    ASSERT_STREQ(d.port, "6380");
    ASSERT_STREQ(d.username, "alice");
    ASSERT_STREQ(d.password, "s3cr3t");
    ASSERT_STREQ(d.dbindex, "2");
}

UTEST(valkey_dsn, password_only_no_user) {
    OK("redis://:mypass@host");
    ASSERT_EQ(d.username[0], '\0');
    ASSERT_STREQ(d.password, "mypass");
}

UTEST(valkey_dsn, percent_decoded_password) {
    OK("redis://u:p%40ss%3Aword@host");
    ASSERT_STREQ(d.password, "p@ss:word");
}

UTEST(valkey_dsn, tls_schemes_verify_default_on) {
    { OK("rediss://host");  ASSERT_EQ(d.tls, 1); ASSERT_EQ(d.verify, 1); }
    { OK("valkeys://host"); ASSERT_EQ(d.tls, 1); ASSERT_EQ(d.verify, 1); }
}

UTEST(valkey_dsn, sslmode_and_timeout_opts) {
    { OK("rediss://host?sslmode=require");      ASSERT_EQ(d.verify, 0); }
    { OK("rediss://host?sslmode=verify-full");  ASSERT_EQ(d.verify, 1); }
    { OK("redis://host?connect_timeout=250");   ASSERT_EQ(d.connect_timeout_ms, 250); }
}

UTEST(valkey_dsn, ipv6_literal) {
    OK("redis://[2001:db8::1]:6379/1");
    ASSERT_STREQ(d.host, "2001:db8::1");
    ASSERT_STREQ(d.port, "6379");
    ASSERT_STREQ(d.dbindex, "1");
}

UTEST(valkey_dsn, ipv6_no_port) {
    OK("redis://[::1]");
    ASSERT_STREQ(d.host, "::1");
    ASSERT_STREQ(d.port, "6379");
}

UTEST(valkey_dsn, scheme_case_insensitive) {
    OK("REDISS://host");
    ASSERT_EQ(d.tls, 1);
}

/* ── rejections ───────────────────────────────────────────────────────── */

UTEST(valkey_dsn, reject_missing_scheme) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("cache.local:6379", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_wrong_scheme) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("http://host", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_missing_host) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("redis://:6379", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_non_numeric_port) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("redis://host:abcd", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_port_out_of_range) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("redis://host:70000", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_non_digit_db) {
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse("redis://host/notadb", &d, e, sizeof e));
}

UTEST(valkey_dsn, reject_oversized_host) {
    char big[400];
    memcpy(big, "redis://", 8);
    memset(big + 8, 'a', 300);
    big[308] = '\0';
    HlValkeyDsn d; char e[128];
    ASSERT_EQ(-1, hl_valkey_dsn_parse(big, &d, e, sizeof e));
}

UTEST(valkey_dsn, scrub_zeros_password) {
    OK("redis://u:secret@host");
    ASSERT_STREQ(d.password, "secret");
    hl_valkey_dsn_scrub(&d);
    ASSERT_EQ(d.password[0], '\0');
}

UTEST_MAIN();

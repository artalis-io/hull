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

/* sslmode=require / verify-* ask for TLS, so they turn it on over a plaintext
 * scheme; they used to set only `verify`, and the connection (and its AUTH)
 * went out in clear. */
UTEST(valkey_dsn, sslmode_turns_tls_on) {
    { OK("redis://host?sslmode=require");      ASSERT_EQ(d.tls, 1); ASSERT_EQ(d.verify, 0); }
    { OK("redis://host?sslmode=verify-full");  ASSERT_EQ(d.tls, 1); ASSERT_EQ(d.verify, 1); }
    { OK("valkey://host?sslmode=verify-ca");   ASSERT_EQ(d.tls, 1); ASSERT_EQ(d.verify, 1); }
    { OK("redis://host?sslmode=disable");      ASSERT_EQ(d.tls, 0); }
    { OK("redis://host");                      ASSERT_EQ(d.tls, 0); }
}

UTEST(valkey_dsn, sslmode_and_timeout_opts) {
    { OK("rediss://host?sslmode=require");      ASSERT_EQ(d.verify, 0); }
    { OK("rediss://host?sslmode=verify-full");  ASSERT_EQ(d.verify, 1); }
    { OK("redis://host?connect_timeout=250");   ASSERT_EQ(d.connect_timeout_ms, 250); }
}

/* A connect_timeout that is not a number is ignored, not read as a prefix:
 * atol took "abc" as 0 and "10x" as 10. */
UTEST(valkey_dsn, timeout_must_be_a_number) {
    int dflt;
    { OK("redis://host"); dflt = d.connect_timeout_ms; }
    { OK("redis://host?connect_timeout=abc"); ASSERT_EQ(d.connect_timeout_ms, dflt); }
    { OK("redis://host?connect_timeout=10x"); ASSERT_EQ(d.connect_timeout_ms, dflt); }
    { OK("redis://host?connect_timeout=");    ASSERT_EQ(d.connect_timeout_ms, dflt); }
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

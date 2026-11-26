/*
 * host_match.h: host-allowlist pattern matching
 *
 * Match a connection host against an allowlist pattern. Shared by the DB
 * dynamic-connection allowlist (roadmap §2.2) and, since §2.8, the
 * `manifest.hosts` gate for http.fetch / ws.connect / smtp.send.
 *
 * Pattern forms:
 *   "db.example.com"   exact hostname (case-insensitive)
 *   "*.example.com"    any-depth subdomain of example.com ("a.example.com",
 *                      "a.b.example.com"); does NOT match the apex "example.com"
 *   "*"                any host
 *   "10.0.0.0/8"       CIDR; matches only when the host is an IP literal in
 *   "2001:db8::/32"    that range. A hostname never matches a CIDR (no DNS
 *                      resolution here, so no rebinding surprise).
 *                      Both the network and the host must be canonical IP
 *                      literals (see hl_host_ip_canonical; audit 42).
 *
 * Any of them may name the one port it admits (#1227): "127.0.0.1:8081",
 * "*.internal:9000", "*:443", "10.0.0.0/8:5432", and for IPv6 only in
 * brackets, "[::1]:8081", "[2001:db8::/32]:443". A pattern with no port
 * admits every port. A ported pattern never matches a check that has no port
 * (the port-less entry points below, port 0).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_UTILS_HOST_MATCH_H
#define HL_UTILS_HOST_MATCH_H

#include <stddef.h>

/* Audit 42: 1 if @p host is an IP literal in the one spelling every resolver
 * reads the same way - IPv4 as four decimal octets 0-255 with no leading zero
 * (but a lone "0"), IPv6 as 1-4-digit hex groups with at most one "::", an
 * optional canonical IPv4 tail and an optional "%zone" - else 0. */
int hl_host_ip_canonical(const char *host);

/* Audit 42: 0 if @p host looks like an IP literal (contains ':', or every
 * dot-separated label is decimal digits or 0x-hex: "0177.0.0.1", "127.1",
 * "2130706433", "0x7f.0.0.1") but is not canonical; 1 for a canonical literal
 * or anything else (a DNS name). hl_host_match refuses a host this rejects,
 * against every pattern, "*" included. */
int hl_host_literal_ok(const char *host);

/* 1 if @p pattern is well formed (a port, when given, is 1-65535, decimal,
 * no leading zero; IPv6 takes one only in brackets), else 0. A malformed
 * pattern matches nothing; the manifest check refuses one at load. */
int hl_host_pattern_valid(const char *pattern);

/* 1 if @p pattern is well formed and names a port. */
int hl_host_pattern_has_port(const char *pattern);

/* 1 if @p host on @p port matches @p pattern, else 0 (@p port 0: unknown,
 * which a ported pattern never matches). NULL-safe. A host that
 * hl_host_literal_ok rejects never matches. */
int hl_host_match_port(const char *pattern, const char *host, int port);

/* hl_host_match_port with no port: for a check with no destination port
 * (a peer address: limits.trusted_proxies). */
int hl_host_match(const char *pattern, const char *host);

/* 1 if @p host matches any of the @p n patterns, else 0. */
int hl_host_match_any(const char *const *patterns, int n, const char *host);

/* Like hl_host_match_any, but a pattern of the form "$VAR" / "${VAR}" is first
 * resolved from the environment (an unset / empty var contributes no match).
 * This is the manifest host-allowlist form shared by http.fetch / ws.connect /
 * smtp.send (manifest.hosts) and databases.dynamic.hosts, so one allowlist
 * entry can come from config without being hardcoded. Resolved at match time,
 * so it works whether or not the manifest was sealed. */
int hl_host_match_any_env(const char *const *patterns, int n, const char *host);

/* hl_host_match_any_env for a destination on @p port: what every outbound
 * check calls (http / ws / smtp / ssh / db.open / kv.open). */
int hl_host_match_any_env_port(const char *const *patterns, int n,
                               const char *host, int port);

/* As hl_host_match_any_env_port, but @p host is ptr + length (not
 * necessarily NUL-terminated, as from a URL parse). Copies into a bounded
 * stack buffer first; an implausibly long host (>= 256 bytes) never matches
 * (fail closed). */
int hl_host_match_any_env_n(const char *const *patterns, int n,
                            const char *host, size_t host_len, int port);

/*
 * The host of a network DSN (scheme://[userinfo@]host[:port][/...]), for the
 * dynamic-connection checks (db.open, kv.open). IPv6 "[::1]" comes back
 * without brackets. Returns 1 + host in buf, or 0 - and 0 for any DSN a
 * backend's own parser could read differently, so the check never passes
 * one host while the connector dials another:
 *   - more than one '@' before the path (checkers took the last, the
 *     Postgres/MySQL parsers the first),
 *   - a '#' anywhere (the Valkey parser does not end the authority there),
 *   - a host byte outside [A-Za-z0-9.-_] (or hex/':'/'.' in brackets): no
 *     percent-encoding, which a parser decodes - "%00" truncated the host,
 *   - a port that is not all digits, or any control byte in the authority.
 */
int hl_dsn_host(const char *dsn, char *buf, size_t bufsz);

/* hl_dsn_host, plus the authority's port in *@p port (0 when the DSN names
 * none: the caller applies its scheme's default). A port outside 1-65535 is
 * refused (returns 0). */
int hl_dsn_host_port(const char *dsn, char *buf, size_t bufsz, int *port);

/*
 * The percent-decoded value of the @p nth (0-based) `key=value` pair named
 * exactly @p key in a network DSN's query - the text after the first '?'
 * past "://", split on '&' and each pair's first '=' - the way the MySQL and
 * Postgres DSN parsers read it. Returns the decoded length (@p out holds it,
 * NUL-terminated), -1 when there is no such pair, -2 when the value does not
 * decode into @p outsz bytes or holds a NUL.
 */
int hl_dsn_query_param(const char *dsn, const char *key, int nth,
                       char *out, size_t outsz);

#endif /* HL_UTILS_HOST_MATCH_H */

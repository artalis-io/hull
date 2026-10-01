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
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_UTILS_HOST_MATCH_H
#define HL_UTILS_HOST_MATCH_H

#include <stddef.h>

/* 1 if @p host matches @p pattern, else 0. NULL-safe (returns 0). */
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

/* As hl_host_match_any_env, but @p host is ptr + length (not necessarily
 * NUL-terminated, as from a URL parse). Copies into a bounded stack buffer
 * first; an implausibly long host (>= 256 bytes) never matches (fail closed). */
int hl_host_match_any_env_n(const char *const *patterns, int n,
                            const char *host, size_t host_len);

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

#endif /* HL_UTILS_HOST_MATCH_H */

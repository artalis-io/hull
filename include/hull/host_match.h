/*
 * host_match.h: host-allowlist pattern matching
 *
 * Match a connection host against an allowlist pattern. Used by the DB dynamic
 * connection allowlist (roadmap §2.2); http.fetch's exact-only manifest.hosts
 * check can adopt this later for glob/CIDR support.
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

/* 1 if @p host matches @p pattern, else 0. NULL-safe (returns 0). */
int hl_host_match(const char *pattern, const char *host);

/* 1 if @p host matches any of the @p n patterns, else 0. */
int hl_host_match_any(const char *const *patterns, int n, const char *host);

#endif /* HL_UTILS_HOST_MATCH_H */

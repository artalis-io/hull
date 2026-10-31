/*
 * hull/limits/input.h - input ceilings and request concurrency defaults
 *
 * Audit convergence §6.4: caps applied before an engine sees request data.
 *
 *   - JSON: nesting depth and the number of keys in any one object,
 *     enforced by every JSON decode in an app VM (Lua `hull.json.decode`,
 *     JS `JSON.parse` / `hull:json` decode). Hull cannot tell which string
 *     came from a request body, so the ceiling is global; the defaults are
 *     far above what real request payloads need.
 *   - RegExp source length and normalize / localeCompare operand length
 *     (JS only, QuickJS patch 0012): global ceilings, for the same reason.
 *     Audit 41 made both builtins linear; these are defence in depth.
 *   - Concurrency: at most `per_ip` requests in flight per client address
 *     (IPv4 as itself, IPv6 by /64), and optionally per route pattern.
 *
 * Manifest (Lua):  limits = { json_max_depth = N, json_max_keys = N,
 *                             regexp_max_source = N, normalize_max_input = N,
 *                             per_ip_concurrency = N, trusted_proxies = {...},
 *                             route_concurrency = { ["/upload"] = 2 } }
 * Manifest (JS):   limits: { jsonMaxDepth, jsonMaxKeys, regexpMaxSource,
 *                            normalizeMaxInput, perIpConcurrency,
 *                            trustedProxies, routeConcurrency }
 *
 * A value must be an integer of at least 1 (capped at the ceiling below);
 * anything else is absent and the default applies. per_ip_concurrency also
 * takes 0, which turns the per-IP cap off. The operator flag
 * --max-requests-per-ip N overrides the manifest (0 = off).
 *
 * Header-only: used by the manifest parsers, serve.c, both runtimes and the
 * unit tests.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_LIMITS_INPUT_H
#define HL_LIMITS_INPUT_H

#include <stdint.h>

#define HL_INPUT_JSON_MAX_DEPTH_DEFAULT      128u
#define HL_INPUT_JSON_MAX_KEYS_DEFAULT       100000u
#define HL_INPUT_REGEXP_MAX_SOURCE_DEFAULT   (64u * 1024u)
#define HL_INPUT_NORMALIZE_MAX_INPUT_DEFAULT (1024u * 1024u)

/* Manifest values above these are clamped. The JSON depth ceiling stays
 * below what either engine's stack holds (Lua's C-call limit ends a
 * recursive decode near 200 levels anyway). */
#define HL_INPUT_JSON_MAX_DEPTH_CEIL         10000u
#define HL_INPUT_JSON_MAX_KEYS_CEIL          100000000u
#define HL_INPUT_REGEXP_MAX_SOURCE_CEIL      (64u * 1024u * 1024u)
#define HL_INPUT_NORMALIZE_MAX_INPUT_CEIL    (256u * 1024u * 1024u)

/* Requests in flight per client address; 0 = off. */
#define HL_REQ_PER_IP_DEFAULT                8u
#define HL_REQ_PER_IP_CEIL                   1000000u
/* Route patterns with a limit of their own, and trusted proxy entries. */
#define HL_MANIFEST_MAX_ROUTE_LIMITS         32
#define HL_MANIFEST_MAX_TRUSTED_PROXIES      32
/* Seconds in the Retry-After of a refused request. */
#define HL_REQ_RETRY_AFTER_S                 1

typedef struct HlInputLimits {
    uint32_t json_max_depth;
    uint32_t json_max_keys;
    uint32_t regexp_max_source;
    uint32_t normalize_max_input;
} HlInputLimits;

/* The defaults with the manifest's values (0 = not set) over them. */
static inline HlInputLimits hl_input_limits_resolve(uint32_t json_max_depth,
                                                    uint32_t json_max_keys,
                                                    uint32_t regexp_max_source,
                                                    uint32_t normalize_max_input)
{
    HlInputLimits l;
    l.json_max_depth = json_max_depth ? json_max_depth : HL_INPUT_JSON_MAX_DEPTH_DEFAULT;
    l.json_max_keys = json_max_keys ? json_max_keys : HL_INPUT_JSON_MAX_KEYS_DEFAULT;
    l.regexp_max_source = regexp_max_source ? regexp_max_source
                                            : HL_INPUT_REGEXP_MAX_SOURCE_DEFAULT;
    l.normalize_max_input = normalize_max_input ? normalize_max_input
                                                : HL_INPUT_NORMALIZE_MAX_INPUT_DEFAULT;
    if (l.json_max_depth > HL_INPUT_JSON_MAX_DEPTH_CEIL)
        l.json_max_depth = HL_INPUT_JSON_MAX_DEPTH_CEIL;
    if (l.json_max_keys > HL_INPUT_JSON_MAX_KEYS_CEIL)
        l.json_max_keys = HL_INPUT_JSON_MAX_KEYS_CEIL;
    if (l.regexp_max_source > HL_INPUT_REGEXP_MAX_SOURCE_CEIL)
        l.regexp_max_source = HL_INPUT_REGEXP_MAX_SOURCE_CEIL;
    if (l.normalize_max_input > HL_INPUT_NORMALIZE_MAX_INPUT_CEIL)
        l.normalize_max_input = HL_INPUT_NORMALIZE_MAX_INPUT_CEIL;
    return l;
}

static inline HlInputLimits hl_input_limits_default(void)
{
    return hl_input_limits_resolve(0, 0, 0, 0);
}

#endif /* HL_LIMITS_INPUT_H */

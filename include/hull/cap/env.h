/**
 * @file cap/env.h
 * @brief Environment-variable capability with strict allowlist.
 *
 * App code calls `env.get("API_KEY")` which delegates here. Names that
 * aren't in the manifest's `env` array return NULL, regardless of
 * whether the variable actually exists in `getenv`. Max 32 entries.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_ENV_H
#define HL_CAP_ENV_H

/**
 * @brief Environment-capability configuration.
 *
 * The caller (typically `manifest.c` during startup) builds this
 * from the app's `env` manifest array. Both pointers are borrowed —
 * the underlying strings live in the manifest's allocator.
 */
typedef struct HlEnvConfig {
    const char **allowed;  /**< Array of allowed env-var names. */
    int          count;    /**< Number of entries. Capped at 32 by manifest extraction. */
} HlEnvConfig;

/**
 * @brief Look up an environment variable, gated by the allowlist.
 *
 * @param cfg   Env capability config.
 * @param name  Variable name to query.
 *
 * @return The variable's value (from `getenv`) if @p name is in the
 *         allowlist AND set in the process environment, otherwise
 *         NULL. Both "not allowlisted" and "not set" produce NULL —
 *         callers cannot distinguish these cases (and shouldn't need to;
 *         the manifest fully determines the visible env).
 */
const char *hl_cap_env_get(const HlEnvConfig *cfg, const char *name);

#endif /* HL_CAP_ENV_H */

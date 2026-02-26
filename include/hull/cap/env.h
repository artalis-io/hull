/*
 * cap/env.h — Environment variable capability with allowlist
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_ENV_H
#define HL_CAP_ENV_H

typedef struct HlEnvConfig {
    const char **allowed;
    int          count;
} HlEnvConfig;

const char *hl_cap_env_get(const HlEnvConfig *cfg, const char *name);

#endif /* HL_CAP_ENV_H */

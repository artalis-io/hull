/*
 * test_module_registry.c — Tests for the canonical module registry
 *
 * Standalone: only links against module_registry.o.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/module_registry.h"

#include <string.h>

/* ── Lookup ────────────────────────────────────────────────────────── */

UTEST(module_registry, find_exact_known_canonical)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/crypto");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s->name, "hull/crypto");
    ASSERT_EQ((int)s->api_major, 1);
    ASSERT_EQ((int)s->intrinsic, 0);
}

UTEST(module_registry, find_returns_null_for_unknown)
{
    ASSERT_EQ(hl_module_registry_find("hull/never_existed"), NULL);
    ASSERT_EQ(hl_module_registry_find(""), NULL);
    ASSERT_EQ(hl_module_registry_find(NULL), NULL);
}

UTEST(module_registry, find_short_prepends_hull_prefix)
{
    const HlModuleSpec *a = hl_module_registry_find_short("crypto");
    const HlModuleSpec *b = hl_module_registry_find("hull/crypto");
    ASSERT_NE(a, NULL);
    ASSERT_EQ(a, b);
}

UTEST(module_registry, find_short_passes_through_namespaced)
{
    /* If user writes the full canonical name, no further prefixing. */
    const HlModuleSpec *a = hl_module_registry_find_short("hull/crypto");
    const HlModuleSpec *b = hl_module_registry_find("hull/crypto");
    ASSERT_NE(a, NULL);
    ASSERT_EQ(a, b);
}

UTEST(module_registry, find_short_unknown_returns_null)
{
    ASSERT_EQ(hl_module_registry_find_short("notreal"), NULL);
    ASSERT_EQ(hl_module_registry_find_short(NULL), NULL);
}

/* ── Intrinsic flags ──────────────────────────────────────────────── */

UTEST(module_registry, intrinsic_modules_are_marked)
{
    const HlModuleSpec *app = hl_module_registry_find("hull/app");
    const HlModuleSpec *log = hl_module_registry_find("hull/log");
    const HlModuleSpec *jsn = hl_module_registry_find("hull/json");
    ASSERT_NE(app, NULL);
    ASSERT_NE(log, NULL);
    ASSERT_NE(jsn, NULL);
    ASSERT_EQ((int)app->intrinsic, 1);
    ASSERT_EQ((int)log->intrinsic, 1);
    ASSERT_EQ((int)jsn->intrinsic, 1);
}

UTEST(module_registry, side_effect_modules_are_not_intrinsic)
{
    const char *side_effecting[] = {
        "hull/crypto", "hull/db", "hull/env", "hull/fs",
        "hull/http", "hull/ws", "hull/time", "hull/compute", "hull/gpu",
        NULL,
    };
    for (int i = 0; side_effecting[i]; i++) {
        const HlModuleSpec *s = hl_module_registry_find(side_effecting[i]);
        ASSERT_NE(s, NULL);
        ASSERT_EQ_MSG((int)s->intrinsic, 0, side_effecting[i]);
    }
}

/* ── Capability bits ──────────────────────────────────────────────── */

UTEST(module_registry, http_requires_hosts_cap)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/http");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_HOSTS), 0u);
}

UTEST(module_registry, fs_requires_fs_cap)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/fs");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_FS), 0u);
}

UTEST(module_registry, env_requires_env_cap)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/env");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_ENV), 0u);
}

UTEST(module_registry, db_requires_compile_db)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/db");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_DB), 0u);
}

UTEST(module_registry, gpu_requires_compile_gpu)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/gpu");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_GPU), 0u);
}

UTEST(module_registry, compute_requires_compile_wasm)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/compute");
    ASSERT_NE(s, NULL);
    ASSERT_NE((s->required_caps & HL_MOD_CAP_WASM), 0u);
}

UTEST(module_registry, crypto_has_no_capability_requirement)
{
    /* Crypto exposes randomness/keys internally but does not require
     * any manifest section. Intentional — declaring crypto is the
     * gate; no separate "crypto allowlist" exists. */
    const HlModuleSpec *s = hl_module_registry_find("hull/crypto");
    ASSERT_NE(s, NULL);
    ASSERT_EQ(s->required_caps, 0u);
}

/* ── Internal dependencies ────────────────────────────────────────── */

UTEST(module_registry, jwt_depends_on_crypto)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/jwt");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s->deps[0], "hull/crypto");
    ASSERT_EQ(s->deps[1], NULL);
}

UTEST(module_registry, session_depends_on_db_and_crypto)
{
    const HlModuleSpec *s = hl_module_registry_find("hull/middleware/session");
    ASSERT_NE(s, NULL);
    ASSERT_STREQ(s->deps[0], "hull/db");
    ASSERT_STREQ(s->deps[1], "hull/crypto");
    ASSERT_EQ(s->deps[2], NULL);
}

UTEST(module_registry, every_dep_points_to_a_real_module)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    ASSERT_GT(total, (size_t)0);

    for (size_t i = 0; i < total; i++) {
        for (int j = 0; j < HL_MODULE_MAX_DEPS && all[i].deps[j]; j++) {
            const HlModuleSpec *dep = hl_module_registry_find(all[i].deps[j]);
            ASSERT_NE_MSG(dep, NULL, all[i].deps[j]);
        }
    }
}

/* ── Sort order ───────────────────────────────────────────────────── */

UTEST(module_registry, table_is_sorted_for_binary_search)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 1; i < total; i++) {
        int cmp = strcmp(all[i - 1].name, all[i].name);
        ASSERT_LT_MSG(cmp, 0, all[i].name);
    }
}

UTEST(module_registry, every_entry_findable_by_binary_search)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 0; i < total; i++) {
        const HlModuleSpec *found = hl_module_registry_find(all[i].name);
        ASSERT_EQ_MSG(found, &all[i], all[i].name);
    }
}

UTEST(module_registry, index_round_trips)
{
    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);
    for (size_t i = 0; i < total; i++) {
        int idx = hl_module_registry_index(&all[i]);
        ASSERT_EQ((size_t)idx, i);
    }
}

UTEST(module_registry, index_for_unrelated_pointer_returns_negative_one)
{
    HlModuleSpec local = {0};
    ASSERT_EQ(hl_module_registry_index(&local), -1);
    ASSERT_EQ(hl_module_registry_index(NULL), -1);
}

UTEST(module_registry, count_matches_iteration)
{
    size_t total = 0;
    (void)hl_module_registry_all(&total);
    ASSERT_EQ(total, hl_module_registry_count());
    ASSERT_GT(total, (size_t)10);  /* sanity: real registry */
}

UTEST_MAIN();

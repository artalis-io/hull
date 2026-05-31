/*
 * test_module_resolver.c — Tests for HlResolvedModuleSet + resolver
 *
 * Validates the manifest-to-set pipeline:
 *   - Intrinsics auto-seeded even with no manifest
 *   - Unknown / wrong-version / duplicate names rejected
 *   - Required manifest capabilities enforced (fs/hosts/env)
 *   - Required build-time subsystems enforced (DB/WASM/GPU)
 *   - Internal first-party deps must be explicitly declared
 *
 * Standalone except for manifest.o (for HlManifestModule).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/manifest.h"
#include "hull/module_registry.h"
#include "hull/module_resolver.h"

#include <string.h>

/* ── Test helpers ──────────────────────────────────────────────────── */

static void clear_manifest(HlManifest *m)
{
    memset(m, 0, sizeof(*m));
}

static void add_module(HlManifest *m, const char *name, uint8_t api_major)
{
    int i = m->modules_count++;
    m->modules[i].name      = name;       /* borrowed for tests */
    m->modules[i].api_major = api_major;
    m->modules_declared     = 1;
}

static void add_host(HlManifest *m, const char *host)
{
    m->hosts[m->hosts_count++] = host;
}

static void add_fs_read(HlManifest *m, const char *path)
{
    m->fs_read[m->fs_read_count++] = path;
}

static void add_env(HlManifest *m, const char *name)
{
    m->env[m->env_count++] = name;
}

/* ── Bitset basics ─────────────────────────────────────────────────── */

UTEST(module_resolver, empty_set_has_no_members)
{
    HlResolvedModuleSet s = {0};
    ASSERT_EQ(hl_module_set_count(&s), 0);
    ASSERT_FALSE(hl_module_set_contains_index(&s, 0));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/crypto"));
}

UTEST(module_resolver, clear_resets_all_bits)
{
    HlResolvedModuleSet s;
    memset(&s, 0xff, sizeof(s));
    hl_module_set_clear(&s);
    ASSERT_EQ(hl_module_set_count(&s), 0);
}

/* ── Intrinsic auto-seed ──────────────────────────────────────────── */

UTEST(module_resolver, null_manifest_admits_only_intrinsics)
{
    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(NULL, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);

    /* Intrinsic core as of v0.1.0 release: just hull/app. log and
     * json are declared modules; apps must put them in
     * manifest.modules to use them. */
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/app"));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/log"));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/json"));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/crypto"));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/fs"));
}

UTEST(module_resolver, manifest_without_modules_key_admits_only_intrinsics)
{
    HlManifest m;
    clear_manifest(&m);
    m.present = 1;
    /* modules_declared = 0 */

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);

    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/app"));
    ASSERT_FALSE(hl_module_set_contains_name(&s, "hull/crypto"));
}

UTEST(module_resolver, empty_modules_table_still_admits_intrinsics)
{
    HlManifest m;
    clear_manifest(&m);
    m.modules_declared = 1;  /* present but empty */

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/app"));
}

/* ── Happy paths ──────────────────────────────────────────────────── */

UTEST(module_resolver, declared_module_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "crypto", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/crypto"));
}

UTEST(module_resolver, short_and_canonical_names_resolve_the_same)
{
    HlManifest a, b;
    clear_manifest(&a);
    clear_manifest(&b);
    add_module(&a, "crypto", 1);
    add_module(&b, "hull/crypto", 1);

    HlResolvedModuleSet sa = {0}, sb = {0};
    char err[256];
    ASSERT_EQ(hl_module_resolver_resolve(&a, &sa, err, sizeof(err)), 0);
    ASSERT_EQ(hl_module_resolver_resolve(&b, &sb, err, sizeof(err)), 0);
    ASSERT_EQ(memcmp(&sa, &sb, sizeof(sa)), 0);
}

UTEST(module_resolver, http_with_hosts_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "http-client", 1);
    add_host(&m, "https://api.example.com");

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/http-client"));
}

UTEST(module_resolver, fs_with_read_path_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "fs", 1);
    add_fs_read(&m, "./data");

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/fs"));
}

UTEST(module_resolver, env_with_allowlist_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "env", 1);
    add_env(&m, "PORT");

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/env"));
}

UTEST(module_resolver, jwt_with_crypto_dep_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "jwt", 1);
    add_module(&m, "crypto", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), 0);
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/jwt"));
    ASSERT_TRUE(hl_module_set_contains_name(&s, "hull/crypto"));
}

/* ── Error paths ──────────────────────────────────────────────────── */

UTEST(module_resolver, unknown_module_rejected)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "nonexistent", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "unknown module"), NULL);
    ASSERT_NE(strstr(err, "nonexistent"), NULL);
}

UTEST(module_resolver, version_mismatch_rejected)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "crypto", 99);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "api_major"), NULL);
}

UTEST(module_resolver, duplicate_declaration_rejected)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "crypto", 1);
    add_module(&m, "hull/crypto", 1);  /* same module via canonical form */

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "more than once"), NULL);
}

/* Module declarations without their matching capability section are
 * intentionally allowed: the per-call cap layer fails closed, and some
 * surface (fs.realpath, future fs.stat, http_client with a hosts list
 * filled in at runtime via a tool, etc.) can be useful without paths.
 * The resolver only hard-blocks compile-time gates. */

UTEST(module_resolver, http_client_without_hosts_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "http-client", 1);
    /* no hosts — resolver still admits the module */

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(err[0], '\0');
}

UTEST(module_resolver, fs_without_paths_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "fs", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(err[0], '\0');
}

UTEST(module_resolver, env_without_allowlist_admitted)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "env", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(err[0], '\0');
}

/* Policy (changed 2026-05-22): declaring a module auto-admits its
 * registry-declared deps transitively. The resolver only errors on
 * build-time gates (HL_ENABLE_*) and TUI's manifest boolean. The two
 * tests below used to assert the old "explicit declaration required"
 * policy; they now assert the auto-admit shape. Per-call cap checks
 * (hosts/fs/env) still gate actual resource access. */

UTEST(module_resolver, jwt_auto_admits_crypto)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "jwt", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(err[0], '\0');

    const HlModuleSpec *jwt    = hl_module_registry_find_short("jwt");
    const HlModuleSpec *crypto = hl_module_registry_find_short("crypto");
    ASSERT_TRUE(jwt != NULL);
    ASSERT_TRUE(crypto != NULL);
    ASSERT_TRUE(hl_module_set_contains_spec(&s, jwt));
    ASSERT_TRUE(hl_module_set_contains_spec(&s, crypto));
}

UTEST(module_resolver, session_auto_admits_db_and_crypto)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "web/middleware/session", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(err[0], '\0');

    const HlModuleSpec *session = hl_module_registry_find_short("web/middleware/session");
    const HlModuleSpec *db      = hl_module_registry_find_short("db");
    const HlModuleSpec *crypto  = hl_module_registry_find_short("crypto");
    ASSERT_TRUE(session != NULL);
    ASSERT_TRUE(db != NULL);
    ASSERT_TRUE(crypto != NULL);
    ASSERT_TRUE(hl_module_set_contains_spec(&s, session));
    ASSERT_TRUE(hl_module_set_contains_spec(&s, db));
    ASSERT_TRUE(hl_module_set_contains_spec(&s, crypto));
}

#ifndef HL_ENABLE_GPU
UTEST(module_resolver, gpu_rejected_when_compiled_out)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "gpu", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "HL_ENABLE_GPU"), NULL);
}
#endif

#ifdef HL_ENABLE_TUI
UTEST(module_resolver, tui_admitted_when_both_build_and_manifest_set)
{
    HlManifest m;
    clear_manifest(&m);
    m.tui = 1;
    add_module(&m, "tui", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, 0);
}

UTEST(module_resolver, tui_rejected_without_manifest_flag)
{
    HlManifest m;
    clear_manifest(&m);
    /* m.tui left at 0 */
    add_module(&m, "tui", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "tui = true"), NULL);
}
#endif

#ifndef HL_ENABLE_TUI
UTEST(module_resolver, tui_rejected_when_compiled_out)
{
    HlManifest m;
    clear_manifest(&m);
    m.tui = 1;  /* even with manifest flag set */
    add_module(&m, "tui", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "HL_ENABLE_TUI"), NULL);
}
#endif

#ifndef HL_ENABLE_WASM
UTEST(module_resolver, compute_rejected_when_compiled_out)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "compute", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "HL_ENABLE_WASM"), NULL);
}
#endif

#ifndef HL_ENABLE_DB
UTEST(module_resolver, db_rejected_when_compiled_out)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "db", 1);

    HlResolvedModuleSet s = {0};
    char err[256] = {0};
    int rc = hl_module_resolver_resolve(&m, &s, err, sizeof(err));
    ASSERT_EQ(rc, -1);
    ASSERT_NE(strstr(err, "HL_ENABLE_DB"), NULL);
}
#endif

/* ── Error-buffer hardening ────────────────────────────────────────── */

UTEST(module_resolver, null_errbuf_does_not_crash)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "nonexistent", 1);

    HlResolvedModuleSet s = {0};
    /* Pass NULL/0 — resolver must still report -1 without segfaulting. */
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, NULL, 0), -1);
}

UTEST(module_resolver, tiny_errbuf_truncates_safely)
{
    HlManifest m;
    clear_manifest(&m);
    add_module(&m, "nonexistent", 1);

    HlResolvedModuleSet s = {0};
    char err[8] = {0};
    ASSERT_EQ(hl_module_resolver_resolve(&m, &s, err, sizeof(err)), -1);
    /* Must be NUL-terminated despite truncation. */
    ASSERT_EQ((int)err[sizeof(err) - 1], 0);
}

UTEST_MAIN();

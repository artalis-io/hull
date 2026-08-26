/*
 * test_manifest_seal.c - Round-trip tests for hl_manifest_seal.
 *
 * Builds a representative HlManifest by hand (without invoking the
 * Lua/JS extractors), seals it into an arena, and verifies:
 *   - all string fields are preserved (content + nul-termination)
 *   - all integer/flag fields are preserved
 *   - sealed pointers point INTO the arena (not the source)
 *   - sealed pointers stay readable after the arena is sealed
 *   - writes to sealed strings fault (via fork+SIGSEGV death test)
 *   - sealing a not-present manifest returns -1 (programming error)
 *   - sealing into an already-sealed arena returns -1 (programming error)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/manifest.h"
#include "hull/module_resolver.h"
#include <sh_seal_arena.h>
#include "hull/utils/alloc.h"

#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── fixture: a manifest with at least one of each interesting field ── */

static void build_fixture(HlManifest *m, HlAllocator *alloc)
{
    memset(m, 0, sizeof(*m));
    m->alloc   = alloc;
    m->present = 1;

    /* Use hl_alloc_malloc + memcpy to mimic what the real extractors
     * do (so hl_manifest_free can release these via the allocator). */
    #define DUP(s) ({ \
        const char *_src = (s); \
        size_t _len = strlen(_src) + 1; \
        char *_dst = hl_alloc_malloc(alloc, _len); \
        memcpy(_dst, _src, _len); \
        (const char *)_dst; \
    })

    m->fs_read[0]      = DUP("config/");      m->fs_read_count  = 1;
    m->fs_write[0]     = DUP("data/");
    m->fs_write[1]     = DUP("uploads/");     m->fs_write_count = 2;
    m->env[0]          = DUP("STRIPE_KEY");
    m->env[1]          = DUP("DATABASE_URL"); m->env_count      = 2;
    m->hosts[0]        = DUP("api.stripe.com");
    m->hosts[1]        = DUP("example.com");  m->hosts_count    = 2;
    m->csp             = DUP("default-src 'self'");
    m->csp_set         = 1;
    m->cors_origins[0] = DUP("https://app.example.com");
    m->cors_origin_count = 1;
    m->cors_methods    = DUP("GET, POST");
    m->cors_headers    = DUP("Content-Type");
    m->cors_credentials = 1;
    m->cors_max_age    = 3600;
    m->cors_set        = 1;
    m->wasm_heap       = 1048576;
    m->wasm_stack      = 65536;
    m->wasm_gas        = 100000000;
    m->wasm_max_input  = 65536;
    m->wasm_max_output = 65536;
    m->gpu             = 1;
    m->compute         = 1;
    m->tui             = 0;
    m->modules[0].name      = DUP("hull/db");
    m->modules[0].api_major = 1;
    m->modules[1].name      = DUP("hull/web/htmx");
    m->modules[1].api_major = 1;
    m->modules_count        = 2;
    m->modules_declared     = 1;
    m->allow_dynamic_code      = 0;
    m->allow_dynamic_libraries = 0;
    #undef DUP
}

/* ── helpers ──────────────────────────────────────────────────────── */

/* Returns 1 if `p` points inside arena's backing region. */
static int in_arena(const ShSealArena *a, const void *p)
{
    if (!a || !a->base || !p) return 0;
    uintptr_t lo = (uintptr_t)a->base;
    uintptr_t hi = lo + a->capacity;
    uintptr_t x  = (uintptr_t)p;
    return x >= lo && x < hi;
}

/* ── round-trip ───────────────────────────────────────────────────── */

UTEST(manifest_seal, roundtrip_preserves_all_fields)
{
    HlAllocator alloc = {0};
    HlManifest src;
    build_fixture(&src, &alloc);

    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, "manifest-test"));

    HlManifest dst;
    ASSERT_EQ(0, hl_manifest_seal(&dst, &src, &arena));

    /* Integer fields */
    ASSERT_EQ(1, dst.fs_read_count);
    ASSERT_EQ(2, dst.fs_write_count);
    ASSERT_EQ(2, dst.env_count);
    ASSERT_EQ(2, dst.hosts_count);
    ASSERT_EQ(1, dst.csp_set);
    ASSERT_EQ(1, dst.cors_origin_count);
    ASSERT_EQ(1, dst.cors_credentials);
    ASSERT_EQ(3600, dst.cors_max_age);
    ASSERT_EQ((uint32_t)1048576, dst.wasm_heap);
    ASSERT_EQ(1, dst.gpu);
    ASSERT_EQ(1, dst.compute);
    ASSERT_EQ(0, dst.tui);
    ASSERT_EQ(2, dst.modules_count);
    ASSERT_EQ(1, dst.modules_declared);
    ASSERT_EQ(1, dst.present);
    ASSERT_TRUE(dst.alloc == NULL); /* sealed dst is not allocator-owned */

    /* String contents */
    ASSERT_STREQ("config/",      dst.fs_read[0]);
    ASSERT_STREQ("data/",        dst.fs_write[0]);
    ASSERT_STREQ("uploads/",     dst.fs_write[1]);
    ASSERT_STREQ("STRIPE_KEY",   dst.env[0]);
    ASSERT_STREQ("DATABASE_URL", dst.env[1]);
    ASSERT_STREQ("api.stripe.com", dst.hosts[0]);
    ASSERT_STREQ("example.com",  dst.hosts[1]);
    ASSERT_STREQ("default-src 'self'", dst.csp);
    ASSERT_STREQ("https://app.example.com", dst.cors_origins[0]);
    ASSERT_STREQ("GET, POST",    dst.cors_methods);
    ASSERT_STREQ("Content-Type", dst.cors_headers);
    ASSERT_STREQ("hull/db",       dst.modules[0].name);
    ASSERT_EQ(1, dst.modules[0].api_major);
    ASSERT_STREQ("hull/web/htmx", dst.modules[1].name);

    /* Every dst string pointer must be IN the arena, not aliasing src */
    ASSERT_TRUE(in_arena(&arena, dst.fs_read[0]));
    ASSERT_TRUE(in_arena(&arena, dst.fs_write[0]));
    ASSERT_TRUE(in_arena(&arena, dst.fs_write[1]));
    ASSERT_TRUE(in_arena(&arena, dst.env[0]));
    ASSERT_TRUE(in_arena(&arena, dst.hosts[0]));
    ASSERT_TRUE(in_arena(&arena, dst.csp));
    ASSERT_TRUE(in_arena(&arena, dst.cors_origins[0]));
    ASSERT_TRUE(in_arena(&arena, dst.cors_methods));
    ASSERT_TRUE(in_arena(&arena, dst.modules[0].name));
    ASSERT_TRUE(in_arena(&arena, dst.modules[1].name));

    /* dst pointers must not equal src pointers (genuine copy) */
    ASSERT_TRUE(dst.fs_read[0] != src.fs_read[0]);
    ASSERT_TRUE(dst.csp        != src.csp);

    hl_manifest_free(&src);
    sh_seal_arena_destroy(&arena);
}

UTEST(manifest_seal, strings_readable_after_seal)
{
    HlAllocator alloc = {0};
    HlManifest src;
    build_fixture(&src, &alloc);

    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));

    HlManifest dst;
    ASSERT_EQ(0, hl_manifest_seal(&dst, &src, &arena));
    ASSERT_EQ(0, sh_seal_arena_seal(&arena));

    /* Reads still work after seal */
    ASSERT_STREQ("data/", dst.fs_write[0]);
    ASSERT_STREQ("default-src 'self'", dst.csp);
    ASSERT_STREQ("hull/db", dst.modules[0].name);

    hl_manifest_free(&src);
    sh_seal_arena_destroy(&arena);
}

UTEST(manifest_seal, write_to_sealed_string_faults)
{
    HlAllocator alloc = {0};
    HlManifest src;
    build_fixture(&src, &alloc);

    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));

    HlManifest dst;
    ASSERT_EQ(0, hl_manifest_seal(&dst, &src, &arena));
    ASSERT_EQ(0, sh_seal_arena_seal(&arena));

    /* Fork: child tries to overwrite a sealed string (e.g. expand
     * fs_write from "data/" to "/etc/" via byte-write). Parent
     * confirms the child died with SIGSEGV/SIGBUS - which is the
     * actual evidence that an attacker's memory-write bug would be
     * caught by the OS rather than silently rewriting the
     * allowlist. */
    pid_t pid = fork();
    if (pid == 0) {
        /* Reset SEGV/BUS handlers to SIG_DFL so the protection fault
         * propagates as a termination (WIFSIGNALED) instead of being
         * caught by a sanitizer runtime (ASan/MSan install their own
         * handlers that print a diagnostic and _exit(1), which would
         * also run leak detection on the still-live src manifest). */
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        /* Deliberate write to a sealed (RO) string to prove it faults. */
        char *target = (char *)(uintptr_t)dst.fs_write[0];
        target[0] = '/';
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    hl_manifest_free(&src);
    sh_seal_arena_destroy(&arena);
}

/* ── error paths ──────────────────────────────────────────────────── */

UTEST(manifest_seal, not_present_returns_error)
{
    HlManifest src = {0}; /* present=0 */
    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));
    HlManifest dst;
    ASSERT_EQ(-1, hl_manifest_seal(&dst, &src, &arena));
    sh_seal_arena_destroy(&arena);
}

UTEST(manifest_seal, sealed_arena_returns_error)
{
    HlAllocator alloc = {0};
    HlManifest src;
    build_fixture(&src, &alloc);

    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));
    ASSERT_EQ(0, sh_seal_arena_seal(&arena));

    HlManifest dst;
    ASSERT_EQ(-1, hl_manifest_seal(&dst, &src, &arena));

    hl_manifest_free(&src);
    sh_seal_arena_destroy(&arena);
}

UTEST(manifest_seal, null_inputs_return_error)
{
    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));
    HlManifest dst;
    HlManifest src = { .present = 1 };
    ASSERT_EQ(-1, hl_manifest_seal(NULL, &src,  &arena));
    ASSERT_EQ(-1, hl_manifest_seal(&dst, NULL,  &arena));
    ASSERT_EQ(-1, hl_manifest_seal(&dst, &src,  NULL));
    sh_seal_arena_destroy(&arena);
}

UTEST(manifest_seal, oom_returns_error)
{
    HlAllocator alloc = {0};
    HlManifest src;
    build_fixture(&src, &alloc);

    /* Tiny arena - won't fit all the strings. */
    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 16, NULL));
    /* Arena init rounds to one page (4096), so even 16-byte request
     * is fine. To actually OOM we need to exceed one page; fixture
     * is way under that. Instead, exhaust the arena manually then
     * try to seal. */
    char *fill = sh_seal_arena_alloc(&arena, arena.capacity - 4, 1);
    ASSERT_TRUE(fill != NULL);

    HlManifest dst;
    ASSERT_EQ(-1, hl_manifest_seal(&dst, &src, &arena));

    hl_manifest_free(&src);
    sh_seal_arena_destroy(&arena);
}

/* ── HlResolvedModuleSet sealed in the manifest arena (v0.1.x+) ──────
 *
 * serve.c::hl_serve_wire_caps memdup's the resolver's output into the
 * same arena that holds the manifest strings, then seals.  This test
 * mirrors that pattern at the unit level: build a fixture, allocate a
 * bitset in the arena, seal, then fork+SIGSEGV-test that a write into
 * the bitset (which would otherwise admit an undeclared module) is
 * caught by the OS.  The actual resolver isn't exercised here - this
 * is purely a "OS mprotect is what's keeping bits in place" check. */

UTEST(manifest_seal, write_to_sealed_module_set_faults)
{
    ShSealArena arena;
    ASSERT_EQ(0, sh_seal_arena_init(&arena, 4096, NULL));

    /* Populate a bitset in the arena and seal. */
    HlResolvedModuleSet seed = {0};
    seed.bits[0] = 0x00FF00FFu;  /* a few bits set, content doesn't matter */
    seed.bits[1] = 0xAA55AA55u;
    HlResolvedModuleSet *sealed =
        (HlResolvedModuleSet *)sh_seal_arena_memdup(&arena, &seed,
                                                     sizeof(seed));
    ASSERT_TRUE(sealed != NULL);
    ASSERT_EQ(0, sh_seal_arena_seal(&arena));

    /* Reads still work post-seal - sanity. */
    ASSERT_EQ(sealed->bits[0], (uint64_t)0x00FF00FFu);
    ASSERT_EQ(sealed->bits[1], (uint64_t)0xAA55AA55u);

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        /* Try to flip a bit to admit an undeclared module - the
         * exact gadget an attacker would use after landing a heap-
         * write inside the bitset's storage.  Sealing turns the
         * write into a fault. */
        ((volatile HlResolvedModuleSet *)sealed)->bits[0] = (uint64_t)-1;
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status));
    int sig = WTERMSIG(status);
    ASSERT_TRUE(sig == SIGSEGV || sig == SIGBUS);

    sh_seal_arena_destroy(&arena);
}

UTEST_MAIN()

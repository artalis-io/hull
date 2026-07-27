/*
 * test_sbom.c. Tests for the SBOM data table + format functions.
 *
 * Orthogonality check: the SBOM module is meant to depend only on
 * cacert.h and mbedTLS (for embedded blob SHA-256). The test links
 * against the same minimal surface. If SBOM accidentally starts
 * pulling in other Hull subsystems, the test build will fail at link
 * time and the regression is caught immediately.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/sbom.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>

/* ── Table integrity ──────────────────────────────────────────────── */

UTEST(sbom, entries_non_empty)
{
    size_t count = 0;
    const HlSbomEntry *entries = hl_sbom_entries(&count);
    ASSERT_NE(entries, NULL);
    ASSERT_GT(count, 0u);
}

UTEST(sbom, hull_entry_is_first)
{
    size_t count = 0;
    const HlSbomEntry *entries = hl_sbom_entries(&count);
    ASSERT_GE(count, 1u);
    ASSERT_STREQ(entries[0].name, "hull");
    ASSERT_STREQ(entries[0].license_spdx, "AGPL-3.0-or-later");
}

UTEST(sbom, every_entry_has_required_fields)
{
    size_t count = 0;
    const HlSbomEntry *entries = hl_sbom_entries(&count);
    for (size_t i = 0; i < count; i++) {
        const HlSbomEntry *e = &entries[i];
        ASSERT_NE_MSG(e->name, NULL, "entry name must not be NULL");
        ASSERT_GT_MSG(strlen(e->name), 0u, "entry name must not be empty");
        ASSERT_NE_MSG(e->license_spdx, NULL, "license_spdx must not be NULL");
        ASSERT_GT_MSG(strlen(e->license_spdx), 0u, "license_spdx must not be empty");
        ASSERT_NE_MSG(e->url, NULL, "url must not be NULL");
        ASSERT_GT_MSG(strlen(e->url), 0u, "url must not be empty");
        ASSERT_NE_MSG(e->role, NULL, "role must not be NULL");
    }
}

UTEST(sbom, submodule_commits_are_hex)
{
    /* Every entry whose commit string is non-empty AND not the
     * "unknown" fallback must be lowercase hex characters only. */
    size_t count = 0;
    const HlSbomEntry *entries = hl_sbom_entries(&count);
    for (size_t i = 0; i < count; i++) {
        const HlSbomEntry *e = &entries[i];
        if (!e->commit || e->commit[0] == '\0') continue;
        if (strcmp(e->commit, "unknown") == 0) continue;
        size_t len = strlen(e->commit);
        ASSERT_GE_MSG(len, 7u, "commit too short to be a SHA");
        for (size_t j = 0; j < len; j++) {
            char c = e->commit[j];
            int ok = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            ASSERT_TRUE_MSG(ok, "commit must be lowercase hex");
        }
    }
}

/* ── Format parsing ───────────────────────────────────────────────── */

UTEST(sbom, parse_format_known_names)
{
    ASSERT_EQ(hl_sbom_parse_format("human"),     HL_SBOM_HUMAN);
    ASSERT_EQ(hl_sbom_parse_format("json"),      HL_SBOM_JSON);
    ASSERT_EQ(hl_sbom_parse_format("cyclonedx"), HL_SBOM_CYCLONEDX);
    ASSERT_EQ(hl_sbom_parse_format("spdx"),      HL_SBOM_SPDX);
}

UTEST(sbom, parse_format_rejects_unknown)
{
    ASSERT_EQ(hl_sbom_parse_format("xml"), -1);
    ASSERT_EQ(hl_sbom_parse_format(""), -1);
    ASSERT_EQ(hl_sbom_parse_format(NULL), -1);
}

/* ── Format output: helper to capture to memory ───────────────────── */

/* Capture sbom output to a heap string. Uses tmpfile()/ftell()/fread()
 * rather than open_memstream() because open_memstream is a GNU/POSIX-2008
 * extension not declared in cosmocc's <stdio.h>. tmpfile() + ftell() +
 * fread() is C89 and works everywhere Hull targets. */
static char *format_to_string(HlSbomFormat fmt)
{
    FILE *fp = tmpfile();
    if (!fp) return NULL;
    if (hl_sbom_format(fmt, fp) != 0) { fclose(fp); return NULL; }
    if (fflush(fp) != 0) { fclose(fp); return NULL; }
    long size = ftell(fp);
    if (size < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

UTEST(sbom, format_human_non_empty)
{
    char *out = format_to_string(HL_SBOM_HUMAN);
    ASSERT_NE(out, NULL);
    ASSERT_GT(strlen(out), 100u);  /* arbitrary lower bound */
    /* Must mention "hull" as the runtime entry */
    ASSERT_NE_MSG(strstr(out, "hull"), NULL, "human format must list 'hull'");
    ASSERT_NE_MSG(strstr(out, "AGPL-3.0"), NULL, "human format must list hull's license");
    free(out);
}

UTEST(sbom, format_json_structure)
{
    char *out = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(out, NULL);
    /* Top-level fields */
    ASSERT_NE_MSG(strstr(out, "\"hull_version\""), NULL, "json must have hull_version");
    ASSERT_NE_MSG(strstr(out, "\"components\""), NULL, "json must have components");
    /* Component fields */
    ASSERT_NE_MSG(strstr(out, "\"name\""), NULL, "json must have name");
    ASSERT_NE_MSG(strstr(out, "\"license_spdx\""), NULL, "json must have license_spdx");
    ASSERT_NE_MSG(strstr(out, "\"url\""), NULL, "json must have url");
    /* Array bracket balance: open count == close count */
    size_t opens = 0, closes = 0;
    for (size_t i = 0; out[i]; i++) {
        if (out[i] == '[') opens++;
        if (out[i] == ']') closes++;
    }
    ASSERT_EQ_MSG(opens, closes, "json bracket count must balance");
    free(out);
}

UTEST(sbom, format_cyclonedx_structure)
{
    char *out = format_to_string(HL_SBOM_CYCLONEDX);
    ASSERT_NE(out, NULL);
    ASSERT_NE_MSG(strstr(out, "\"bomFormat\":\"CycloneDX\""), NULL,
                  "must have bomFormat:CycloneDX");
    ASSERT_NE_MSG(strstr(out, "\"specVersion\":\"1.5\""), NULL,
                  "must have specVersion:1.5");
    ASSERT_NE_MSG(strstr(out, "\"components\""), NULL,
                  "must have components array");
    ASSERT_NE_MSG(strstr(out, "\"type\":\"library\""), NULL,
                  "components must be typed as library");
    /* CPE strings present for the components that have one registered
     * (currently lua, quickjs, sqlite, mbedtls, tinycc). The mbedTLS
     * entry is unconditional in any build with HTTP enabled, so this
     * assertion holds on the default test build. */
    ASSERT_NE_MSG(strstr(out, "\"cpe\":\"cpe:2.3:a:arm:mbed_tls"), NULL,
                  "CycloneDX must emit cpe for mbedTLS");
    free(out);
}

UTEST(sbom, format_spdx_structure)
{
    char *out = format_to_string(HL_SBOM_SPDX);
    ASSERT_NE(out, NULL);
    ASSERT_NE_MSG(strstr(out, "\"spdxVersion\":\"SPDX-2.3\""), NULL,
                  "must have spdxVersion:SPDX-2.3");
    ASSERT_NE_MSG(strstr(out, "\"dataLicense\":\"CC0-1.0\""), NULL,
                  "must have dataLicense:CC0-1.0");
    ASSERT_NE_MSG(strstr(out, "SPDXRef-DOCUMENT"), NULL,
                  "must have document SPDXID");
    ASSERT_NE_MSG(strstr(out, "\"packages\""), NULL,
                  "must have packages array");
    free(out);
}

/* ── Embedded blob SHA-256 ────────────────────────────────────────── */

UTEST(sbom, embedded_blob_sha256_cached)
{
    /* Find an entry with an embedded blob (CA bundle on default builds);
     * if none on this build flavor, skip. Two calls must return the same
     * pointer (cached). */
    size_t count = 0;
    const HlSbomEntry *entries = hl_sbom_entries(&count);
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].embedded_blob_sha256) continue;
        const char *s1 = entries[i].embedded_blob_sha256();
        const char *s2 = entries[i].embedded_blob_sha256();
        ASSERT_NE(s1, NULL);
        ASSERT_EQ_MSG(s1, s2, "cached SHA-256 pointer must be stable");
        /* Must be 64-char lowercase hex (SHA-256). */
        ASSERT_EQ(strlen(s1), 64u);
        for (size_t j = 0; j < 64; j++) {
            char c = s1[j];
            int ok = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            ASSERT_TRUE_MSG(ok, "SHA-256 must be lowercase hex");
        }
        return;
    }
    /* Build flavor without any embedded blob. Nothing to test; OK. */
}

/* ── Format determinism (call twice, get same bytes) ──────────────── */

UTEST(sbom, format_json_is_deterministic)
{
    char *a = format_to_string(HL_SBOM_JSON);
    char *b = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(a, NULL);
    ASSERT_NE(b, NULL);
    ASSERT_STREQ_MSG(a, b, "JSON output must be byte-identical between calls");
    free(a); free(b);
}

UTEST(sbom, format_cyclonedx_is_deterministic)
{
    char *a = format_to_string(HL_SBOM_CYCLONEDX);
    char *b = format_to_string(HL_SBOM_CYCLONEDX);
    ASSERT_NE(a, NULL);
    ASSERT_NE(b, NULL);
    ASSERT_STREQ(a, b);
    free(a); free(b);
}

UTEST(sbom, format_spdx_is_deterministic)
{
    char *a = format_to_string(HL_SBOM_SPDX);
    char *b = format_to_string(HL_SBOM_SPDX);
    ASSERT_NE(a, NULL);
    ASSERT_NE(b, NULL);
    ASSERT_STREQ(a, b);
    free(a); free(b);
}

/* ── Error paths ──────────────────────────────────────────────────── */

UTEST(sbom, format_rejects_null_fp)
{
    /* Public API contract: hl_sbom_format returns -1 if fp is NULL. */
    ASSERT_EQ(hl_sbom_format(HL_SBOM_HUMAN, NULL), -1);
    ASSERT_EQ(hl_sbom_format(HL_SBOM_JSON, NULL), -1);
    ASSERT_EQ(hl_sbom_format(HL_SBOM_CYCLONEDX, NULL), -1);
    ASSERT_EQ(hl_sbom_format(HL_SBOM_SPDX, NULL), -1);
}

UTEST(sbom, format_rejects_unknown_enum)
{
    /* Defensive: an out-of-range enum value should error, not crash.
     * Cast through int to bypass enum type-check; production callers
     * shouldn't do this, but the function shouldn't trust them. */
    FILE *fp = tmpfile();
    ASSERT_TRUE(fp != NULL);
    int rc = hl_sbom_format((HlSbomFormat)999, fp);
    fclose(fp);
    ASSERT_EQ(rc, -1);
}

/* ── Binary self-SHA-256 (gap §0.3.11) ────────────────────────────── */

UTEST(sbom, binary_sha256_absent_when_path_unset)
{
    /* Default state (no set_binary_path call): json should NOT contain
     * the binary_sha256 key. Clear the path explicitly in case a prior
     * test set it. */
    hl_sbom_set_binary_path(NULL);
    char *out = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(out, NULL);
    ASSERT_TRUE(strstr(out, "\"binary_sha256\":") == NULL);
    free(out);
}

UTEST(sbom, binary_sha256_present_when_path_set)
{
    /* MSan + libc-stdio + mbedtls_sha256 interact badly on the CI MSan
     * runner: fopen+fread of an mkstemp file followed by mbedtls_sha256
     * silently returns NULL from sha256_binary (no MSan diagnostic; the
     * one-shot and streaming SHA-256 APIs both fail in the same way).
     * The feature itself is verified on every other CI variant + an
     * e2e smoke check against ./build/hull's actual SHA-256. Skip
     * rather than block MSan on a test-method incompatibility. */
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
    return;
#  endif
#endif

    /* Create a tiny temp file with known content and point set_binary_path
     * at it. Avoids env dependencies (/proc/self/exe absent under some
     * runners; /usr/bin/true absent on minimal containers; argv[0] not
     * always a real path). */
    char path[] = "/tmp/hull_sbom_test_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return;  /* skip cleanly on sandboxed envs */
    FILE *fp = fdopen(fd, "wb");
    if (!fp) { close(fd); unlink(path); return; }
    const char *fixture = "hull-sbom-test-fixture";
    size_t fixlen = strlen(fixture);
    if (fwrite(fixture, 1, fixlen, fp) != fixlen) {
        fclose(fp); unlink(path); return;
    }
    fclose(fp);

    hl_sbom_set_binary_path(path);
    char *out = format_to_string(HL_SBOM_JSON);
    unlink(path);
    ASSERT_NE(out, NULL);

    const char *key = strstr(out, "\"binary_sha256\":\"");
    ASSERT_NE_MSG(key, NULL, "binary_sha256 field must be present");

    free(out);
    /* Reset to NULL so determinism tests see the default shape. */
    hl_sbom_set_binary_path(NULL);
}

/* ── libhull scope (hull sbom --subject=libhull) ───────────────────── */

static int sbom_count(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) c++;
    return c;
}

/* The excluded set: script runtimes + compiler backend + the hull
 * self-entry. Everything else is part of the libhull embedding surface. */
static int sbom_is_excluded(const char *name)
{
    return strcmp(name, "lua") == 0 || strcmp(name, "quickjs") == 0 ||
           strcmp(name, "tinycc") == 0 || strcmp(name, "hull") == 0;
}

UTEST(sbom, in_libhull_flags_match_policy)
{
    size_t n = 0;
    const HlSbomEntry *e = hl_sbom_entries(&n);
    ASSERT_GT(n, 0u);
    for (size_t i = 0; i < n; i++) {
        if (sbom_is_excluded(e[i].name))
            ASSERT_EQ_MSG(e[i].in_libhull, 0, e[i].name);
        else
            ASSERT_EQ_MSG(e[i].in_libhull, 1, e[i].name);
    }
}

UTEST(sbom, libhull_scope_filters_and_renames)
{
    hl_sbom_set_scope_libhull(0);
    char *def = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(def, NULL);
    int def_components = sbom_count(def, "\"name\"");

    hl_sbom_set_scope_libhull(1);
    char *lh = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(lh, NULL);
    int lh_components = sbom_count(lh, "\"name\"");

    /* Subject renamed, core kept, JS runtime dropped, strictly fewer
     * components than the whole-hull SBOM. */
    ASSERT_NE_MSG(strstr(lh, "libhull"), NULL, "libhull scope names the subject");
    ASSERT_NE_MSG(strstr(lh, "mbedtls"), NULL, "libhull keeps the linked core");
    ASSERT_EQ_MSG(strstr(lh, "quickjs"), NULL, "libhull drops the JS runtime");
    ASSERT_LT(lh_components, def_components);

    free(def);
    free(lh);
    hl_sbom_set_scope_libhull(0);  /* restore global for later tests */
}

UTEST(sbom, all_formats_render_under_libhull_scope)
{
    hl_sbom_set_scope_libhull(1);
    const HlSbomFormat fmts[] = {
        HL_SBOM_HUMAN, HL_SBOM_JSON, HL_SBOM_CYCLONEDX, HL_SBOM_SPDX,
    };
    for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        char *out = format_to_string(fmts[i]);
        ASSERT_NE(out, NULL);
        ASSERT_GT(strlen(out), 0u);
        ASSERT_NE_MSG(strstr(out, "libhull"), NULL, "libhull subject present");
        free(out);
    }
    hl_sbom_set_scope_libhull(0);
}

/* ── flavor scope (hull sbom --flavor=X) ───────────────────────────── */

UTEST(sbom, needs_http_flags_match_policy)
{
    size_t n = 0;
    const HlSbomEntry *e = hl_sbom_entries(&n);
    for (size_t i = 0; i < n; i++) {
        int http = strcmp(e[i].name, "keel") == 0 || strcmp(e[i].name, "mbedtls") == 0;
        ASSERT_EQ_MSG(e[i].needs_http, http, e[i].name);
    }
}

UTEST(sbom, flavor_scope_unknown_rejected)
{
    ASSERT_EQ(hl_sbom_set_scope_flavor("bogus"), -1);
    ASSERT_EQ(hl_sbom_set_scope_flavor("full"), 0);
    ASSERT_EQ(hl_sbom_set_scope_flavor("pure-compute"), 0);
    ASSERT_EQ(hl_sbom_set_scope_flavor(NULL), 0);   /* clears */
}

UTEST(sbom, pure_compute_flavor_drops_http_deps)
{
    hl_sbom_set_scope_flavor("pure-compute");
    char *pc = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(pc, NULL);
    ASSERT_NE_MSG(strstr(pc, "pure-compute"), NULL, "subject names the flavor");
    ASSERT_EQ_MSG(strstr(pc, "\"mbedtls\""), NULL, "pure-compute drops mbedTLS");
    ASSERT_EQ_MSG(strstr(pc, "\"keel\""), NULL, "pure-compute drops Keel");

    /* full keeps the HTTP deps (same vendored set as the binary). */
    hl_sbom_set_scope_flavor("full");
    char *fl = format_to_string(HL_SBOM_JSON);
    ASSERT_NE(fl, NULL);
    ASSERT_NE_MSG(strstr(fl, "mbedtls"), NULL, "full keeps mbedTLS");
    ASSERT_LT(sbom_count(pc, "\"name\""), sbom_count(fl, "\"name\""));

    free(pc);
    free(fl);
    hl_sbom_set_scope_flavor(NULL);
}

UTEST_MAIN()

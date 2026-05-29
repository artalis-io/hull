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

static char *format_to_string(HlSbomFormat fmt)
{
    char *buf = NULL;
    size_t size = 0;
    FILE *fp = open_memstream(&buf, &size);
    if (!fp) return NULL;
    int rc = hl_sbom_format(fmt, fp);
    fclose(fp);
    if (rc != 0) { free(buf); return NULL; }
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
    char *buf = NULL; size_t size = 0;
    FILE *fp = open_memstream(&buf, &size);
    ASSERT_NE(fp, NULL);
    int rc = hl_sbom_format((HlSbomFormat)999, fp);
    fclose(fp);
    free(buf);
    ASSERT_EQ(rc, -1);
}

UTEST_MAIN()

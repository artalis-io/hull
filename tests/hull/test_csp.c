/*
 * test_csp.c — Tests for the CSP preset registry.
 *
 * Covers hl_csp_resolve (preset-name -> expanded policy; literal
 * passthrough; NULL passthrough) and hl_csp_preset_name_for (reverse
 * lookup used by inspect / agent output).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/csp.h"
#include <string.h>

UTEST(hl_csp, resolve_null_passthrough)
{
    ASSERT_EQ(hl_csp_resolve(NULL), (const char *)NULL);
}

UTEST(hl_csp, resolve_htmx_preset_expands)
{
    const char *out = hl_csp_resolve("htmx");
    ASSERT_TRUE(out != NULL);
    ASSERT_TRUE(out != (const char *)"htmx");  /* not the input */
    /* The expanded policy is the documented htmx preset shape. */
    ASSERT_TRUE(strstr(out, "default-src 'none'") != NULL);
    ASSERT_TRUE(strstr(out, "script-src 'self'") != NULL);
    ASSERT_TRUE(strstr(out, "style-src 'self' 'unsafe-inline'") != NULL);
    ASSERT_TRUE(strstr(out, "connect-src 'self'") != NULL);
    ASSERT_TRUE(strstr(out, "frame-ancestors 'none'") != NULL);
}

UTEST(hl_csp, resolve_literal_passthrough)
{
    /* Unknown preset names must pass through unchanged, NOT silently
     * fall back to the default. This is the typo-safety property the
     * design depends on. */
    const char *literal = "default-src 'self'; script-src 'none'";
    const char *out = hl_csp_resolve(literal);
    ASSERT_EQ(out, literal);  /* same pointer back */
}

UTEST(hl_csp, resolve_empty_string_is_literal)
{
    /* Empty string is a (degenerate) literal, not a preset name. */
    const char *empty = "";
    const char *out = hl_csp_resolve(empty);
    ASSERT_EQ(out, empty);
}

UTEST(hl_csp, preset_name_for_htmx_expansion)
{
    /* The reverse lookup: given an expanded policy, find the preset
     * name. Used by inspect / agent to render "csp = htmx (preset)". */
    const char *expanded = hl_csp_resolve("htmx");
    const char *name = hl_csp_preset_name_for(expanded);
    ASSERT_TRUE(name != NULL);
    ASSERT_EQ(strcmp(name, "htmx"), 0);
}

UTEST(hl_csp, preset_name_for_literal_returns_null)
{
    const char *name = hl_csp_preset_name_for("default-src 'self'");
    ASSERT_EQ(name, (const char *)NULL);
}

UTEST(hl_csp, preset_name_for_null_returns_null)
{
    ASSERT_EQ(hl_csp_preset_name_for(NULL), (const char *)NULL);
}

UTEST_MAIN()

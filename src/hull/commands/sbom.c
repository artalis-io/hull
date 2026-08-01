/*
 * commands/sbom.c. `hull sbom` subcommand.
 *
 * Thin argv-parsing wrapper around `hl_sbom_format()`. All the actual
 * SBOM data + format logic lives in `src/hull/sbom.c`. This file is
 * just the CLI surface.
 *
 *   hull sbom                      . Human-readable table (default)
 *   hull sbom --format=json        . Flat JSON array (also hull agent sbom)
 *   hull sbom --format=cyclonedx   . CycloneDX 1.5 JSON (industry std)
 *   hull sbom --format=spdx        . SPDX 2.3 JSON (industry std)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/sbom.h"
#include "hull/sbom.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *fp)
{
    fprintf(fp,
        "Usage: hull sbom [--format=<format>] [--subject=<hull|libhull>]\n"
        "                 [--flavor=<full|pure-compute>]\n"
        "\n"
        "Print the Software Bill of Materials for this hull binary.\n"
        "\n"
        "Subject (default hull):\n"
        "  hull        The whole hull binary.\n"
        "  libhull     The no-runtime embedding archive (libhull.a): drops the\n"
        "              Lua/QuickJS runtimes; keeps the linked core + Keel.\n"
        "\n"
        "Flavor (report a `hull build --flavor` preset's dependency set):\n"
        "  full           Same vendored set as the binary.\n"
        "  pure-compute   The preset for an app declaring no HTTP/TLS; drops\n"
        "                 Keel + mbedTLS. (Since Phase 4.3 a flavor is a\n"
        "                 build.lua preset on the composable base, not a\n"
        "                 pre-built platform lib.)\n"
        "\n"
        "Formats:\n"
        "  human       Default. Pretty table mirroring LICENSING.md style.\n"
        "  json        Flat JSON object {hull_version, components: [...]}.\n"
        "              Same shape as `hull agent sbom`.\n"
        "  cyclonedx   CycloneDX 1.5 (NTIA-aligned, defense/regulated default).\n"
        "  spdx        SPDX 2.3 (common in OSS compliance pipelines).\n"
        "\n"
        "Each component carries a modularization tier: `base` (always linked),\n"
        "`feature` (auto-composed - embedded in hull, composed per app when the\n"
        "app needs it), or `with` (opt-in `hull build --with=<name>`). So the\n"
        "SBOM reflects that Hull's base is composed, not monolithic.\n"
        "\n"
        "Data is baked at build time; the binary self-describes its actual\n"
        "vendored contents. Submodule SHAs come from `git rev-parse HEAD`\n"
        "at the time of `make`; snapshot versions are hardcoded in the\n"
        "static entry table. Build-flag gating means a `make HL_ENABLE_DB=0`\n"
        "build correctly omits SQLite from the SBOM.\n");
}

int hl_cmd_sbom(int argc, char **argv, const HlCommandEnv *env)
{
    /* Make the running binary's path available to the SBOM module so it
     * can emit binary_sha256 in json/cyclonedx/spdx output. argv[0] is
     * usually a usable path (./hull, /usr/local/bin/hull, etc.); if it's
     * just "hull" via $PATH lookup the SHA is silently omitted. */
    if (env && env->hull_exe) hl_sbom_set_binary_path(env->hull_exe);

    HlSbomFormat fmt = HL_SBOM_HUMAN;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        /* --subject=<hull|libhull>: scope the SBOM. "libhull" describes the
         * no-runtime embedding archive (drops the Lua/QuickJS runtimes).
         * Self-contained: a --subject arg always continues or returns, so no
         * value-consuming (i-advancing) path falls through to --format below. */
        if (strcmp(argv[i], "--subject") == 0 ||
            strncmp(argv[i], "--subject=", 10) == 0) {
            const char *sub = NULL;
            if (argv[i][9] == '=') {
                sub = argv[i] + 10;          /* --subject=VALUE */
            } else if (i + 1 < argc) {
                sub = argv[++i];             /* --subject VALUE */
            }
            if (!sub) {
                fprintf(stderr,
                        "hull sbom: --subject requires a value (hull|libhull)\n");
                usage(stderr);
                return 1;
            }
            if (strcmp(sub, "libhull") == 0)   hl_sbom_set_scope_libhull(1);
            else if (strcmp(sub, "hull") == 0) hl_sbom_set_scope_libhull(0);
            else {
                fprintf(stderr,
                        "hull sbom: unknown subject '%s' (expected hull|libhull)\n",
                        sub);
                usage(stderr);
                return 1;
            }
            continue;
        }
        /* --flavor=<full|pure-compute>: report the
         * dependency set that flavor's platform lib links (the SBOM analogue
         * of `hull build --flavor`). Self-contained like --subject above. */
        if (strcmp(argv[i], "--flavor") == 0 ||
            strncmp(argv[i], "--flavor=", 9) == 0) {
            const char *fl = NULL;
            if (argv[i][8] == '=') {
                fl = argv[i] + 9;            /* --flavor=VALUE */
            } else if (i + 1 < argc) {
                fl = argv[++i];              /* --flavor VALUE */
            }
            if (!fl) {
                fprintf(stderr, "hull sbom: --flavor requires a value "
                        "(full|pure-compute)\n");
                usage(stderr);
                return 1;
            }
            if (hl_sbom_set_scope_flavor(fl) != 0) {
                fprintf(stderr, "hull sbom: unknown flavor '%s' "
                        "(expected full|pure-compute)\n",
                        fl);
                usage(stderr);
                return 1;
            }
            continue;
        }
        const char *val = NULL;
        if (strncmp(argv[i], "--format=", 9) == 0) {
            val = argv[i] + 9;
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            val = argv[++i];
        } else {
            fprintf(stderr, "hull sbom: unknown argument: %s\n", argv[i]);
            usage(stderr);
            return 1;
        }
        int parsed = hl_sbom_parse_format(val);
        if (parsed < 0) {
            fprintf(stderr, "hull sbom: unknown format '%s'\n", val);
            usage(stderr);
            return 1;
        }
        fmt = (HlSbomFormat)parsed;
    }

    return hl_sbom_format(fmt, stdout) == 0 ? 0 : 1;
}

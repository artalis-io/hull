/*
 * commands/help.c — top-level `hull help` / `hull --help` usage printer.
 *
 * Lists every subcommand grouped by purpose. The list mirrors the
 * dispatch table in `commands/dispatch.c`; subcommands gated by
 * build-time flags (HL_ENABLE_HTTP_SERVER, HL_ENABLE_DB,
 * HL_ENABLE_HTTP_CLIENT) are suppressed when those flags are off so
 * the help only advertises what this binary can actually do.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/help.h"

#include <stdio.h>
#include <string.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

static void section(FILE *f, const char *title)
{
    fprintf(f, "\n%s:\n", title);
}

static void row(FILE *f, const char *name, const char *desc)
{
    fprintf(f, "  %-16s %s\n", name, desc);
}

int hl_cmd_help(int argc, char **argv, const HlCommandEnv *env)
{
    (void)argc;
    (void)argv;
    (void)env;

    FILE *f = stdout;
    fprintf(f,
        "hull %s — capability-secure local-first runtime\n"
        "\n"
        "Usage: hull [global flags] <command> [command flags]\n"
        "       hull <app.lua|app.js>          run an app (implicit `dev`)\n"
        "\n"
        "Global flags:\n"
        "  --version, -v          print version and exit\n"
        "  --help, -h             show this help\n"
        "  --app-dir DIR          run the command against DIR (default: .)\n"
        "  --verbose              chattier output where supported\n"
        "  --json                 machine-readable output where supported\n",
        HL_VERSION);

    section(f, "Scaffolding");
    row(f, "init",          "initialise an app in the current dir");
    row(f, "new",           "scaffold a new app in a fresh directory");

    section(f, "Build & ship");
    row(f, "build",         "compile an app to a standalone binary");
    row(f, "eject",         "emit a standalone Makefile + sources for an app");
    row(f, "verify",        "verify an app or built binary's signature");
    row(f, "inspect",       "show the manifest + signature of a built binary");
    row(f, "manifest",      "extract / print an app's manifest");
    row(f, "sign-platform", "(maintainer) sign libhull_platform.a");
    row(f, "sign-release",  "(maintainer) sign a release manifest");
    row(f, "verify-release","verify an Ed25519 signature on hull.sha256");
    row(f, "keygen",        "generate an Ed25519 keypair");

#ifdef HL_ENABLE_HTTP_SERVER
    section(f, "Develop & test");
    row(f, "dev",           "run an app with hot reload");
    row(f, "test",          "run an app's tests against the in-process harness");
    row(f, "agent",         "machine-readable introspection for AI / tooling");
    row(f, "mcp",           "Model Context Protocol server for agents");
#endif

    section(f, "Diagnostics");
    row(f, "doctor",        "check this hull binary's capabilities + host toolchain");
    row(f, "check",         "validate an app's manifest before running");
    row(f, "modules",       "list / explain the first-party module registry");
    row(f, "version",       "print version (also --version / -v)");

    section(f, "Compute / WASM");
    row(f, "compute",       "scaffold, build, test, check WASM compute modules");

#ifdef HL_ENABLE_DB
    section(f, "Database");
    row(f, "migrate",       "apply / list / scaffold SQL migrations");
#endif

    section(f, "Deployment");
    row(f, "deploy",        "render Dockerfile / systemd / fly.toml from manifest");

#ifdef HL_ENABLE_HTTP_CLIENT
    section(f, "Self-management");
    row(f, "update",        "self-update hull from GitHub releases");
    row(f, "tools",         "install / list / uninstall side-loaded tools (e.g. wamrc)");
    row(f, "flavor",        "install / list published build flavors for hull build --flavor");
    row(f, "cache",         "list / prune / clear runtime cache pool (HULL_CACHE_DIR)");
#else
    section(f, "Self-management");
    row(f, "cache",         "list / prune / clear runtime cache pool (HULL_CACHE_DIR)");
#endif

    fprintf(f,
        "\n"
        "Run `hull <command> --help` for per-command flags.\n"
        "Docs: https://gethull.dev\n"
        "\n"
        "AI agents: bootstrap with\n"
        "  hull agent context --task=orientation --level=minimal\n"
        "Then `hull agent context --list` to discover topics, "
        "or `hull agent overview` inside an app dir.\n");
    return 0;
}

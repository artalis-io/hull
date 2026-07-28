/*
 * commands/modules.c — `hull modules` subcommand.
 *
 * Three forms:
 *   hull modules list [APP_DIR]   — what an app declares (parsed via Lua tool)
 *   hull modules available        — full first-party registry
 *   hull modules explain <NAME>   — spec for one module
 *
 * `--json` (global flag) toggles machine-readable output. The `list`
 * form delegates to a tiny Lua helper that mirrors `hull manifest`'s
 * extraction; the other two read directly from the in-memory registry
 * compiled into the hull binary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/modules.h"
#include "hull/module_registry.h"
#include "hull/tool.h"

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Canonical cap-bit → label table. Keep in sync with the bits in
 * include/hull/module_registry.h (HL_MOD_CAP_*). Used by both the
 * human formatter (above) and the JSON writer (below) so the two
 * outputs never drift. HL_MOD_CAP_HTTP is the back-compat alias
 * for HTTP_CLIENT | HTTP_SERVER; we don't print it separately
 * because every module that has it also has one of the two
 * granular bits set. */
static const struct {
    uint32_t bit;
    const char *name;
} CAP_TABLE[] = {
    { HL_MOD_CAP_FS,          "fs"               },
    { HL_MOD_CAP_HOSTS,       "hosts"            },
    { HL_MOD_CAP_ENV,         "env"              },
    { HL_MOD_CAP_DB,          "build:db"         },
    { HL_MOD_CAP_WASM,        "build:wasm"       },
    { HL_MOD_CAP_GPU,         "build:gpu"        },
    { HL_MOD_CAP_HTTP_CLIENT, "build:http-client" },
    { HL_MOD_CAP_HTTP_SERVER, "build:http-server" },
    { HL_MOD_CAP_TUI,         "build:tui"        },
    { HL_MOD_CAP_IMAGE,       "build:image"      },
    { 0, NULL }
};

static void format_caps_human(uint32_t caps, char *out, size_t cap)
{
    /* Append a comma-separated list of human cap names. */
    if (!caps) { snprintf(out, cap, "none"); return; }
    out[0] = '\0';
    int first = 1;
    for (int i = 0; CAP_TABLE[i].name; i++) {
        if (caps & CAP_TABLE[i].bit) {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s",
                     first ? "" : ", ", CAP_TABLE[i].name);
            first = 0;
        }
    }
}

static void write_spec_json(ShJsonWriter *w, const HlModuleSpec *s)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "name", s->name);
    sh_json_write_kv_int   (w, "api_major", s->api_major);
    sh_json_write_kv_bool  (w, "intrinsic", s->intrinsic ? 1 : 0);
    sh_json_write_kv_bool  (w, "pure",      s->pure ? 1 : 0);

    sh_json_write_key(w, "required_caps");
    sh_json_write_array_start(w);
    /* Same CAP_TABLE as format_caps_human (single source of truth). */
    for (int i = 0; CAP_TABLE[i].name; i++)
        if (s->required_caps & CAP_TABLE[i].bit)
            sh_json_write_string(w, CAP_TABLE[i].name);
    sh_json_write_array_end(w);

    sh_json_write_key(w, "deps");
    sh_json_write_array_start(w);
    for (int i = 0; i < HL_MODULE_MAX_DEPS && s->deps[i]; i++)
        sh_json_write_string(w, s->deps[i]);
    sh_json_write_array_end(w);

    sh_json_write_object_end(w);
}

static void print_spec_human(const HlModuleSpec *s)
{
    char caps[128];
    format_caps_human(s->required_caps, caps, sizeof(caps));

    printf("%-32s api_major=%-3d  %s%s\n",
        s->name, (int)s->api_major,
        s->intrinsic ? "intrinsic " : "",
        s->pure ? "pure" : "side-effect");
    printf("  required caps : %s\n", caps);

    if (s->deps[0]) {
        printf("  deps          :");
        for (int i = 0; i < HL_MODULE_MAX_DEPS && s->deps[i]; i++)
            printf(" %s", s->deps[i]);
        printf("\n");
    }
}

/* ── available ───────────────────────────────────────────────────── */

static int cmd_available(int argc, char **argv, const HlCommandEnv *env)
{
    int tui = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--tui") == 0) { tui = 1; break; }
    }
#ifdef HL_TUI_LINKED
    if (tui) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr,
                "hull modules available --tui: not attached to a "
                "terminal. Run without --tui for plain text.\n");
            return 1;
        }
        return hull_tool("hull.modules_available_tui",
                         argc, argv, env->hull_exe);
    }
#else
    if (tui) {
        fprintf(stderr, "hull modules available --tui: this build was "
                        "compiled without HL_ENABLE_TUI.\n");
        return 1;
    }
#endif

    size_t total = 0;
    const HlModuleSpec *all = hl_module_registry_all(&total);

    if (env->json_output) {
        ShJsonBuf jb;
        sh_json_buf_init(&jb);
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, &jb);
        sh_json_write_object_start(&w);
        sh_json_write_kv_int(&w, "count", (int)total);
        sh_json_write_key(&w, "modules");
        sh_json_write_array_start(&w);
        for (size_t i = 0; i < total; i++)
            write_spec_json(&w, &all[i]);
        sh_json_write_array_end(&w);
        sh_json_write_object_end(&w);
        char *out = sh_json_buf_take(&jb);
        if (out) {
            printf("%s\n", out);
            free(out);
        }
        sh_json_buf_free(&jb);
        return 0;
    }

    printf("First-party Hull module registry — %zu entries\n", total);

    /* Section grouping (v0.2.0): show the namespace shape so a user
     * eyeballing the list sees the web/ cluster as a unit. Registry
     * sort order doesn't naturally cluster sections (hull/worker
     * sorts after hull/web entries), so iterate once per section.
     * The order is intrinsic -> general utilities -> web
     * (HTTP/HTML/realtime) -> web middleware. */
    static const struct {
        const char *header;
        int         sec_id;
    } SECTIONS[] = {
        { "Intrinsic (always declared)",              0 },
        { "Core (runtime + cross-cutting utilities)", 1 },
        { "Web (HTTP / HTML / real-time)",            2 },
        { "Web middleware",                           3 },
    };
    for (size_t s = 0; s < sizeof(SECTIONS) / sizeof(SECTIONS[0]); s++) {
        int printed_header = 0;
        for (size_t i = 0; i < total; i++) {
            const char *name = all[i].name;
            int section;
            if      (strcmp(name, "hull/app") == 0)                  section = 0;
            else if (strncmp(name, "hull/web/middleware/", 20) == 0) section = 3;
            else if (strncmp(name, "hull/web/", 9) == 0)             section = 2;
            else                                                     section = 1;
            if (section != SECTIONS[s].sec_id) continue;
            if (!printed_header) {
                printf("\n== %s ==\n\n", SECTIONS[s].header);
                printed_header = 1;
            }
            print_spec_human(&all[i]);
            printf("\n");
        }
    }
    return 0;
}

/* ── explain ─────────────────────────────────────────────────────── */

static int cmd_explain(int argc, char **argv, const HlCommandEnv *env)
{
    if (argc < 1) {
        fprintf(stderr, "hull modules explain: missing module name\n");
        fprintf(stderr, "Usage: hull modules explain <NAME>\n");
        return 2;
    }
    const HlModuleSpec *s = hl_module_registry_find_short(argv[0]);
    if (!s) {
        fprintf(stderr, "hull modules explain: unknown module '%s'\n", argv[0]);
        fprintf(stderr, "Run `hull modules available` for the full list.\n");
        return 1;
    }
    if (env->json_output) {
        ShJsonBuf jb;
        sh_json_buf_init(&jb);
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, &jb);
        write_spec_json(&w, s);
        char *out = sh_json_buf_take(&jb);
        if (out) { printf("%s\n", out); free(out); }
        sh_json_buf_free(&jb);
        return 0;
    }
    print_spec_human(s);
    return 0;
}

/* ── list ────────────────────────────────────────────────────────── */
/*
 * `hull modules list` delegates to a Lua tool — extracting the
 * manifest needs the runtime to load the app's `app.lua` / `app.js`,
 * which is exactly what every other manifest-introspection command
 * already does (`hull manifest`, `hull inspect`). The Lua helper
 * (`stdlib/lua/hull/modules.lua`) prints the result.
 */
static int cmd_list(int argc, char **argv, const HlCommandEnv *env)
{
    /* Forward --json to the Lua tool by appending it to argv. The tool
     * VM sandboxes os.getenv so an env-var hand-off is not available.
     * The Lua tool scans arg[] for "--json". */
    if (env->json_output) {
        char **argv2 = malloc((argc + 2) * sizeof(*argv2));
        if (!argv2) return 1;
        for (int i = 0; i < argc; i++) argv2[i] = argv[i];
        argv2[argc] = (char *)"--json";
        argv2[argc + 1] = NULL;
        int rc = hull_tool("hull.modules", argc + 1, argv2, env->hull_exe);
        free(argv2);
        return rc;
    }
    return hull_tool("hull.modules", argc, argv, env->hull_exe);
}

/* ── analyze ─────────────────────────────────────────────────────── */
/*
 * Delegates to hull.analyze (Lua) which walks the app source, finds
 * every `require("hull.X")` / `import "hull:X"`, and compares against
 * the declared modules block. Exits non-zero on undeclared imports;
 * unused declarations are advisory.
 */
static int cmd_analyze(int argc, char **argv, const HlCommandEnv *env)
{
    if (env->json_output) {
        char **argv2 = malloc((argc + 2) * sizeof(*argv2));
        if (!argv2) return 1;
        for (int i = 0; i < argc; i++) argv2[i] = argv[i];
        argv2[argc] = (char *)"--json";
        argv2[argc + 1] = NULL;
        int rc = hull_tool("hull.analyze", argc + 1, argv2, env->hull_exe);
        free(argv2);
        return rc;
    }
    return hull_tool("hull.analyze", argc, argv, env->hull_exe);
}

/* ── Usage ───────────────────────────────────────────────────────── */

static int usage(int rc)
{
    fprintf(stderr,
        "Usage: hull modules <subcommand> [args]\n"
        "\n"
        "Subcommands:\n"
        "  list [APP_DIR]        Print the modules an app declares.\n"
        "  available             Print the full first-party module registry.\n"
        "  explain <NAME>        Print the spec for one module.\n"
        "  analyze [APP_DIR]     Walk source files; warn on undeclared imports\n"
        "                          and unused declarations.\n"
        "\n"
        "Add --json (a global flag) for machine-readable output.\n");
    return rc;
}

/* ── Entry point ─────────────────────────────────────────────────── */

int hl_cmd_modules(int argc, char **argv, const HlCommandEnv *env)
{
    if (argc < 2)
        return usage(2);

    const char *sub = argv[1];
    /* For `list`, forward argv starting at "list" so the Lua tool sees
     * arg[0]="list" and arg[1]=<app_dir> (matches the convention every
     * other hull_tool command uses). */
    if (strcmp(sub, "list") == 0)      return cmd_list(argc - 1, argv + 1, env);
    if (strcmp(sub, "available") == 0) return cmd_available(argc - 1, argv + 1, env);
    if (strcmp(sub, "explain") == 0)   return cmd_explain(argc - 2, argv + 2, env);
    if (strcmp(sub, "analyze") == 0)   return cmd_analyze(argc - 1, argv + 1, env);
    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0)
        return usage(0);

    fprintf(stderr, "hull modules: unknown subcommand '%s'\n", sub);
    return usage(2);
}

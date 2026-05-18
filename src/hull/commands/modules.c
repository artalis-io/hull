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

/* ── Helpers ─────────────────────────────────────────────────────── */

static void format_caps_human(uint32_t caps, char *out, size_t cap)
{
    /* Append a comma-separated list of human cap names. */
    if (!caps) { snprintf(out, cap, "none"); return; }
    out[0] = '\0';
    const struct { uint32_t bit; const char *name; } table[] = {
        { HL_MOD_CAP_FS,    "fs"      },
        { HL_MOD_CAP_HOSTS, "hosts"   },
        { HL_MOD_CAP_ENV,   "env"     },
        { HL_MOD_CAP_DB,    "build:db"   },
        { HL_MOD_CAP_WASM,  "build:wasm" },
        { HL_MOD_CAP_GPU,   "build:gpu"  },
        { 0, NULL }
    };
    int first = 1;
    for (int i = 0; table[i].name; i++) {
        if (caps & table[i].bit) {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", first ? "" : ", ", table[i].name);
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
    const struct { uint32_t bit; const char *name; } table[] = {
        { HL_MOD_CAP_FS,    "fs"      },
        { HL_MOD_CAP_HOSTS, "hosts"   },
        { HL_MOD_CAP_ENV,   "env"     },
        { HL_MOD_CAP_DB,    "build:db"   },
        { HL_MOD_CAP_WASM,  "build:wasm" },
        { HL_MOD_CAP_GPU,   "build:gpu"  },
        { 0, NULL }
    };
    for (int i = 0; table[i].name; i++)
        if (s->required_caps & table[i].bit)
            sh_json_write_string(w, table[i].name);
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

static int cmd_available(const HlCommandEnv *env)
{
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

    printf("First-party Hull module registry — %zu entries\n\n", total);
    for (size_t i = 0; i < total; i++) {
        print_spec_human(&all[i]);
        printf("\n");
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
    if (strcmp(sub, "available") == 0) return cmd_available(env);
    if (strcmp(sub, "explain") == 0)   return cmd_explain(argc - 2, argv + 2, env);
    if (strcmp(sub, "analyze") == 0)   return cmd_analyze(argc - 1, argv + 1, env);
    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0)
        return usage(0);

    fprintf(stderr, "hull modules: unknown subcommand '%s'\n", sub);
    return usage(2);
}

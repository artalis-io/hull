/*
 * agent/capabilities.c — `hull agent capabilities`: declared (manifest)
 * vs used (source-walk) capability analysis.
 *
 * Surfaces:
 *   - declared-but-unused (manifest could be tightened)
 *   - used-but-undeclared (will be blocked by sandbox at runtime; fix the manifest)
 *
 * The "used" set is determined by a substring scan over app source files.
 * It's a heuristic — false positives (e.g. capability names inside string
 * literals) are possible but rare in practice, and false negatives only
 * happen when the app accesses caps through indirect indexing
 * (`(_G or {})["http"]`). Both edge cases are also detected as
 * sandbox-violation attempts by `hull agent validate`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/manifest.h"
#include "hull/runtime.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#endif

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Capabilities that require manifest declarations to use at all. */
struct cap_probe {
    const char *name;            /* canonical capability name */
    const char *lua_pattern;     /* substring to look for in Lua source */
    const char *js_pattern;      /* substring to look for in JS source */
};

static const struct cap_probe PROBES[] = {
    /* Capabilities that REQUIRE manifest entries: */
    { "fs.read",       "fs.read",        "fs.read"     },
    { "fs.write",      "fs.write",       "fs.write"    },
    { "fs.mmap",       "fs.mmap",        "fs.mmap"     },
    { "http.fetch",    "http.fetch",     "http.fetch"  },
    { "http.get",      "http.get",       "http.get"    },
    { "http.post",     "http.post",      "http.post"   },
    { "ws.connect",    "ws.connect",     "ws.connect"  },
    { "env.get",       "env.get",        "env.get"     },
    { "gpu.",          "gpu.",           "gpu."        },
    /* Capabilities that don't require manifest but tell us app shape: */
    { "compute.",      "compute.",       "compute."    },
    { "db.",           "db.",            "db."         },
    { "crypto.",       "crypto.",        "crypto."     },
    { "smtp.",         "smtp.",          "smtp."       },
    { NULL, NULL, NULL }
};

/* Recursively read every .lua / .js / .json source file in app_dir
 * (skip migrations/, static/, .hull/) and accumulate into `aggregate`.
 * Bounded by max_bytes to avoid runaway memory use. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Aggregate;

static int agg_append(Aggregate *a, const char *src, size_t n)
{
    size_t need = a->len + n + 1;
    if (need > a->cap) {
        size_t ncap = a->cap ? a->cap * 2 : 65536;
        while (ncap < need) ncap *= 2;
        char *grow = realloc(a->buf, ncap);
        if (!grow) return -1;
        a->buf = grow;
        a->cap = ncap;
    }
    memcpy(a->buf + a->len, src, n);
    a->len += n;
    a->buf[a->len] = '\0';
    return 0;
}

static void agg_free(Aggregate *a) { free(a->buf); }

static int read_file_into(Aggregate *a, const char *path, size_t max_bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return 0; }
    if ((size_t)n > max_bytes) n = (long)max_bytes;
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    int rc = agg_append(a, buf, got);
    free(buf);
    /* Separator between files so substring matches don't span boundaries. */
    if (rc == 0) agg_append(a, "\n//---\n", 7);
    return rc;
}

static int is_source_ext(const char *name)
{
    size_t l = strlen(name);
    if (l > 4 && strcmp(name + l - 4, ".lua") == 0) return 1;
    if (l > 3 && strcmp(name + l - 3, ".js")  == 0) return 1;
    return 0;
}

static int is_skip_dir(const char *name)
{
    return strcmp(name, "migrations") == 0 ||
           strcmp(name, "static")     == 0 ||
           strcmp(name, "templates")  == 0 ||
           strcmp(name, "compute")    == 0 ||
           strcmp(name, "shaders")    == 0 ||
           strcmp(name, ".hull")      == 0 ||
           strcmp(name, ".git")       == 0 ||
           strcmp(name, "build")      == 0 ||
           strcmp(name, "node_modules") == 0;
}

static void walk_dir(const char *dir, Aggregate *a, int depth)
{
    if (depth > 8) return;        /* sanity cap */
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        char path[HL_AGENT_PATH_DEEP];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path))
            continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_skip_dir(ent->d_name)) continue;
            walk_dir(path, a, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_source_ext(ent->d_name)) {
            read_file_into(a, path, HL_AGENT_CAPS_MAX_PER_FILE);
        }
    }
    closedir(d);
}

/* Check if a manifest already covers a fs path / host / env via prefix
 * or exact match. */
static int covers_fs_read(const HlManifest *m) { return m->fs_read_count > 0; }
static int covers_fs_write(const HlManifest *m) { return m->fs_write_count > 0; }
static int covers_hosts(const HlManifest *m) { return m->hosts_count > 0; }
static int covers_env(const HlManifest *m) { return m->env_count > 0; }
static int covers_gpu(const HlManifest *m) { return m->gpu ? 1 : 0; }

/* Map a probe → whether its category is covered by the manifest. */
static int manifest_covers(const struct cap_probe *p, const HlManifest *m)
{
    if (strcmp(p->name, "fs.read") == 0)    return covers_fs_read(m);
    if (strcmp(p->name, "fs.mmap") == 0)    return covers_fs_read(m);
    if (strcmp(p->name, "fs.write") == 0)   return covers_fs_write(m);
    if (strncmp(p->name, "http.", 5) == 0)  return covers_hosts(m);
    if (strcmp(p->name, "ws.connect") == 0) return covers_hosts(m);
    if (strcmp(p->name, "env.get") == 0)    return covers_env(m);
    if (strncmp(p->name, "gpu.", 4) == 0)   return covers_gpu(m);
    return 1;     /* compute./db./crypto./smtp. don't require manifest */
}

int hl_agent_capabilities_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

    int is_lua = 0;
#ifdef HL_ENABLE_LUA
    if (rt->vt == &hl_lua_vtable) is_lua = 1;
#endif

    /* Extract the declared manifest */
    HlManifest m = {0};
    if (rt->vt->extract_manifest) (void)rt->vt->extract_manifest(rt, &m);

    /* Aggregate app source */
    Aggregate agg = {0};
    walk_dir(hl_app_context_app_dir(ctx), &agg, 0);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "runtime", is_lua ? "lua" : "js");
    sh_json_write_kv_bool(&w, "manifest_declared", m.present ? true : false);

    /* Probe each capability */
    sh_json_write_key(&w, "capabilities");
    sh_json_write_array_start(&w);
    for (const struct cap_probe *p = PROBES; p->name; p++) {
        const char *needle = is_lua ? p->lua_pattern : p->js_pattern;
        int used = agg.buf && needle && strstr(agg.buf, needle) != NULL;
        int declared = manifest_covers(p, &m);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", p->name);
        sh_json_write_kv_bool(&w, "used", used ? true : false);
        sh_json_write_kv_bool(&w, "declared_or_unrestricted", declared ? true : false);
        if (used && !declared)
            sh_json_write_kv_string(&w, "status", "used_but_undeclared");
        else if (!used && p->name[0] != '\0' && declared &&
                 (strncmp(p->name, "fs.", 3) == 0 ||
                  strncmp(p->name, "http.", 5) == 0 ||
                  strcmp(p->name, "env.get") == 0))
            sh_json_write_kv_string(&w, "status", "declared_but_unused");
        else
            sh_json_write_kv_string(&w, "status", "ok");
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

    /* Compact recommendations */
    int n_used_undeclared = 0;
    for (const struct cap_probe *p = PROBES; p->name; p++) {
        const char *needle = is_lua ? p->lua_pattern : p->js_pattern;
        if (agg.buf && needle && strstr(agg.buf, needle) && !manifest_covers(p, &m))
            n_used_undeclared++;
    }
    sh_json_write_kv_int(&w, "used_but_undeclared_count", n_used_undeclared);

    sh_json_write_object_end(&w);
    hl_manifest_free(&m);
    agg_free(&agg);
    return 0;
}

int hl_agent_capabilities(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";
    HlAppContextOpts opts = { .app_dir = app_dir };
    HlAppContext *ctx = NULL;
    if (hl_app_context_init(&ctx, &opts) != 0)
        return hl_agent_write_error(out, "failed to load app");
    int rc = hl_agent_capabilities_ctx(ctx, out);
    hl_app_context_free(ctx);
    return rc;
}

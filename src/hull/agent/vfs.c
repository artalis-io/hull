/*
 * agent/vfs.c — `hull agent vfs`: list embedded files (path → size).
 *
 * Useful for verifying what `hull build` will embed vs what's on disk
 * (e.g. "why isn't my new template showing up after build?").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/vfs.h"
#include "hull/stdlib_feature.h"

#include <sh_json.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Bucket a VFS entry name by its filesystem-style prefix so an agent can
 * see at a glance what category each entry came from. */
static const char *bucket_for(const char *name)
{
    if (!name) return "module";
    if (strncmp(name, "templates/", 10) == 0)  return "template";
    if (strncmp(name, "static/", 7) == 0)      return "static";
    if (strncmp(name, "migrations/", 11) == 0) return "migration";
    if (strncmp(name, "shaders/", 8) == 0)     return "shader";
    if (strncmp(name, "compute/", 8) == 0)     return "compute";
    if (strncmp(name, "hull:", 5) == 0)        return "stdlib-js";
    if (strncmp(name, "hull.", 5) == 0)        return "stdlib-lua";
    if (strncmp(name, "./", 2) == 0)           return "app-module";
    return "module";
}

static void emit_entries(ShJsonWriter *w, const HlEntry *entries,
                         const char *root_dir, const char *array_key)
{
    sh_json_write_key(w, array_key);
    sh_json_write_array_start(w);
    if (entries) {
        for (const HlEntry *e = entries; e->name; e++) {
            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "name", e->name);
            sh_json_write_kv_int(w, "size", e->len);
            sh_json_write_kv_string(w, "bucket", bucket_for(e->name));
            sh_json_write_object_end(w);
        }
    }
    sh_json_write_array_end(w);
    (void)root_dir;
}

int hl_agent_vfs_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    extern const HlEntry hl_app_entries[];

    sh_json_write_kv_string(&w, "app_dir", hl_app_context_app_dir(ctx));
    emit_entries(&w, hl_app_entries, hl_app_context_app_dir(ctx), "app");

    /* stdlib = base U each composed runtime's stdlib (merged, sorted). */
    HlVfs stdlib_vfs;
    HlEntry *stdlib_owned = NULL;
    hl_platform_vfs_init(&stdlib_vfs, &stdlib_owned);
    emit_entries(&w, stdlib_vfs.entries, NULL, "stdlib");
    hl_platform_vfs_dispose(stdlib_owned);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_vfs(const char *app_dir, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    extern const HlEntry hl_app_entries[];

    sh_json_write_kv_string(&w, "app_dir", app_dir ? app_dir : ".");
    emit_entries(&w, hl_app_entries, app_dir, "app");

    /* stdlib = base U each composed runtime's stdlib (merged, sorted). */
    HlVfs stdlib_vfs;
    HlEntry *stdlib_owned = NULL;
    hl_platform_vfs_init(&stdlib_vfs, &stdlib_owned);
    emit_entries(&w, stdlib_vfs.entries, NULL, "stdlib");
    hl_platform_vfs_dispose(stdlib_owned);

    sh_json_write_object_end(&w);
    return 0;
}

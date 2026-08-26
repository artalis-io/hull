/*
 * agent/gpu.c - `hull agent gpu`: list compiled WGSL shaders and
 * GPU device info.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/entry.h"

#ifdef HL_ENABLE_GPU
#include "hull/cap/gpu.h"
#endif

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static void emit_from_vfs(ShJsonWriter *w, const HlEntry *entries)
{
    if (!entries) return;
    for (const HlEntry *e = entries; e->name; e++) {
        if (strncmp(e->name, "shaders/", 8) != 0) continue;
        const char *base = e->name + 8;
        size_t blen = strlen(base);
        if (blen <= 5 || memcmp(base + blen - 5, ".wgsl", 5) != 0) continue;

        char name[HL_AGENT_IDENT_MED];
        if (blen - 5 >= sizeof(name)) continue;
        memcpy(name, base, blen - 5);
        name[blen - 5] = '\0';

        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "name", name);
        sh_json_write_kv_int(w, "size", e->len);
        sh_json_write_object_end(w);
    }
}

static void emit_from_disk(ShJsonWriter *w, const char *app_dir)
{
    char dir[HL_AGENT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/shaders", app_dir);
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t nlen = strlen(ent->d_name);
        if (nlen <= 5 || strcmp(ent->d_name + nlen - 5, ".wgsl") != 0)
            continue;

        char name[HL_AGENT_IDENT_MED];
        size_t blen = nlen - 5;
        if (blen >= sizeof(name)) continue;
        memcpy(name, ent->d_name, blen);
        name[blen] = '\0';

        char path[HL_AGENT_PATH_PLUS];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        struct stat st;
        long size = 0;
        if (stat(path, &st) == 0) size = (long)st.st_size;

        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "name", name);
        sh_json_write_kv_int(w, "size", size);
        sh_json_write_object_end(w);
    }
    closedir(d);
}

int hl_agent_gpu_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

#ifdef HL_ENABLE_GPU
    sh_json_write_kv_bool(&w, "available", true);
#else
    sh_json_write_kv_bool(&w, "available", false);
#endif

    sh_json_write_key(&w, "shaders");
    sh_json_write_array_start(&w);

    extern const HlEntry hl_app_entries[];
    int has_vfs = 0;
    for (const HlEntry *e = hl_app_entries; e->name; e++) {
        if (strncmp(e->name, "shaders/", 8) == 0) { has_vfs = 1; break; }
    }
    if (has_vfs) emit_from_vfs(&w, hl_app_entries);
    else         emit_from_disk(&w, hl_app_context_app_dir(ctx));

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_gpu(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

#ifdef HL_ENABLE_GPU
    sh_json_write_kv_bool(&w, "available", true);
#else
    sh_json_write_kv_bool(&w, "available", false);
#endif
    sh_json_write_kv_string(&w, "app_dir", app_dir);

    sh_json_write_key(&w, "shaders");
    sh_json_write_array_start(&w);
    emit_from_disk(&w, app_dir);
    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w);
    return 0;
}

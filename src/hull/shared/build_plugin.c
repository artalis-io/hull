/*
 * build_plugin.c - the plugin side of a build plugin's channel (see
 * include/hull/shared/build_plugin.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/build_plugin.h"

#include "sh_arena.h"
#include "sh_json.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

static int g_fd = -1;

void hl_build_plugin_put_done(HlPvWriter *w, const char *msg, size_t n)
{
    if (!msg) {
        hl_pv_put_map(w, 0);
        return;
    }
    char buf[HL_BP_ERROR_MAX + 3];
    if (n > HL_BP_ERROR_MAX) {
        size_t cut = HL_BP_ERROR_MAX;
        /* Back over the continuation bytes of a sequence the cut splits. */
        for (int i = 0; i < 3 && cut > 0 && ((unsigned char)msg[cut] & 0xC0) == 0x80; i++)
            cut--;
        memcpy(buf, msg, cut);
        memcpy(buf + cut, "...", 3);
        msg = buf;
        n = cut + 3;
    }
    hl_pv_put_map(w, 1);
    hl_pv_put_key(w, "error", 5);
    hl_pv_put_bytes(w, msg, n);
}

int hl_build_plugin_take_env(void)
{
    const char *v = getenv(HL_BUILD_PLUGIN_ENV);
    if (!v) return 0;
    int ok = strcmp(v, "3") == 0;
    (void)unsetenv(HL_BUILD_PLUGIN_ENV);
#ifdef HL_CHAN_SUPPORTED
    struct stat st;
    if (!ok || fstat(3, &st) != 0 || !S_ISSOCK(st.st_mode)) return -1;
    (void)fcntl(3, F_SETFD, FD_CLOEXEC);
    hl_chan_set_nonblock(3);
    hl_chan_set_nosigpipe(3);
    g_fd = 3;
    return 1;
#else
    (void)ok;
    return -1;   /* no plugin channel on this host */
#endif
}

int hl_build_plugin_fd(void)
{
    return g_fd;
}

void hl_build_plugin_end(void)
{
#ifdef HL_CHAN_SUPPORTED
    if (g_fd >= 0) (void)close(g_fd);
#endif
    g_fd = -1;
}

int hl_build_plugin_send(uint32_t type, uint32_t id, const void *payload,
                         size_t len, uint64_t deadline)
{
#ifdef HL_CHAN_SUPPORTED
    if (g_fd < 0) return -1;
    if (hl_chan_write_frame(g_fd, HL_BUILD_PLUGIN_MAGIC, type, id, payload, len,
                            NULL, 0, deadline) == 0)
        return 0;
    hl_build_plugin_end();
#else
    (void)type; (void)id; (void)payload; (void)len; (void)deadline;
#endif
    return -1;
}

int hl_build_plugin_recv(HlChanFrame *f, uint64_t deadline)
{
    memset(f, 0, sizeof *f);
    f->magic = HL_BUILD_PLUGIN_MAGIC;
    f->max = HL_BP_MAX_PAYLOAD;
#ifdef HL_CHAN_SUPPORTED
    if (g_fd < 0) return HL_CHAN_EOF;
    int rc = hl_chan_frame_read(g_fd, f, deadline);
    if (rc != HL_CHAN_DONE && rc != HL_CHAN_MORE) hl_build_plugin_end();
    return rc;
#else
    (void)deadline;
    return HL_CHAN_EOF;
#endif
}

/* Pure modules only: no I/O, no time, no randomness. Transitive deps of
 * these are in the list too (hull/template needs hull/json). */
static const char *const ALLOWED[] = {
    "hull/app", "hull/build-plugin", "hull/csv", "hull/encoding", "hull/json",
    "hull/log", "hull/path", "hull/template", "hull/validate", NULL,
};

int hl_build_plugin_module_allowed(const char *name)
{
    if (!name) return 0;
    const char *at = strchr(name, '@');
    size_t n = at ? (size_t)(at - name) : strlen(name);
    for (int i = 0; ALLOWED[i]; i++)
        if (strlen(ALLOWED[i]) == n && memcmp(ALLOWED[i], name, n) == 0)
            return 1;
    return 0;
}

int hl_build_plugin_preset_check(const HlManifest *m, char *err, size_t errsz)
{
    const char *section = NULL;
    if (m->fs_read_count || m->fs_write_count) section = "fs";
    else if (m->env_count) section = "env";
    else if (m->secrets_count) section = "secrets";
    else if (m->hosts_count) section = "hosts";
    else if (m->databases.declared) section = "databases";
    else if (m->kv.declared) section = "kv";
    else if (m->ssh.declared) section = "ssh";
    else if (m->tui) section = "tui";
    else if (m->state_name || m->state_invalid) section = "state";
    else if (m->gpu) section = "gpu";
    else if (m->compute) section = "compute";
    if (section) {
        snprintf(err, errsz, "a build plugin's manifest may not declare `%s`: "
                 "every input comes through ctx.inputs", section);
        return -1;
    }
    int declares = 0;
    for (int i = 0; i < m->modules_count; i++) {
        const char *name = m->modules[i].name;
        if (!hl_build_plugin_module_allowed(name)) {
            snprintf(err, errsz, "a build plugin may not declare the module "
                     "'%s': only pure modules (no I/O, time or randomness)",
                     name ? name : "?");
            return -1;
        }
        if (strncmp(name, "hull/build-plugin", 17) == 0) declares = 1;
    }
    if (!declares) {
        snprintf(err, errsz, "a build plugin's manifest must declare "
                 "hull/build-plugin@1");
        return -1;
    }
    return 0;
}

#define BP_CONFIG_MAX (1024 * 1024)

/* hull.build.json's bytes (NUL-terminated, malloc'd), or NULL. Not through
 * a link, a regular file, at most BP_CONFIG_MAX bytes. */
static char *read_config(const char *app_dir, size_t *len)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/hull.build.json", app_dir);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    char *buf = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        st.st_size <= BP_CONFIG_MAX && (buf = malloc((size_t)st.st_size + 1))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        buf[got] = '\0';
        *len = got;
    }
    close(fd);
    return buf;
}

void hl_build_plugin_drop_dirs(const char *app_dir, char **files)
{
    if (!app_dir || !files || !files[0]) return;
    size_t len = 0;
    char *json = read_config(app_dir, &len);
    if (!json) return;
    SHArena *arena = sh_arena_create(4 * BP_CONFIG_MAX);
    ShJsonValue *root = NULL;
    if (arena && sh_json_parse(json, len, arena, &root) == SH_JSON_OK) {
        ShJsonValue *list = sh_json_get(root, "plugins");
        size_t np = sh_json_array_len(list);
        size_t alen = strlen(app_dir);
        size_t w = 0;
        for (size_t r = 0; files[r]; r++) {
            const char *f = files[r];
            int drop = 0;
            for (size_t i = 0; i < np && !drop; i++) {
                const char *p = sh_json_as_string(sh_json_get(sh_json_array_get(list, i), "path"), NULL);
                size_t pl = p ? strlen(p) : 0;
                drop = pl > 0 && strncmp(f, app_dir, alen) == 0 && f[alen] == '/' &&
                       strncmp(f + alen + 1, p, pl) == 0 && f[alen + 1 + pl] == '/';
            }
            if (drop) free(files[r]);
            else files[w++] = files[r];
        }
        files[w] = NULL;
    }
    if (arena) sh_arena_free(arena);
    free(json);
}

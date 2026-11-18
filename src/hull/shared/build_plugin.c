/*
 * build_plugin.c - the plugin side of a build plugin's channel (see
 * include/hull/shared/build_plugin.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/build_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HL_CHAN_SUPPORTED
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int g_fd = -1;

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

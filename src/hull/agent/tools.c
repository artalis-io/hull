/*
 * agent/tools.c — `hull agent tools`: machine-readable side-loaded
 * tool registry + install state.
 *
 * Generic over the registry — adding a new tool to
 * src/hull/tools_install.c automatically surfaces it here. The
 * per-subsystem panels (`hull agent compute` for wamrc, future
 * `hull agent gpu` for wgpu-native) report the same tool by name
 * within their semantic context; this command is the "what's
 * installed and what's available" overview.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/tools_install.h"

#include <sh_json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Platform identifier — kept independent of release_io.c so this file
 * compiles even on HL_ENABLE_HTTP_CLIENT=0 builds where the registry
 * is still useful for read-only lookup. */
static const char *agent_tools_platform(void)
{
#if defined(__COSMOPOLITAN__)
    return "cosmo";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__)
    return "cosmo";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && (defined(__aarch64__) || defined(__arm64__))
    return "linux-aarch64";
#else
    return "cosmo";
#endif
}

int hl_agent_tools(ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    const char *platform = agent_tools_platform();
    sh_json_write_kv_string(&w, "platform", platform);

    sh_json_write_key(&w, "tools");
    sh_json_write_array_start(&w);

    for (const HlToolSpec *t = hl_tools_registry(); t->name; t++) {
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "name", t->name);
        sh_json_write_kv_string(&w, "description", t->description);

        int available = hl_tools_published_for(t, platform);
        sh_json_write_kv_bool(&w, "available_for_platform", available != 0);

        /* installed: is there an executable at the canonical install
         * location? We don't fall back to PATH here — the install
         * state is specifically about hull-managed installs. */
        char path[HL_AGENT_PATH_MAX];
        int has_path = hl_tools_install_path(t->name, path, sizeof(path)) == 0;
        struct stat st;
        int installed = has_path && stat(path, &st) == 0 && S_ISREG(st.st_mode);

        sh_json_write_kv_bool(&w, "installed", installed != 0);
        if (installed) {
            sh_json_write_kv_string(&w, "path", path);
            sh_json_write_kv_int(&w, "size_bytes", (int64_t)st.st_size);
        } else {
            sh_json_write_kv_null(&w, "path");
        }

        if (available) {
            char asset[128];
            if (hl_tools_asset_name(t, platform, asset, sizeof(asset)) == 0)
                sh_json_write_kv_string(&w, "asset_name", asset);
        }

        /* Concrete action for the agent: install / refresh / build
         * from source, depending on state and availability. */
        if (!installed && available) {
            char hint[128];
            snprintf(hint, sizeof(hint), "hull tools install %s", t->name);
            sh_json_write_kv_string(&w, "install_hint", hint);
        } else if (!installed && !available) {
            sh_json_write_kv_string(&w, "install_hint",
                "no signed binary published for this platform");
        }

        sh_json_write_object_end(&w);
    }

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);
    return 0;
}

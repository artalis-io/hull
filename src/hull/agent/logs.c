/*
 * agent/logs.c - `hull agent logs` / MCP `hull_logs`.
 *
 * This used to tail <app>/.hull/dev.log. Nothing in Hull writes that file:
 * `hull dev` sends the served app's log to its own stderr. So any dev.log an
 * agent was handed was content of the app tree - a cloned repo could commit
 * one, and its lines reached the agent as Hull's own output (audit 36). The
 * command now reads no file from the app tree: it reports that there is no
 * Hull-written log, and whether a .hull/dev.log was found and ignored.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"

#include <sh_json.h>

#include <stdio.h>
#include <sys/stat.h>

int hl_agent_logs(const char *app_dir, int tail_n, ShJsonBuf *out)
{
    (void)tail_n;
    if (!app_dir) app_dir = ".";

    char path[HL_AGENT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.hull/dev.log", app_dir);
    struct stat st;
    int present = lstat(path, &st) == 0;

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "path", path);
    sh_json_write_kv_bool(&w, "exists", present);
    sh_json_write_key(&w, "lines");
    sh_json_write_array_start(&w);
    sh_json_write_array_end(&w);
    sh_json_write_kv_string(&w, "note",
        "hull dev writes no log file: the served app logs to the dev "
        "server's stderr (run `hull dev --agent 2>&1 | tee <file>` outside "
        "the app tree to keep a copy)");
    if (present)
        sh_json_write_kv_string(&w, "ignored",
            ".hull/dev.log was not read: Hull does not write it, so it is "
            "repo content, not Hull output");
    sh_json_write_object_end(&w);
    return 0;
}

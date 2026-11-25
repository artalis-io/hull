/*
 * cap/exec_child.c - a declared command's child manifest: which app fs
 * grants a command may take, and the exact kernel path each one names.
 * See hull/cap/exec_child.h and docs/design/exec_broker.md §3.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec_child.h"
#include "hull/cap/exec.h"
#include "hull/cap/fs_policy.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *const HL_EXEC_CHILD_MANIFEST_KEYS[] = { "fs", NULL };

const char *const HL_EXEC_APP_ONLY_KEYS[] = {
    "modules", "env", "secrets", "hosts", "http", "csp", "cors", "databases",
    "kv", "ssh", "state", "tui", "exec", "compute", "gpu", "wasm", "limits",
    "allow_dynamic_code", "allowDynamicCode",
    "allow_dynamic_libraries", "allowDynamicLibraries", NULL };

const char *const HL_EXEC_ENTRY_ONLY_KEYS[] = {
    "path", "argv", "sha256", "host", "manifest", "timeout_ms", "timeoutMs",
    "stdout_max", "stdoutMax", "stderr_max", "stderrMax", "concurrency",
    "stdin_max", "stdinMax", "unix_sockets", "unixSockets", NULL };

static int key_in(const char *k, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(k, list[i]) == 0) return 1;
    return 0;
}

int hl_exec_child_key(HlExecBuf *b, const char *cmd, const char *key)
{
    if (key_in(key, HL_EXEC_CHILD_MANIFEST_KEYS)) return 0;
    if (strcmp(key, "env") == 0)
        hl_exec_b_fail(b, "exec.%s.manifest.env: an app's env allowlist is not "
                       "something a command can be given (a command gets only "
                       "the literal values of exec.%s.env)", cmd, cmd);
    else if (key_in(key, HL_EXEC_ENTRY_ONLY_KEYS))
        hl_exec_b_fail(b, "exec.%s.manifest.%s: not a manifest field; it belongs on "
                       "the command entry (exec.%s.%s)", cmd, key, cmd, key);
    else if (key_in(key, HL_EXEC_APP_ONLY_KEYS))
        hl_exec_b_fail(b, "exec.%s.manifest.%s: an app capability a command cannot "
                       "be given (a command's manifest takes only: fs)", cmd, key);
    else
        hl_exec_b_fail(b, "exec.%s.manifest: unknown key '%s'", cmd, key);
    return -1;
}

int hl_exec_child_grant_entry(HlExecBuf *b, const char *cmd, const char *list,
                              unsigned idx, const char *raw, size_t n)
{
    char why[160];
    if (hl_exec_child_grant_check(raw, n, why, sizeof why) == 0) return 0;
    hl_exec_b_fail(b, "exec.%s.manifest.fs.%s[%u] '%.*s' %s", cmd, list, idx,
                   (int)(n > 64 ? 64 : n), raw, why);
    return -1;
}

void hl_exec_legacy_note(const char *cmd, const char *legacy, const char *replacement)
{
    static int warned;
    if (warned) return;
    warned = 1;
    log_warn("[manifest] exec.%s.%s is the v0.26.0 spelling, taken as exec.%s.%s; "
             "it is removed in 0.28.0 (`hull check` lists every use)",
             cmd, legacy, cmd, replacement);
}

int hl_exec_child_grant_check(const char *raw, size_t n, char *why, size_t why_size)
{
    HlFsGrant g = HL_FS_GRANT_INIT;
    const char *e = NULL;
    if (hl_fs_grant_parse(raw, n, NULL, &g, &e) != 0) {
        if (e && strcmp(e, "reserved_path") == 0)
            snprintf(why, why_size, "is reserved (.hull, or an '@' root other "
                     "than @state)");
        else if (e && strcmp(e, "unsupported_pattern") == 0)
            snprintf(why, why_size, "uses a pattern an fs grant does not take");
        else
            snprintf(why, why_size, "is not an fs grant: a path under the app "
                     "directory or \"@state/\", with no '..'");
        return -1;
    }
    int rc = 0;
    if (g.n == 0) {
        snprintf(why, why_size, "grants the whole %s directory, which a command "
                 "cannot be given: its .hull would be included (name a "
                 "subdirectory)", g.state ? "state" : "app");
        rc = -1;
    } else if (g.first_pattern < g.n) {
        snprintf(why, why_size, "is a pattern, which a command cannot be given "
                 "(the kernel grants files and directories; name one)");
        rc = -1;
    }
    hl_fs_grant_free(&g);
    return rc;
}

int hl_exec_child_grant_resolve(const char *app_dir, const char *state_dir,
                                const char *raw, char *out, size_t out_size,
                                char *why, size_t why_size)
{
    size_t rn = strlen(raw);
    if (hl_exec_child_grant_check(raw, rn, why, why_size) != 0) return -1;
    HlFsGrant g = HL_FS_GRANT_INIT;
    const char *e = NULL;
    if (hl_fs_grant_parse(raw, rn, NULL, &g, &e) != 0) {
        snprintf(why, why_size, "is not an fs grant");
        return -1;
    }
    const char *root = g.state ? state_dir : app_dir;
    int rc = -1;
    char path[PATH_MAX];
    if (!root || !root[0]) {
        snprintf(why, why_size, g.state ? "names @state/, but the app declares no "
                 "state directory" : "has no app directory to resolve against");
        goto done;
    }
    size_t at = (size_t)snprintf(path, sizeof path, "%s", root);
    if (at >= sizeof path) { snprintf(why, why_size, "is too long"); goto done; }
    /* Component by component, never through a symbolic link: the grant must
     * name the path the app's own grant names, inside its root. */
    for (size_t i = 0; i < g.n; i++) {
        int w = snprintf(path + at, sizeof path - at, "/%s", g.comp[i]);
        if (w < 0 || (size_t)w >= sizeof path - at) {
            snprintf(why, why_size, "is too long");
            goto done;
        }
        at += (size_t)w;
        struct stat st;
        if (lstat(path, &st) != 0) {
            if (errno == ENOENT && g.directory_intent) {
                /* A "dir/" grant: created, as the app's sandbox creates it. */
                if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                    snprintf(why, why_size, "cannot be created: %s", strerror(errno));
                    goto done;
                }
                continue;
            }
            snprintf(why, why_size, "does not exist; a command's grant names an "
                     "existing file or directory (end it with '/' for a "
                     "directory to create)");
            goto done;
        }
        if (S_ISLNK(st.st_mode)) {
            snprintf(why, why_size, "runs through a symbolic link");
            goto done;
        }
        int last = i + 1 == g.n;
        if (!last && !S_ISDIR(st.st_mode)) {
            snprintf(why, why_size, "runs through a non-directory");
            goto done;
        }
        if (last && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) {
            snprintf(why, why_size, "is neither a regular file nor a directory");
            goto done;
        }
        if (last && g.directory_intent && !S_ISDIR(st.st_mode)) {
            snprintf(why, why_size, "ends in '/' but is not a directory");
            goto done;
        }
    }
    char real[PATH_MAX];
    if (!realpath(path, real)) {
        snprintf(why, why_size, "cannot be resolved: %s", strerror(errno));
        goto done;
    }
    if (strlen(real) >= out_size) { snprintf(why, why_size, "is too long"); goto done; }
    memcpy(out, real, strlen(real) + 1);
    rc = 0;
done:
    hl_fs_grant_free(&g);
    return rc;
}

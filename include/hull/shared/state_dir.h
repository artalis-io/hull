/**
 * @file state_dir.h
 * @brief The per-user state directory an app.main tool declares (platform
 *        gap 3, docs/design/platform_gaps_hsctl.md section 11).
 *
 * A built binary's app directory is its working directory and every fs grant
 * is relative to it, so a CLI had to run from a dedicated directory. With
 *
 *     app.manifest({ state = { name = "hsctl" },
 *                    fs = { read = { "@state/keys/" }, write = { "@state/" } } })
 *
 * the app gets a second fs root, `@state/`, that resolves to a directory of
 * its own under the user's state home, created 0700 by Hull before the
 * sandbox applies and granted read-write-create to the kernel sandbox:
 *
 *     $HULL_STATE_DIR                      (an operator override, see below)
 *     $XDG_STATE_HOME/<name>               (when XDG_STATE_HOME is absolute)
 *     <home>/.local/state/<name>           (POSIX)
 *     %LOCALAPPDATA%\<name>                (Windows)
 *
 * `home` is hl_host_home (never a raw relative $HOME; audit 20). The directory
 * is then held to the cache directory's trust rule (hl_hull_dir_trusted):
 * owned by the user, not group- or world-writable, parents likewise. The
 * HULL_STATE_DIR override takes the same refusals as HULL_CACHE_DIR: it must
 * be absolute with no `.` / `..` component, and may not be `/`, the home or
 * above, or `~/.hull`, inside it or above - the directory is granted rwc to
 * the app. A refused or untrusted directory is an ERROR (logged): the app does
 * not start without the state it declared.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_STATE_DIR_H
#define HL_SHARED_STATE_DIR_H

#include <stddef.h>

/* The grant / caller-path prefix naming the state root. A path's first
 * component is exactly this; any other component beginning with '@' is
 * reserved and refused (fs_policy.h). */
#define HL_FS_STATE_PREFIX "@state"

/* `state.name` is a directory name under the state home: [a-z0-9] then up to
 * 63 of [a-z0-9._-]; never "." or "..", never a `..` anywhere. 1 / 0. */
int hl_state_dir_name_valid(const char *name);

/* Resolve, create (0700, mkdir -p) and trust-check the state directory for
 * @p name. 0 with *out an absolute path (no trailing slash); -1, logged at
 * ERROR, when the name is invalid, no home can be found, the override is
 * refused, the directory cannot be made or is not trusted. */
int hl_state_dir_resolve(const char *name, char *out, size_t out_sz);

#endif /* HL_SHARED_STATE_DIR_H */

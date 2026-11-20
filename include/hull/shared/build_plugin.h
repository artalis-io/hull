/*
 * build_plugin.h - the plugin side of a build plugin's channel
 * (docs/buildcontext_design.md §9.1).
 *
 * A build plugin is a Hull application (its manifest declares
 * hull/build-plugin@1) that the build host spawns with one socket as its
 * fd 3 and HULL_BUILD_PLUGIN_FD=3 in its environment. The runner takes the
 * descriptor before any app code runs (hl_build_plugin_take_env); the
 * runtimes' channel calls (`app.transform`'s next / call / done) go over it.
 *
 * Frames (shared/child_channel.h, magic "HBP1"), each payload one plain
 * value (shared/plain_value.h):
 *   ACTION  host -> plugin   { id, config, meta }        an action starts
 *   CALL    plugin -> host   { op, args }                a host operation
 *   REPLY   host -> plugin   { ok } | { error }          its answer, same id
 *   DONE    plugin -> host   {} | { error }              the transform ended
 *   STOP    host -> plugin   (empty)                     no more actions
 * A frame of another type, or out of turn, ends the channel: the plugin
 * gives up (its runner exits), and the host aborts the action.
 *
 * The manifest preset: a plugin declares no capability section (fs, env,
 * secrets, hosts, databases, kv, ssh, tui, state, gpu, compute) and only
 * the pure modules hl_build_plugin_module_allowed admits - no time or
 * randomness either: a plugin's output is a function of its inputs and its
 * configuration (§10).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_BUILD_PLUGIN_H
#define HL_SHARED_BUILD_PLUGIN_H

#include "hull/manifest.h"
#include "hull/shared/child_channel.h"
#include "hull/shared/plain_value.h"

#include <stddef.h>
#include <stdint.h>

#define HL_BUILD_PLUGIN_ENV   "HULL_BUILD_PLUGIN_FD"
#define HL_BUILD_PLUGIN_MAGIC 0x31504248u   /* "HBP1" little-endian */

#define HL_BP_ACTION 1u
#define HL_BP_CALL   2u
#define HL_BP_REPLY  3u
#define HL_BP_DONE   4u
#define HL_BP_STOP   5u

/* The largest payload either side sends (a workspace file read, a blob
 * written, each crosses whole). */
#define HL_BP_MAX_PAYLOAD (64u * 1024u * 1024u)

/* How long a plugin waits for the host's REPLY when its run has no
 * deadline of its own. */
#define HL_BP_REPLY_TIMEOUT_MS 60000u

/* The longest transform error a DONE carries; a longer one is cut on a
 * UTF-8 boundary and ends "...". */
#define HL_BP_ERROR_MAX 4096u

/* The DONE payload: {} for @p msg NULL, else { error = <msg, cut to
 * HL_BP_ERROR_MAX> } (bytes: a message may not be UTF-8). Written into @p w,
 * whose max must hold HL_BP_ERROR_MAX + 64: a long error used to fail the
 * writer, no DONE went out, and the host waited out its deadline (audit 49). */
void hl_build_plugin_put_done(HlPvWriter *w, const char *msg, size_t n);

/* Take the inherited channel: 1 when this process is a build plugin (the
 * descriptor is kept, close-on-exec and non-blocking, and the variable is
 * removed), 0 when it is not, -1 when the variable is set but names no
 * socket on fd 3 (the runner refuses to start). Call before any app code
 * runs. */
int hl_build_plugin_take_env(void);

/* The channel's descriptor, or -1 (not a plugin, or the channel ended). */
int hl_build_plugin_fd(void);

/* End the channel (a protocol error, the host gone): closes it, and every
 * later call answers "channel closed". */
void hl_build_plugin_end(void);

/* Send one frame. 0, or -1 (the channel is ended). */
int hl_build_plugin_send(uint32_t type, uint32_t id, const void *payload,
                         size_t len, uint64_t deadline);

/* Read one frame into @p f (initialised by this call; reset it after).
 * HL_CHAN_DONE, HL_CHAN_MORE on the deadline, or an error (the channel is
 * then ended). */
int hl_build_plugin_recv(HlChanFrame *f, uint64_t deadline);

/* Whether a plugin may declare the module @p name ("hull/json", with or
 * without "@N"). */
int hl_build_plugin_module_allowed(const char *name);

/* Check @p m against the preset. 0, or -1 with the reason in @p err. */
int hl_build_plugin_preset_check(const HlManifest *m, char *err, size_t errsz);

/* Drop from @p files - a NULL-terminated list of malloc'd paths under
 * @p app_dir, as hl_tool_find_files returns it - every file inside a build
 * plugin directory the app's hull.build.json names (freed, the list
 * compacted): a plugin is not part of the app (§9.2), and `hull test` of the
 * app does not run its tests. A missing or unreadable hull.build.json drops
 * nothing; checking it is the registry's job (hull.buildctx.registry). */
void hl_build_plugin_drop_dirs(const char *app_dir, char **files);

#endif /* HL_SHARED_BUILD_PLUGIN_H */

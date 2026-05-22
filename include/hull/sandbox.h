/*
 * sandbox.h — Kernel-level sandbox enforcement
 *
 * Applies pledge/unveil (or platform equivalents) based on the
 * application manifest.  After hl_sandbox_apply(), the process
 * can only access the filesystem paths and syscall families that
 * the manifest declares.
 *
 * Platform support:
 *   OpenBSD      — native pledge + unveil
 *   Cosmopolitan — pledge + unveil (built-in)
 *   Linux 5.13+  — Landlock (unveil), seccomp-bpf (pledge)
 *   macOS        — Seatbelt (sandbox_init_with_parameters)
 *   other        — no-op (C-level cap validation is the defense)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SANDBOX_H
#define HL_SANDBOX_H

#include "hull/manifest.h"
#include "hull/cap/tool.h"

/* ── HlSandboxPolicy ────────────────────────────────────────────────── */

/*
 * Pre-resolved sandbox policy. Decouples the sandbox enforcer from
 * HlManifest layout: the enforcer reads only this struct, so a manifest
 * field rename or addition no longer drags sandbox.c with it.
 *
 * All string/array pointers are BORROWED — they must outlive the
 * HlSandboxPolicy. For policies built from HlManifest via
 * hl_sandbox_policy_from_manifest(), this means the manifest must
 * stay alive for the policy's lifetime (trivially the case in main.c
 * where they share the process lifetime).
 *
 * Building a policy by hand (e.g. for tests) is also supported — just
 * populate the fields directly.
 */
typedef struct HlSandboxPolicy {
    /* Filesystem read allowlist */
    const char *const *fs_read;
    int                fs_read_count;

    /* Filesystem write allowlist */
    const char *const *fs_write;
    int                fs_write_count;

    /* Network: 1 if the app declares any outbound HTTP hosts.
     * The host allowlist itself is enforced in cap/http.c — the sandbox
     * only decides whether to unveil network syscalls at all. */
    int network_outbound;

    /* Network-inbound: 1 if the app may need to accept connections
     * (i.e. it's a server app, not a CLI app.main). Defaults to 1 from
     * the manifest builder — overridden to 0 by serve.c right before
     * applying the sandbox when the loaded app registered app.main
     * with no routes. When 0, pledge's `inet` promise is dropped
     * entirely (unless network_outbound is also 1, in which case
     * outbound sockets still need it). */
    int network_inbound;

    /* GPU access. If gpu == 0, no GPU paths are unveiled.
     * If gpu == 1 and gpu_device_count == 0, all devices are allowed.
     * If gpu_device_count > 0, gpu_devices[0..gpu_device_count) lists
     * the specific allowed device minor numbers. */
    int        gpu;
    const int *gpu_devices;
    int        gpu_device_count;

    /* Terminal UI. When 1, the sandbox unveils /dev/tty and adds the
     * Seatbelt clauses needed for tcsetattr / TIOCGWINSZ. Mirrors
     * manifest.tui — the cap layer's hl_cap_tui_acquire requires this
     * bit to be set. */
    int tui;

    /* ── W^X / no runtime dynamic code ────────────────────────────────
     *
     * wx_enforced — when 1, the sandbox refuses to start unless the
     *   platform can enforce the W^X invariant: no guest-controlled
     *   memory is ever executable, and no W→X transition is possible.
     *   On platforms where this cannot be enforced (no kernel sandbox
     *   support, or manifest opts into dynamic code), startup fails.
     *   Default: 1.
     *
     * allow_dynamic_code — mirrored from manifest.allow_dynamic_code.
     *   When 0, the manifest declares no need for JIT/runtime codegen
     *   and `hl_sandbox_apply` rejects any conflicting request.
     *
     * allow_dynamic_libraries — mirrored from
     *   manifest.allow_dynamic_libraries. When 0, the manifest declares
     *   no need to dlopen() native libraries at runtime. */
    int wx_enforced;
    int allow_dynamic_code;
    int allow_dynamic_libraries;
} HlSandboxPolicy;

/*
 * Populate `policy` from the resolved capability fields of `manifest`.
 * Borrows pointers — manifest must outlive policy.
 *
 * `policy` is fully overwritten; no need to pre-zero. Always returns
 * successfully (no allocation, no failure modes).
 */
void hl_sandbox_policy_from_manifest(HlSandboxPolicy *policy,
                                     const HlManifest *manifest);

/*
 * Phase 1: pledge-only sandbox (no unveil) — blocks exec/proc/fork.
 * Call before load_app() to limit syscalls during module loading.
 * On unsupported platforms, logs a warning and returns 0.
 *
 * Returns 0 on success, -1 on error (logged).
 */
int hl_sandbox_apply_pledge(void);

/*
 * Phase 2: full sandbox based on a resolved policy.
 *
 *   policy         — pre-resolved capability bundle (see HlSandboxPolicy)
 *   app_dir        — application directory (always unveiled read-only)
 *   db_path        — SQLite database path (always allowed rw)
 *   ca_bundle_path — CA certificate bundle (unveiled read-only, may be NULL)
 *   tls_cert_path  — TLS certificate file (unveiled read-only, may be NULL)
 *   tls_key_path   — TLS private key file (unveiled read-only, may be NULL)
 *
 * The sandbox always applies (default-deny).  The app directory is
 * always unveiled for reading (templates, static assets, source files).
 * Returns 0 on success, -1 on error (logged).
 */
int hl_sandbox_apply(const HlSandboxPolicy *policy, const char *app_dir,
                      const char *db_path,
                      const char *ca_bundle_path,
                      const char *tls_cert_path,
                      const char *tls_key_path);

/*
 * Initialize tool-mode unveil context for `hull build`.
 * Populates the HlToolUnveilCtx with allowed paths for build operations.
 * Also calls kernel-level unveil() on supported platforms.
 *
 *   ctx           — tool unveil context to populate
 *   app_dir       — application source directory (read)
 *   output_dir    — directory for output binary (write/create)
 *   platform_dir  — directory containing libhull_platform.a (read)
 *
 * Returns 0 on success, -1 on error.
 */
int hl_tool_sandbox_init(HlToolUnveilCtx *ctx,
                         const char *app_dir,
                         const char *output_dir,
                         const char *platform_dir);

#endif /* HL_SANDBOX_H */

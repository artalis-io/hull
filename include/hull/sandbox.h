/*
 * sandbox.h - Kernel-level sandbox enforcement
 *
 * Applies pledge/unveil (or platform equivalents) based on the
 * application manifest.  After hl_sandbox_apply(), the process
 * can only access the filesystem paths and syscall families that
 * the manifest declares.
 *
 * Platform support:
 *   OpenBSD      - native pledge + unveil
 *   Cosmopolitan - pledge + unveil (built-in)
 *   Linux 5.13+  - Landlock (unveil), seccomp-bpf (pledge)
 *   macOS        - Seatbelt (sandbox_init_with_parameters)
 *   other        - no-op (C-level cap validation is the defense)
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
 * All string/array pointers are BORROWED - they must outlive the
 * HlSandboxPolicy. For policies built from HlManifest via
 * hl_sandbox_policy_from_manifest(), this means the manifest must
 * stay alive for the policy's lifetime (trivially the case in main.c
 * where they share the process lifetime).
 *
 * Building a policy by hand (e.g. for tests) is also supported - just
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
     * The host allowlist itself is enforced in cap/http.c - the sandbox
     * only decides whether to unveil network syscalls at all. */
    int network_outbound;

    /* Network-inbound: 1 if the app may need to accept connections
     * (i.e. it's a server app, not a CLI app.main). Defaults to 1 from
     * the manifest builder - overridden to 0 by serve.c right before
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
     * manifest.tui - the cap layer's hl_cap_tui_acquire requires this
     * bit to be set. */
    int tui;

    /* ── W^X / no runtime dynamic code ────────────────────────────────
     *
     * wx_enforced - when 1, the sandbox refuses to start unless the
     *   platform can enforce the W^X invariant: no guest-controlled
     *   memory is ever executable, and no W→X transition is possible.
     *   On platforms where this cannot be enforced (no kernel sandbox
     *   support, or manifest opts into dynamic code), startup fails.
     *   Default: 1.
     *
     * allow_dynamic_code - mirrored from manifest.allow_dynamic_code.
     *   When 0, the manifest declares no need for JIT/runtime codegen
     *   and `hl_sandbox_apply` rejects any conflicting request.
     *
     * allow_dynamic_libraries - mirrored from
     *   manifest.allow_dynamic_libraries. When 0, the manifest declares
     *   no need to dlopen() native libraries at runtime. */
    int wx_enforced;
    int allow_dynamic_code;
    int allow_dynamic_libraries;

    /* Linux only: permit the seccomp + capability-layer fallback when the
     * running kernel has no Landlock filesystem LSM.  This is deliberately
     * operator policy rather than a manifest capability: application code
     * must not be able to downgrade its own kernel confinement. */
    int allow_degraded;
} HlSandboxPolicy;

/*
 * Populate `policy` from the resolved capability fields of `manifest`.
 * Borrows pointers - manifest must outlive policy.
 *
 * `policy` is fully overwritten; no need to pre-zero. Always returns
 * successfully (no allocation, no failure modes).
 */
/**
 * True if a DSN names a network database or KV backend (it dials out), or is a
 * "$VAR" env-ref whose scheme cannot be known until connect. A bare path is a
 * local SQLite file.
 */
int hl_sandbox_dsn_is_network(const char *dsn);

/* The local file a database DSN names, for the sandbox to gate - NULL for an
 * in-memory or network database (never the DSN itself, which may carry a
 * password). Points into @p dsn. */
const char *hl_sandbox_db_path(const char *dsn);

/**
 * True if the manifest declares a connection that may dial a network database
 * or KV backend: a named connection with a network (or env-ref) DSN, or a
 * databases.dynamic / kv.dynamic policy admitting a network scheme. The one
 * definition behind both the sandbox's network_outbound grant and the entry
 * points' decision to resolve a TLS trust anchor.
 */
int hl_sandbox_manifest_has_network_db(const HlManifest *m);

void hl_sandbox_policy_from_manifest(HlSandboxPolicy *policy,
                                     const HlManifest *manifest);

/*
 * Phase 1: pledge-only sandbox (no unveil) - blocks exec/proc/fork.
 * Call before load_app() to limit syscalls during module loading.
 * On unsupported platforms, logs a warning and returns 0.
 *
 * Returns 0 on success, -1 on error (logged).
 */
int hl_sandbox_apply_pledge(void);

/*
 * Phase 2: full sandbox based on a resolved policy.
 *
 *   policy         - pre-resolved capability bundle (see HlSandboxPolicy)
 *   app_dir        - application directory (always unveiled read-only)
 *   db_path        - SQLite database path (always allowed rw)
 *   ca_bundle_path - CA certificate bundle (unveiled read-only, may be NULL)
 *   tls_cert_path  - TLS certificate file (unveiled read-only, may be NULL)
 *   tls_key_path   - TLS private key file (unveiled read-only, may be NULL)
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
 * The tool sandbox's grants, computed once. hl_tool_sandbox_init applies the
 * SAME plan to both lists - the userspace allowlist the tool bindings check
 * (the only tool sandbox on macOS, Windows and the other BSDs) and the kernel
 * unveil (OpenBSD, Linux with Landlock) - so the two cannot drift: grants
 * that reached only one of them were how a feature archive under
 * ~/.hull/feature, or a key under ~/.hull/keys, went unreadable on one host
 * family and not the other. grant[0] is always "/tmp" (the kernel probe).
 */
#define HL_TOOL_SANDBOX_MAX_GRANTS 40
#define HL_TOOL_SANDBOX_MAX_FILES  8

typedef struct {
    char *path;       /* as granted (resolved where it exists); owned */
    char  perms[8];   /* "r", "rx", "rwc", "rwcx" */
    int   optional;   /* kernel: skipped when the path does not exist */
} HlToolGrant;

typedef struct {
    HlToolGrant g[HL_TOOL_SANDBOX_MAX_GRANTS];
    int         n;
} HlToolSandboxPlan;

/*
 * Compute the plan (no side effects beyond the cache-dir mkdir that
 * hl_hull_cache_dir always does).
 *
 *   app_dir       - application source directory the caller NAMED (read);
 *                   NULL when none was. Dropped when it is "/" or the
 *                   user's home or above.
 *   output_dir    - directory written into (read-write-create): `-o`'s
 *                   directory, a named app directory, or the scaffold
 *                   target; NULL for none. Refused (-1) when it is "/" or
 *                   the user's home or above.
 *   platform_dir  - directory containing libhull_platform.a (read+exec);
 *                   dropped when it is that broad
 *   read_files    - NULL-terminated list of single FILES named on the
 *                   command line (signing keys, --platform-sig, --binary),
 *                   granted read-only each; a path that is not a regular
 *                   file is not granted. At most HL_TOOL_SANDBOX_MAX_FILES.
 *
 * The invocation directory is granted read-write-create unless it is "/" or
 * the user's home or above. Returns 0, or -1 (nothing allocated).
 */
int  hl_tool_sandbox_plan(HlToolSandboxPlan *plan,
                          const char *app_dir,
                          const char *output_dir,
                          const char *platform_dir,
                          const char *const *read_files);
void hl_tool_sandbox_plan_free(HlToolSandboxPlan *plan);

/*
 * Initialize tool-mode unveil context for `hull build` and the other tool
 * commands: compute the plan above, load it into ctx (sealed), and apply it
 * as kernel unveil on hosts that enforce one. Same arguments as the plan.
 *
 * Returns 0 on success, -1 on error.
 */
int hl_tool_sandbox_init(HlToolUnveilCtx *ctx,
                         const char *app_dir,
                         const char *output_dir,
                         const char *platform_dir,
                         const char *const *read_files);

/*
 * Is `path` (resolved) the filesystem root, or the user's home directory
 * ($HOME, else %USERPROFILE%) or one of its ancestors? Compared without
 * regard to case on a Windows host, whose filesystem ignores it. An
 * unresolvable path counts as broad.
 */
int hl_tool_path_too_broad(const char *path);


/*
 * Does THIS host have a kernel sandbox backend behind pledge()/unveil()?
 *
 * Not a compile-time fact for a Cosmopolitan build: the same APE runs on hosts
 * that enforce (Linux, OpenBSD) and hosts where both calls return 0 and do
 * nothing (Windows, macOS, the other BSDs). Exposed so `hull doctor` can report
 * the state BEFORE an app is run, rather than leaving it to a warning at
 * startup. Returns 1 when a backend is present, 0 when the C capability layer
 * is the only boundary.
 */
int hl_sandbox_kernel_available(void);

/*
 * The absolute path the kernel grant for a manifest fs.read (@p for_write 0)
 * or fs.write (1) entry covers, resolved against @p app_dir: a glob's literal
 * directory, a "dir/" grant (created if absent), a file write grant's parent
 * directory - or the file alone when that parent is app_dir itself and the
 * file exists, so one top-level grant does not make the app directory
 * writable - else the path, or its parent when absent. No component may be a
 * symlink. Exposed for tests; hl_sandbox_apply is the caller. 0, or -1 when
 * the entry is refused (absolute, "..", a symlink, too long).
 */
int hl_sandbox_resolve_grant(const char *app_dir, const char *relpath,
                             char *out_abs, size_t out_cap, int for_write);

#endif /* HL_SANDBOX_H */

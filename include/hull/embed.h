/*
 * embed.h — stable C ABI for the libhull no-runtime flavor.
 *
 * This is the ONLY header a native host (C/Rust/Zig) needs to embed the
 * runtime-free Hull core (build/libhull.a). It exposes an opaque handle
 * and a small, versioned function surface over the internal
 * hl_sandbox_* / hl_cap_* layer, so an embedder never reaches into
 * Hull's internal headers (whose layout is not stable across releases).
 *
 * Deliberately depends only on <stddef.h> / <stdint.h> — no HlManifest,
 * HlSandboxPolicy, or HlFsConfig leak across this boundary.
 *
 * Lifecycle (fail-closed): the capability calls refuse to run until the
 * kernel sandbox has been applied via hl_embed_seal(). The intended order
 * is:
 *
 *     HlEmbed *e = hl_embed_new(app_dir);
 *     hl_embed_sandbox_phase1(e);          // optional, recommended
 *     hl_embed_allow_read(e, "data");      // build policy in C
 *     hl_embed_allow_write(e, "out");
 *     if (hl_embed_seal(e, db_path) != 0)  // apply sandbox — MUST check
 *         abort();                         // fail closed on seal failure
 *     hl_embed_fs_write(e, "out/x", ...);  // capabilities now live
 *     hl_embed_free(e);
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_EMBED_H
#define HL_EMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI version. Bumped on any breaking change to the signatures or
 * semantics below. A host can compile-time check HL_EMBED_ABI_VERSION
 * and runtime-check hl_embed_abi_version() to detect a mismatch against
 * the libhull.a it linked.
 */
#define HL_EMBED_ABI_VERSION 1

/* Returns the ABI version compiled into libhull.a. */
int hl_embed_abi_version(void);

/* Opaque embedding handle. Never inspect its fields. */
typedef struct HlEmbed HlEmbed;

/* ── Construction ──────────────────────────────────────────────────── */

/*
 * Create an embedding handle rooted at @p app_dir (an ABSOLUTE path).
 * All capability filesystem access resolves relative to this directory,
 * and the kernel sandbox unveils it. Returns NULL on allocation failure
 * or if @p app_dir is NULL / not absolute.
 */
HlEmbed *hl_embed_new(const char *app_dir);

/* Destroy a handle and free its policy storage. NULL-safe. */
void hl_embed_free(HlEmbed *e);

/* ── Policy (call before hl_embed_seal) ────────────────────────────── */

/*
 * Add an app_dir-relative path to the filesystem read / write allowlist.
 * Same contract as a manifest's fs.read / fs.write: relative paths only
 * (absolute paths and ".." are rejected). Up to 32 entries each.
 *
 * Returns 0 on success, -1 if the list is full, the handle is already
 * sealed, or @p rel_path is NULL.
 */
int hl_embed_allow_read(HlEmbed *e, const char *rel_path);
int hl_embed_allow_write(HlEmbed *e, const char *rel_path);

/*
 * Declare network intent. @p inbound = the process may accept
 * connections; @p outbound = it may open sockets. The host allowlist
 * itself is still enforced per-call in the capability layer; this only
 * decides whether the sandbox unveils network syscalls at all.
 * No-op after seal.
 */
void hl_embed_allow_network(HlEmbed *e, int inbound, int outbound);

/* Enable GPU device access (all devices). No-op after seal. */
void hl_embed_allow_gpu(HlEmbed *e, int enabled);

/* Enable controlling-tty access for terminal UI. No-op after seal. */
void hl_embed_allow_tui(HlEmbed *e, int enabled);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/*
 * Phase-1 sandbox: pledge-only, blocks exec/proc/fork. Optional but
 * recommended — call early, before building policy, to reduce the
 * syscall surface during host setup. Returns 0 on success, -1 on error.
 */
int hl_embed_sandbox_phase1(HlEmbed *e);

/*
 * Phase-2 sandbox: apply the default-deny kernel sandbox from the policy
 * accumulated above and mark the handle sealed. @p db_path may be NULL
 * (no database); when set, it is granted read/write.
 *
 * W^X is enforced (no runtime dynamic code). Returns 0 on success. A
 * non-zero return means the sandbox could not be applied — the host MUST
 * treat this as fatal and NOT proceed, because the capability layer is
 * then running without kernel enforcement. Idempotent-safe: a second
 * call after a successful seal returns -1.
 */
int hl_embed_seal(HlEmbed *e, const char *db_path);

/* ── Capabilities (require a successful hl_embed_seal) ──────────────── */

/*
 * Read a whole file into @p buf. Returns bytes read (>= 0), or -1 on
 * failure (unsealed handle, path rejected, too large, I/O error). The
 * optional @p err receives a static description.
 */
int64_t hl_embed_fs_read(HlEmbed *e, const char *path,
                         char *buf, size_t buf_size, const char **err);

/*
 * Write @p len bytes to @p path (parent dirs created, atomic replace).
 * Returns 0 on success, -1 on failure. @p err optional.
 */
int hl_embed_fs_write(HlEmbed *e, const char *path,
                      const void *data, size_t len, const char **err);

/*
 * Test existence. Returns 1 if present, 0 if absent, -1 on rejection /
 * unsealed handle. @p err optional.
 */
int hl_embed_fs_exists(HlEmbed *e, const char *path, const char **err);

/* Delete a file. Returns 0 on success or already-absent, -1 otherwise. */
int hl_embed_fs_delete(HlEmbed *e, const char *path, const char **err);

/*
 * SHA-256 of @p data into @p out (32 bytes). Stateless — needs no handle
 * and works before seal. Returns 0 on success, -1 on internal failure.
 */
int hl_embed_sha256(const void *data, size_t len, uint8_t out[32]);

/* ── Identity ──────────────────────────────────────────────────────── */

/* Platform arch string, e.g. "darwin-arm64" / "linux-x86_64". */
const char *hl_embed_platform(void);

/* Number of first-party modules in the embedded registry. */
size_t hl_embed_module_count(void);

#ifdef __cplusplus
}
#endif

#endif /* HL_EMBED_H */

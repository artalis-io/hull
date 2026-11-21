/*
 * generation.h - a pipeline generation handed to an app run from source
 * (docs/dev_pipeline_generations_design.md §1.4, §2).
 *
 * The parent (the `hull dev` supervisor, or the pipeline child of a one-shot
 * entry point) gives the app process the outputs of the build pipeline over
 * an inherited descriptor named by HULL_GENERATION_FD. The app process takes
 * it before any app code runs - reads it whole, closes it, removes the
 * variable - checks every payload against the hash its manifest names, and
 * installs the result as the loaders' overlay (vfs.h). Nothing on disk is
 * ever read as a generation, and the app process needs no key: the
 * manifest's authority is "my parent handed it over", and the payloads come
 * with it.
 *
 * The descriptor carries one bundle:
 *
 *   "hull-generation 1 <manifest length, decimal>\n"
 *   <manifest: JSON, that many bytes>
 *   <payloads: each entry's bytes, in entry order, nothing after the last>
 *
 *   { "format": "hull-pipeline-generation", "version": 1,
 *     "generation_id": "...", "pipeline_plan_hash": "...",
 *     "entries": [ { "vfs_name": "app.ts",       app-relative, no "./"
 *                    "content_hash": "<sha256>",  64 lowercase hex
 *                    "size": 1234,
 *                    "origin": { ... },           provenance, not read here
 *                    "position_map": "identity" } ],   "none" | "identity"
 *     "diagnostics_summary": ..., "inputs": [ ... ] }  not read here
 *
 * A built binary ignores the descriptor (it runs its embedded files), and
 * --verify-sig refuses one (the signature covers source, not a generation).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_GENERATION_H
#define HL_SHARED_GENERATION_H

#include <stddef.h>
#include <stdint.h>

#define HL_GENERATION_ENV         "HULL_GENERATION_FD"
#define HL_GENERATION_MAGIC       "hull-generation 1 "
#define HL_GENERATION_FORMAT      "hull-pipeline-generation"

/* Bounds on what the app process reads and keeps. */
#define HL_GENERATION_MAX_BYTES    (256u * 1024u * 1024u)
#define HL_GENERATION_MAX_MANIFEST (4u * 1024u * 1024u)
#define HL_GENERATION_MAX_ENTRIES  16384u
#define HL_GENERATION_NAME_MAX     1024u

/* Take the generation the parent handed over. `embedded`: this is a built
 * binary, which consults no descriptor (the variable is removed, the
 * descriptor left alone). Otherwise the descriptor is read whole and closed,
 * and the bundle kept for hl_generation_apply. Once per process: a later
 * call finds nothing. 0 = none (or ignored), 1 = taken, -1 = the variable
 * names no readable descriptor, or the bundle cannot be read whole within
 * HL_GENERATION_MAX_BYTES (not logged: the entry point prints
 * hl_generation_error() and refuses to start). */
int hl_generation_take_env(int embedded);

/* hl_generation_take_env's work once the descriptor is known: read `fd`
 * whole, close it, keep the bundle for hl_generation_apply. For a process
 * handed the descriptor another way (hull build's manifest-extraction
 * child, named in its arguments). Once per process; same results. */
int hl_generation_take_fd(int fd);

/* A descriptor holding `len` bytes of bundle `b`, at offset 0, to hand to a
 * child (HULL_GENERATION_FD or an argument): an unlinked temporary file
 * under TMPDIR (else /tmp), so no path names it. -1 with errno set. */
int hl_generation_fd(const uint8_t *b, size_t len);

/* hl_generation_take_env, and when no generation was handed over: run the
 * app's build pipeline in a child (`<this hull> plugins generation <app_dir>`,
 * its diagnostics on this process's stderr) when `app_dir` holds a
 * hull.build.json, and keep its generation for hl_generation_apply
 * (docs/dev_pipeline_generations_design.md §4.3). Nothing for a built binary,
 * for a build plugin (a process with a plugin channel), under --verify-sig
 * (`verify_sig` non-zero: a generation is refused there, so the pipeline -
 * a cloned tree's plugins - is never run; audit 50) or once per process.
 * Call before any sandbox forbids spawning. 0 = nothing, 1 = taken, -1 =
 * failed (hl_generation_error() says why; the entry point refuses to start -
 * never a fallback to the sources the generation would shadow). */
int hl_generation_take_ex(int embedded, const char *app_dir, int verify_sig);

/* hl_generation_take_ex for a process not under --verify-sig. */
int hl_generation_take(int embedded, const char *app_dir);

/* Why the last hl_generation_take / _take_env failed. */
const char *hl_generation_error(void);

/* Run the build pipeline of the app at `app_dir` in a child and return its
 * generation (malloc'd, *len bytes). 0, or -1 with hl_generation_error()
 * set. The `hull dev` supervisor calls it before every (re)spawn. */
int hl_generation_produce(const char *app_dir, uint8_t **out, size_t *len);

/* 1 when `app_dir` has a build pipeline to run: build plugins (a regular
 * hull.build.json), or a TypeScript entry (app.ts / app.mts), whose modules
 * the pipeline erases. */
int hl_generation_has_pipeline(const char *app_dir);

/* The workspace paths the generation's inputs name (its manifest's
 * "inputs"), each passed to `fn`. 0, or -1 for a bundle that is not one. */
int hl_generation_inputs(const uint8_t *buf, size_t len,
                         void (*fn)(const char *path, void *ud), void *ud);

/* Install the taken generation as the loaders' overlay (vfs.h) - before the
 * app loads. `verify_sig`: --verify-sig is on, which refuses a generation
 * with entries (an empty one - pipeline work producing no app file - is
 * nothing to cover).
 * 0 when nothing was taken or it is installed; -1 when it is refused or
 * invalid (logged; the app must not start - never a fallback to the
 * sources it would have shadowed). The taken bundle is freed either way. */
int hl_generation_apply(int verify_sig);

/* Check a bundle and install it as the overlay. 0, or -1 with the reason in
 * `err`. (hl_generation_apply's core; tests call it directly.) */
int hl_generation_install(const uint8_t *buf, size_t len,
                          char *err, size_t errsz);

/* hl_generation_install, refusing a generation with entries when
 * `verify_sig`; an empty one is then not installed. */
int hl_generation_install_ex(const uint8_t *buf, size_t len, int verify_sig,
                             char *err, size_t errsz);

/* 1 when `name` is a name an overlay entry may have: app-relative, '/'
 * separated, no empty, "." or ".." component, no leading "./", no control
 * byte, backslash or ':', valid UTF-8, not under the reserved ".hull" (any
 * case), and not starting "hull." / "hull:" (a stdlib name). */
int hl_generation_name_ok(const char *name, size_t len);

/* Forget a taken bundle (tests). */
void hl_generation_reset(void);

#endif /* HL_SHARED_GENERATION_H */

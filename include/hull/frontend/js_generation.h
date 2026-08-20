/*
 * hull/frontend/js_generation.h - the C-owned JS frontend generation/session manager.
 *
 * Owns one QuickJS tooling session per project-analysis GENERATION (shared across its JS files),
 * behind a monotonic positive int64 session token that is NEVER reused (fail-closed at INT64_MAX,
 * never wrapped). A manager mutex serializes lookup+invocation and close, so a close cannot free a
 * session mid-analyze/semantics/scope. See docs/js_frontend_slice6_dispatcher.md.
 *
 * All *_analyze / *_declaration_semantics / *_scope calls return a malloc'd JSON string (caller
 * frees) and an int rc; a stale/closed/unknown token yields the OPERATION-SPECIFIC stale JSON,
 * never a dereference of freed state. Teardown is explicit + C-owned.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_FRONTEND_JS_GENERATION_H
#define HULL_FRONTEND_JS_GENERATION_H

#include <stddef.h>
#include <stdint.h>

/* True iff the JS tooling engine is compiled into this build (QuickJS + the session linked). */
int hl_js_gen_available(void);

/* Open a generation session. Returns a positive int64 token, or 0 on failure (cap reached, the
 * monotonic counter exhausted, or session-create OOM). */
int64_t hl_js_gen_open(void);

/* Analyze one source in the generation `token`. `src`/`src_len` are the raw file bytes; `path` the
 * (project-relative) path. On success returns 0 and sets *out_json (malloc'd, caller frees) to the
 * validated facts JSON; a stale token yields the facts-shaped stale JSON (rc -1). */
int hl_js_gen_analyze(int64_t token, const uint8_t *src, size_t src_len, const char *path,
                      char **out_json, size_t *out_len);

/* Recover the declaration semantics of `decl_id` in `token`. Success -> the record JSON; stale ->
 * { "error": Diagnostic }. */
int hl_js_gen_declaration_semantics(int64_t token, int64_t decl_id, char **out_json, size_t *out_len);

/* Resolve the scope model for `unit_id` in `token`. Success -> the model JSON; stale ->
 * { "ok":false, "error": Diagnostic }. */
int hl_js_gen_scope(int64_t token, int64_t unit_id, char **out_json, size_t *out_len);

/* Close (destroy) the generation session for `token`. Idempotent; an unknown/closed/<=0 token is a
 * safe no-op. The token integer is permanently dead (never reissued). */
void hl_js_gen_close(int64_t token);

/* Destroy any still-live sessions (defensive, on tool-VM shutdown). MUST NOT reset the monotonic
 * token counter (else a later open would reissue a low token and reintroduce ABA in-process). */
void hl_js_gen_shutdown(void);

#ifdef HL_JS_GEN_TESTING
/* TEST-ONLY (compiled only under HL_JS_GEN_TESTING; absent from every shipped build). */

/* Number of live generation sessions. Lets a test assert ownership discipline (open/close
 * leaves zero; shutdown reaps a leaked session) without reaching manager internals. */
int hl_js_gen_live_count(void);

/* Run the test-only authority probe (hull:source:tests:frontend_probe) in the session bound to
 * `token`, routing the minimal-authority claim THROUGH the real manager session. Success ->
 * 0 and *out_json (malloc'd, caller frees) is the probe report; a stale token yields the
 * facts-shaped stale JSON (rc -1). */
int hl_js_gen_probe(int64_t token, char **out_json, size_t *out_len);

/* Attempt to load `module` as the entry in the session bound to `token` (routes through the
 * tooling module loader). A name the loader does not resolve fails rc -1 with the loader's
 * "tooling entry module not found" message in *out_json -- how the test proves a fake
 * application module is REJECTED, not resolved. */
int hl_js_gen_probe_import(int64_t token, const char *module, char **out_json, size_t *out_len);
#endif /* HL_JS_GEN_TESTING */

#endif /* HULL_FRONTEND_JS_GENERATION_H */

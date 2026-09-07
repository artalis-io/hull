/*
 * manifest_extract_file.c - runtime-neutral helper that spins up a
 * transient JS runtime to read app.manifest({...}) from a `.js` file.
 *
 * See include/hull/manifest_extract_file.h for the rationale + the
 * caller-side contract. The only consumer today is
 * src/hull/runtime/lua/mod_tool.c (`tool.extract_manifest_js`); this
 * TU isolates the `hull/runtime/js.h` include so the Lua tool binding
 * doesn't have to cross the sibling-runtime boundary itself.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/manifest_extract_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy a NUL-terminated string onto the heap (malloc'd). NULL-safe.
 * Used by both the JS-enabled and JS-disabled paths below - keep it
 * above the HL_ENABLE_JS gate. */
static char *strdup_safe(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

/* Result-file writer. Deliberately OUTSIDE the HL_ENABLE_JS guard: it is
 * pure stdio, and hull __extract-manifest-js (its only caller) is compiled
 * whenever the make-level HL_ENABLE_JS is on - which includes RUNTIME=lua,
 * where the macro is NOT defined. Guarding it there left the child command
 * with an undefined reference at link. */
#define HL_MEXTRACT_MAGIC "HULLMANIFEST1 "

int hl_manifest_extract_write_result(const char *out_path, const char *status,
                                     const char *payload, size_t payload_len)
{
    if (!out_path || !status) return -1;
    FILE *f = fopen(out_path, "wb");
    if (!f) return -1;
    int ok = (fputs(HL_MEXTRACT_MAGIC, f) >= 0) &&
             (fputs(status, f) >= 0) &&
             (fputc('\n', f) != EOF);
    if (ok && payload && payload_len)
        ok = (fwrite(payload, 1, payload_len, f) == payload_len);
    /* fflush before fclose so a write error surfaces here rather than being
     * swallowed; the parent's magic check is the backstop either way. */
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { (void)remove(out_path); return -1; }
    return 0;
}


#ifdef HL_ENABLE_JS

#include "hull/runtime/js.h"
#include "hull/stdlib_feature.h"   /* hl_platform_vfs_init / _dispose */
#include "hull/vfs.h"              /* HlVfs */
#include "quickjs.h"
#include "hull/cap/tool.h"         /* hl_tool_spawn, hl_tool_cosmo_tmpdir */
#include "hull/release_io.h"       /* hl_release_io_self_path */
#include "log.h"

#include <limits.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

int hl_manifest_extract_js_in_process(const char *path,
                                       const char *early_result_path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    if (out_json)     *out_json = NULL;
    if (out_json_len) *out_json_len = 0;
    if (out_err)      *out_err = NULL;
    if (!path) {
        if (out_err) *out_err = strdup_safe("null path");
        if (early_result_path)
            (void)hl_manifest_extract_write_result(early_result_path, "err",
                                                   "null path", 9);
        return -1;
    }

    HlJS *js = calloc(1, sizeof(*js));
    if (!js) {
        if (out_err) *out_err = strdup_safe("out of memory");
        return -1;
    }

    HlJSConfig cfg = HL_JS_CONFIG_DEFAULT;
    if (hl_js_init(js, &cfg) != 0) {
        free(js);
        if (out_err) *out_err = strdup_safe("hl_js_init failed");
        if (early_result_path)
            (void)hl_manifest_extract_write_result(early_result_path, "err",
                                                   "hl_js_init failed", 17);
        return -1;
    }

    /* Give the transient extractor the SAME composed platform VFS the real app
     * runtime gets. Without it js->base.platform_vfs is NULL, so every stdlib
     * `hull:*` import (hull:web:attachment-serve, hull:json, ...) misses the VFS
     * lookup in hl_js_module_loader and falls to the manifest-extract lenient
     * stub below - and an app that imports a NAMED export from such a module then
     * dies at link with "Could not find export X", failing extraction. Composing
     * it lets stdlib modules resolve for real; a genuine feature-only module
     * (hull:tui, not in the base VFS) still hits the stub. Disposed in cleanup. */
    HlVfs pvfs;
    void *pvfs_owned = NULL;
    hl_platform_vfs_init(&pvfs, &pvfs_owned);
    js->base.platform_vfs = &pvfs;

    /* Only reading app.manifest(): tolerate an unresolvable `hull:*` import (a
     * feature-module stdlib file that rides the composed feature, e.g. hull:tui)
     * so extraction succeeds instead of failing and forcing callers onto their
     * fail-safe path (issue #114). */
    js->manifest_extract_lenient = 1;

    /* Single cleanup path (goto) so the composed platform VFS is disposed on
     * every exit without repeating it at each error site. */
    int    rc       = 0;
    char  *copy     = NULL;
    size_t json_len = 0;

    /* Run the app; a top-level throw / rejected import now fails the load
     * (hl_js_load_app observes the rejected module eval promise). Do NOT bail
     * on that yet: app.manifest() runs synchronously before any throw, so the
     * authoritative manifest may already be captured. */
    int load_rc = hl_js_load_app(js, path);

    {
        JSContext *ctx = js->ctx;
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue manifest = JS_GetPropertyStr(ctx, global, "__hull_manifest");
        JS_FreeValue(ctx, global);

        int have_manifest = !(JS_IsException(manifest) ||
                              JS_IsUndefined(manifest) || JS_IsNull(manifest));

        /* Capture-then-tolerate, parity with the Lua extractor: if app.manifest
         * was called we HAVE the authoritative composition info and use it even
         * though the module later threw. If NO manifest was captured, a load
         * FAILURE (throw/unresolved import BEFORE app.manifest) is a real
         * extraction failure - fatal, since extraction drives composition; a
         * SUCCESSFUL run with no app.manifest() is a valid manifest-less app. */
        if (!have_manifest) {
            JS_FreeValue(ctx, manifest);
            if (load_rc != 0) {
                if (out_err) *out_err = strdup_safe(
                    "failed to load app (syntax error, throw at top-level, "
                    "or unresolved import?)");
                rc = -1;
            }
            goto cleanup;   /* rc stays 0 for a valid manifest-less app */
        }

        JSValue json = JS_JSONStringify(ctx, manifest, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(ctx, manifest);
        if (JS_IsException(json)) {
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe("JSON.stringify(manifest) failed");
            rc = -1;
            goto cleanup;
        }

        const char *json_str = JS_ToCStringLen(ctx, &json_len, json);
        if (!json_str) {
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe(
                "cannot convert JSON value to string");
            rc = -1;
            goto cleanup;
        }

        /* Heap-copy the JSON BEFORE tearing down the JS runtime - the
         * cstring lifetime ends with JS_FreeCString. */
        copy = malloc(json_len + 1);
        if (!copy) {
            JS_FreeCString(ctx, json_str);
            JS_FreeValue(ctx, json);
            if (out_err) *out_err = strdup_safe("out of memory");
            json_len = 0;
            rc = -1;
            goto cleanup;
        }
        memcpy(copy, json_str, json_len);
        copy[json_len] = '\0';

        JS_FreeCString(ctx, json_str);
        JS_FreeValue(ctx, json);
    }

cleanup:
    /* Report BEFORE tearing the runtime down. QuickJS can abort the process
     * in JS_FreeRuntime's cycle collector (issue #427); by then the manifest
     * is already on disk, so the parent still gets a complete result and the
     * build succeeds instead of dying on a teardown bug in code that has
     * finished doing its job. */
    if (early_result_path) {
        if (rc != 0) {
            const char *m = (out_err && *out_err) ? *out_err
                                                  : "manifest extraction failed";
            (void)hl_manifest_extract_write_result(early_result_path, "err",
                                                   m, strlen(m));
        } else if (copy) {
            (void)hl_manifest_extract_write_result(early_result_path, "ok",
                                                   copy, json_len);
        } else {
            (void)hl_manifest_extract_write_result(early_result_path, "none",
                                                   NULL, 0);
        }
    }

    hl_js_free(js);
    free(js);
    hl_platform_vfs_dispose(pvfs_owned);

    if (rc == 0 && copy && out_json) {
        *out_json = copy;
    } else {
        /* Error, no-manifest, or caller doesn't want the buffer - don't leak. */
        free(copy);
    }
    if (out_json_len) *out_json_len = json_len;
    return rc;
}

/* ── Isolated-child half (issue #427) ───────────────────────────────── *
 *
 * A transient QuickJS runtime can abort the process in JS_FreeRuntime's
 * cycle collector when the app left a self-referential pending microtask.
 * That kills `hull build` outright, so the extraction runs in a re-exec'd
 * child and the parent only ever reads a file.
 *
 * Result-file format - one header line, then the payload verbatim:
 *
 *     HULLMANIFEST1 ok\n{"modules":[...]}      manifest captured
 *     HULLMANIFEST1 none\n                     valid app, no app.manifest()
 *     HULLMANIFEST1 err\nmessage               extraction failed
 *
 * The magic makes a truncated or never-written file detectable, which
 * matters because the child may die AFTER a successful extraction. The
 * child writes and closes this file BEFORE tearing its runtime down, so a
 * teardown abort still leaves a complete, usable result and the build
 * SUCCEEDS rather than merely failing cleanly.
 */

/* Slurp a whole file. NULL on any failure (absent, unreadable, OOM). */
static char *read_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (len == cap) {
            if (cap > SIZE_MAX / 2) { free(buf); fclose(f); return NULL; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    char *nb = realloc(buf, len + 1);
    if (nb) buf = nb;
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Build a writable temp path for the result file. Prefers the cosmo tmpdir on
 * Windows (where /tmp is not a path), else $TMPDIR, else /tmp - which the tool
 * sandbox unveils "rwcx" (sandbox_tool.c). */
static int make_result_path(char *out, size_t outsz)
{
    char dir[512];
    const char *d;
    if (hl_tool_cosmo_tmpdir(dir, sizeof(dir)) == 0) {
        d = dir;
    } else {
        d = getenv("TMPDIR");
        if (!d || !*d) d = "/tmp";
    }
    int n = snprintf(out, outsz, "%s/hull-manifest-XXXXXX", d);
    if (n <= 0 || (size_t)n >= outsz) return -1;
    int fd = mkstemp(out);
    if (fd < 0) return -1;
    close(fd);           /* the child reopens by name; we only wanted the name */
    return 0;
}

int hl_manifest_extract_js_from_file(const char *path,
                                       const char *hull_exe,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    if (out_json)     *out_json = NULL;
    if (out_json_len) *out_json_len = 0;
    if (out_err)      *out_err = NULL;
    if (!path) {
        if (out_err) *out_err = strdup_safe("null path");
        return -1;
    }

    /* Resolve the binary to re-exec. Prefer the OS's own answer; fall back to
     * the caller's argv[0], which is the only route on cosmo (no /proc). */
    char self[PATH_MAX];
    const char *exe = NULL;
    if (hl_release_io_self_path(self, sizeof(self)) == 0) exe = self;
    else if (hull_exe && *hull_exe)                       exe = hull_exe;

    char result_path[600];
    if (!exe || make_result_path(result_path, sizeof(result_path)) != 0) {
        /* Could not set isolation up at all. Degrade to the historical
         * in-process behaviour rather than inventing a new way to fail. */
        log_debug("manifest extraction: no isolation available "
                  "(exe=%s), running in-process", exe ? exe : "unresolved");
        return hl_manifest_extract_js_in_process(path, NULL, out_json,
                                                 out_json_len, out_err);
    }

    const char *argv[] = { exe, "__extract-manifest-js", path, result_path,
                           NULL };
    /* Visible under --verbose. Whether extraction is isolated or fell back is
     * otherwise unobservable from outside the process, which makes a silent
     * regression to the in-process path (and its crash) easy to miss. */
    log_debug("manifest extraction: isolating in child %s", exe);
    /* spawn_SELF, not spawn: the allowlist bounds external tools and cannot
     * name this binary anyway (hull ships as `hull`, `hull.com`,
     * `hull-cosmo.exe`), so routing through it denied the re-exec outright. */
    int spawn_rc = hl_tool_spawn_self(argv);

    size_t rlen = 0;
    char *raw = read_all(result_path, &rlen);
    (void)remove(result_path);

    const size_t maglen = sizeof(HL_MEXTRACT_MAGIC) - 1;
    if (!raw || rlen < maglen || memcmp(raw, HL_MEXTRACT_MAGIC, maglen) != 0) {
        /* No marker at all: the child never reached its first statement.
         * Falling back in-process is safe only if nothing ran, so the three
         * cases hl_tool_spawn_self distinguishes matter here:
         *
         *   >= 0        exited NORMALLY without writing a marker: the verb was
         *               not recognised (an older hull on $PATH, or a JS-less
         *               build falling through to the serve path), or exec
         *               failed with 127. Nothing of the app ran; fall back.
         *   NOSTART     never started (spawn refused / failed). Fall back -
         *               otherwise a spawn that cannot work anywhere would break
         *               every JS build rather than degrading to the old path.
         *   -1          started and died on a signal. Do NOT fall back: that
         *               re-runs the abort this child exists to contain. */
        free(raw);
        if (spawn_rc >= 0 || spawn_rc == HL_TOOL_SPAWN_NOSTART) {
            log_debug("manifest extraction: child produced no result "
                      "(status %d), running in-process", spawn_rc);
            return hl_manifest_extract_js_in_process(path, NULL, out_json,
                                                     out_json_len, out_err);
        }
        if (out_err) *out_err = strdup_safe(
            "manifest extraction crashed (the JS runtime died before it "
            "could report a result)");
        return -1;
    }

    char *body = raw + maglen;
    char *nl   = memchr(body, '\n', rlen - maglen);
    if (!nl) {
        free(raw);
        if (out_err) *out_err = strdup_safe(
            "manifest extraction produced a truncated result");
        return -1;
    }
    *nl = '\0';
    const char *status  = body;
    char       *payload = nl + 1;
    size_t      plen    = rlen - maglen - (size_t)(payload - body);

    int rc = -1;
    if (strcmp(status, "start") == 0) {
        /* The child claimed the file and then died without replacing the
         * marker - the JS runtime aborted mid-extraction. Contained, exactly
         * as intended: report it and never retry in-process. */
        if (out_err) *out_err = strdup_safe(
            "manifest extraction crashed (the JS runtime died before it "
            "could report a result)");
    } else if (strcmp(status, "ok") == 0) {
        char *copy = malloc(plen + 1);
        if (copy) {
            memcpy(copy, payload, plen);
            copy[plen] = '\0';
            if (out_json) *out_json = copy; else free(copy);
            if (out_json_len) *out_json_len = plen;
            rc = 0;
        } else if (out_err) {
            *out_err = strdup_safe("out of memory");
        }
    } else if (strcmp(status, "none") == 0) {
        rc = 0;                     /* valid app that declares no manifest */
    } else {
        if (out_err)
            *out_err = strdup_safe(plen ? payload : "manifest extraction failed");
    }
    free(raw);
    return rc;
}

#else /* HL_ENABLE_JS */

int hl_manifest_extract_js_in_process(const char *path,
                                       const char *early_result_path,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    (void)path; (void)early_result_path;
    if (out_json)     *out_json = NULL;
    if (out_json_len) *out_json_len = 0;
    if (out_err)      *out_err = strdup_safe(
        "this hull was built without HL_ENABLE_JS");
    return -1;
}

int hl_manifest_extract_js_from_file(const char *path,
                                       const char *hull_exe,
                                       char **out_json,
                                       size_t *out_json_len,
                                       char **out_err)
{
    (void)hull_exe;
    return hl_manifest_extract_js_in_process(path, NULL, out_json,
                                             out_json_len, out_err);
}

#endif /* HL_ENABLE_JS */

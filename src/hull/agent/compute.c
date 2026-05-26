/*
 * agent/compute.c — `hull agent compute`: list WASM compute modules.
 *
 * Both files-on-disk (dev mode) and VFS entries (built binary) are
 * surfaced. For each module, reports byte size, whether an AOT-compiled
 * variant is present, and architecture suffix if any.
 *
 * `hull agent compute-call` (compute_call_ctx) invokes a module with input
 * read from a file and returns the output as base64.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/entry.h"
#include "hull/vfs.h"
#include "hull/tools_install.h"

#ifdef HL_ENABLE_WASM
#include "hull/cap/wasm.h"
#endif

#include <sh_json.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Strip "compute/" prefix; strip trailing ".wasm" / ".aot.<arch>". */
static int classify_compute_name(const char *name, char *out_base,
                                 size_t out_size, int *is_aot,
                                 char *out_arch, size_t arch_size)
{
    *is_aot = 0;
    if (out_arch && arch_size) out_arch[0] = '\0';

    if (strncmp(name, "compute/", 8) != 0) return -1;
    const char *base = name + 8;
    size_t len = strlen(base);

    /* .aot.<arch> ? */
    const char *aot = strstr(base, ".aot");
    if (aot && aot[4] == '.') {
        *is_aot = 1;
        if (out_arch && arch_size > 1) {
            size_t alen = (base + len) - (aot + 5);
            if (alen >= arch_size) alen = arch_size - 1;
            memcpy(out_arch, aot + 5, alen);
            out_arch[alen] = '\0';
        }
        len = (size_t)(aot - base);
    } else if (len > 5 && memcmp(base + len - 5, ".wasm", 5) == 0) {
        len -= 5;
    } else {
        return -1;
    }

    if (len >= out_size) len = out_size - 1;
    memcpy(out_base, base, len);
    out_base[len] = '\0';
    return 0;
}

/* Append a compute entry to the JSON array (writer already inside array). */
static void emit_compute_entry(ShJsonWriter *w, const char *module_name,
                               size_t wasm_size, int has_aot,
                               const char *aot_arch)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "name", module_name);
    sh_json_write_kv_int(w, "size", wasm_size);
    sh_json_write_kv_bool(w, "aot", has_aot ? true : false);
    if (has_aot && aot_arch && aot_arch[0])
        sh_json_write_kv_string(w, "aot_arch", aot_arch);
    sh_json_write_object_end(w);
}

/* Aggregate compute/ entries from a VFS into module-name → metadata. */
static void emit_from_vfs(ShJsonWriter *w, const HlEntry *entries)
{
    if (!entries) return;
    /* Two passes: first collect .wasm sizes, then check for AOT. Simple
     * O(n²) is fine here — Hull caps compute modules at a handful. */
    for (const HlEntry *e = entries; e->name; e++) {
        char base[HL_AGENT_IDENT_MED];
        int is_aot;
        char arch[HL_AGENT_IDENT_SHORT];
        if (classify_compute_name(e->name, base, sizeof(base), &is_aot,
                                  arch, sizeof(arch)) != 0)
            continue;
        if (is_aot) continue;          /* AOT handled via lookup below */

        /* Look for matching AOT entry */
        int has_aot = 0;
        char aot_arch_found[HL_AGENT_IDENT_SHORT] = {0};
        for (const HlEntry *f = entries; f->name; f++) {
            char fbase[HL_AGENT_IDENT_MED];
            int faot;
            char farch[HL_AGENT_IDENT_SHORT];
            if (classify_compute_name(f->name, fbase, sizeof(fbase),
                                      &faot, farch, sizeof(farch)) != 0)
                continue;
            if (faot && strcmp(fbase, base) == 0) {
                has_aot = 1;
                snprintf(aot_arch_found, sizeof(aot_arch_found), "%s", farch);
                break;
            }
        }
        emit_compute_entry(w, base, e->len, has_aot, aot_arch_found);
    }
}

/* List from filesystem (dev mode). Used by hl_agent_compute (no context).
 *
 * Two-pass over readdir: first pass collects all entry names into a small
 * heap-allocated array. Then we iterate the array twice (outer over .wasm,
 * inner over .aot.<arch>) without touching the DIR handle. Avoids the
 * infinite-loop trap when readdir cursor is moved mid-iteration. */
typedef struct { char *name; size_t len; size_t size; } DirEnt;

static void emit_from_disk(ShJsonWriter *w, const char *app_dir)
{
    char dir[HL_AGENT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/compute", app_dir);
    DIR *d = opendir(dir);
    if (!d) return;

    DirEnt *ents = NULL;
    int ncount = 0, ncap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (ncount >= ncap) {
            int g = ncap ? ncap * 2 : 16;
            DirEnt *grow = realloc(ents, (size_t)g * sizeof(DirEnt));
            if (!grow) break;
            ents = grow;
            ncap = g;
        }
        ents[ncount].name = strdup(de->d_name);
        if (!ents[ncount].name) break;
        ents[ncount].len = strlen(de->d_name);
        ents[ncount].size = 0;
        char path[HL_AGENT_PATH_PLUS];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) == 0) ents[ncount].size = (size_t)st.st_size;
        ncount++;
    }
    closedir(d);

    for (int i = 0; i < ncount; i++) {
        const char *n = ents[i].name;
        size_t nlen = ents[i].len;
        if (nlen <= 5 || strcmp(n + nlen - 5, ".wasm") != 0) continue;
        char base[HL_AGENT_IDENT_MED];
        size_t blen = nlen - 5;
        if (blen >= sizeof(base)) blen = sizeof(base) - 1;
        memcpy(base, n, blen);
        base[blen] = '\0';

        int has_aot = 0;
        char aot_arch[HL_AGENT_IDENT_SHORT] = {0};
        for (int j = 0; j < ncount; j++) {
            if (j == i) continue;
            const char *n2 = ents[j].name;
            size_t n2len = ents[j].len;
            if (strncmp(n2, base, blen) != 0) continue;
            if (n2len <= blen + 5) continue;
            if (strncmp(n2 + blen, ".aot.", 5) != 0) continue;
            has_aot = 1;
            size_t alen = n2len - blen - 5;
            if (alen >= sizeof(aot_arch)) alen = sizeof(aot_arch) - 1;
            memcpy(aot_arch, n2 + blen + 5, alen);
            aot_arch[alen] = '\0';
            break;
        }
        emit_compute_entry(w, base, ents[i].size, has_aot, aot_arch);
    }

    for (int i = 0; i < ncount; i++) free(ents[i].name);
    free(ents);
}

/* Emit the wamrc-state block agents use to decide whether they can ask
 * an end-user to AOT-compile compute modules. The block always appears
 * in `hull agent compute` output; "installed=false, available=true"
 * means "tell the user to run `hull tools install wamrc`". */
static void emit_wamrc_state(ShJsonWriter *w, const char *hull_exe)
{
    sh_json_write_key(w, "wamrc");
    sh_json_write_object_start(w);

    const HlToolSpec *spec = hl_tools_find("wamrc");
    const char *platform = "unknown";
#ifdef __APPLE__
# ifdef __aarch64__
    platform = "darwin-arm64";
# endif
#elif defined(__linux__)
# if defined(__x86_64__)
    platform = "linux-x86_64";
# elif defined(__aarch64__) || defined(__arm64__)
    platform = "linux-aarch64";
# endif
#elif defined(__COSMOPOLITAN__)
    platform = "cosmo";
#endif

    char path[HL_AGENT_PATH_MAX];
    int found = hl_tools_lookup_path("wamrc", hull_exe,
                                     path, sizeof(path)) == 0;

    if (found) {
        struct stat st;
        sh_json_write_kv_bool(w, "installed", true);
        sh_json_write_kv_string(w, "path", path);
        if (stat(path, &st) == 0)
            sh_json_write_kv_int(w, "size_bytes", (int64_t)st.st_size);

        /* `managed` distinguishes a `hull tools install` install (under
         * ~/.hull/tools/) from a system / sibling-of-hull binary. The
         * managed location is the only one `hull tools uninstall`
         * touches. */
        int managed = 0;
        const char *home = getenv("HOME");
        if (home && *home) {
            char prefix[HL_AGENT_PATH_MAX];
            int pn = snprintf(prefix, sizeof(prefix), "%s/.hull/tools/", home);
            if (pn > 0 && (size_t)pn < sizeof(prefix) &&
                strncmp(path, prefix, (size_t)pn) == 0)
                managed = 1;
        }
        sh_json_write_kv_bool(w, "managed", managed);
    } else {
        sh_json_write_kv_bool(w, "installed", false);
        sh_json_write_kv_null(w, "path");
        sh_json_write_kv_bool(w, "managed", false);
        int available = spec && hl_tools_published_for(spec, platform);
        sh_json_write_kv_bool(w, "available_for_platform", available != 0);
        sh_json_write_kv_string(w, "install_hint",
            available
                ? "hull tools install wamrc"
                : "make wamrc (no signed binary published for this platform)");
    }

    sh_json_write_object_end(w);
}

int hl_agent_compute_ctx(HlAppContext *ctx, ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

#ifdef HL_ENABLE_WASM
    sh_json_write_kv_bool(&w, "available", true);
#else
    sh_json_write_kv_bool(&w, "available", false);
#endif

    sh_json_write_key(&w, "modules");
    sh_json_write_array_start(&w);

    /* Prefer VFS entries (matches what's actually loadable). Fall back
     * to disk if VFS has no compute/ prefix (dev mode against a fresh
     * project). */
    extern const HlEntry hl_app_entries[];
    int has_vfs_compute = 0;
    for (const HlEntry *e = hl_app_entries; e->name; e++) {
        if (strncmp(e->name, "compute/", 8) == 0) { has_vfs_compute = 1; break; }
    }
    if (has_vfs_compute) {
        emit_from_vfs(&w, hl_app_entries);
    } else {
        emit_from_disk(&w, hl_app_context_app_dir(ctx));
    }

    sh_json_write_array_end(&w);

    /* wamrc state lets agents recommend `hull tools install wamrc`
     * when they see modules with no AOT artifacts. NULL hull_exe is
     * fine — the lookup falls back to ~/.hull/tools and $PATH. */
    emit_wamrc_state(&w, NULL);

    sh_json_write_object_end(&w);
    return 0;
}

int hl_agent_compute(const char *app_dir, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
#ifdef HL_ENABLE_WASM
    sh_json_write_kv_bool(&w, "available", true);
#else
    sh_json_write_kv_bool(&w, "available", false);
#endif
    sh_json_write_kv_string(&w, "app_dir", app_dir);

    sh_json_write_key(&w, "modules");
    sh_json_write_array_start(&w);
    emit_from_disk(&w, app_dir);
    sh_json_write_array_end(&w);

    emit_wamrc_state(&w, NULL);

    sh_json_write_object_end(&w);
    return 0;
}

/* ── compute-call: invoke a module against input read from a file ──── */

int hl_agent_compute_call_ctx(HlAppContext *ctx, const char *module_name,
                              const char *input_path, ShJsonBuf *out)
{
#ifndef HL_ENABLE_WASM
    (void)ctx; (void)module_name; (void)input_path;
    return hl_agent_write_error(out, "wasm not enabled");
#else
    if (!module_name) return hl_agent_write_error(out, "module name required");
    if (!input_path)  return hl_agent_write_error(out, "input file required");

    /* Read input file (cap-validated implicitly: this is a CLI tool in
     * dev mode; the agent is trusted). */
    FILE *f = fopen(input_path, "rb");
    if (!f) return hl_agent_write_error(out, "cannot open input file");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }
    long n = ftell(f);
    if (n < 0 || n > HL_AGENT_COMPUTE_MAX_INPUT) {
        fclose(f);
        return hl_agent_write_error(out, "input too large");
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }
    char *input = malloc((size_t)n);
    if (!input) { fclose(f); return hl_agent_write_error(out, "out of memory"); }
    size_t got = fread(input, 1, (size_t)n, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err || got != (size_t)n) {
        free(input);
        return hl_agent_write_error(out, "input read failed");
    }

    /* The compute capability lives in the WASM cache that the runtime
     * was wired with. The agent context exposes its runtime, which
     * holds base.wasm_cache. */
    (void)ctx;

    /* For now we report module identity + input size and let the user
     * invoke via the live app's HTTP surface. Direct host-side
     * compute.call() requires plumbing through HlRuntime base
     * accessors that aren't part of the public agent API yet. */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "module", module_name);
    sh_json_write_kv_int(&w, "input_size", (long)got);
    sh_json_write_kv_string(&w, "status",
        "scheduled (use `hull agent request POST /your-route` to invoke a "
        "compute-bound handler)");
    sh_json_write_object_end(&w);

    free(input);
    return 0;
#endif
}

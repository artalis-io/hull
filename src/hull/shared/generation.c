/*
 * generation.c - take, check and install the pipeline generation the parent
 * handed over (see include/hull/shared/generation.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/generation.h"

#include "hull/cap/crypto.h"
#include "hull/entry.h"
#include "hull/shared/build_plugin.h"   /* hl_build_plugin_fd */
#include "hull/shared/child_channel.h"  /* hl_chan_self_exe */
#include "hull/shared/term_escape.h"   /* hl_utf8_valid */
#include "../utils/hex.h"
#include "hull/vfs.h"

#include "log.h"
#include "sh_arena.h"
#include "sh_json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <sys/stat.h>
#include <unistd.h>
#ifdef HL_CHAN_SUPPORTED
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

static uint8_t *g_pending;
static size_t   g_pending_len;
static int      g_taken;
static char     g_err[256];

const char *hl_generation_error(void)
{
    return g_err[0] ? g_err : "no error";
}

void hl_generation_reset(void)
{
    free(g_pending);
    g_pending = NULL;
    g_pending_len = 0;
    g_taken = 0;
    g_err[0] = '\0';
}

__attribute__((format(printf, 3, 4)))
static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
    if (!err || errsz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

int hl_generation_name_ok(const char *name, size_t len)
{
    if (!name || len == 0 || len > HL_GENERATION_NAME_MAX)
        return 0;
    if (memchr(name, '\0', len) || name[0] == '/')
        return 0;
    if (!hl_utf8_valid((const unsigned char *)name, len))
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':')
            return 0;
    }
    /* A stdlib name, as the loaders would read it (hull.x / hull:x). */
    if (len >= 5 && (strncmp(name, "hull.", 5) == 0 ||
                     strncmp(name, "hull:", 5) == 0))
        return 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && name[i] != '/')
            continue;
        size_t cl = i - start;
        const char *comp = name + start;
        if (cl == 0)
            return 0;                                   /* "a//b", "a/" */
        if ((cl == 1 && comp[0] == '.') ||
            (cl == 2 && comp[0] == '.' && comp[1] == '.'))
            return 0;
        /* The reserved .hull (audit 37), in any letter case. */
        if (start == 0 && cl == 5 && comp[0] == '.' &&
            strncasecmp(comp + 1, "hull", 4) == 0)
            return 0;
        start = i + 1;
    }
    return 1;
}

/* A JSON number that is a whole value in 0..max. */
static int json_size(const ShJsonValue *v, size_t max, size_t *out)
{
    if (sh_json_type(v) != SH_JSON_NUMBER)
        return -1;
    double d = sh_json_as_double(v, -1.0);
    if (!(d >= 0.0) || d > (double)max || d != (double)(size_t)d)
        return -1;
    *out = (size_t)d;
    return 0;
}

static int json_str_is(const ShJsonValue *v, const char *want)
{
    const char *s = sh_json_as_string(v, NULL);
    return s && sh_json_string_len(v) == strlen(want) && strcmp(s, want) == 0;
}

int hl_generation_install(const uint8_t *buf, size_t len,
                          char *err, size_t errsz)
{
    return hl_generation_install_ex(buf, len, 0, err, errsz);
}

int hl_generation_install_ex(const uint8_t *buf, size_t len, int verify_sig,
                             char *err, size_t errsz)
{
    if (!buf) {
        set_err(err, errsz, "no generation");
        return -1;
    }
    /* Header: magic, decimal manifest length, LF. */
    size_t ml = strlen(HL_GENERATION_MAGIC);
    if (len < ml || memcmp(buf, HL_GENERATION_MAGIC, ml) != 0) {
        set_err(err, errsz, "not a pipeline generation (bad header)");
        return -1;
    }
    size_t p = ml, mlen = 0, digits = 0;
    while (p < len && buf[p] >= '0' && buf[p] <= '9' && digits < 9) {
        mlen = mlen * 10 + (size_t)(buf[p] - '0');
        p++;
        digits++;
    }
    if (digits == 0 || (digits > 1 && buf[ml] == '0') || p >= len ||
        buf[p] != '\n') {
        set_err(err, errsz, "bad generation header");
        return -1;
    }
    p++;
    if (mlen == 0 || mlen > HL_GENERATION_MAX_MANIFEST || mlen > len - p) {
        set_err(err, errsz, "bad generation manifest length");
        return -1;
    }
    const char *mtext = (const char *)buf + p;
    const uint8_t *payloads = buf + p + mlen;
    size_t payload_total = len - p - mlen;

    int rc = -1;
    HlEntry *entries = NULL;
    SHArena *arena = sh_arena_create(16 * mlen + 65536);
    ShJsonValue *root = NULL;
    if (!arena || sh_json_parse(mtext, mlen, arena, &root) != SH_JSON_OK ||
        sh_json_type(root) != SH_JSON_OBJECT) {
        set_err(err, errsz, "the generation manifest is not a JSON object");
        goto out;
    }
    if (!json_str_is(sh_json_get(root, "format"), HL_GENERATION_FORMAT)) {
        set_err(err, errsz, "the generation manifest has no format \""
                HL_GENERATION_FORMAT "\"");
        goto out;
    }
    size_t version = 0;
    if (json_size(sh_json_get(root, "version"), 1000, &version) != 0 ||
        version != 1) {
        set_err(err, errsz, "unsupported generation version");
        goto out;
    }
    const ShJsonValue *list = sh_json_get(root, "entries");
    if (sh_json_type(list) != SH_JSON_ARRAY) {
        set_err(err, errsz, "the generation manifest has no entries list");
        goto out;
    }
    size_t n = sh_json_array_len(list);
    if (n > HL_GENERATION_MAX_ENTRIES) {
        set_err(err, errsz, "too many generation entries (%zu, at most %u)",
                n, HL_GENERATION_MAX_ENTRIES);
        goto out;
    }
    entries = calloc(n + 1, sizeof *entries);
    if (!entries) {
        set_err(err, errsz, "out of memory");
        goto out;
    }

    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        const ShJsonValue *e = sh_json_array_get(list, i);
        if (sh_json_type(e) != SH_JSON_OBJECT) {
            set_err(err, errsz, "generation entry %zu is not an object", i);
            goto out;
        }
        const ShJsonValue *nv = sh_json_get(e, "vfs_name");
        const char *name = sh_json_as_string(nv, NULL);
        if (!name || !hl_generation_name_ok(name, sh_json_string_len(nv)) ||
            strlen(name) != sh_json_string_len(nv)) {
            set_err(err, errsz, "generation entry %zu has a bad vfs_name", i);
            goto out;
        }
        const ShJsonValue *hv = sh_json_get(e, "content_hash");
        const char *hash = sh_json_as_string(hv, NULL);
        if (!hash || sh_json_string_len(hv) != 64) {
            set_err(err, errsz, "generation entry %s has a bad content_hash",
                    name);
            goto out;
        }
        size_t size = 0;
        if (json_size(sh_json_get(e, "size"), HL_GENERATION_MAX_BYTES,
                      &size) != 0 || size > payload_total - off) {
            set_err(err, errsz, "generation entry %s has a bad size", name);
            goto out;
        }
        /* A position table has no schema yet: a payload's positions are
         * its source's ("identity"), or it has none (a WASM module). */
        const ShJsonValue *pm = sh_json_get(e, "position_map");
        if (pm && !json_str_is(pm, "none") && !json_str_is(pm, "identity")) {
            set_err(err, errsz, "generation entry %s has an unsupported "
                    "position_map", name);
            goto out;
        }
        uint8_t digest[32];
        char hex[65];
        if (hl_cap_crypto_sha256(payloads + off, size, digest) != 0 ||
            hl_hex_encode(digest, 32, hex, sizeof hex) != 0 ||
            memcmp(hex, hash, 64) != 0) {
            set_err(err, errsz, "generation entry %s does not match its "
                    "content_hash", name);
            goto out;
        }
        entries[i].name = name;
        entries[i].data = payloads + off;
        entries[i].len = (unsigned int)size;
        off += size;
    }
    if (off != payload_total) {
        set_err(err, errsz, "the generation has %zu bytes past its last "
                "payload", payload_total - off);
        goto out;
    }
    if (verify_sig && n > 0) {
        /* The signature and the disk gate hash SOURCE; a generation is not
         * covered by them (design §6). */
        set_err(err, errsz, "--verify-sig refuses a pipeline generation: run "
                "the built binary to verify a signed app");
        goto out;
    }
    if (verify_sig) {
        rc = 0;            /* nothing to install, nothing to cover */
        goto out;
    }
    if (hl_vfs_overlay_install(entries, n) != 0) {
        set_err(err, errsz, "cannot install the generation (a duplicate "
                "vfs_name, or out of memory)");
        goto out;
    }
    rc = 0;
out:
    free(entries);
    if (arena) sh_arena_free(arena);
    return rc;
}

/* Read the descriptor whole (it may be a file or a pipe), at most
 * HL_GENERATION_MAX_BYTES. */
static int read_all(int fd, uint8_t **out, size_t *out_len)
{
    size_t cap = 65536, used = 0;
    uint8_t *b = malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (used == cap) {
            if (cap >= HL_GENERATION_MAX_BYTES + 1u) { free(b); return -1; }
            size_t nc = cap * 2;
            if (nc > HL_GENERATION_MAX_BYTES + 1u)
                nc = HL_GENERATION_MAX_BYTES + 1u;
            uint8_t *nb = realloc(b, nc);
            if (!nb) { free(b); return -1; }
            b = nb;
            cap = nc;
        }
        ssize_t r = read(fd, b + used, cap - used);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(b);
            return -1;
        }
        if (r == 0) break;
        used += (size_t)r;
        if (used > HL_GENERATION_MAX_BYTES) { free(b); return -1; }
    }
    *out = b;
    *out_len = used;
    return 0;
}

int hl_generation_take_env(int embedded)
{
    if (g_taken) return 0;
    const char *v = getenv(HL_GENERATION_ENV);
    if (!v) return 0;
    char val[16];
    size_t vl = strlen(v);
    int fd = -1;
    if (vl > 0 && vl < sizeof val) {
        memcpy(val, v, vl + 1);
        char *end = NULL;
        long l = strtol(val, &end, 10);
        if (end && *end == '\0' && end != val && l >= 3 && l <= 65535)
            fd = (int)l;
    }
    (void)unsetenv(HL_GENERATION_ENV);
    if (embedded) {
        /* A built binary runs its embedded files and consults no
         * descriptor (design §1.5). */
        g_taken = 1;
        log_debug("[hull:c] " HL_GENERATION_ENV " ignored: a built binary "
                  "loads no generation");
        return 0;
    }
    return hl_generation_take_fd(fd);
}

int hl_generation_take_fd(int fd)
{
    if (g_taken) return 0;
    g_taken = 1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 ||
        !(S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) ||
          S_ISSOCK(st.st_mode))) {
        snprintf(g_err, sizeof g_err, HL_GENERATION_ENV " names no readable "
                 "descriptor");
        return -1;
    }
    uint8_t *b = NULL;
    size_t blen = 0;
    int rr = read_all(fd, &b, &blen);
    (void)close(fd);
    if (rr != 0) {
        snprintf(g_err, sizeof g_err, "cannot read the pipeline generation "
                 "(a read error, or more than %u bytes)", HL_GENERATION_MAX_BYTES);
        return -1;
    }
    g_pending = b;
    g_pending_len = blen;
    return 1;
}

int hl_generation_apply(int verify_sig)
{
    if (!g_pending) return 0;
    uint8_t *b = g_pending;
    size_t blen = g_pending_len;
    g_pending = NULL;
    g_pending_len = 0;
    char err[256];
    err[0] = '\0';
    int rc = hl_generation_install_ex(b, blen, verify_sig, err, sizeof err);
    if (rc != 0)
        log_error("[hull:c] the pipeline generation is refused: %s", err);
    free(b);
    return rc;
}

int hl_generation_fd(const uint8_t *b, size_t len)
{
    const char *t = getenv("TMPDIR");
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/hull-generation-XXXXXX",
                     t && t[0] == '/' ? t : "/tmp");
    if (n < 0 || (size_t)n >= sizeof path) return -1;
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    (void)unlink(path);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, b + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) { close(fd); return -1; }
    return fd;
}

/* ── Running the pipeline ─────────────────────────────────────────── */

/* 1 when `<app_dir>/<name>` is a regular file. */
static int app_file(const char *app_dir, const char *name)
{
    char path[4096];
    struct stat st;
    int n = app_dir ? snprintf(path, sizeof path, "%s/%s", app_dir, name) : -1;
    return n > 0 && (size_t)n < sizeof path && lstat(path, &st) == 0 &&
           S_ISREG(st.st_mode);
}

int hl_generation_has_pipeline(const char *app_dir)
{
    /* Build plugins, or a TypeScript entry: its erased modules come from
     * the pipeline's erasure route (docs/typescript_design.md §4.4). */
    return app_file(app_dir, "hull.build.json") ||
           app_file(app_dir, "app.ts") || app_file(app_dir, "app.mts");
}

int hl_generation_produce(const char *app_dir, uint8_t **out, size_t *len)
{
#ifdef HL_CHAN_SUPPORTED
    char exe[4096];
    if (hl_chan_self_exe(exe, sizeof exe) != 0) {
        snprintf(g_err, sizeof g_err, "cannot find this hull binary to run "
                 "the build pipeline");
        return -1;
    }
    int p[2];
    if (pipe(p) != 0) {
        snprintf(g_err, sizeof g_err, "cannot run the build pipeline: %s",
                 strerror(errno));
        return -1;
    }
    (void)fcntl(p[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int ok = posix_spawn_file_actions_init(&fa) == 0;
    int at_ok = ok && posix_spawnattr_init(&at) == 0;
    ok = at_ok;
    /* stdin nothing, stdout the bundle, stderr shared: the plugins'
     * diagnostics are the developer's to read. */
    ok = ok && posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_adddup2(&fa, p[1], 1) == 0;
    ok = ok && posix_spawn_file_actions_addclose(&fa, p[1]) == 0;
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    ok = ok && posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK |
                                             POSIX_SPAWN_SETSIGDEF) == 0;
    ok = ok && posix_spawnattr_setsigmask(&at, &none) == 0;
    ok = ok && posix_spawnattr_setsigdefault(&at, &all) == 0;
    char *argv[] = { exe, (char *)"plugins", (char *)"generation", NULL, NULL };
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", app_dir);
    argv[3] = dir;
    pid_t pid = -1;
    int err = ok ? posix_spawn(&pid, exe, &fa, &at, argv, environ) : ENOMEM;
    if (at_ok) posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (err != 0) {
        close(p[0]);
        snprintf(g_err, sizeof g_err, "cannot run the build pipeline: %s",
                 strerror(err));
        return -1;
    }
    uint8_t *b = NULL;
    size_t blen = 0;
    int rr = read_all(p[0], &b, &blen);
    close(p[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(b);
        snprintf(g_err, sizeof g_err, "the build pipeline failed (its "
                 "diagnostics are above)");
        return -1;
    }
    if (rr != 0) {
        snprintf(g_err, sizeof g_err, "the build pipeline's generation is "
                 "larger than %u bytes", HL_GENERATION_MAX_BYTES);
        return -1;
    }
    *out = b;
    *len = blen;
    return 0;
#else
    (void)app_dir; (void)out; (void)len;
    snprintf(g_err, sizeof g_err, "build plugins need a native hull (this "
             "build cannot spawn them)");
    return -1;
#endif
}

int hl_generation_take(int embedded, const char *app_dir, int verify_sig)
{
    int rc = hl_generation_take_env(embedded);
    if (rc != 0 || g_taken)
        return rc;
    g_taken = 1;
    /* --verify-sig refuses any generation (hl_generation_apply), so running
     * the pipeline first only ran a cloned tree's plugins and top level
     * before the refusal (audit 50). */
    if (embedded || verify_sig || hl_build_plugin_fd() >= 0 ||
        !hl_generation_has_pipeline(app_dir))
        return 0;
    uint8_t *b = NULL;
    size_t blen = 0;
    if (hl_generation_produce(app_dir, &b, &blen) != 0)
        return -1;
    g_pending = b;
    g_pending_len = blen;
    return 1;
}

int hl_generation_inputs(const uint8_t *buf, size_t len,
                         void (*fn)(const char *path, void *ud), void *ud)
{
    size_t ml = strlen(HL_GENERATION_MAGIC);
    if (!buf || len < ml || memcmp(buf, HL_GENERATION_MAGIC, ml) != 0)
        return -1;
    size_t p = ml, mlen = 0, digits = 0;
    while (p < len && buf[p] >= '0' && buf[p] <= '9' && digits < 9) {
        mlen = mlen * 10 + (size_t)(buf[p] - '0');
        p++;
        digits++;
    }
    if (digits == 0 || p >= len || buf[p] != '\n')
        return -1;
    p++;
    if (mlen == 0 || mlen > HL_GENERATION_MAX_MANIFEST || mlen > len - p)
        return -1;
    SHArena *arena = sh_arena_create(16 * mlen + 65536);
    ShJsonValue *root = NULL;
    int rc = -1;
    if (arena && sh_json_parse((const char *)buf + p, mlen, arena, &root) ==
                     SH_JSON_OK) {
        const ShJsonValue *in = sh_json_get(root, "inputs");
        if (sh_json_type(in) == SH_JSON_ARRAY) {
            size_t n = sh_json_array_len(in);
            for (size_t i = 0; i < n; i++) {
                const char *s = sh_json_as_string(sh_json_array_get(in, i), NULL);
                if (s) fn(s, ud);
            }
            rc = 0;
        }
    }
    if (arena) sh_arena_free(arena);
    return rc;
}

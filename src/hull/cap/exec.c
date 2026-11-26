/*
 * cap/exec.c - the declared-command policy and the one argument matcher
 * (docs/design/exec_broker.md §3, §4; header: hull/cap/exec.h).
 *
 * Encoding (little-endian; every string is u16 length, the bytes, then a
 * NUL that the length does not count, so a reader can hand it out as a C
 * string):
 *
 *   policy  := "HXP1" u16:ncmds cmd*ncmds
 *   cmd     := str:name str:path u32:timeout_ms u32:stdout_max
 *              u32:stderr_max u8:concurrency u8:flags
 *              [32 bytes: the sha256 pin, if flags has SHA256]
 *              [u32:stdin_max, if flags has STDIN]
 *              u16:n_env (str:name str:value)*n_env
 *              u16:n_fs_read str*  u16:n_fs_write str*
 *              u16:n_argv arg*n_argv
 *   arg     := u8:0 str                                   (fixed)
 *            | u8:1 u8:flags u16:min u16:max u8:rep_min u8:rep_max
 *              str:prefix str:suffix [32 bytes: chars bitmap, if HAS_CHARS]
 *              u16:n_enum str*n_enum                      (slot)
 *
 * Every rule a policy must obey is checked in hl_exec_policy_validate and
 * nowhere else; the builder only encodes (and parses the `chars` spec).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec.h"
#include "hull/utils/alloc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t MAGIC[4] = { 'H', 'X', 'P', '1' };

/* ── Bounded reader ────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    int            bad;
} Rd;

static int rd_need(Rd *r, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n) { r->bad = 1; return 0; }
    return 1;
}

static uint8_t rd_u8(Rd *r)
{
    if (!rd_need(r, 1)) return 0;
    return *r->p++;
}

static uint16_t rd_u16(Rd *r)
{
    if (!rd_need(r, 2)) return 0;
    uint16_t v = (uint16_t)(r->p[0] | (r->p[1] << 8));
    r->p += 2;
    return v;
}

static uint32_t rd_u32(Rd *r)
{
    if (!rd_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
                 ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}

/* A string: its length, its bytes, a NUL; no NUL inside. */
static const char *rd_str(Rd *r, size_t *len)
{
    uint16_t n = rd_u16(r);
    if (!rd_need(r, (size_t)n + 1)) { *len = 0; return NULL; }
    const char *s = (const char *)r->p;
    if (r->p[n] != 0 || memchr(s, 0, n)) { r->bad = 1; *len = 0; return NULL; }
    r->p += (size_t)n + 1;
    *len = n;
    return s;
}

static const uint8_t *rd_bytes(Rd *r, size_t n)
{
    if (!rd_need(r, n)) return NULL;
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

/* ── Builder ───────────────────────────────────────────────────────── */

void hl_exec_b_init(HlExecBuf *b, HlAllocator *alloc)
{
    memset(b, 0, sizeof *b);
    b->alloc = alloc;
    /* header written lazily by the first put, so an empty buffer is NULL */
}

void hl_exec_b_fail(HlExecBuf *b, const char *fmt, ...)
{
    if (b->failed) return;
    b->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->err, sizeof b->err, fmt, ap);
    va_end(ap);
}

static void put(HlExecBuf *b, const void *src, size_t n)
{
    if (b->failed) return;
    if (!b->data) {
        size_t cap = 256;
        b->data = hl_alloc_malloc(b->alloc, cap);
        if (!b->data) { hl_exec_b_fail(b, "exec: out of memory"); return; }
        b->cap = cap;
        memcpy(b->data, MAGIC, 4);
        b->data[4] = b->data[5] = 0;            /* ncmds, written at finish */
        b->len = 6;
    }
    if (n > HL_EXEC_POLICY_MAX_BYTES - b->len) {
        hl_exec_b_fail(b, "exec: the section is larger than %u bytes",
                       (unsigned)HL_EXEC_POLICY_MAX_BYTES);
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap;
        while (cap < b->len + n) cap *= 2;
        uint8_t *g = hl_alloc_realloc(b->alloc, b->data, b->cap, cap);
        if (!g) { hl_exec_b_fail(b, "exec: out of memory"); return; }
        b->data = g;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, src, n);
    b->len += n;
}

static void put_u8(HlExecBuf *b, uint8_t v) { put(b, &v, 1); }

static void put_u16(HlExecBuf *b, uint16_t v)
{
    const uint8_t x[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put(b, x, 2);
}

static void put_u32(HlExecBuf *b, uint32_t v)
{
    const uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    put(b, x, 4);
}

static void put_str(HlExecBuf *b, const char *s, size_t n)
{
    if (n > 0xFFFFu) {
        hl_exec_b_fail(b, "exec: a string is longer than 65535 bytes");
        return;
    }
    if (s && memchr(s, 0, n)) {
        hl_exec_b_fail(b, "exec: a string holds a NUL byte");
        return;
    }
    put_u16(b, (uint16_t)n);
    if (n) put(b, s, n);
    put_u8(b, 0);
}

/* -1 = the default; anything else out of the field's range is written as
 * its all-ones value, which every ceiling refuses. */
static int hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t opt_u32(int64_t v, uint32_t dflt)
{
    if (v == -1) return dflt;
    if (v < 0 || v > (int64_t)0xFFFFFFFEu) return 0xFFFFFFFFu;
    return (uint32_t)v;
}

static uint16_t opt_u16(int64_t v, uint16_t dflt)
{
    if (v == -1) return dflt;
    if (v < 0 || v > 0xFFFE) return 0xFFFF;
    return (uint16_t)v;
}

static uint8_t opt_u8(int64_t v, uint8_t dflt)
{
    if (v == -1) return dflt;
    if (v < 0 || v > 0xFE) return 0xFF;
    return (uint8_t)v;
}

void hl_exec_b_cmd(HlExecBuf *b, const char *name, size_t name_len,
                   const char *path, size_t path_len,
                   int64_t timeout_ms, int64_t stdout_max, int64_t stderr_max,
                   int64_t concurrency, int unix_sockets, int64_t stdin_max,
                   const char *sha256_hex, size_t sha256_len)
{
    if (b->ncmds == 0xFFFF) { hl_exec_b_fail(b, "exec: too many commands"); return; }
    uint8_t pin[32];
    if (sha256_hex) {
        int ok = sha256_len == 64;
        for (size_t i = 0; ok && i < 32; i++) {
            int hi = hexval((unsigned char)sha256_hex[2 * i]);
            int lo = hexval((unsigned char)sha256_hex[2 * i + 1]);
            if (hi < 0 || lo < 0) ok = 0;
            else pin[i] = (uint8_t)(hi << 4 | lo);
        }
        if (!ok) {
            hl_exec_b_fail(b, "exec.%.*s: sha256 must be 64 hex characters",
                           (int)(name_len > 64 ? 64 : name_len), name);
            return;
        }
    }
    b->ncmds++;
    put_str(b, name, name_len);
    put_str(b, path, path_len);
    put_u32(b, opt_u32(timeout_ms, HL_EXEC_TIMEOUT_DEFAULT_MS));
    put_u32(b, opt_u32(stdout_max, HL_EXEC_STDOUT_DEFAULT));
    put_u32(b, opt_u32(stderr_max, HL_EXEC_STDERR_DEFAULT));
    put_u8(b, opt_u8(concurrency, HL_EXEC_CONCURRENCY_DEFAULT));
    int has_stdin = stdin_max != -1 && stdin_max != 0;
    b->flags_at = b->len;
    put_u8(b, (uint8_t)((unix_sockets ? HL_EXEC_CMD_UNIX_SOCKETS : 0) |
                        (sha256_hex ? HL_EXEC_CMD_SHA256 : 0) |
                        (has_stdin ? HL_EXEC_CMD_STDIN : 0)));
    if (sha256_hex) put(b, pin, sizeof pin);
    if (has_stdin) put_u32(b, opt_u32(stdin_max, 0));   /* out of range: kept, refused */
}

void hl_exec_b_app_fs(HlExecBuf *b)
{
    if (b->failed || b->ncmds == 0 || b->flags_at >= b->len) return;
    b->data[b->flags_at] |= HL_EXEC_CMD_APP_FS;
}

size_t hl_exec_b_count_begin(HlExecBuf *b)
{
    put_u16(b, 0);
    return b->failed ? 0 : b->len - 2;
}

void hl_exec_b_count_end(HlExecBuf *b, size_t mark, size_t n)
{
    if (b->failed) return;
    uint16_t v = n > 0xFFFF ? 0xFFFF : (uint16_t)n;
    b->data[mark] = (uint8_t)v;
    b->data[mark + 1] = (uint8_t)(v >> 8);
}

void hl_exec_b_str(HlExecBuf *b, const char *s, size_t n) { put_str(b, s, n); }

void hl_exec_b_fixed(HlExecBuf *b, const char *s, size_t n)
{
    put_u8(b, 0);
    put_str(b, s, n);
}

/* Parse a `chars` spec (§4): literals and a-z style ranges of printable
 * ASCII other than space; '-' is a literal at the start or the end. */
static int chars_parse(const char *s, size_t n, uint8_t bm[32], char *why,
                       size_t why_size)
{
    memset(bm, 0, 32);
    if (n == 0) { snprintf(why, why_size, "chars is empty"); return -1; }
    for (size_t i = 0; i < n; i++) {
        unsigned char lo = (unsigned char)s[i], hi = lo;
        if (i + 2 < n && s[i + 1] == '-') {
            hi = (unsigned char)s[i + 2];
            i += 2;
        }
        if (lo < 0x21 || lo > 0x7e || hi < 0x21 || hi > 0x7e) {
            snprintf(why, why_size, "chars may hold only printable ASCII "
                     "other than space");
            return -1;
        }
        if (lo > hi) {
            snprintf(why, why_size, "chars has a range '%c-%c' that runs "
                     "backwards", lo, hi);
            return -1;
        }
        for (unsigned c = lo; c <= hi; c++)
            bm[c >> 3] |= (uint8_t)(1u << (c & 7));
    }
    return 0;
}

void hl_exec_b_slot(HlExecBuf *b, int allow_dash,
                    int64_t min, int64_t max,
                    int repeat, int64_t repeat_min, int64_t repeat_max,
                    const char *prefix, size_t prefix_len,
                    const char *suffix, size_t suffix_len,
                    const char *chars, size_t chars_len)
{
    uint8_t bm[32];
    uint8_t flags = (uint8_t)((allow_dash ? HL_EXEC_SLOT_ALLOW_DASH : 0) |
                              (repeat ? HL_EXEC_SLOT_REPEAT : 0));
    if (chars) {
        char why[96];
        if (chars_parse(chars, chars_len, bm, why, sizeof why) != 0) {
            hl_exec_b_fail(b, "exec: a slot's %s", why);
            return;
        }
        flags |= HL_EXEC_SLOT_HAS_CHARS;
    }
    put_u8(b, 1);
    put_u8(b, flags);
    put_u16(b, opt_u16(min, HL_EXEC_SLOT_DEFAULT_MIN));
    put_u16(b, opt_u16(max, HL_EXEC_SLOT_DEFAULT_MAX));
    put_u8(b, repeat ? opt_u8(repeat_min, 1) : 1);
    put_u8(b, repeat ? opt_u8(repeat_max, 0xFF) : 1);
    put_str(b, prefix, prefix ? prefix_len : 0);
    put_str(b, suffix, suffix ? suffix_len : 0);
    if (chars) put(b, bm, 32);
}

void hl_exec_buf_free(HlExecBuf *b)
{
    if (b->data) hl_alloc_free(b->alloc, b->data, b->cap);
    b->data = NULL;
    b->len = b->cap = 0;
}

int hl_exec_b_finish(HlExecBuf *b)
{
    if (!b->failed && !b->data) put(b, NULL, 0);   /* an empty section */
    if (b->failed) { hl_exec_buf_free(b); return -1; }
    b->data[4] = (uint8_t)b->ncmds;
    b->data[5] = (uint8_t)(b->ncmds >> 8);
    if (hl_exec_policy_validate(b->data, b->len, b->err, sizeof b->err) != 0) {
        b->failed = 1;
        hl_exec_buf_free(b);
        return -1;
    }
    return 0;
}

/* ── Validation ────────────────────────────────────────────────────── */

int hl_exec_name_valid(const char *name, size_t n)
{
    if (!name || n == 0 || n > HL_EXEC_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (i > 0 && c == '-');
        if (!ok) return 0;
    }
    return 1;
}

/* An absolute path with no empty, "." or ".." component. */
static int path_ok(const char *p, size_t n)
{
    if (n < 2 || n > HL_EXEC_PATH_MAX || p[0] != '/') return 0;
    size_t i = 1;
    while (i <= n) {
        size_t j = i;
        while (j < n && p[j] != '/') j++;
        size_t c = j - i;
        if (c == 0) return 0;                         /* "//" or trailing '/' */
        if (c == 1 && p[i] == '.') return 0;
        if (c == 2 && p[i] == '.' && p[i + 1] == '.') return 0;
        i = j + 1;
    }
    return 1;
}

static int has_prefix(const char *s, size_t n, const char *pre)
{
    size_t m = strlen(pre);
    return n > m && memcmp(s, pre, m) == 0;
}

static int env_name_ok(const char *s, size_t n)
{
    if (n == 0 || n > 128) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
                 (i > 0 && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

/* Names a child's environment may not set: the loader's, the shells', and
 * the two Hull sets itself (§3). */
static int env_name_refused(const char *s, size_t n)
{
    if ((n >= 3 && memcmp(s, "LD_", 3) == 0) ||
        (n >= 5 && memcmp(s, "DYLD_", 5) == 0))
        return 1;
    static const char *const fixed[] = { "PATH", "IFS", "ENV", "BASH_ENV", NULL };
    for (int i = 0; fixed[i]; i++)
        if (n == strlen(fixed[i]) && memcmp(s, fixed[i], n) == 0) return 1;
    return 0;
}

#define VERR(...) do { snprintf(err, err_size, __VA_ARGS__); return -1; } while (0)

/* Validate one string list of @p n entries, each a path; @p write = fs.write. */
static int v_fs_list(Rd *r, const char *cmd, const char *what, int write,
                     char *err, size_t err_size)
{
    uint16_t n = rd_u16(r);
    if (r->bad) VERR("exec: the policy is truncated");
    if (n > HL_EXEC_MAX_FS)
        VERR("exec.%s: fs.%s has more than %d paths", cmd, what, HL_EXEC_MAX_FS);
    for (uint16_t i = 0; i < n; i++) {
        size_t len;
        const char *s = rd_str(r, &len);
        if (!s) VERR("exec: the policy is malformed");
        if (!path_ok(s, len))
            VERR("exec.%s: fs.%s entry %u must be an absolute path with no "
                 "'.', '..' or empty component", cmd, what, (unsigned)i + 1);
        if (write && !has_prefix(s, len, "/dev/") && !has_prefix(s, len, "/run/") &&
            !has_prefix(s, len, "/tmp/"))
            VERR("exec.%s: fs.write entry %u must be under /dev, /run or /tmp",
                 cmd, (unsigned)i + 1);
    }
    return 0;
}

/* A child manifest's fs list (§3): the shape only. Each entry is an app-root
 * or "@state/" relative grant; the canonical rules (hl_fs_grant_parse) and
 * what a command may be granted are checked by hl_exec_child_grant_check,
 * which the extractors and the broker run - this file stays free of the fs
 * layer, so the fuzzers link it alone. */
static int v_app_list(Rd *r, const char *cmd, const char *what,
                      char *err, size_t err_size)
{
    uint16_t n = rd_u16(r);
    if (r->bad) VERR("exec: the policy is truncated");
    if (n > HL_EXEC_MAX_FS)
        VERR("exec.%s: manifest.fs.%s has more than %d paths", cmd, what, HL_EXEC_MAX_FS);
    for (uint16_t i = 0; i < n; i++) {
        size_t len;
        const char *s = rd_str(r, &len);
        if (!s) VERR("exec: the policy is malformed");
        if (len == 0 || len >= 4096 || s[0] == '/' || memchr(s, '\0', len))
            VERR("exec.%s: manifest.fs.%s entry %u must be a path under the app "
                 "directory or \"@state/\" (as an app's fs grant)", cmd, what,
                 (unsigned)i + 1);
    }
    return 0;
}

static int bm_has(const uint8_t *bm, unsigned char c)
{
    return (bm[c >> 3] >> (c & 7)) & 1;
}

static int v_slot(Rd *r, const char *cmd, unsigned idx, int last,
                  char *err, size_t err_size)
{
    uint8_t flags = rd_u8(r);
    uint16_t min = rd_u16(r), max = rd_u16(r);
    uint8_t rmin = rd_u8(r), rmax = rd_u8(r);
    size_t plen, slen;
    const char *pre = rd_str(r, &plen);
    const char *suf = rd_str(r, &slen);
    const uint8_t *bm = NULL;
    if (!r->bad && (flags & HL_EXEC_SLOT_HAS_CHARS)) bm = rd_bytes(r, 32);
    uint16_t n_enum = rd_u16(r);
    if (r->bad || !pre || !suf) VERR("exec: the policy is malformed");

    if (flags & ~(HL_EXEC_SLOT_ALLOW_DASH | HL_EXEC_SLOT_HAS_CHARS | HL_EXEC_SLOT_REPEAT))
        VERR("exec.%s: argv %u: unknown slot flags", cmd, idx);
    int dash = flags & HL_EXEC_SLOT_ALLOW_DASH;
    if (min < 1) VERR("exec.%s: argv %u: min must be at least 1", cmd, idx);
    if (max > HL_EXEC_SLOT_LEN_CEIL)
        VERR("exec.%s: argv %u: max is at most %d", cmd, idx, HL_EXEC_SLOT_LEN_CEIL);
    if (min > max) VERR("exec.%s: argv %u: min is larger than max", cmd, idx);
    if (flags & HL_EXEC_SLOT_REPEAT) {
        if (!last)
            VERR("exec.%s: argv %u: only the last argv entry may repeat", cmd, idx);
        if (rmax < 1 || rmax > HL_EXEC_MAX_REPEAT)
            VERR("exec.%s: argv %u: repeat.max must be 1..%d", cmd, idx,
                 HL_EXEC_MAX_REPEAT);
        if (rmin > rmax)
            VERR("exec.%s: argv %u: repeat.min is larger than repeat.max", cmd, idx);
    } else if (rmin != 1 || rmax != 1) {
        VERR("exec: the policy is malformed");
    }
    if (bm) {
        for (unsigned c = 0; c < 256; c++)
            if (bm_has(bm, (unsigned char)c) && (c < 0x21 || c > 0x7e))
                VERR("exec.%s: argv %u: chars may hold only printable ASCII "
                     "other than space", cmd, idx);
    }
    if (n_enum > HL_EXEC_MAX_ENUM)
        VERR("exec.%s: argv %u: enum has more than %d values", cmd, idx,
             HL_EXEC_MAX_ENUM);
    if (n_enum > 0 && (plen || slen || bm))
        VERR("exec.%s: argv %u: enum excludes prefix, suffix and chars", cmd, idx);
    if (plen + slen > max)
        VERR("exec.%s: argv %u: prefix and suffix are longer than max", cmd, idx);
    if (plen && pre[0] == '-' && !dash)
        VERR("exec.%s: argv %u: the prefix starts with '-', which needs "
             "allow_dash", cmd, idx);
    for (uint16_t i = 0; i < n_enum; i++) {
        size_t len;
        const char *s = rd_str(r, &len);
        if (!s) VERR("exec: the policy is malformed");
        if (len < min || len > max)
            VERR("exec.%s: argv %u: enum value %u is outside min..max", cmd, idx,
                 (unsigned)i + 1);
        if (s[0] == '-' && !dash)
            VERR("exec.%s: argv %u: enum value %u starts with '-', which needs "
                 "allow_dash", cmd, idx, (unsigned)i + 1);
    }
    return 0;
}

static int v_cmd(Rd *r, const char **names, size_t *name_lens, int ci,
                 char *err, size_t err_size)
{
    size_t nlen, plen;
    const char *name = rd_str(r, &nlen);
    const char *path = rd_str(r, &plen);
    uint32_t timeout = rd_u32(r), so = rd_u32(r), se = rd_u32(r);
    uint8_t conc = rd_u8(r), flags = rd_u8(r);
    if (r->bad || !name || !path) VERR("exec: the policy is malformed");

    if (!hl_exec_name_valid(name, nlen))
        VERR("exec: command %d: a name is [a-z0-9] then up to 63 of [a-z0-9-]",
             ci + 1);
    for (int j = 0; j < ci; j++)
        if (name_lens[j] == nlen && memcmp(names[j], name, nlen) == 0)
            VERR("exec: command '%s' is declared twice", name);
    names[ci] = name;
    name_lens[ci] = nlen;

    if (!path_ok(path, plen))
        VERR("exec.%s: path must be an absolute path with no '.', '..' or "
             "empty component", name);
    if (timeout < HL_EXEC_TIMEOUT_MIN_MS || timeout > HL_EXEC_TIMEOUT_MAX_MS)
        VERR("exec.%s: timeout_ms must be %d..%d", name, HL_EXEC_TIMEOUT_MIN_MS,
             HL_EXEC_TIMEOUT_MAX_MS);
    if (so > HL_EXEC_STDOUT_CEIL)
        VERR("exec.%s: stdout_max is at most %u", name, (unsigned)HL_EXEC_STDOUT_CEIL);
    if (se > HL_EXEC_STDERR_CEIL)
        VERR("exec.%s: stderr_max is at most %u", name, (unsigned)HL_EXEC_STDERR_CEIL);
    if (conc < 1 || conc > HL_EXEC_CONCURRENCY_MAX)
        VERR("exec.%s: concurrency must be 1..%d", name, HL_EXEC_CONCURRENCY_MAX);
    if (flags & ~(HL_EXEC_CMD_UNIX_SOCKETS | HL_EXEC_CMD_SHA256 | HL_EXEC_CMD_STDIN |
                  HL_EXEC_CMD_APP_FS))
        VERR("exec.%s: unknown command flags", name);
    if ((flags & HL_EXEC_CMD_SHA256) && !rd_bytes(r, 32))
        VERR("exec: the policy is truncated");
    if (flags & HL_EXEC_CMD_STDIN) {
        uint32_t sm = rd_u32(r);
        if (r->bad) VERR("exec: the policy is truncated");
        if (sm < 1 || sm > HL_EXEC_STDIN_CEIL)
            VERR("exec.%s: stdin_max must be 1..%u", name, (unsigned)HL_EXEC_STDIN_CEIL);
    }

    /* env */
    uint16_t n_env = rd_u16(r);
    if (r->bad) VERR("exec: the policy is truncated");
    if (n_env > HL_EXEC_MAX_ENV)
        VERR("exec.%s: env has more than %d entries", name, HL_EXEC_MAX_ENV);
    const char *env_names[HL_EXEC_MAX_ENV];
    size_t env_lens[HL_EXEC_MAX_ENV];
    for (uint16_t i = 0; i < n_env; i++) {
        size_t kl, vl;
        const char *k = rd_str(r, &kl);
        const char *v = rd_str(r, &vl);
        if (!k || !v) VERR("exec: the policy is malformed");
        if (!env_name_ok(k, kl))
            VERR("exec.%s: env name %u is not a variable name", name, (unsigned)i + 1);
        if (env_name_refused(k, kl))
            VERR("exec.%s: env may not set %s (LD_*, DYLD_*, PATH, IFS, ENV and "
                 "BASH_ENV are refused)", name, k);
        if (vl > HL_EXEC_ENV_VALUE_MAX)
            VERR("exec.%s: env %s is longer than %d bytes", name, k,
                 HL_EXEC_ENV_VALUE_MAX);
        for (uint16_t j = 0; j < i; j++)
            if (env_lens[j] == kl && memcmp(env_names[j], k, kl) == 0)
                VERR("exec.%s: env %s is set twice", name, k);
        env_names[i] = k;
        env_lens[i] = kl;
    }

    if (v_fs_list(r, name, "read", 0, err, err_size) != 0) return -1;
    if (v_fs_list(r, name, "write", 1, err, err_size) != 0) return -1;

    /* argv */
    uint16_t n_argv = rd_u16(r);
    if (r->bad) VERR("exec: the policy is truncated");
    if (n_argv > HL_EXEC_MAX_ARGV)
        VERR("exec.%s: argv has more than %d entries", name, HL_EXEC_MAX_ARGV);
    size_t fixed_bytes = 0;
    for (uint16_t i = 0; i < n_argv; i++) {
        uint8_t kind = rd_u8(r);
        if (r->bad) VERR("exec: the policy is truncated");
        if (kind == 0) {
            size_t len;
            if (!rd_str(r, &len)) VERR("exec: the policy is malformed");
            fixed_bytes += len;
            if (fixed_bytes > HL_EXEC_MAX_FIXED_BYTES)
                VERR("exec.%s: the fixed argv is longer than %d bytes", name,
                     HL_EXEC_MAX_FIXED_BYTES);
        } else if (kind == 1) {
            if (v_slot(r, name, (unsigned)i + 1, i + 1 == n_argv, err, err_size) != 0)
                return -1;
        } else {
            VERR("exec: the policy is malformed");
        }
    }
    if (flags & HL_EXEC_CMD_APP_FS) {
        if (v_app_list(r, name, "read", err, err_size) != 0) return -1;
        if (v_app_list(r, name, "write", err, err_size) != 0) return -1;
    }
    return 0;
}

int hl_exec_policy_validate(const uint8_t *data, size_t len,
                            char *err, size_t err_size)
{
    char dummy[1];
    if (!err || err_size == 0) { err = dummy; err_size = sizeof dummy; }
    if (!data || len < 6 || len > HL_EXEC_POLICY_MAX_BYTES ||
        memcmp(data, MAGIC, 4) != 0)
        VERR("exec: not a policy");
    Rd r = { data + 4, data + len, 0 };
    uint16_t n = rd_u16(&r);
    if (n > HL_EXEC_MAX_COMMANDS)
        VERR("exec: more than %d commands", HL_EXEC_MAX_COMMANDS);
    const char *names[HL_EXEC_MAX_COMMANDS];
    size_t name_lens[HL_EXEC_MAX_COMMANDS];
    for (int i = 0; i < n; i++)
        if (v_cmd(&r, names, name_lens, i, err, err_size) != 0) return -1;
    if (r.bad) VERR("exec: the policy is truncated");
    if (r.p != r.end) VERR("exec: the policy has trailing bytes");
    return 0;
}

#undef VERR

/* ── Reading ───────────────────────────────────────────────────────── */

/* Skip @p n strings. */
static void skip_strs(Rd *r, size_t n)
{
    for (size_t i = 0; i < n && !r->bad; i++) {
        size_t len;
        (void)rd_str(r, &len);
    }
}

static void read_slot(Rd *r, HlExecSlot *s)
{
    memset(s, 0, sizeof *s);
    s->flags = rd_u8(r);
    s->min = rd_u16(r);
    s->max = rd_u16(r);
    s->repeat_min = rd_u8(r);
    s->repeat_max = rd_u8(r);
    s->prefix = rd_str(r, &s->prefix_len);
    s->suffix = rd_str(r, &s->suffix_len);
    if (s->flags & HL_EXEC_SLOT_HAS_CHARS) s->chars = rd_bytes(r, 32);
    s->n_enum = rd_u16(r);
    s->enums = r->p;
    skip_strs(r, s->n_enum);
}

/* Read one command at r (validated policy) and leave r past it. */
static void read_cmd(Rd *r, HlExecCmd *c)
{
    size_t len;
    memset(c, 0, sizeof *c);
    c->name = rd_str(r, &len);
    c->path = rd_str(r, &len);
    c->timeout_ms = rd_u32(r);
    c->stdout_max = rd_u32(r);
    c->stderr_max = rd_u32(r);
    c->concurrency = rd_u8(r);
    c->flags = rd_u8(r);
    if (c->flags & HL_EXEC_CMD_SHA256) c->sha256 = rd_bytes(r, 32);
    if (c->flags & HL_EXEC_CMD_STDIN) c->stdin_max = rd_u32(r);
    c->n_env = rd_u16(r);
    c->env = r->p;
    skip_strs(r, (size_t)c->n_env * 2);
    c->n_fs_read = rd_u16(r);
    c->fs_read = r->p;
    skip_strs(r, c->n_fs_read);
    c->n_fs_write = rd_u16(r);
    c->fs_write = r->p;
    skip_strs(r, c->n_fs_write);
    c->n_argv = rd_u16(r);
    c->argv = r->p;
    c->end = r->end;
    for (uint16_t i = 0; i < c->n_argv && !r->bad; i++) {
        uint8_t kind = rd_u8(r);
        if (kind == 0) { (void)rd_str(r, &len); }
        else { HlExecSlot s; read_slot(r, &s); }
    }
    if (c->flags & HL_EXEC_CMD_APP_FS) {
        c->n_app_read = rd_u16(r);
        c->app_read = r->p;
        skip_strs(r, c->n_app_read);
        c->n_app_write = rd_u16(r);
        c->app_write = r->p;
        skip_strs(r, c->n_app_write);
    }
}

int hl_exec_policy_count(const uint8_t *data, size_t len)
{
    if (!data || len < 6 || memcmp(data, MAGIC, 4) != 0) return -1;
    return data[4] | (data[5] << 8);
}

int hl_exec_policy_at(const uint8_t *data, size_t len, int i, HlExecCmd *out)
{
    int n = hl_exec_policy_count(data, len);
    if (n < 0 || i < 0 || i >= n) return -1;
    Rd r = { data + 6, data + len, 0 };
    HlExecCmd c;
    for (int k = 0; k <= i; k++) read_cmd(&r, &c);
    if (r.bad) return -1;
    *out = c;
    return 0;
}

int hl_exec_policy_slice(const uint8_t *data, size_t len, int i,
                         uint8_t **out, size_t *out_len)
{
    int n = hl_exec_policy_count(data, len);
    if (n < 0 || i < 0 || i >= n || !out || !out_len) return -1;
    Rd r = { data + 6, data + len, 0 };
    HlExecCmd c;
    for (int k = 0; k < i; k++) read_cmd(&r, &c);
    const uint8_t *start = r.p;
    read_cmd(&r, &c);
    if (r.bad) return -1;
    size_t elen = (size_t)(r.p - start);
    uint8_t *s = malloc(6 + elen);
    if (!s) return -1;
    memcpy(s, data, 4);                  /* the magic */
    s[4] = 1;
    s[5] = 0;
    memcpy(s + 6, start, elen);
    *out = s;
    *out_len = 6 + elen;
    return 0;
}

int hl_exec_policy_find(const uint8_t *data, size_t len,
                        const char *name, size_t name_len, HlExecCmd *out)
{
    int n = hl_exec_policy_count(data, len);
    if (n < 0 || !name) return -1;
    Rd r = { data + 6, data + len, 0 };
    for (int k = 0; k < n && !r.bad; k++) {
        HlExecCmd c;
        read_cmd(&r, &c);
        if (!r.bad && strlen(c.name) == name_len &&
            memcmp(c.name, name, name_len) == 0) {
            if (out) *out = c;
            return 0;
        }
    }
    return -1;
}

const char *hl_exec_next_str(const uint8_t **cursor, size_t *len_out)
{
    /* A validated policy: the bound is the string's own length. */
    const uint8_t *p = *cursor;
    size_t n = (size_t)(p[0] | (p[1] << 8));
    if (len_out) *len_out = n;
    *cursor = p + 2 + n + 1;
    return (const char *)(p + 2);
}

void hl_exec_next_arg(const uint8_t **cursor, const uint8_t *end, HlExecArg *out)
{
    Rd r = { *cursor, end, 0 };
    memset(out, 0, sizeof *out);
    uint8_t kind = rd_u8(&r);
    if (kind == 0) {
        out->fixed = rd_str(&r, &out->fixed_len);
    } else {
        out->is_slot = 1;
        read_slot(&r, &out->slot);
    }
    *cursor = r.p;
}

/* ── Matching ──────────────────────────────────────────────────────── */

/* The class a slot without `chars` admits: [A-Za-z0-9._-]. */
static int default_class(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

#define WHY(...) do { if (why && why_size) snprintf(why, why_size, __VA_ARGS__); \
                      return -1; } while (0)

int hl_exec_slot_match(const HlExecSlot *slot, const char *v, size_t n,
                       char *why, size_t why_size)
{
    if (!slot || !v || n == 0) WHY("is empty");
    if (memchr(v, 0, n)) WHY("holds a NUL byte");
    if (n < slot->min || n > slot->max)
        WHY("is %zu bytes; this slot takes %u..%u", n, (unsigned)slot->min,
            (unsigned)slot->max);
    if (v[0] == '-' && !(slot->flags & HL_EXEC_SLOT_ALLOW_DASH))
        WHY("starts with '-' (option injection); a slot takes one only with "
            "allow_dash");
    if (slot->n_enum) {
        const uint8_t *cur = slot->enums;
        for (uint16_t i = 0; i < slot->n_enum; i++) {
            size_t len;
            const char *e = hl_exec_next_str(&cur, &len);
            if (len == n && memcmp(e, v, n) == 0) return 0;
        }
        WHY("is not one of the slot's enum values");
    }
    if (slot->prefix_len &&
        (n < slot->prefix_len || memcmp(v, slot->prefix, slot->prefix_len) != 0))
        WHY("does not start with the slot's prefix");
    if (slot->suffix_len &&
        (n - slot->prefix_len < slot->suffix_len ||
         memcmp(v + n - slot->suffix_len, slot->suffix, slot->suffix_len) != 0))
        WHY("does not end with the slot's suffix");
    for (size_t i = slot->prefix_len; i < n - slot->suffix_len; i++) {
        unsigned char c = (unsigned char)v[i];
        int ok = slot->chars ? bm_has(slot->chars, c) : default_class(c);
        if (!ok) WHY("has a byte at offset %zu outside the slot's chars", i);
    }
    return 0;
}

#undef WHY

int hl_exec_check_call(const uint8_t *policy, size_t policy_len,
                       const char *name, size_t name_len,
                       const char *const *args, const size_t *lens, size_t n,
                       HlExecCmd *cmd_out, char *err, size_t err_size)
{
    char dummy[1];
    if (!err || err_size == 0) { err = dummy; err_size = sizeof dummy; }
    HlExecCmd c;
    if (!policy || hl_exec_policy_find(policy, policy_len, name, name_len, &c) != 0) {
        snprintf(err, err_size, "no command of that name is declared in the "
                 "manifest's exec section");
        return HL_EXEC_E_UNKNOWN;
    }
    size_t total = 0;
    for (size_t i = 0; i < n; i++) {
        if (lens[i] > HL_EXEC_MAX_ARG_BYTES - total) {
            snprintf(err, err_size, "the arguments are longer than %u bytes "
                     "together", (unsigned)HL_EXEC_MAX_ARG_BYTES);
            return HL_EXEC_E_ARGS;
        }
        total += lens[i];
    }

    /* How many values the slots take: one each, M..N for the repeat slot
     * (only ever the last argv entry, so a greedy fill is exact). */
    size_t want_min = 0, want_max = 0;
    const uint8_t *cur = c.argv;
    for (uint16_t e = 0; e < c.n_argv; e++) {
        HlExecArg a;
        hl_exec_next_arg(&cur, c.end, &a);
        if (!a.is_slot) continue;
        int rep = a.slot.flags & HL_EXEC_SLOT_REPEAT;
        want_min += rep ? a.slot.repeat_min : 1;
        want_max += rep ? a.slot.repeat_max : 1;
    }
    if (n < want_min || n > want_max) {
        if (want_min == want_max)
            snprintf(err, err_size, "takes %zu argument%s, got %zu", want_min,
                     want_min == 1 ? "" : "s", n);
        else
            snprintf(err, err_size, "takes %zu..%zu arguments, got %zu",
                     want_min, want_max, n);
        return HL_EXEC_E_ARGS;
    }

    size_t ai = 0;
    cur = c.argv;
    for (uint16_t e = 0; e < c.n_argv; e++) {
        HlExecArg a;
        hl_exec_next_arg(&cur, c.end, &a);
        if (!a.is_slot) continue;
        size_t take = (a.slot.flags & HL_EXEC_SLOT_REPEAT) ? n - ai : 1;
        for (size_t k = 0; k < take; k++, ai++) {
            char why[128];
            if (hl_exec_slot_match(&a.slot, args[ai], lens[ai], why, sizeof why) != 0) {
                snprintf(err, err_size, "argument %zu %s", ai + 1, why);
                return HL_EXEC_E_ARGS;
            }
        }
    }
    if (cmd_out) *cmd_out = c;
    return HL_EXEC_OK;
}

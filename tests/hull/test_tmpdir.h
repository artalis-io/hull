/**
 * @file test_tmpdir.h
 * @brief TEST-ONLY: create temp files/dirs in the host's real temp directory.
 *
 * Replaces the `char tmpl[] = "/tmp/hull_test_XXXXXX"` pattern, which has two
 * defects. It hardcodes `/tmp`, so it ignores `$TMPDIR` even on POSIX, where
 * honouring it is the convention and a hermetic test runner may point it at a
 * per-job scratch. And the buffer is sized by the literal, so a longer real
 * temp path cannot be substituted without also changing the declaration - the
 * fixed buffer is what makes the hardcoded path sticky.
 *
 * On Windows the same literal is a live failure mode. A Cosmopolitan APE maps
 * `/tmp` through `$TMP` / `$TEMP` (rewritten to POSIX form: `C:\Users\x\Temp`
 * is seen as `/C/Users/x/Temp`), falling back to the Win32 temp path when
 * neither is set. Measured with cosmocc 4.0.2 on Windows 11:
 *
 *     TMP=/C/Users/Mark/AppData/Local/Temp   stat("/tmp") is a dir   mkdtemp ok
 *     TMP unset (Win32 fallback)             stat("/tmp") is a dir   mkdtemp ok
 *     TMP=/D/a/_temp  (does not exist)       stat("/tmp") ENOENT     mkdtemp NULL
 *
 * The third row is the one that costs hours: every suite that opens a temp dir
 * fails at once, each at whatever assertion happens to follow its mkdtemp, and
 * none of them names the cause. So resolution here is explicit and checked -
 * a candidate is used only if it stats as a directory - and a failure prints
 * one line naming every candidate tried, once per process.
 *
 * Sizing: callers pass a buffer, and `HL_TEST_PATH_MAX` is the right size for
 * one. Never size it by the template literal.
 */
#ifndef HULL_TEST_TMPDIR_H
#define HULL_TEST_TMPDIR_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Deliberately well under PATH_MAX. Test code routinely builds a derived path
 * ("%s/.hull/build", "%s.new") into a PATH_MAX buffer from one of these, and
 * the headroom is what lets the compiler prove those cannot truncate.
 */
#define HL_TEST_PATH_MAX 512

/*
 * Drop any trailing '/'. macOS sets $TMPDIR with one ("/var/folders/xy/.../T/"),
 * and a prefix with a trailing separator breaks boundary checks that ask whether
 * the next character is '/' or NUL - hl_tool_unveil_check does exactly that, so
 * an unveiled ".../T/" would deny every path beneath it.
 */
static inline void hl_test_rstrip_slash_(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
}

static inline int hl_test_is_dir_(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * The directory temp files should be created in, or NULL if there isn't a
 * usable one. Resolution order: $TMPDIR, $TMP, $TEMP, then /tmp - each used
 * only if it stats as a directory. Deliberately does NOT fall back to the
 * current directory: a test that scatters artifacts through the source tree
 * is worse than one that fails.
 */
static inline const char *hl_test_tmpdir(void)
{
    /* Copied, not aliased: getenv's return may be invalidated by a later
     * setenv, and some suites do set environment variables. */
    static char cached[HL_TEST_PATH_MAX];
    static int resolved;
    if (resolved) return cached[0] ? cached : NULL;
    resolved = 1;

    const char *names[] = { "TMPDIR", "TMP", "TEMP" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (hl_test_is_dir_(v) && strlen(v) < sizeof cached) {
            memcpy(cached, v, strlen(v) + 1);
            hl_test_rstrip_slash_(cached);
            return cached;
        }
    }
    if (hl_test_is_dir_("/tmp")) { memcpy(cached, "/tmp", 5); return cached; }

    fprintf(stderr,
            "test_tmpdir: no usable temp directory; every temp-backed test in "
            "this suite will fail.\n"
            "  $TMPDIR = %s\n  $TMP    = %s\n  $TEMP   = %s\n"
            "  /tmp    = not a directory\n"
            "On Windows an APE resolves /tmp through $TMP / $TEMP, so a value "
            "naming a path that does not exist breaks both.\n",
            getenv("TMPDIR") ? getenv("TMPDIR") : "(unset)",
            getenv("TMP") ? getenv("TMP") : "(unset)",
            getenv("TEMP") ? getenv("TEMP") : "(unset)");
    return NULL;
}

/**
 * Write "<tmpdir>/<stem>_XXXXXX<suffix>" into @p buf. @p suffix may be NULL.
 * Returns buf, or NULL if there is no temp dir or the path would not fit.
 */
static inline char *hl_test_tmpl(char *buf, size_t n, const char *stem,
                                 const char *suffix)
{
    const char *dir = hl_test_tmpdir();
    if (!dir) return NULL;
    size_t dlen = strlen(dir);
    /* Avoid "//" when TMPDIR is given with a trailing slash. */
    const char *sep = (dlen && dir[dlen - 1] == '/') ? "" : "/";
    int len = snprintf(buf, n, "%s%s%s_XXXXXX%s", dir, sep, stem,
                       suffix ? suffix : "");
    if (len < 0 || (size_t)len >= n) { errno = ENAMETOOLONG; return NULL; }
    return buf;
}

/** mkdtemp() in the host temp dir. Returns buf (the created path) or NULL. */
static inline char *hl_test_mkdtemp(char *buf, size_t n, const char *stem)
{
    if (!hl_test_tmpl(buf, n, stem, NULL)) return NULL;
    return mkdtemp(buf);
}

/**
 * mkstemp()/mkstemps() in the host temp dir; @p suffix may be NULL. Returns
 * the open fd, or -1. @p buf receives the created path.
 */
static inline int hl_test_mkstemp(char *buf, size_t n, const char *stem,
                                  const char *suffix)
{
    if (!hl_test_tmpl(buf, n, stem, suffix)) return -1;
    return suffix ? mkstemps(buf, (int)strlen(suffix)) : mkstemp(buf);
}

/**
 * Write "<tmpdir>/" followed by the formatted name into @p buf. Use for a
 * caller-named temp path (typically pid-suffixed) rather than a mkdtemp
 * template. Returns 0, or -1 if there is no temp dir or it would not fit.
 */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static inline int hl_test_path(char *buf, size_t n, const char *fmt, ...)
{
    const char *dir = hl_test_tmpdir();
    if (!dir) return -1;
    size_t dlen = strlen(dir);
    const char *sep = (dlen && dir[dlen - 1] == '/') ? "" : "/";
    int head = snprintf(buf, n, "%s%s", dir, sep);
    if (head < 0 || (size_t)head >= n) { errno = ENAMETOOLONG; return -1; }

    va_list ap;
    va_start(ap, fmt);
    int tail = vsnprintf(buf + head, n - (size_t)head, fmt, ap);
    va_end(ap);
    if (tail < 0 || (size_t)(head + tail) >= n) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

#endif /* HULL_TEST_TMPDIR_H */

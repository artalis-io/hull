/*
 * shared/host.c - host-OS facts and user-facing command rendering.
 *
 * See include/hull/shared/host.h for the rationale. Leaf module: libc only,
 * no Hull headers beyond its own, so it links into every flavor (it rides
 * CAP_OBJS like host_match.o / hex.o) and is cheap to unit-test.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/host.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ── Host detection ─────────────────────────────────────────────────
 *
 * A native build knows its host at compile time. A cosmo APE is the ONLY
 * build that reaches Windows, and the same bytes also run on Linux/macOS/BSD,
 * so it must decide at runtime. Windows always exports SystemRoot (and
 * windir); no POSIX host does. This is the same probe cap/tool.c uses to
 * decide whether to route cosmocc through the bundled busybox - kept
 * byte-identical in meaning, but defined once here.
 */
static int detect_windows(void)
{
#if defined(_WIN32)
    return 1;
#elif defined(__COSMOPOLITAN__)
    const char *v = getenv("SystemRoot");
    if (v && *v) return 1;
    v = getenv("SYSTEMROOT");
    if (v && *v) return 1;
    v = getenv("windir");
    if (v && *v) return 1;
    return 0;
#else
    return 0;
#endif
}

int hl_host_is_windows(void)
{
    /* Cache: this is consulted per rendered hint and per PATH probe. The
     * host cannot change mid-process. -1 = not yet computed. */
    static int cached = -1;
    if (cached < 0) cached = detect_windows();
    return cached;
}

const char *hl_host_os(void)
{
    if (hl_host_is_windows()) return "windows";
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(__COSMOPOLITAN__)
    /* A cosmo APE on a non-Windows host: we know it is POSIX but not which
     * one without probing further, and nothing needs the distinction. */
    return "posix";
#else
    return "posix";
#endif
}

char hl_host_path_list_sep(void) { return hl_host_is_windows() ? ';' : ':'; }
char hl_host_dir_sep(void)       { return hl_host_is_windows() ? '\\' : '/'; }

const char *hl_host_exe_suffix(void)
{
    /* Windows will not execute an extensionless file. Hull's build output is
     * a Cosmopolitan APE, whose Windows-facing convention is ".com" (which is
     * also how install.ps1 names hull itself: hull.com). */
    return hl_host_is_windows() ? ".com" : "";
}

/* Would a shell need this rendering quoted to be runnable as one word?
 *
 * Conservative allowlist; anything outside it gets quoted. A space is the
 * case that matters in practice - `C:\Users\Jane Doe\myapp\app.com` printed
 * bare is not a command, it is two words. */
static int host_exec_needs_quoting(const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9'))
            continue;
        switch (c) {
        case '_': case '-': case '.': case '/': case '\\':
        case ':': case '+': case '=': case '@': case ',': case '~':
            continue;
        default:
            return 1;
        }
    }
    return 0;
}

int hl_host_render_exec(const char *path, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    if (!path || !*path) return -1;

    int win = hl_host_is_windows();

    /* Already absolute? Emit as-is (with display separators normalized on
     * Windows). Absolute paths need no "current directory" prefix. */
    int absolute = (path[0] == '/') || (path[0] == '\\') ||
                   (win && path[0] && path[1] == ':');

    /* A path that already carries an explicit relative prefix keeps it; we
     * only normalize separators. Otherwise we add one, because NEITHER
     * PowerShell NOR a POSIX shell searches the current directory. */
    int has_prefix = (strncmp(path, "./", 2) == 0) ||
                     (strncmp(path, ".\\", 2) == 0) ||
                     (strncmp(path, "../", 3) == 0) ||
                     (strncmp(path, "..\\", 3) == 0);

    const char *prefix = "";
    if (!absolute && !has_prefix) prefix = win ? ".\\" : "./";

    /* A path carrying a space (or any other shell-significant byte) is not a
     * runnable instruction unquoted, so quote the whole rendering:
     *
     *   POSIX        '/home/jane doe/myapp/app'
     *   PowerShell   & 'C:\Users\Jane Doe\myapp\app.com'
     *
     * PowerShell needs the call operator to execute a quoted string, and
     * escapes an embedded single quote by doubling it; sh closes the quote,
     * emits an escaped one, and reopens. */
    int quote = host_exec_needs_quoting(path) || host_exec_needs_quoting(prefix);

    size_t n = 0;
#define HOST_PUT(ch) do {                                       \
        if (n + 1 >= out_sz) { out[0] = '\0'; return -1; }       \
        out[n++] = (char)(ch);                                  \
    } while (0)

    if (quote) {
        if (win) { HOST_PUT('&'); HOST_PUT(' '); }
        HOST_PUT('\'');
    }
    for (const char *p = prefix; *p; p++) HOST_PUT(*p);
    for (const char *p = path; *p; p++) {
        char c = *p;
        if (win && c == '/') c = '\\';
        if (quote && c == '\'') {
            if (win) {
                HOST_PUT('\'');        /* '' inside a PowerShell literal */
            } else {
                HOST_PUT('\'');        /* close  */
                HOST_PUT('\\');        /* escape */
                HOST_PUT('\'');        /* reopen */
            }
        }
        HOST_PUT(c);
    }
    if (quote) HOST_PUT('\'');
#undef HOST_PUT

    out[n] = '\0';
    return 0;
}

/* ── PATH search ────────────────────────────────────────────────────
 *
 * The previous (doctor-local) implementation split on ':' and joined with
 * '/', which cannot work on Windows: PATH is ';'-separated and components
 * are `C:\...`, so ':' splitting shreds the drive letters and every probe
 * reports "not found" regardless of what is installed.
 */
/* Join one PATH component - passed as a (pointer, length) slice INTO the
 * caller's PATH string, so the component is never copied out - with `name` +
 * `ext`, and test it for executability.
 *
 * Composing in a SINGLE buffer keeps the frame at one PATH_MAX. This runs on
 * the tool VM's stack alongside the build machinery, so three chained
 * PATH_MAX buffers (component + leaf + candidate) was worth collapsing. An
 * over-long join is rejected rather than truncated - a truncated path could
 * name a different, existing file. */
static int try_candidate(const char *dir, size_t dlen, char sep,
                         const char *name, const char *ext,
                         char *out, size_t out_sz)
{
    char candidate[PATH_MAX];
    /* Bound dlen BEFORE narrowing it for "%.*s". A precision argument is an
     * int, and a negative one means "precision omitted" - so a component
     * longer than INT_MAX would silently print past the intended slice, to
     * the NUL. Any component that cannot fit the buffer anyway is rejected
     * here, which makes the cast provably in range. */
    if (dlen >= sizeof(candidate)) return 0;

    int n = snprintf(candidate, sizeof(candidate), "%.*s%c%s%s",
                     (int)dlen, dir, sep, name, ext);
    if (n <= 0 || (size_t)n >= sizeof(candidate)) return 0;
    if (access(candidate, X_OK) != 0) return 0;

    n = snprintf(out, out_sz, "%s", candidate);
    if (n <= 0 || (size_t)n >= out_sz) {
        /* The caller's buffer is too small for the resolved path. snprintf has
         * ALREADY written a truncated string, so clearing it is not optional:
         * this function's contract is that a 0 return leaves `out` empty, and
         * a truncated path is worse than none - it names a different file. */
        out[0] = '\0';
        return 0;
    }
    return 1;
}

int hl_host_find_in_path(const char *name, char *out, size_t out_sz)
{
    return hl_host_find_in_path_ex(getenv("PATH"), name, out, out_sz);
}

int hl_host_find_in_path_ex(const char *path_env, const char *name,
                            char *out, size_t out_sz)
{
    if (!name || !*name || !out || out_sz == 0) return 0;
    out[0] = '\0';

    if (!path_env || !*path_env) return 0;

    int  win      = hl_host_is_windows();
    char list_sep = win ? ';' : ':';
    /* Windows accepts either separator in a path; '\' is conventional. */
    char dir_sep  = win ? '\\' : '/';

    /* The PATHEXT forms worth probing for a toolchain binary. An entry with
     * an explicit extension already (e.g. "busybox.exe") still tries the bare
     * form first, so an exact name always wins. */
    static const char *win_ext[] = { "", ".exe", ".com", ".bat", ".cmd", NULL };
    static const char *posix_ext[] = { "", NULL };
    const char **exts = win ? win_ext : posix_ext;

    /* Scan PATH IN PLACE, copying only one component at a time.
     *
     * The obvious implementation - copy the whole PATH into a fixed stack
     * buffer and tokenize destructively - silently loses entries: a Windows
     * user PATH is a registry value that may legitimately run to tens of KB,
     * so any fixed cap can drop the tail and make a genuinely installed
     * toolchain report as "not found" (the exact failure mode this function
     * exists to fix). Scanning in place has no cap, and drops ~8 KB of stack.
     *
     * try_candidate composes straight from the (pointer, length) slice, so no
     * per-component copy happens either. */
    for (const char *dir = path_env; dir; ) {
        const char *sep = strchr(dir, list_sep);
        size_t dlen = sep ? (size_t)(sep - dir) : strlen(dir);

        /* Windows PATH components are frequently quoted; strip a pair. */
        const char *d = dir;
        if (dlen >= 2 && d[0] == '"' && d[dlen - 1] == '"') {
            d++;
            dlen -= 2;
        }

        /* Skip empty components (a leading, doubled or trailing separator -
         * very common on Windows). An empty component must NOT be probed:
         * joining it would yield a current-directory-relative candidate, so
         * a file in the process's cwd could shadow a real toolchain binary. */
        if (dlen > 0) {
            for (const char **e = exts; *e; e++)
                if (try_candidate(d, dlen, dir_sep, name, *e, out, out_sz))
                    return 1;
        }

        dir = sep ? sep + 1 : NULL;
    }
    return 0;
}

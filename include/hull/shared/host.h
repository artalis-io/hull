/*
 * shared/host.h - host-OS facts and user-facing command rendering.
 *
 * Hull ships one binary that runs on POSIX hosts AND, as a Cosmopolitan APE,
 * on Windows. A handful of user-visible things differ per host and were
 * previously open-coded (or simply assumed POSIX) in doctor, build, and the
 * tool VM:
 *
 *   - the PATH list separator (':' vs ';') and the directory separator,
 *   - the executable suffix a produced artifact needs to be launchable,
 *   - how you spell "run the thing in this directory" in the host's shell.
 *
 * This is the ONE place those live. Everything user-facing (doctor hints,
 * `hull build`'s completion line, scaffolding next-steps) renders through
 * hl_host_render_exec() rather than hard-coding "./app".
 *
 * Detection: a native build knows its host at compile time. A cosmo APE does
 * not - the same bytes run on Linux, macOS, BSD, and Windows - so it probes
 * the environment for Windows' always-present variables. That idiom is
 * already load-bearing in cap/tool.c (hl_tool_cosmo_shell), and it is
 * centralized here so there is exactly one definition of "are we on Windows".
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_SHARED_HOST_H
#define HL_SHARED_HOST_H

#include <stddef.h>

/**
 * @brief Is this process running on Windows?
 *
 * 1 on Windows, 0 elsewhere. Constant-folded on native builds; probed from
 * the environment on a cosmo APE (which is the only build that reaches
 * Windows). The result is computed once and cached.
 */
int hl_host_is_windows(void);

/**
 * @brief Short host-OS label: "windows" | "darwin" | "linux" | "posix".
 *
 * For diagnostics and for the tool VM (`tool.host_os()`), which needs to
 * branch on the host rather than on the BUILD platform (`tool.platform_name()`
 * returns "cosmo" for every cosmo build regardless of where it runs).
 */
const char *hl_host_os(void);

/** @brief PATH list separator: ';' on Windows, ':' elsewhere. */
char hl_host_path_list_sep(void);

/** @brief Directory separator for DISPLAY: '\' on Windows, '/' elsewhere. */
char hl_host_dir_sep(void);

/**
 * @brief Suffix a produced executable needs to be launchable on this host.
 *
 * ".com" on Windows, "" elsewhere. Hull's build output is a Cosmopolitan
 * APE; Windows will not execute an extensionless file, and ".com" is the
 * APE convention (it is also how the installer names hull itself). Never
 * returns NULL.
 */
const char *hl_host_exe_suffix(void);

/**
 * @brief Render "run this executable" the way the host's shell needs it.
 *
 * A bare relative path is not runnable in either PowerShell or a POSIX shell
 * (neither searches the current directory), so both need an explicit prefix:
 *
 *   POSIX     ./app
 *   Windows   .\app.com
 *
 * @p path may be absolute (returned as-is, separators normalized for display),
 * relative with a directory part, or a bare name. Windows rendering converts
 * '/' to '\'. This function does NOT append @ref hl_host_exe_suffix - the
 * caller passes the real artifact path, which already carries it.
 *
 * A path carrying a space (or any other shell-significant byte) is QUOTED, so
 * the result stays ONE runnable command rather than several words:
 *
 *   POSIX     '/home/jane doe/myapp/app'
 *   Windows   & 'C:\Users\Jane Doe\myapp\app.com'
 *
 * PowerShell needs the call operator to execute a quoted string; an embedded
 * quote is escaped the way the host's shell expects. The result is a string
 * to PRINT, never a path to exec - callers that need the raw path have it.
 *
 * @returns 0 on success, -1 on a NULL/oversized argument (in which case @p out
 *          is set to an empty string when it is non-NULL and non-zero-sized).
 */
int hl_host_render_exec(const char *path, char *out, size_t out_sz);

/**
 * @brief Buffer size a caller needs for @ref hl_host_normalize_path.
 *
 * The rewrite grows a path by exactly one byte, so this is one over the
 * conventional 4096 POSIX limit.
 */
#define HL_HOST_PATH_MAX 4097

/**
 * @brief Rewrite a drive-letter path into the rooted form POSIX code needs.
 *
 * An MSYS2 / Git Bash shell rewrites a POSIX argument it passes to a NATIVE
 * program into the MIXED form "D:/a/app/data.db" - drive letter, colon,
 * forward slashes. A Cosmopolitan APE is a native program, so this is what
 * hull is handed. The OS layer coped: open(), stat() and mkdir() all accept
 * the mixed form (measured, cosmocc 4.0.2 on Windows 11).
 *
 * Vendored POSIX code does not. SQLite's unix VFS decides absolute-vs-relative
 * with a leading '/' (sqlite3.c: a zPath[0] != slash test), so it treated
 * "D:/a/app/data.db" as RELATIVE, prepended the cwd, and failed to open the
 * nonexistent result - surfacing as "cannot open database D:/a/app/data.db",
 * on a path that plainly exists.
 *
 * So the rewrite is to the one absolute form BOTH layers accept, which is
 * also what cosmo's own getcwd() returns:
 *
 *   D:/a/app/data.db   ->  /D/a/app/data.db
 *   D:\a\app\data.db   ->  /D/a/app/data.db
 *
 * Only a SINGLE letter followed by ':' and a separator is treated as a drive.
 * That cannot collide with a URI scheme (no real scheme is one character), so
 * a "postgres://" DSN passes through untouched, and it cannot collide with a
 * POSIX path, which never has a ':' at index 1. Backslashes are folded to '/'
 * only in the drive-letter case, so a POSIX filename that legitimately
 * contains a backslash is left alone.
 *
 * On a non-Windows host this is a verbatim copy: the mixed form is not a path
 * there, and rewriting it would corrupt a legal (if odd) relative filename.
 *
 * @returns 1 if @p out was rewritten, 0 if copied verbatim, -1 on a NULL or
 *          oversized argument (@p out is emptied when it can be).
 */
int hl_host_normalize_path(const char *path, char *out, size_t out_sz);

/**
 * @brief Find @p name as an executable on PATH, honouring host conventions.
 *
 * Splits PATH on the separator the LIST ITSELF uses, not the one the host
 * nominally prefers: on Windows both a Win32 `C:\a;C:\b` and a POSIX-shaped
 * `/C/a:/C/b` occur, and the second is what a Cosmopolitan APE - the only Hull
 * build that runs there - is actually handed. Each hit is composed with the
 * separator its own PATH component uses, so a POSIX-shaped entry stays
 * POSIX-shaped. On Windows the ".exe" / ".com" / ".bat" / ".cmd" forms are
 * tried too (the PATHEXT entries that matter for a toolchain probe). Writes
 * the resolved absolute-ish path to @p out.
 *
 * @returns 1 when found, 0 otherwise.
 */
int hl_host_find_in_path(const char *name, char *out, size_t out_sz);

/**
 * @brief hl_host_find_in_path over an EXPLICIT search list.
 *
 * Same splitting and extension rules, but @p path_list is supplied by the
 * caller instead of read from the environment. Exists so the splitter can be
 * unit-tested without mutating the process environment (and so a caller with
 * its own search list does not have to fake one through setenv).
 *
 * @returns 1 when found, 0 otherwise.
 */
int hl_host_find_in_path_ex(const char *path_list, const char *name,
                            char *out, size_t out_sz);

#endif /* HL_SHARED_HOST_H */

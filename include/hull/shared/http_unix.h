/*
 * http_unix.h - the AF_UNIX sockets an app may dial with an http+unix:// URL
 * (manifest http.unix_sockets, JS http.unixSockets; #1228).
 *
 * An entry is one of:
 *   "/abs/path.sock"     an absolute path, used as given;
 *   "@state/<rel>"       under the app's state directory (shared/state_dir.h);
 *   "<rel>"              under the app directory.
 * <rel> is one or more non-empty components with no "." or "..", no
 * backslash, and a first component that is not ".hull" (any case). The app
 * names the socket in the URL's authority by the entry's own spelling,
 * percent-encoded (http+unix://%40state%2Finfer.sock/v1/infer); the cap layer
 * rewrites the URL to the resolved absolute path before Keel dials it
 * (hl_http_unix_rewrite), so the app never needs the state directory's path.
 *
 * At each dial, the resolved path is checked again on disk
 * (hl_http_unix_path_ok): below its root (the app or state directory) no
 * component may be a symbolic link; an absolute entry's leaf may not be one;
 * and a leaf that exists must be a socket. A link made between that check and
 * the connect is not seen: the kernel sandbox's path grant (Seatbelt) is the
 * second line on macOS; on Linux, Landlock cannot restrict connect(2) by
 * path, so the capability layer is the boundary there.
 *
 * Base-resident (rides CAP_OBJS): the manifest's load check uses it, and the
 * HTTP feature archive's dial paths.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_HTTP_UNIX_H
#define HL_SHARED_HTTP_UNIX_H

#include <stddef.h>

/* sun_path is 104 bytes on macOS and the BSDs, 108 on Linux: the smaller one,
 * NUL included, so a path that resolves fits everywhere. */
#define HL_HTTP_UNIX_PATH_MAX 104
#define HL_HTTP_UNIX_MAX      16

typedef struct HlHttpUnixSet {
    int         count;
    /* The entry as the manifest spells it (points into the sealed manifest). */
    const char *declared[HL_HTTP_UNIX_MAX];
    /* The absolute path it resolves to, NUL-terminated. */
    char        resolved[HL_HTTP_UNIX_MAX][HL_HTTP_UNIX_PATH_MAX];
    /* Length of the root prefix of resolved[i] below which no component may
     * be a symbolic link (the app or state directory, realpath'd); 0 for an
     * absolute entry, whose leaf alone is checked. */
    unsigned short root_len[HL_HTTP_UNIX_MAX];
} HlHttpUnixSet;

/* 1 when @p entry is a well-formed http.unix_sockets entry; else 0 with a
 * short reason in @p why (may be NULL). Shape only: nothing on disk. */
int hl_http_unix_entry_valid(const char *entry, char *why, size_t why_size);

/* Resolve @p entries into @p out: absolute entries as given, "@state/<rel>"
 * under realpath(@p state_dir), "<rel>" under realpath(@p app_dir). -1 with
 * a message in @p err when an entry is malformed, names "@state/" without a
 * state directory, or resolves past HL_HTTP_UNIX_PATH_MAX; 0 otherwise. */
int hl_http_unix_resolve(HlHttpUnixSet *out, const char *const *entries, int count,
                         const char *app_dir, const char *state_dir,
                         char *err, size_t err_size);

/* The index of the entry an http+unix:// URL's decoded authority names - its
 * declared spelling, or (a redirect Keel built from the rewritten URL) its
 * resolved path - or -1 when none does. */
int hl_http_unix_find(const HlHttpUnixSet *set, const char *unix_path);

/* 1 when entry @p i may be dialled now: no symbolic link below its root (or,
 * for an absolute entry, at its leaf), and its leaf, if it exists, a socket. */
int hl_http_unix_path_ok(const HlHttpUnixSet *set, int i);

/* @p url with its authority replaced by entry @p i's resolved path,
 * percent-encoded, into a malloc'd string the caller frees; NULL when
 * @p url has no "://" or allocation fails. */
char *hl_http_unix_rewrite(const HlHttpUnixSet *set, int i, const char *url);

#endif /* HL_SHARED_HTTP_UNIX_H */

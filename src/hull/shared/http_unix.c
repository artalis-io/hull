/*
 * http_unix.c - the AF_UNIX sockets an app may dial with an http+unix:// URL
 * (#1228). See hull/shared/http_unix.h.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/http_unix.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define STATE_PREFIX "@state/"

static void set_why(char *why, size_t n, const char *msg)
{
    if (why && n) snprintf(why, n, "%s", msg);
}

/* The relative part of an entry: one or more components separated by single
 * '/', none empty, ".", ".." or holding a backslash; the first not ".hull"
 * in any case (the app's reserved directory, cap/fs_policy.c). */
static int rel_valid(const char *rel, char *why, size_t n)
{
    if (!*rel) { set_why(why, n, "an empty path"); return 0; }
    if (rel[0] == '/') { set_why(why, n, "a doubled '/'"); return 0; }
    const char *p = rel;
    int first = 1;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0) { set_why(why, n, "an empty path component"); return 0; }
        if ((len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) {
            set_why(why, n, "a '.' or '..' component");
            return 0;
        }
        if (memchr(p, '\\', len)) { set_why(why, n, "a backslash"); return 0; }
        if (first && len == 5 && strncasecmp(p, ".hull", 5) == 0) {
            set_why(why, n, "the reserved .hull directory");
            return 0;
        }
        first = 0;
        if (!e) break;
        p = e + 1;
        if (!*p) { set_why(why, n, "a trailing '/'"); return 0; }
    }
    return 1;
}

int hl_http_unix_entry_valid(const char *entry, char *why, size_t why_size)
{
    if (!entry || !*entry) { set_why(why, why_size, "an empty entry"); return 0; }
    if (strlen(entry) >= HL_HTTP_UNIX_PATH_MAX) {
        set_why(why, why_size, "longer than a socket path can be");
        return 0;
    }
    for (const unsigned char *c = (const unsigned char *)entry; *c; c++)
        if (*c < 0x20 || *c == 0x7f) {
            set_why(why, why_size, "a control character");
            return 0;
        }
    if (entry[0] == '/')
        return rel_valid(entry + 1, why, why_size);
    if (entry[0] == '@') {
        if (strncmp(entry, STATE_PREFIX, sizeof STATE_PREFIX - 1) != 0) {
            set_why(why, why_size, "an '@' root other than @state/");
            return 0;
        }
        return rel_valid(entry + sizeof STATE_PREFIX - 1, why, why_size);
    }
    return rel_valid(entry, why, why_size);
}

int hl_http_unix_resolve(HlHttpUnixSet *out, const char *const *entries, int count,
                         const char *app_dir, const char *state_dir,
                         char *err, size_t err_size)
{
    memset(out, 0, sizeof *out);
    if (count < 0 || count > HL_HTTP_UNIX_MAX) {
        snprintf(err, err_size, "http.unix_sockets: more than %d entries",
                 HL_HTTP_UNIX_MAX);
        return -1;
    }
    char app_real[PATH_MAX], state_real[PATH_MAX];
    int have_app = app_dir && realpath(app_dir, app_real) != NULL;
    int have_state = state_dir && state_dir[0] && realpath(state_dir, state_real) != NULL;
    for (int i = 0; i < count; i++) {
        const char *e = entries[i];
        char why[64];
        if (!hl_http_unix_entry_valid(e, why, sizeof why)) {
            snprintf(err, err_size, "http.unix_sockets: \"%s\" holds %s", e ? e : "", why);
            return -1;
        }
        const char *root = NULL, *rel = e;
        if (e[0] == '@') {
            if (!have_state) {
                snprintf(err, err_size, "http.unix_sockets: \"%s\" needs a state "
                         "directory (manifest state = { name = ... })", e);
                return -1;
            }
            root = state_real;
            rel = e + sizeof STATE_PREFIX - 1;
        } else if (e[0] != '/') {
            if (!have_app) {
                snprintf(err, err_size, "http.unix_sockets: \"%s\": the app "
                         "directory cannot be resolved", e);
                return -1;
            }
            root = app_real;
        }
        int n = root ? snprintf(out->resolved[i], HL_HTTP_UNIX_PATH_MAX, "%s/%s", root, rel)
                     : snprintf(out->resolved[i], HL_HTTP_UNIX_PATH_MAX, "%s", e);
        if (n < 0 || n >= HL_HTTP_UNIX_PATH_MAX) {
            snprintf(err, err_size, "http.unix_sockets: \"%s\" resolves to a path "
                     "longer than a socket path can be (%d bytes)", e,
                     HL_HTTP_UNIX_PATH_MAX - 1);
            return -1;
        }
        out->declared[i] = e;
        out->root_len[i] = root ? (unsigned short)strlen(root) : 0;
    }
    out->count = count;
    return 0;
}

int hl_http_unix_find(const HlHttpUnixSet *set, const char *unix_path)
{
    if (!set || !unix_path) return -1;
    for (int i = 0; i < set->count; i++)
        if ((set->declared[i] && strcmp(set->declared[i], unix_path) == 0) ||
            strcmp(set->resolved[i], unix_path) == 0)
            return i;
    return -1;
}

int hl_http_unix_path_ok(const HlHttpUnixSet *set, int i)
{
    if (!set || i < 0 || i >= set->count) return 0;
    const char *path = set->resolved[i];
    char buf[HL_HTTP_UNIX_PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    struct stat st;
    /* Below the root: every component, the leaf last. An absolute entry: the
     * leaf only. A component that does not exist ends the walk (the connect
     * then fails with ENOENT). */
    size_t start = set->root_len[i] ? set->root_len[i] : strlen(buf);
    for (size_t k = start + 1; k <= strlen(path); k++) {
        if (path[k] != '/' && path[k] != '\0') continue;
        buf[k] = '\0';
        if (lstat(buf, &st) != 0) return errno == ENOENT ? 1 : 0;
        if (S_ISLNK(st.st_mode)) return 0;
        if (path[k] == '\0') return S_ISSOCK(st.st_mode);
        if (!S_ISDIR(st.st_mode)) return 0;
        buf[k] = '/';
    }
    if (set->root_len[i] == 0) {
        if (lstat(path, &st) != 0) return errno == ENOENT ? 1 : 0;
        return !S_ISLNK(st.st_mode) && S_ISSOCK(st.st_mode);
    }
    return 0;
}

static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

char *hl_http_unix_rewrite(const HlHttpUnixSet *set, int i, const char *url)
{
    if (!set || i < 0 || i >= set->count || !url) return NULL;
    const char *sep = strstr(url, "://");
    if (!sep) return NULL;
    const char *auth = sep + 3;
    size_t alen = strcspn(auth, "/?#");
    const char *rest = auth + alen;
    const char *path = set->resolved[i];
    size_t plen = strlen(path);
    size_t scheme_len = (size_t)(auth - url);
    size_t rest_len = strlen(rest);
    if (rest_len > SIZE_MAX / 2) return NULL;
    char *out = malloc(scheme_len + plen * 3 + rest_len + 1);
    if (!out) return NULL;
    memcpy(out, url, scheme_len);
    char *w = out + scheme_len;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t k = 0; k < plen; k++) {
        unsigned char c = (unsigned char)path[k];
        if (unreserved(c)) {
            *w++ = (char)c;
        } else {
            *w++ = '%';
            *w++ = hex[c >> 4];
            *w++ = hex[c & 15];
        }
    }
    memcpy(w, rest, rest_len + 1);
    return out;
}

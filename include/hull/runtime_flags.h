/*
 * runtime_flags.h - Hull's runtime options in a BUILT binary.
 *
 * A built binary's argv is shared with its app: `./tool --no-sandbox x`
 * cannot tell an operator's option from an argument a wrapper forwarded
 * (a file name that happens to read "--no-sandbox"). So in a built binary
 * the options that WEAKEN the process - sandbox off, TLS verification off
 * or re-anchored, the platform check off, the agent API on, a resource
 * limit changed (instructions, -m/-M/-s, WASM gas/heap/stack/IO, body
 * size), the server exposed (-b, --tls-cert/--tls-key), a database path
 * granted to the sandbox (-d) - are taken only in their reserved spelling
 * `--hull-<name>` (`--hull-no-sandbox`, `--hull-ca-bundle PATH`,
 * `--hull-d PATH`). A bare one is refused, not passed to the app and not
 * honoured. Every option may be spelled `--hull-<name>`, under hull as well
 * (`--hull-d PATH` is `-d PATH`). A `--hull-<name>` the parser does not
 * take is an error, never an app argument.
 *
 * Shared by serve.c and serve_cli.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_RUNTIME_FLAGS_H
#define HL_RUNTIME_FLAGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Rewrite argv[i] "--hull-<name>" to the plain option ("-<c>" for a one-
 * letter name, else "--<name>", keeping any "=value"). 1 when it was
 * prefixed (and rewritten), 0 when it was not, -1 (reported) when it cannot
 * be taken: "--hull-x=value" for a one-letter name (no "-x=value" form
 * exists; spell it "--hull-x value"), or out of memory. The rewrite is a
 * heap copy that argv keeps pointing at for the life of the process.
 *
 * It used to go into a fixed 32 x 96-byte pool and, when a rewrite did not
 * fit (a --hull-ca-bundle= path of 84+ characters) or the pool was full,
 * return 0 and leave "--hull-..." in place - which a built binary then took
 * as the first of the app's arguments, silently dropping the option and
 * every Hull option after it (a --hull-verify-sig among them). */
static inline int hl_runtime_flag_unprefix(char **argp)
{
    const char *a = *argp;
    if (strncmp(a, "--hull-", 7) != 0 || a[7] == '\0') return 0;
    const char *rest = a + 7;
    size_t nlen = strcspn(rest, "=");
    if (nlen == 0) {
        fprintf(stderr, "hull: malformed option %s\n", a);
        return -1;
    }
    if (nlen == 1 && rest[1] == '=') {
        fprintf(stderr, "hull: --hull-%c takes its value as the next argument "
                "(--hull-%c VALUE), not --hull-%c=VALUE\n", rest[0], rest[0], rest[0]);
        return -1;
    }
    /* Kept reachable from here for the life of the process (argv points at
     * them, and a leak checker should not see them as lost). */
    static char **kept;
    static size_t nkept, capkept;
    if (nkept == capkept) {
        size_t nc = capkept ? capkept * 2 : 16;
        char **t = (nc < ((size_t)-1) / sizeof(*t))
            ? realloc(kept, nc * sizeof(*t)) : NULL;
        if (!t) {
            fprintf(stderr, "hull: out of memory\n");
            return -1;
        }
        kept = t;
        capkept = nc;
    }
    size_t rlen = strlen(rest);
    size_t need = rlen + (nlen == 1 ? 2 : 3);   /* "-" or "--", plus NUL */
    char *out = malloc(need);
    if (!out) {
        fprintf(stderr, "hull: out of memory\n");
        return -1;
    }
    if (nlen == 1) { out[0] = '-'; memcpy(out + 1, rest, rlen + 1); }
    else { out[0] = '-'; out[1] = '-'; memcpy(out + 2, rest, rlen + 1); }
    kept[nkept++] = out;
    *argp = out;
    return 1;
}

/* An option that weakens the process (see the header comment): turns a
 * kernel or TLS protection off, re-anchors trust, exposes the server (-b,
 * TLS key/cert), grants the sandbox a path (-d), or raises a resource
 * limit (-m/-M/-s, instructions, WASM gas/heap/stack/IO, body size). */
static inline int hl_runtime_flag_is_downgrade(const char *a)
{
    static const char *const names[] = {
        "--no-sandbox", "--allow-degraded-sandbox", "--no-ca-bundle",
        "--skip-ca-bundle", "--ca-bundle", "--no-verify-platform",
        "--agent-api", "--max-instructions",
        "-b", "-d", "-m", "-M", "-s",
        "--tls-cert", "--tls-key",
        "--wasm-gas", "--wasm-heap", "--wasm-stack",
        "--wasm-max-input", "--wasm-max-output", "--body-max-size",
        NULL
    };
    for (int i = 0; names[i]; i++) {
        size_t l = strlen(names[i]);
        if (strncmp(a, names[i], l) == 0 && (a[l] == '\0' || a[l] == '='))
            return 1;
    }
    return 0;
}

/* A --hull-<name> option the parser did not take (unknown, or missing its
 * value). Always an error: it is never the app's - a built binary used to
 * start the app's arguments there, so the option and everything after it
 * were dropped without a word. Returns -1 after reporting. */
static inline int hl_runtime_flag_unknown(const char *plain)
{
    const char *n = plain;
    while (*n == '-') n++;
    fprintf(stderr, "hull: unknown option --hull-%s, or it is missing its value\n", n);
    return -1;
}

/* In a built binary: refuse a bare downgrade option. 0 to go on, -1 after
 * reporting it. */
static inline int hl_runtime_flag_check(const char *a, int prefixed, int built)
{
    if (!built || prefixed || !hl_runtime_flag_is_downgrade(a)) return 0;
    const char *n = a;
    while (*n == '-') n++;
    size_t dashes = (size_t)(n - a);
    size_t nlen = strcspn(n, "=");
    fprintf(stderr,
        "hull: in a built binary this option is spelled --hull-%.*s "
        "(a bare %.*s could be one of the app's own arguments)\n",
        (int)nlen, n, (int)(nlen + dashes), a);
    return -1;
}

#endif /* HL_RUNTIME_FLAGS_H */

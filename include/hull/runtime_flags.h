/*
 * runtime_flags.h - Hull's runtime options in a BUILT binary.
 *
 * A built binary's argv is shared with its app: `./tool --no-sandbox x`
 * cannot tell an operator's option from an argument a wrapper forwarded
 * (a file name that happens to read "--no-sandbox"). So in a built binary
 * the options that WEAKEN the process - sandbox off, TLS verification off
 * or re-anchored, the platform check off, the agent API or sidecars on, a
 * resource limit changed (instructions, connections, read / drain timeout,
 * workers, queue, -m/-M/-s, WASM gas/heap/stack/timeout/IO, body size), the
 * server exposed (-b, --tls-cert/--tls-key), a database path granted to the
 * sandbox (-d) - are taken only in their
 * reserved spelling `--hull-<name>` (`--hull-no-sandbox`, `--hull-ca-bundle
 * PATH`, `--hull-d PATH`). A bare one is refused, not passed to the app and
 * not honoured. Every option may be spelled `--hull-<name>`, under hull as
 * well (`--hull-d PATH` is `-d PATH`). A `--hull-<name>` the parser does not
 * take is an error, never an app argument.
 *
 * The rule applies only to the options the RUNNER implements. The app.main
 * runner (serve_cli.c, every built app without HTTP) has no -s, -m, -b, TLS
 * or server options, so refusing those bare protected nothing and only took
 * them from the app (`./tool -s pattern`): there they are the app's own
 * arguments, like any option Hull does not take. Which runner takes which
 * option, and which options are downgrades, is ONE table
 * (hl_runtime_flags()); both the parsers and the reservation read it.
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

/* Which runner parses an option: serve.c (the Keel server, every built app
 * with HTTP and `hull app.lua` under hull) and serve_cli.c (the app.main
 * runner, every built app without HTTP). */
#define HL_RF_SERVE     0x1u
#define HL_RF_CLI       0x2u
/* The option weakens the process (see the header comment): turns a kernel or
 * TLS protection off, re-anchors trust, exposes the server (-b, TLS key/cert,
 * the agent API), writes into the app directory (--agent sidecars), grants
 * the sandbox a path (-d), or raises a resource limit (-m/-M/-s,
 * instructions, connections, timeouts, workers, queue, WASM gas/heap/stack/
 * timeout/IO, body size). */
#define HL_RF_DOWNGRADE 0x4u

/* The CLI runner parses the --wasm-* ceilings only when WASM is compiled in;
 * without it they are an app.main app's own arguments. */
#ifdef HL_ENABLE_WASM
#define HL_RF_CLI_WASM HL_RF_CLI
#else
#define HL_RF_CLI_WASM 0u
#endif

typedef struct HlRuntimeFlag {
    const char *name;
    unsigned    flags;   /* HL_RF_* */
} HlRuntimeFlag;

/* EVERY option either runner takes, and which of them weaken the process.
 * The single source of truth: each parser first asks hl_runtime_flag_takes()
 * and treats anything this table does not give it as an option Hull does not
 * take (the app's, in a built binary), so a branch for a name missing here
 * is never reached; and the built-binary reservation is derived from the
 * same rows. Adding an option to a parser therefore means adding its row,
 * and deciding there whether it is HL_RF_DOWNGRADE. Before this the runners
 * parsed one list and reserved another, and they drifted (round 7: the
 * app.main runner parsed --wasm-* without reserving them, the server
 * honoured a bare --read-timeout / --workers / --queue-capacity /
 * --drain-timeout in a built binary). */
static inline const HlRuntimeFlag *hl_runtime_flags(void)
{
    static const HlRuntimeFlag table[] = {
        { "-p",                       HL_RF_SERVE },
        { "-l",                       HL_RF_SERVE },
        { "-b",                       HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "-d",                       HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "-m",                       HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "-M",                       HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "-s",                       HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--tls-cert",               HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--tls-key",                HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--verify-sig",             HL_RF_SERVE | HL_RF_CLI },
        { "--no-migrate",             HL_RF_SERVE | HL_RF_CLI },
        { "--no-sandbox",             HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--allow-degraded-sandbox", HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--no-verify-platform",     HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--no-db",                  HL_RF_SERVE },
        { "--no-compress",            HL_RF_SERVE },
        { "--no-ca-bundle",           HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--skip-ca-bundle",         HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--ca-bundle",              HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--agent",                  HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--agent-api",              HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--audit",                  HL_RF_SERVE | HL_RF_CLI },
        { "--max-instructions",       HL_RF_SERVE | HL_RF_CLI | HL_RF_DOWNGRADE },
        { "--max-connections",        HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--body-max-size",          HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--read-timeout",           HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--workers",                HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--queue-capacity",         HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--drain-timeout",          HL_RF_SERVE | HL_RF_DOWNGRADE },
        { "--wasm-heap",              HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--wasm-stack",             HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--wasm-gas",               HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--wasm-timeout-ms",        HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--wasm-max-input",         HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--wasm-max-output",        HL_RF_SERVE | HL_RF_CLI_WASM | HL_RF_DOWNGRADE },
        { "--gpu-device",             HL_RF_SERVE },
        /* The dispatcher's global flags, which the runners skip under hull
         * only (a built binary has no dispatcher: there they are the app's). */
        { "-h",                       HL_RF_SERVE },
        { "--help",                   HL_RF_SERVE },
        { "--verbose",                HL_RF_SERVE | HL_RF_CLI },
        { "--json",                   HL_RF_SERVE | HL_RF_CLI },
        { "--app-dir",                HL_RF_SERVE | HL_RF_CLI },
        { NULL, 0 }
    };
    return table;
}

/* The row naming @p a ("--x" or "--x=value"), or NULL. */
static inline const HlRuntimeFlag *hl_runtime_flag_find(const char *a)
{
    for (const HlRuntimeFlag *f = hl_runtime_flags(); f->name; f++) {
        size_t l = strlen(f->name);
        if (strncmp(a, f->name, l) == 0 && (a[l] == '\0' || a[l] == '='))
            return f;
    }
    return NULL;
}

/* @p runner (HL_RF_SERVE / HL_RF_CLI) takes option @p a. A parser must not
 * act on an option this denies: it is one Hull does not take. */
static inline int hl_runtime_flag_takes(const char *a, unsigned runner)
{
    const HlRuntimeFlag *f = hl_runtime_flag_find(a);
    return f && (f->flags & runner);
}

/* @p a weakens the process in at least one runner. */
static inline int hl_runtime_flag_is_downgrade(const char *a)
{
    const HlRuntimeFlag *f = hl_runtime_flag_find(a);
    return f && (f->flags & HL_RF_DOWNGRADE);
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

/* In a built binary: refuse a bare downgrade option that @p runner takes. One
 * the runner does not take is left for the app. 0 to go on, -1 after
 * reporting it. */
static inline int hl_runtime_flag_check(const char *a, int prefixed, int built,
                                        unsigned runner)
{
    if (!built || prefixed) return 0;
    const HlRuntimeFlag *f = hl_runtime_flag_find(a);
    if (!f || !(f->flags & runner) || !(f->flags & HL_RF_DOWNGRADE)) return 0;
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

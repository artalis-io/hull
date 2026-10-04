/*
 * runtime_flags.h - Hull's runtime options in a BUILT binary.
 *
 * A built binary's argv is shared with its app: `./tool --no-sandbox x`
 * cannot tell an operator's option from an argument a wrapper forwarded
 * (a file name that happens to read "--no-sandbox"). So in a built binary
 * the options that WEAKEN the process - sandbox off, TLS verification off
 * or re-anchored, the platform check off, the agent API on, the instruction
 * limit changed - are taken only in their reserved spelling `--hull-<name>`
 * (`--hull-no-sandbox`, `--hull-ca-bundle PATH`). A bare one is refused,
 * not passed to the app and not honoured. Every option may be spelled
 * `--hull-<name>`, under hull as well (`--hull-d PATH` is `-d PATH`).
 *
 * Shared by serve.c and serve_cli.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_RUNTIME_FLAGS_H
#define HL_RUNTIME_FLAGS_H

#include <stdio.h>
#include <string.h>

/* Rewrite argv[i] "--hull-<name>" to the plain option ("-<c>" for a one-
 * letter name, else "--<name>", keeping any "=value"). 1 when it was
 * prefixed (and rewritten), 0 otherwise. The rewritten strings live in a
 * static pool: argv keeps pointing at them for the life of the process. */
static inline int hl_runtime_flag_unprefix(char **argp)
{
    static char pool[32][96];
    static int used;
    const char *a = *argp;
    if (strncmp(a, "--hull-", 7) != 0 || a[7] == '\0') return 0;
    if (used >= (int)(sizeof pool / sizeof pool[0])) return 0;
    const char *rest = a + 7;
    size_t nlen = strcspn(rest, "=");
    char *out = pool[used];
    int n = nlen == 1 ? snprintf(out, sizeof pool[0], "-%s", rest)
                      : snprintf(out, sizeof pool[0], "--%s", rest);
    if (n < 0 || (size_t)n >= sizeof pool[0]) return 0;
    used++;
    *argp = out;
    return 1;
}

/* An option that weakens the process (see the header comment). */
static inline int hl_runtime_flag_is_downgrade(const char *a)
{
    static const char *const names[] = {
        "--no-sandbox", "--allow-degraded-sandbox", "--no-ca-bundle",
        "--skip-ca-bundle", "--ca-bundle", "--no-verify-platform",
        "--agent-api", "--max-instructions", NULL
    };
    for (int i = 0; names[i]; i++) {
        size_t l = strlen(names[i]);
        if (strncmp(a, names[i], l) == 0 && (a[l] == '\0' || a[l] == '='))
            return 1;
    }
    return 0;
}

/* In a built binary: refuse a bare downgrade option. 0 to go on, -1 after
 * reporting it. */
static inline int hl_runtime_flag_check(const char *a, int prefixed, int built)
{
    if (!built || prefixed || !hl_runtime_flag_is_downgrade(a)) return 0;
    size_t nlen = strcspn(a + 2, "=");
    fprintf(stderr,
        "hull: in a built binary this option is spelled --hull-%.*s "
        "(a bare %.*s could be one of the app's own arguments)\n",
        (int)nlen, a + 2, (int)(nlen + 2), a);
    return -1;
}

#endif /* HL_RUNTIME_FLAGS_H */

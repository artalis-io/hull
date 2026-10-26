/*
 * fuzz_migrate_name.c: libFuzzer harness for hl_migrate_embedded_name
 * (audits 25 / 27).
 *
 * Every reader of embedded migrations (the runner, the agent's schema-diff)
 * takes the entries this predicate admits, and development reads only
 * top-level migrations/<name>.sql - so a nested "migrations/down/001.sql", a
 * backslash path or a non-.sql file must never run in a built binary.
 * Oracles: the result is NULL or exactly the name past "migrations/"; it is
 * non-NULL iff that name is at least 5 bytes, ends ".sql" and holds no '/'
 * or '\\'; the call reads nothing past the name's NUL (exact-size buffer).
 * The input is the name past the prefix, cut at its first NUL.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/migrate.h"
#include "hull/entry.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *what)
{
    fprintf(stderr, "fuzz_migrate_name: %s\n", what);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const char prefix[] = "migrations/";
    const size_t plen = sizeof prefix - 1;
    if (size > 4096) return 0;
    const uint8_t *z = memchr(data, '\0', size);
    size_t n = z ? (size_t)(z - data) : size;

    char *name = malloc(plen + n + 1);
    if (!name) return 0;
    memcpy(name, prefix, plen);
    memcpy(name + plen, data, n);
    name[plen + n] = '\0';

    const char *rest = name + plen;
    int want = n >= 5 && memcmp(rest + n - 4, ".sql", 4) == 0 &&
               !memchr(rest, '/', n) && !memchr(rest, '\\', n);

    HlEntry e = { name, (const unsigned char *)"", 0 };
    const char *got = hl_migrate_embedded_name(&e);
    if (want && got != rest) fail("a top-level .sql migration was skipped");
    if (!want && got) fail("a nested or non-.sql entry was admitted");
    free(name);
    return 0;
}

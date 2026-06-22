/*
 * fuzz_sh_json.c — libFuzzer harness for the sh_json parser + accessors.
 *
 * sh_json parses untrusted input (request bodies, config), so it is a
 * prime fuzz target. We parse the fuzz bytes, then recursively walk the
 * result via the public API + struct so traversal/accessor bugs are
 * exercised (not just the parser), and feed the raw bytes to the
 * path-expression mini-parser (sh_json_get_path) too.
 *
 * Build/run: make fuzz-sh-json   (clang -fsanitize=fuzzer,address,undefined)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "sh_json.h"
#include "sh_arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void walk(const ShJsonValue *v, int depth)
{
    if (!v || depth > 64)
        return;
    switch (sh_json_type(v)) {
    case SH_JSON_NULL:
        (void)sh_json_is_null(v);
        break;
    case SH_JSON_BOOL:
        (void)sh_json_as_bool(v, false);
        break;
    case SH_JSON_NUMBER:
        (void)sh_json_as_double(v, 0.0);
        (void)sh_json_as_int(v, 0);
        break;
    case SH_JSON_STRING: {
        const char *s = sh_json_as_string(v, "");
        if (s) (void)strlen(s);
        break;
    }
    case SH_JSON_ARRAY: {
        size_t n = sh_json_array_len(v);
        for (size_t i = 0; i < n; i++)
            walk(sh_json_array_get(v, i), depth + 1);
        break;
    }
    case SH_JSON_OBJECT: {
        size_t n = sh_json_object_len(v);
        for (size_t i = 0; i < n && i < v->u.object_val.count; i++) {
            const ShJsonMember *m = &v->u.object_val.members[i];
            if (m->key) (void)sh_json_get(v, m->key);   /* exercise lookup */
            walk(m->value, depth + 1);
        }
        break;
    }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    SHArena *arena = sh_arena_create(8 * 1024 * 1024); /* 8 MB */
    if (!arena)
        return 0;

    ShJsonValue *root = NULL;
    if (sh_json_parse((const char *)data, size, arena, &root) == SH_JSON_OK && root) {
        walk(root, 0);
        /* Fuzz the "a.b[2].c" path-expression parser with a NUL-terminated
         * copy of the same bytes (its own little parser). */
        char *p = sh_arena_alloc(arena, size + 1);
        if (p) {
            memcpy(p, data, size);
            p[size] = '\0';
            (void)sh_json_get_path(root, p);
        }
    }

    sh_arena_free(arena);
    return 0;
}

/*
 * libFuzzer harness for hl_path_normalize().
 *
 * hl_path_normalize collapses `.`/`..` segments in place and feeds module
 * require()/import() resolution — a security-relevant path: a bug that let
 * `..` escape the root, or an out-of-bounds write into the in-place buffer,
 * is a sandbox-traversal primitive. ASan catches OOB; we just feed it
 * arbitrary NUL-terminated bytes.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_path_normalize fuzz/corpus_path_normalize/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/path_normalize.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* hl_path_normalize mutates a NUL-terminated string in place. Give it
     * an exactly-sized heap buffer so ASan red-zones bracket any OOB. */
    char *buf = malloc(size + 1);
    if (!buf)
        return 0;
    memcpy(buf, data, size);
    buf[size] = '\0';

    int rc = hl_path_normalize(buf);

    /* Post-conditions that must always hold:
     *   - still NUL-terminated within the original capacity,
     *   - on success (rc==0) the result never contains a "/.." escape that
     *     the function is supposed to have collapsed or rejected. */
    size_t out_len = strlen(buf);   /* OOB read here if not NUL-terminated */
    if (rc == 0) {
        if (strcmp(buf, "..") == 0 || strncmp(buf, "../", 3) == 0)
            __builtin_trap(); /* escaped the root but returned success */
        (void)out_len;
    }

    free(buf);
    return 0;
}

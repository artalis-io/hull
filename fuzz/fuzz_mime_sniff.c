/*
 * libFuzzer harness for hl_cap_mime_sniff().
 *
 * The MIME sniffer runs on the first ~4 KiB of UNTRUSTED stored upload
 * bytes (multipart parts) and decides "truth-by-bytes" content type via
 * magic-byte + shape inspection. An over-read past `len` while matching a
 * magic prefix or scanning structure on attacker-controlled input would be
 * a crash / info-leak; ASan red-zones bracket the exact-sized buffer so any
 * such over-read traps. The returned type must be a valid C string.
 *
 * Build:
 *   make fuzz  (requires clang with -fsanitize=fuzzer support)
 *
 * Run:
 *   ./fuzz/fuzz_mime_sniff fuzz/corpus_mime_sniff/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/mime.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Exact-sized heap copy so ASan brackets any read past `len`. */
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf)
        return 0;
    if (size)
        memcpy(buf, data, size);

    const char *mime = hl_cap_mime_sniff(buf, size);
    if (mime)
        (void)strlen(mime); /* must be a NUL-terminated string */

    free(buf);
    return 0;
}

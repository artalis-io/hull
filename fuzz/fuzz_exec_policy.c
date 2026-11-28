/*
 * libFuzzer harness for hl_exec_policy_validate() and the readers behind it
 * (cap/exec.c, docs/design/exec_broker.md).
 *
 * The encoded `exec` policy is built by the app's manifest extractor and, in
 * the broker (PR 2), arrives over a socket from the app process - which the
 * broker treats as untrusted after READY. The validator must refuse anything
 * that is not a well-formed policy without reading past `len`, and every
 * reader (hl_exec_policy_at / _find / hl_exec_check_call) must stay inside a
 * policy it accepted. ASan brackets an exact-sized copy.
 *
 * Run:
 *   ./fuzz/fuzz_exec_policy fuzz/corpus_exec_policy/ -max_total_time=60
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/exec.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf) return 0;
    if (size) memcpy(buf, data, size);

    char err[256];
    if (hl_exec_policy_validate(buf, size, err, sizeof err) == 0) {
        static const char *const vals[] = { "hs-api.service", "a", "--x", "0" };
        static const size_t lens[] = { 14, 1, 3, 1 };
        int n = hl_exec_policy_count(buf, size);
        for (int i = 0; i < n; i++) {
            HlExecCmd c;
            if (hl_exec_policy_at(buf, size, i, &c) != 0) abort();
            (void)strlen(c.name);
            (void)strlen(c.path);
            /* exec.run's limits on a stream command are never past its
             * ceilings, and are exactly those derived from the stream's. */
            if (c.timeout_ms > HL_EXEC_TIMEOUT_MAX_MS || c.stdout_max > HL_EXEC_STDOUT_CEIL ||
                c.stderr_max > HL_EXEC_STDERR_CEIL || c.stdin_max > HL_EXEC_STDIN_CEIL)
                abort();
            if (c.flags & HL_EXEC_CMD_STREAM) {
                HlExecStreamBase bs = hl_exec_stream_base(c.stream_timeout_ms,
                                                          c.stream_stdout_max,
                                                          c.stream_stderr_max,
                                                          c.stream_stdin_max);
                if (bs.timeout_ms != c.timeout_ms || bs.stdout_max != c.stdout_max ||
                    bs.stderr_max != c.stderr_max || bs.stdin_max != c.stdin_max)
                    abort();
            }
            const uint8_t *cur = c.env;
            for (unsigned k = 0; k < (unsigned)c.n_env * 2; k++)
                (void)hl_exec_next_str(&cur, NULL);
            cur = c.fs_read;
            for (unsigned k = 0; k < c.n_fs_read; k++) (void)hl_exec_next_str(&cur, NULL);
            cur = c.fs_write;
            for (unsigned k = 0; k < c.n_fs_write; k++) (void)hl_exec_next_str(&cur, NULL);
            cur = c.app_read;
            for (unsigned k = 0; k < c.n_app_read; k++) (void)hl_exec_next_str(&cur, NULL);
            cur = c.app_write;
            for (unsigned k = 0; k < c.n_app_write; k++) (void)hl_exec_next_str(&cur, NULL);
            for (size_t na = 0; na <= 4; na++)
                (void)hl_exec_check_call(buf, size, c.name, strlen(c.name),
                                         vals, lens, na, NULL, err, sizeof err);
            HlExecCmd f;
            if (hl_exec_policy_find(buf, size, c.name, strlen(c.name), &f) != 0) abort();
        }
    }
    free(buf);
    return 0;
}

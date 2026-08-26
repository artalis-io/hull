/*
 * agent/logs.c - `hull agent logs`: read the last N lines from
 * app_dir/.hull/dev.log if it exists. Falls back to empty array.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hl_agent_logs(const char *app_dir, int tail_n, ShJsonBuf *out)
{
    if (!app_dir) app_dir = ".";
    if (tail_n <= 0) tail_n = HL_AGENT_LOGS_DEFAULT_TAIL;
    if (tail_n > HL_AGENT_LOGS_MAX_TAIL) tail_n = HL_AGENT_LOGS_MAX_TAIL;

    char path[HL_AGENT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.hull/dev.log", app_dir);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "path", path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        sh_json_write_kv_bool(&w, "exists", false);
        sh_json_write_key(&w, "lines");
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
        sh_json_write_object_end(&w);
        return 0;
    }

    /* Seek to end, read up to MAX_LOG_BYTES from the tail. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }
    long fsize = ftell(f);
    if (fsize < 0) fsize = 0;
    long start = fsize > HL_AGENT_LOG_TAIL_BYTES ? fsize - HL_AGENT_LOG_TAIL_BYTES : 0;
    if (fseek(f, start, SEEK_SET) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }

    size_t to_read = (size_t)(fsize - start);
    char *buf = malloc(to_read + 1);
    if (!buf) { fclose(f); return hl_agent_write_error(out, "out of memory"); }
    size_t got = fread(buf, 1, to_read, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err) { free(buf); return hl_agent_write_error(out, "read failed"); }
    buf[got] = '\0';

    /* Count line breaks; emit the last tail_n lines. */
    /* First pass: find each newline so we can step back tail_n lines. */
    long *nl_offsets = NULL;
    int nl_count = 0;
    int nl_cap = 0;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] == '\n') {
            if (nl_count >= nl_cap) {
                nl_cap = nl_cap ? nl_cap * 2 : HL_AGENT_NL_CAP_INITIAL;
                long *grow = realloc(nl_offsets, (size_t)nl_cap * sizeof(long));
                if (!grow) { free(nl_offsets); free(buf);
                             return hl_agent_write_error(out, "out of memory"); }
                nl_offsets = grow;
            }
            nl_offsets[nl_count++] = (long)i;
        }
    }

    sh_json_write_kv_bool(&w, "exists", true);
    sh_json_write_kv_int(&w, "total_lines_in_tail", nl_count);
    sh_json_write_kv_int(&w, "truncated", start > 0 ? 1 : 0);

    sh_json_write_key(&w, "lines");
    sh_json_write_array_start(&w);

    int first_line_idx = nl_count > tail_n ? nl_count - tail_n : 0;
    long line_start = first_line_idx == 0 ? 0 : nl_offsets[first_line_idx - 1] + 1;
    for (int i = first_line_idx; i < nl_count; i++) {
        long line_end = nl_offsets[i];
        size_t len = (size_t)(line_end - line_start);
        /* Trim trailing \r if present (Windows-style line endings). */
        if (len > 0 && buf[line_start + len - 1] == '\r') len--;
        sh_json_write_string_n(&w, buf + line_start, len);
        line_start = line_end + 1;
    }
    /* Trailing partial line (no terminating \n)? Include if non-empty. */
    if ((size_t)line_start < got) {
        size_t len = got - (size_t)line_start;
        if (len > 0)
            sh_json_write_string_n(&w, buf + line_start, len);
    }

    sh_json_write_array_end(&w);
    sh_json_write_object_end(&w);

    free(nl_offsets);
    free(buf);
    return 0;
}

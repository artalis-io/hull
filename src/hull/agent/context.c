/*
 * agent/context.c — `hull agent context` — task-relevant doc lookup.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "hull/entry.h"
#include "hull/vfs.h"

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int hl_agent_context(const char *task, const char *level, ShJsonBuf *out)
{
    if (!task || !level)
        return hl_agent_write_error(out, "task and level are required");

    /* Look up context doc from platform VFS */
    extern const HlEntry hl_stdlib_entries[];
    HlVfs platform_vfs;
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);

    char key[128];
    snprintf(key, sizeof(key), "context:%s", task);

    const HlEntry *entry = hl_vfs_find(&platform_vfs, key);
    if (!entry)
        return hl_agent_write_error(out, "unknown task");

    /* Find the content boundaries based on level markers */
    const char *content = (const char *)entry->data;
    size_t content_len = entry->len;

    /* Level markers in the document */
    const char *minimal_marker = "<!-- minimal -->";
    const char *compact_marker = "<!-- compact -->";
    const char *full_marker    = "<!-- full -->";

    /* Find the start of content (after the level marker for the requested level) */
    const char *start = content;
    size_t start_offset = 0;

    /* Find marker positions */
    const char *minimal_pos = NULL;
    const char *compact_pos = NULL;
    const char *full_pos = NULL;

    /* Scan for markers */
    for (size_t i = 0; i + 15 < content_len; i++) {
        if (memcmp(content + i, "<!-- ", 5) == 0) {
            if (i + strlen(minimal_marker) <= content_len &&
                memcmp(content + i, minimal_marker, strlen(minimal_marker)) == 0) {
                minimal_pos = content + i + strlen(minimal_marker);
            }
            if (i + strlen(compact_marker) <= content_len &&
                memcmp(content + i, compact_marker, strlen(compact_marker)) == 0) {
                compact_pos = content + i + strlen(compact_marker);
            }
            if (i + strlen(full_marker) <= content_len &&
                memcmp(content + i, full_marker, strlen(full_marker)) == 0) {
                full_pos = content + i + strlen(full_marker);
            }
        }
    }

    /* Determine content range based on level */
    const char *end_ptr = content + content_len;

    if (strcmp(level, "minimal") == 0) {
        if (minimal_pos) start = minimal_pos;
        if (compact_pos) end_ptr = compact_pos - strlen(compact_marker);
        else if (full_pos) end_ptr = full_pos - strlen(full_marker);
    } else if (strcmp(level, "compact") == 0) {
        if (minimal_pos) start = minimal_pos;
        if (full_pos) end_ptr = full_pos - strlen(full_marker);
    } else if (strcmp(level, "full") == 0) {
        if (minimal_pos) start = minimal_pos;
        /* end_ptr stays at end of content */
    } else {
        return hl_agent_write_error(out, "level must be minimal, compact, or full");
    }

    if (end_ptr < start) end_ptr = start;
    start_offset = (size_t)(start - content);
    (void)start_offset;

    /* Skip leading whitespace */
    while (start < end_ptr && (*start == '\n' || *start == '\r'))
        start++;

    /* Trim trailing whitespace */
    while (end_ptr > start && (end_ptr[-1] == '\n' || end_ptr[-1] == '\r' ||
                                end_ptr[-1] == ' '))
        end_ptr--;

    size_t result_len = (size_t)(end_ptr - start);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "task", task);
    sh_json_write_kv_string(&w, "level", level);
    sh_json_write_key(&w, "content");
    sh_json_write_string_n(&w, start, result_len);
    sh_json_write_object_end(&w);
    return 0;
}

/* ── Phase 4: hl_agent_migrate_status ──────────────────────────────── */

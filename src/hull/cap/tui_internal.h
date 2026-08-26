/*
 * src/hull/cap/tui_internal.h - Shared types between tui.c and
 * tui_input.c. Not part of the public API.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TUI_INTERNAL_H
#define HL_CAP_TUI_INTERNAL_H

#include "hull/cap/tui.h"

#include <stddef.h>
#include <stdint.h>

#define HL_TUI_MAX_COMBINING       4
#define HL_TUI_EVENT_QUEUE         64
#define HL_TUI_PASTE_INITIAL_CAP   4096
#define HL_TUI_PASTE_MAX           (64 * 1024)
#define HL_TUI_KEY_NAME_MAX        32

/* SGR mouse encoding bit: param flag 0x40 means scroll wheel. */
#define HL_TUI_MOUSE_FLAG_WHEEL    64

/* A single screen cell. */
typedef struct {
    uint32_t codepoint;                   /* 0 = empty/space; or wide-char continuation when width==0 */
    uint8_t  width;                       /* 0 = continuation of wide char (or empty), 1, 2 */
    uint8_t  combining_count;
    uint16_t _pad;
    uint32_t fg;
    uint32_t bg;
    uint32_t attr;
    uint32_t combining[HL_TUI_MAX_COMBINING];
} HlTuiCell;

/* Parser state machine. */
typedef enum {
    HL_TUI_PS_GROUND = 0,
    HL_TUI_PS_ESC,
    HL_TUI_PS_CSI,
    HL_TUI_PS_SS3,
    HL_TUI_PS_OSC,
    HL_TUI_PS_PASTE,    /* inside bracketed paste */
} HlTuiParserState;

/* A queued event. We copy strings into the event itself rather than
 * pointing into a circular buffer, so callers can hold the event
 * across one cap call (only the NEXT cap call invalidates the
 * pointers). */
typedef struct {
    HlTuiEvent  ev;
    /* Backing storage for ev.key / ev.text. The HlTuiEvent fields
     * point into these arrays for whichever event kind is active. */
    char     key_buf[HL_TUI_KEY_NAME_MAX];
    char    *text_buf;       /* heap; only used for PASTE */
    size_t   text_len;
} HlTuiQueuedEvent;

/* Parser state. */
typedef struct {
    HlTuiParserState state;

    /* Accumulator for in-flight escape sequences. CSI / SS3 / OSC
     * append parameter bytes here. Bounded; if exceeded we drop the
     * sequence. */
    char   acc[64];
    size_t acc_len;

    /* In-flight UTF-8 multi-byte sequence (in GROUND). */
    char   utf8[4];
    size_t utf8_len;
    size_t utf8_need;

    /* Bracketed-paste accumulator. */
    char   *paste_buf;
    size_t  paste_len;
    size_t  paste_cap;

    /* Event queue (ring). */
    HlTuiQueuedEvent queue[HL_TUI_EVENT_QUEUE];
    int    q_head;
    int    q_tail;
    int    q_count;

    /* The currently-published event borrowed by the caller. We keep
     * it stable until the next pop. */
    HlTuiQueuedEvent published;
    int              has_published;
} HlTuiParser;

/* ── API (internal) ────────────────────────────────────────────── */

void hl_tui_parser_init(HlTuiParser *p);
void hl_tui_parser_free(HlTuiParser *p);
void hl_tui_parser_feed(HlTuiParser *p, const char *bytes, size_t len);
int  hl_tui_parser_pop (HlTuiParser *p, HlTuiEvent *out);

/* Commit any in-flight in-progress state as a best-effort event.
 * Called by the cap layer after an idle slice without new bytes -
 * resolves the "lone ESC vs. start of CSI" ambiguity. Today:
 *   - ESC state with no follow-up → emit a bare "escape" key event.
 * Returns 1 if a new event was queued, 0 otherwise. */
int  hl_tui_parser_flush_idle(HlTuiParser *p);

/* Synthesize a resize event (cap layer invokes this on SIGWINCH).
 * Resize events jump to the front of the queue so the caller sees
 * them ahead of any buffered input. */
void hl_tui_parser_push_resize(HlTuiParser *p, int cols, int rows);

#endif /* HL_CAP_TUI_INTERNAL_H */

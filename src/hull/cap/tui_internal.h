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
/* Input bytes held back while the event queue is (nearly) full, parsed as
 * events are popped. The readers never read more than the room left. */
#define HL_TUI_INPUT_BACKLOG       1024

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

    /* OSC: the previous byte was an ESC (a possible ST, ESC '\'). Kept
     * apart from acc[], which stops recording once full - the terminator
     * of a long OSC string was never seen (audit 10). */
    int    osc_esc;
    /* OSC: payload bytes seen (any count; acc_len is bounded). */
    size_t osc_len;

    /* Unparsed input (see HL_TUI_INPUT_BACKLOG). */
    char   backlog[HL_TUI_INPUT_BACKLOG];
    size_t backlog_len;

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
/* Parse @p bytes. Stops queueing events before the queue overflows: the
 * rest is kept (up to HL_TUI_INPUT_BACKLOG) and parsed as events are popped,
 * instead of being dropped (audit 10). Returns the bytes taken - parsed or
 * kept; fewer than @p len only when the backlog is full. */
size_t hl_tui_parser_feed(HlTuiParser *p, const char *bytes, size_t len);

/* Bytes the backlog can still take: a reader reads no more than this, so
 * input it cannot hold stays in the kernel's buffer. */
size_t hl_tui_parser_room(const HlTuiParser *p);
int  hl_tui_parser_pop (HlTuiParser *p, HlTuiEvent *out);

/* Commit any in-flight in-progress state as a best-effort event.
 * Called by the cap layer after an idle slice without new bytes -
 * resolves the "lone ESC vs. start of CSI" ambiguity. Today:
 *   - ESC state with no follow-up → emit a bare "escape" key event.
 *   - OSC state with no payload (ESC ']' then quiet) → Alt+']'; an OSC
 *     that stalled part-way is abandoned (audit 10).
 * Returns 1 if a new event was queued, 0 otherwise. */
int  hl_tui_parser_flush_idle(HlTuiParser *p);

/* Synthesize a resize event (cap layer invokes this on SIGWINCH).
 * Resize events jump to the front of the queue so the caller sees
 * them ahead of any buffered input. */
void hl_tui_parser_push_resize(HlTuiParser *p, int cols, int rows);

#endif /* HL_CAP_TUI_INTERNAL_H */

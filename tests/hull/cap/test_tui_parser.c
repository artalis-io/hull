/*
 * test_tui_parser.c - ANSI input parser tests.
 *
 * Drives hl_tui_parser_feed / _pop directly (no PTY, no acquire) and
 * asserts on the decoded event stream. Covers:
 *   - ASCII printable + control characters
 *   - UTF-8 multi-byte input
 *   - CSI arrow keys + modified arrows
 *   - CSI <n> ~ keys (Home/End/Insert/Delete/PgUp/PgDn/F5+)
 *   - SS3 keys (F1-F4, application-keypad arrows)
 *   - SGR mouse
 *   - Bracketed paste
 *   - Focus in/out
 *   - Resize event injection
 *   - Multiple events from one feed
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "../../../src/hull/cap/tui_internal.h"

#include <string.h>

/* Helper: feed a literal string and pop the next event. */
static int feed_pop(HlTuiParser *p, const char *bytes, size_t n, HlTuiEvent *ev)
{
    hl_tui_parser_feed(p, bytes, n);
    return hl_tui_parser_pop(p, ev);
}

#define FEED_POP(P, S, EV)  feed_pop((P), (S), sizeof(S) - 1, (EV))

/* ── Printable + control ────────────────────────────────────────── */

UTEST(tui_parser, ascii_letter)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "a", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_KEY);
    ASSERT_STREQ(ev.key, "a");
    ASSERT_EQ(ev.codepoint, (uint32_t)'a');
    ASSERT_EQ(ev.mods, 0u);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, enter_key_cr)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\r", &ev), 0);
    ASSERT_STREQ(ev.key, "enter");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, enter_key_lf)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\n", &ev), 0);
    ASSERT_STREQ(ev.key, "enter");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, tab_and_backspace)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\t", &ev), 0);
    ASSERT_STREQ(ev.key, "tab");
    ASSERT_EQ(FEED_POP(&p, "\x7f", &ev), 0);
    ASSERT_STREQ(ev.key, "backspace");
    ASSERT_EQ(FEED_POP(&p, "\x08", &ev), 0);
    ASSERT_STREQ(ev.key, "backspace");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, ctrl_letter)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x03", &ev), 0);  /* Ctrl+C */
    ASSERT_STREQ(ev.key, "ctrl+c");
    ASSERT_EQ(ev.mods, HL_TUI_MOD_CTRL);
    ASSERT_EQ(FEED_POP(&p, "\x01", &ev), 0);  /* Ctrl+A */
    ASSERT_STREQ(ev.key, "ctrl+a");
    ASSERT_EQ(FEED_POP(&p, "\x1a", &ev), 0);  /* Ctrl+Z */
    ASSERT_STREQ(ev.key, "ctrl+z");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, lone_escape)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* A single ESC byte with no follow-up: the parser holds state.
     * Only after a *second* ESC do we emit the first one. */
    hl_tui_parser_feed(&p, "\x1b\x1b", 2);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_STREQ(ev.key, "escape");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, lone_escape_commits_via_flush_idle)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* Feed a lone ESC, no follow-up. Pop returns empty until
     * flush_idle commits it. */
    hl_tui_parser_feed(&p, "\x1b", 1);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 1);   /* nothing yet */

    ASSERT_EQ(hl_tui_parser_flush_idle(&p), 1); /* one event queued */
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_STREQ(ev.key, "escape");

    /* Subsequent flush_idle when GROUND is a no-op. */
    ASSERT_EQ(hl_tui_parser_flush_idle(&p), 0);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, flush_idle_does_not_commit_partial_csi)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* Partial CSI ("\x1b[" with no final byte). flush_idle should
     * NOT synthesize anything - CSI sequences are bounded by their
     * final byte; the kernel hasn't sent it yet but we still wait. */
    hl_tui_parser_feed(&p, "\x1b[", 2);
    ASSERT_EQ(hl_tui_parser_flush_idle(&p), 0);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 1);

    /* Now complete the sequence and ensure it still decodes. */
    hl_tui_parser_feed(&p, "A", 1);
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_STREQ(ev.key, "up");
    hl_tui_parser_free(&p);
}

/* ── UTF-8 ─────────────────────────────────────────────────────── */

UTEST(tui_parser, utf8_two_byte)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* é = C3 A9, U+00E9 */
    ASSERT_EQ(FEED_POP(&p, "\xC3\xA9", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_KEY);
    ASSERT_EQ(ev.codepoint, 0x00E9u);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, utf8_four_byte_emoji)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* 😀 = F0 9F 98 80, U+1F600 */
    ASSERT_EQ(FEED_POP(&p, "\xF0\x9F\x98\x80", &ev), 0);
    ASSERT_EQ(ev.codepoint, 0x1F600u);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, utf8_split_across_feeds)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* Feed first byte of "é", then the second separately. Parser
     * should buffer and decode after both bytes. */
    hl_tui_parser_feed(&p, "\xC3", 1);
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 1);  /* no event yet */
    hl_tui_parser_feed(&p, "\xA9", 1);
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_EQ(ev.codepoint, 0x00E9u);
    hl_tui_parser_free(&p);
}

/* ── CSI arrow keys ─────────────────────────────────────────────── */

UTEST(tui_parser, csi_arrow_up)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[A", &ev), 0);
    ASSERT_STREQ(ev.key, "up");
    ASSERT_EQ(ev.mods, 0u);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_arrow_all_four)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[A", &ev), 0); ASSERT_STREQ(ev.key, "up");
    ASSERT_EQ(FEED_POP(&p, "\x1b[B", &ev), 0); ASSERT_STREQ(ev.key, "down");
    ASSERT_EQ(FEED_POP(&p, "\x1b[C", &ev), 0); ASSERT_STREQ(ev.key, "right");
    ASSERT_EQ(FEED_POP(&p, "\x1b[D", &ev), 0); ASSERT_STREQ(ev.key, "left");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_modified_arrow_ctrl)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* CSI 1;5A = Ctrl+Up (modifier = 5 → ctrl) */
    ASSERT_EQ(FEED_POP(&p, "\x1b[1;5A", &ev), 0);
    ASSERT_STREQ(ev.key, "up");
    ASSERT_EQ(ev.mods, HL_TUI_MOD_CTRL);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_modified_arrow_shift_alt)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* CSI 1;4B = Shift+Alt+Down (modifier = 4 = shift+alt) */
    ASSERT_EQ(FEED_POP(&p, "\x1b[1;4B", &ev), 0);
    ASSERT_STREQ(ev.key, "down");
    ASSERT_TRUE(ev.mods & HL_TUI_MOD_SHIFT);
    ASSERT_TRUE(ev.mods & HL_TUI_MOD_ALT);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_home_end)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[H", &ev), 0); ASSERT_STREQ(ev.key, "home");
    ASSERT_EQ(FEED_POP(&p, "\x1b[F", &ev), 0); ASSERT_STREQ(ev.key, "end");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_backtab)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[Z", &ev), 0);
    ASSERT_STREQ(ev.key, "backtab");
    ASSERT_TRUE(ev.mods & HL_TUI_MOD_SHIFT);
    hl_tui_parser_free(&p);
}

/* ── CSI <n> ~ ──────────────────────────────────────────────────── */

UTEST(tui_parser, csi_tilde_insert)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[2~", &ev), 0);
    ASSERT_STREQ(ev.key, "insert");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_tilde_delete)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[3~", &ev), 0);
    ASSERT_STREQ(ev.key, "delete");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_tilde_pageup_pagedown)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[5~", &ev), 0); ASSERT_STREQ(ev.key, "pageup");
    ASSERT_EQ(FEED_POP(&p, "\x1b[6~", &ev), 0); ASSERT_STREQ(ev.key, "pagedown");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, csi_tilde_f5_to_f12)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[15~", &ev), 0); ASSERT_STREQ(ev.key, "f5");
    ASSERT_EQ(FEED_POP(&p, "\x1b[17~", &ev), 0); ASSERT_STREQ(ev.key, "f6");
    ASSERT_EQ(FEED_POP(&p, "\x1b[24~", &ev), 0); ASSERT_STREQ(ev.key, "f12");
    hl_tui_parser_free(&p);
}

/* ── SS3 (F1-F4, app-keypad arrows) ─────────────────────────────── */

UTEST(tui_parser, ss3_f1_to_f4)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1bOP", &ev), 0); ASSERT_STREQ(ev.key, "f1");
    ASSERT_EQ(FEED_POP(&p, "\x1bOQ", &ev), 0); ASSERT_STREQ(ev.key, "f2");
    ASSERT_EQ(FEED_POP(&p, "\x1bOR", &ev), 0); ASSERT_STREQ(ev.key, "f3");
    ASSERT_EQ(FEED_POP(&p, "\x1bOS", &ev), 0); ASSERT_STREQ(ev.key, "f4");
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, ss3_arrows)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1bOA", &ev), 0); ASSERT_STREQ(ev.key, "up");
    ASSERT_EQ(FEED_POP(&p, "\x1bOB", &ev), 0); ASSERT_STREQ(ev.key, "down");
    hl_tui_parser_free(&p);
}

/* ── Mouse ──────────────────────────────────────────────────────── */

UTEST(tui_parser, sgr_mouse_left_click)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* CSI < 0 ; 5 ; 10 M  - left button at col 5 row 10 */
    ASSERT_EQ(FEED_POP(&p, "\x1b[<0;5;10M", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_MOUSE);
    ASSERT_EQ(ev.x, 5);
    ASSERT_EQ(ev.y, 10);
    ASSERT_EQ(ev.button, HL_TUI_MOUSE_LEFT);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, sgr_mouse_release)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[<0;5;10m", &ev), 0);
    ASSERT_EQ(ev.button, HL_TUI_MOUSE_RELEASE);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, sgr_mouse_wheel_up)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* btn 64 = wheel up */
    ASSERT_EQ(FEED_POP(&p, "\x1b[<64;1;1M", &ev), 0);
    ASSERT_EQ(ev.button, HL_TUI_MOUSE_WHEEL_UP);
    hl_tui_parser_free(&p);
}

/* ── Focus ──────────────────────────────────────────────────────── */

UTEST(tui_parser, focus_in_out)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    ASSERT_EQ(FEED_POP(&p, "\x1b[I", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_FOCUS);
    ASSERT_EQ(ev.focus_in, 1);
    ASSERT_EQ(FEED_POP(&p, "\x1b[O", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_FOCUS);
    ASSERT_EQ(ev.focus_in, 0);
    hl_tui_parser_free(&p);
}

/* ── Bracketed paste ────────────────────────────────────────────── */

UTEST(tui_parser, bracketed_paste)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* CSI 200 ~ <payload> CSI 201 ~ */
    const char in[] = "\x1b[200~hello world\x1b[201~";
    hl_tui_parser_feed(&p, in, sizeof in - 1);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_PASTE);
    ASSERT_EQ(ev.text_len, 11u);
    ASSERT_EQ(strncmp(ev.text, "hello world", 11), 0);
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, bracketed_paste_with_newlines)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    const char in[] = "\x1b[200~line1\nline2\nline3\x1b[201~";
    hl_tui_parser_feed(&p, in, sizeof in - 1);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_PASTE);
    ASSERT_EQ(ev.text_len, 17u);
    ASSERT_EQ(strncmp(ev.text, "line1\nline2\nline3", 17), 0);
    hl_tui_parser_free(&p);
}

/* ── Multi-event drain ──────────────────────────────────────────── */

UTEST(tui_parser, multiple_events_one_feed)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* "abc\x1b[Ad" = a, b, c, up, d */
    hl_tui_parser_feed(&p, "abc\x1b[Ad", 7);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0); ASSERT_EQ(ev.codepoint, (uint32_t)'a');
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0); ASSERT_EQ(ev.codepoint, (uint32_t)'b');
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0); ASSERT_EQ(ev.codepoint, (uint32_t)'c');
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0); ASSERT_STREQ(ev.key, "up");
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0); ASSERT_EQ(ev.codepoint, (uint32_t)'d');
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 1); /* empty */
    hl_tui_parser_free(&p);
}

/* ── Alt + char ─────────────────────────────────────────────────── */

UTEST(tui_parser, alt_letter)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* ESC + 'a' = Alt+a (split string to terminate the hex escape) */
    ASSERT_EQ(FEED_POP(&p, "\x1b" "a", &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_KEY);
    ASSERT_EQ(ev.codepoint, (uint32_t)'a');
    ASSERT_EQ(ev.mods, HL_TUI_MOD_ALT);
    hl_tui_parser_free(&p);
}

/* ── Resize injection ───────────────────────────────────────────── */

UTEST(tui_parser, resize_jumps_to_front)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* Queue a key event, then push a resize - resize should pop first. */
    hl_tui_parser_feed(&p, "a", 1);
    hl_tui_parser_push_resize(&p, 80, 24);
    HlTuiEvent ev = {0};
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_RESIZE);
    ASSERT_EQ(ev.cols, 80);
    ASSERT_EQ(ev.rows, 24);
    /* Now the key. */
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 0);
    ASSERT_EQ((int)ev.kind, (int)HL_TUI_EV_KEY);
    ASSERT_EQ(ev.codepoint, (uint32_t)'a');
    hl_tui_parser_free(&p);
}

/* ── Robustness ─────────────────────────────────────────────────── */

UTEST(tui_parser, unknown_csi_dropped)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    HlTuiEvent ev = {0};
    /* Unknown CSI sequence - parser shouldn't crash; nothing is
     * emitted. Following keystroke should still work. */
    hl_tui_parser_feed(&p, "\x1b[99;99;99X", 11);
    ASSERT_EQ(hl_tui_parser_pop(&p, &ev), 1);  /* empty */
    ASSERT_EQ(FEED_POP(&p, "a", &ev), 0);
    ASSERT_EQ(ev.codepoint, (uint32_t)'a');
    hl_tui_parser_free(&p);
}

UTEST(tui_parser, queue_full_drops_silently)
{
    HlTuiParser p; hl_tui_parser_init(&p);
    /* Feed > queue capacity keys; older ones survive, newer ones drop. */
    char buf[256];
    memset(buf, 'x', sizeof buf);
    hl_tui_parser_feed(&p, buf, sizeof buf);
    int n = 0;
    HlTuiEvent ev = {0};
    while (hl_tui_parser_pop(&p, &ev) == 0) n++;
    ASSERT_LE(n, HL_TUI_EVENT_QUEUE);
    ASSERT_GT(n, 0);
    hl_tui_parser_free(&p);
}

UTEST_MAIN();

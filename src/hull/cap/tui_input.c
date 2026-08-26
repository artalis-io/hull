/*
 * src/hull/cap/tui_input.c - ANSI input parser + event queue.
 *
 * State machine handles: printable UTF-8 bytes, control characters,
 * CSI sequences (`\x1b[...`), SS3 sequences (`\x1bO...`), OSC
 * sequences (`\x1b]...` - currently swallowed), and bracketed paste.
 *
 * SGR mouse, Kitty keyboard protocol, and focus tracking are
 * speced-as-modes in the public header; their parser branches are
 * wired but minimal. Refinement comes when first-party tools start
 * using these modes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "tui_internal.h"
#include "hull/cap/tui_width.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Queue helpers ──────────────────────────────────────────────── */

static HlTuiQueuedEvent *queue_push_back(HlTuiParser *p)
{
    if (p->q_count >= HL_TUI_EVENT_QUEUE) return NULL;
    HlTuiQueuedEvent *q = &p->queue[p->q_tail];
    p->q_tail = (p->q_tail + 1) % HL_TUI_EVENT_QUEUE;
    p->q_count++;
    memset(q, 0, sizeof *q);
    return q;
}

static HlTuiQueuedEvent *queue_push_front(HlTuiParser *p)
{
    if (p->q_count >= HL_TUI_EVENT_QUEUE) return NULL;
    p->q_head = (p->q_head - 1 + HL_TUI_EVENT_QUEUE) % HL_TUI_EVENT_QUEUE;
    HlTuiQueuedEvent *q = &p->queue[p->q_head];
    p->q_count++;
    memset(q, 0, sizeof *q);
    return q;
}

static HlTuiQueuedEvent *queue_pop_front(HlTuiParser *p)
{
    if (p->q_count == 0) return NULL;
    HlTuiQueuedEvent *q = &p->queue[p->q_head];
    p->q_head = (p->q_head + 1) % HL_TUI_EVENT_QUEUE;
    p->q_count--;
    return q;
}

/* ── Key emission ───────────────────────────────────────────────── */

/* Encode a codepoint as UTF-8 (up to 4 bytes). Returns bytes written. */
static size_t enc_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void emit_key_named(HlTuiParser *p, const char *name, uint32_t mods)
{
    HlTuiQueuedEvent *q = queue_push_back(p);
    if (!q) return;
    q->ev.kind = HL_TUI_EV_KEY;
    q->ev.mods = mods;
    size_t n = strlen(name);
    if (n >= sizeof q->key_buf) n = sizeof q->key_buf - 1;
    memcpy(q->key_buf, name, n);
    q->key_buf[n] = '\0';
    q->ev.key = q->key_buf;
    q->ev.codepoint = 0;
}

static void emit_key_cp(HlTuiParser *p, uint32_t cp, uint32_t mods)
{
    HlTuiQueuedEvent *q = queue_push_back(p);
    if (!q) return;
    q->ev.kind = HL_TUI_EV_KEY;
    q->ev.mods = mods;
    q->ev.codepoint = cp;
    size_t n = enc_utf8(cp, q->key_buf);
    if (n >= sizeof q->key_buf) n = sizeof q->key_buf - 1;
    q->key_buf[n] = '\0';
    q->ev.key = q->key_buf;
}

static void emit_ctrl_letter(HlTuiParser *p, char letter)
{
    /* "ctrl+a" .. "ctrl+z" */
    HlTuiQueuedEvent *q = queue_push_back(p);
    if (!q) return;
    q->ev.kind = HL_TUI_EV_KEY;
    q->ev.mods = HL_TUI_MOD_CTRL;
    q->ev.codepoint = (uint32_t)letter;
    snprintf(q->key_buf, sizeof q->key_buf, "ctrl+%c", letter);
    q->ev.key = q->key_buf;
}

/* ── CSI / SS3 decode ───────────────────────────────────────────── */

/* CSI sequences are `\x1b [ <params> <final>` where params are
 * optional digits and semicolons, final is a single byte 0x40..0x7E.
 * Format: ESC [ Pn [;Pn]* final  */

static void decode_csi_arrow(HlTuiParser *p, char final, int param)
{
    uint32_t mods = 0;
    if (param > 1) {
        /* param encodes modifiers: 2=Shift, 3=Alt, 4=Shift+Alt,
         * 5=Ctrl, 6=Shift+Ctrl, 7=Alt+Ctrl, 8=Shift+Alt+Ctrl. */
        int m = param - 1;
        if (m & 1) mods |= HL_TUI_MOD_SHIFT;
        if (m & 2) mods |= HL_TUI_MOD_ALT;
        if (m & 4) mods |= HL_TUI_MOD_CTRL;
    }
    const char *name = NULL;
    switch (final) {
    case 'A': name = HL_TUI_KEY_UP;    break;
    case 'B': name = HL_TUI_KEY_DOWN;  break;
    case 'C': name = HL_TUI_KEY_RIGHT; break;
    case 'D': name = HL_TUI_KEY_LEFT;  break;
    case 'H': name = HL_TUI_KEY_HOME;  break;
    case 'F': name = HL_TUI_KEY_END;   break;
    case 'Z': name = HL_TUI_KEY_BACKTAB; mods |= HL_TUI_MOD_SHIFT; break;
    default: break;
    }
    if (name) emit_key_named(p, name, mods);
}

static void decode_csi_tilde(HlTuiParser *p, int param, int mod_param)
{
    /* CSI <n> ~  - n selects key. Optional second param = modifier. */
    uint32_t mods = 0;
    if (mod_param > 1) {
        int m = mod_param - 1;
        if (m & 1) mods |= HL_TUI_MOD_SHIFT;
        if (m & 2) mods |= HL_TUI_MOD_ALT;
        if (m & 4) mods |= HL_TUI_MOD_CTRL;
    }
    const char *name = NULL;
    switch (param) {
    case 1: case 7: name = HL_TUI_KEY_HOME;     break;
    case 2:          name = HL_TUI_KEY_INSERT;   break;
    case 3:          name = HL_TUI_KEY_DELETE;   break;
    case 4: case 8: name = HL_TUI_KEY_END;      break;
    case 5:          name = HL_TUI_KEY_PAGEUP;   break;
    case 6:          name = HL_TUI_KEY_PAGEDOWN; break;
    case 15:         name = "f5";  break;
    case 17:         name = "f6";  break;
    case 18:         name = "f7";  break;
    case 19:         name = "f8";  break;
    case 20:         name = "f9";  break;
    case 21:         name = "f10"; break;
    case 23:         name = "f11"; break;
    case 24:         name = "f12"; break;
    default: break;
    }
    if (name) emit_key_named(p, name, mods);
}

/* Parse the params buffer (digits + semicolons). Returns count of
 * params written into `out[]` (capacity = max). Empty params count
 * as 0. */
static int parse_params(const char *s, size_t n, int *out, int max)
{
    int count = 0;
    int cur = 0;
    int have_digit = 0;
    for (size_t i = 0; i < n && count < max; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have_digit = 1;
        } else if (c == ';') {
            out[count++] = have_digit ? cur : 0;
            cur = 0;
            have_digit = 0;
        }
    }
    if (count < max) out[count++] = have_digit ? cur : 0;
    return count;
}

static void decode_csi(HlTuiParser *p, char final)
{
    /* Bracketed paste markers. */
    if (final == '~') {
        int params[4] = {0};
        int nparams = parse_params(p->acc, p->acc_len, params, 4);
        if (nparams >= 1 && params[0] == 200) {
            /* Enter paste mode. */
            p->state = HL_TUI_PS_PASTE;
            if (!p->paste_buf) {
                p->paste_cap = HL_TUI_PASTE_INITIAL_CAP;
                p->paste_buf = malloc(p->paste_cap);
            }
            p->paste_len = 0;
            return;
        }
        if (nparams >= 1 && params[0] == 201) {
            /* Should be consumed by paste mode; defensive no-op here. */
            return;
        }
        decode_csi_tilde(p, params[0], nparams > 1 ? params[1] : 1);
        return;
    }

    /* Mouse SGR: CSI < <btn> ; <x> ; <y> M|m */
    if ((final == 'M' || final == 'm') && p->acc_len > 0 && p->acc[0] == '<') {
        int params[3] = {0};
        parse_params(p->acc + 1, p->acc_len - 1, params, 3);
        HlTuiQueuedEvent *q = queue_push_back(p);
        if (!q) return;
        q->ev.kind = HL_TUI_EV_MOUSE;
        q->ev.x = params[1];
        q->ev.y = params[2];
        int btn = params[0];
        if (btn & HL_TUI_MOUSE_FLAG_WHEEL)
            q->ev.button = (btn & 1) ? HL_TUI_MOUSE_WHEEL_DOWN : HL_TUI_MOUSE_WHEEL_UP;
        else if (final == 'm') q->ev.button = HL_TUI_MOUSE_RELEASE;
        else if ((btn & 3) == 0) q->ev.button = HL_TUI_MOUSE_LEFT;
        else if ((btn & 3) == 1) q->ev.button = HL_TUI_MOUSE_MIDDLE;
        else                     q->ev.button = HL_TUI_MOUSE_RIGHT;
        return;
    }

    /* Focus in/out: CSI I / CSI O */
    if (final == 'I' || final == 'O') {
        HlTuiQueuedEvent *q = queue_push_back(p);
        if (!q) return;
        q->ev.kind = HL_TUI_EV_FOCUS;
        q->ev.focus_in = (final == 'I');
        return;
    }

    /* Arrow / home / end (with optional 1;<mod> for modifiers). */
    if (final >= 'A' && final <= 'Z') {
        int params[2] = {0, 0};
        int nparams = parse_params(p->acc, p->acc_len, params, 2);
        int mod = (nparams >= 2) ? params[1] : 1;
        decode_csi_arrow(p, final, mod);
        return;
    }
}

/* SS3 sequences are `\x1b O <final>`. Used for F1-F4 and arrow keys
 * in "application keypad" mode. */
static void decode_ss3(HlTuiParser *p, char final)
{
    const char *name = NULL;
    switch (final) {
    case 'A': name = HL_TUI_KEY_UP;    break;
    case 'B': name = HL_TUI_KEY_DOWN;  break;
    case 'C': name = HL_TUI_KEY_RIGHT; break;
    case 'D': name = HL_TUI_KEY_LEFT;  break;
    case 'H': name = HL_TUI_KEY_HOME;  break;
    case 'F': name = HL_TUI_KEY_END;   break;
    case 'P': name = "f1"; break;
    case 'Q': name = "f2"; break;
    case 'R': name = "f3"; break;
    case 'S': name = "f4"; break;
    default: break;
    }
    if (name) emit_key_named(p, name, 0);
}

/* ── Control character → key ────────────────────────────────────── */

static void handle_control(HlTuiParser *p, unsigned char c)
{
    if (c == 0x0D) { emit_key_named(p, HL_TUI_KEY_ENTER, 0);     return; }
    if (c == 0x0A) { emit_key_named(p, HL_TUI_KEY_ENTER, 0);     return; }
    if (c == 0x09) { emit_key_named(p, HL_TUI_KEY_TAB, 0);       return; }
    if (c == 0x08 || c == 0x7F) { emit_key_named(p, HL_TUI_KEY_BACKSPACE, 0); return; }
    if (c == 0x1B) {
        /* Bare ESC - likely the start of an escape sequence; the
         * caller drives us back to the state machine. If we reach
         * here it means a lone ESC was delivered (no follow-up). */
        emit_key_named(p, HL_TUI_KEY_ESCAPE, 0);
        return;
    }
    if (c < 0x20) {
        /* Ctrl+<letter>. ^A=0x01, ^B=0x02, ... ^Z=0x1A. */
        emit_ctrl_letter(p, (char)('a' + (c - 1)));
        return;
    }
}

/* ── Paste mode ─────────────────────────────────────────────────── */

static void paste_flush(HlTuiParser *p)
{
    HlTuiQueuedEvent *q = queue_push_back(p);
    if (!q) { p->paste_len = 0; return; }
    q->ev.kind = HL_TUI_EV_PASTE;
    /* Move the heap buffer to the queued event so it survives the
     * next state-machine cycle. */
    q->text_buf = malloc(p->paste_len + 1);
    if (!q->text_buf) { p->paste_len = 0; return; }
    memcpy(q->text_buf, p->paste_buf, p->paste_len);
    q->text_buf[p->paste_len] = '\0';
    q->text_len = p->paste_len;
    q->ev.text = q->text_buf;
    q->ev.text_len = p->paste_len;
    p->paste_len = 0;
}

static int paste_append(HlTuiParser *p, char c)
{
    if (p->paste_len >= HL_TUI_PASTE_MAX) return 0;  /* drop overflow */
    if (p->paste_len + 1 > p->paste_cap) {
        size_t nc = p->paste_cap ? p->paste_cap * 2 : HL_TUI_PASTE_INITIAL_CAP;
        if (nc > HL_TUI_PASTE_MAX) nc = HL_TUI_PASTE_MAX;
        char *nb = realloc(p->paste_buf, nc);
        if (!nb) return 0;
        p->paste_buf = nb;
        p->paste_cap = nc;
    }
    p->paste_buf[p->paste_len++] = c;
    return 1;
}

/* ── Main feed ──────────────────────────────────────────────────── */

void hl_tui_parser_init(HlTuiParser *p)
{
    memset(p, 0, sizeof *p);
    p->state = HL_TUI_PS_GROUND;
}

void hl_tui_parser_free(HlTuiParser *p)
{
    if (!p) return;
    /* Free heap text buffers in queued events. */
    for (int i = 0; i < HL_TUI_EVENT_QUEUE; i++) {
        free(p->queue[i].text_buf);
    }
    free(p->published.text_buf);
    free(p->paste_buf);
    memset(p, 0, sizeof *p);
}

void hl_tui_parser_feed(HlTuiParser *p, const char *bytes, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];

        switch (p->state) {

        case HL_TUI_PS_GROUND:
            if (c == 0x1B) {
                p->state = HL_TUI_PS_ESC;
                p->acc_len = 0;
                break;
            }
            if (c < 0x20 || c == 0x7F) {
                /* Control character. */
                if (p->utf8_len) { p->utf8_len = p->utf8_need = 0; }
                handle_control(p, c);
                break;
            }
            if (c < 0x80) {
                /* ASCII printable. */
                if (p->utf8_len) { p->utf8_len = p->utf8_need = 0; }
                emit_key_cp(p, c, 0);
                break;
            }
            /* UTF-8 multi-byte. */
            if (p->utf8_len == 0) {
                if ((c & 0xE0) == 0xC0) p->utf8_need = 2;
                else if ((c & 0xF0) == 0xE0) p->utf8_need = 3;
                else if ((c & 0xF8) == 0xF0) p->utf8_need = 4;
                else { /* invalid lead - drop */ break; }
                p->utf8[0] = (char)c;
                p->utf8_len = 1;
            } else {
                if ((c & 0xC0) != 0x80) {
                    /* Invalid continuation; reset. */
                    p->utf8_len = p->utf8_need = 0;
                    break;
                }
                p->utf8[p->utf8_len++] = (char)c;
                if (p->utf8_len == p->utf8_need) {
                    uint32_t cp = 0;
                    int n = hl_tui_utf8_decode(p->utf8, p->utf8_len, &cp);
                    (void)n;
                    p->utf8_len = p->utf8_need = 0;
                    if (cp != 0xFFFD) emit_key_cp(p, cp, 0);
                }
            }
            break;

        case HL_TUI_PS_ESC:
            if (c == '[') {
                p->state = HL_TUI_PS_CSI;
                p->acc_len = 0;
            } else if (c == 'O') {
                p->state = HL_TUI_PS_SS3;
                p->acc_len = 0;
            } else if (c == ']') {
                p->state = HL_TUI_PS_OSC;
                p->acc_len = 0;
            } else if (c == 0x1B) {
                /* ESC ESC - emit one ESC, stay in ESC for the next. */
                emit_key_named(p, HL_TUI_KEY_ESCAPE, 0);
            } else if (c < 0x20 || c == 0x7F) {
                /* Alt+<control> - emit the control then exit. */
                emit_key_named(p, HL_TUI_KEY_ESCAPE, 0);
                handle_control(p, c);
                p->state = HL_TUI_PS_GROUND;
            } else if (c < 0x80) {
                /* Alt+<printable>: emit as printable with ALT modifier. */
                emit_key_cp(p, c, HL_TUI_MOD_ALT);
                p->state = HL_TUI_PS_GROUND;
            } else {
                /* Alt + UTF-8: not handled distinctly; treat as escape
                 * then re-feed the byte through ground. */
                emit_key_named(p, HL_TUI_KEY_ESCAPE, 0);
                p->state = HL_TUI_PS_GROUND;
                i--; /* re-process this byte */
            }
            break;

        case HL_TUI_PS_CSI:
            if (c >= 0x40 && c <= 0x7E) {
                /* Final byte. */
                decode_csi(p, (char)c);
                if (p->state != HL_TUI_PS_PASTE)
                    p->state = HL_TUI_PS_GROUND;
                p->acc_len = 0;
            } else if (p->acc_len < sizeof p->acc) {
                p->acc[p->acc_len++] = (char)c;
            } /* else: overflow - keep waiting for final */
            break;

        case HL_TUI_PS_SS3:
            decode_ss3(p, (char)c);
            p->state = HL_TUI_PS_GROUND;
            break;

        case HL_TUI_PS_OSC:
            /* Consume until BEL or ST (ESC \). We don't process OSC
             * responses asynchronously; the only OSC we care about
             * (background query) is handled synchronously during
             * acquire. */
            if (c == 0x07) {
                p->state = HL_TUI_PS_GROUND;
                p->acc_len = 0;
            } else if (c == '\\' && p->acc_len > 0 &&
                       p->acc[p->acc_len - 1] == 0x1B) {
                /* ST: the previous byte recorded a pending ESC. */
                p->state = HL_TUI_PS_GROUND;
                p->acc_len = 0;
            } else if (p->acc_len + 1 < sizeof p->acc) {
                /* Includes ESC: we record it so the next byte can
                 * decide whether this is ST (ESC '\\') or random
                 * payload. acc[] is bounded; long OSC strings drop
                 * trailing bytes but the terminator still works. */
                p->acc[p->acc_len++] = (char)c;
            }
            break;

        case HL_TUI_PS_PASTE:
            /* Look for the end marker: ESC [ 2 0 1 ~ */
            if (c == 0x1B) {
                p->acc_len = 0;
                p->acc[p->acc_len++] = (char)c;
            } else if (p->acc_len > 0 && p->acc_len < 6) {
                static const char end[] = "\x1b[201~";
                if (c == (unsigned char)end[p->acc_len]) {
                    p->acc[p->acc_len++] = (char)c;
                    if (p->acc_len == sizeof end - 1) {
                        paste_flush(p);
                        p->state = HL_TUI_PS_GROUND;
                        p->acc_len = 0;
                    }
                } else {
                    /* Not the end marker - fold accumulated bytes
                     * back into the paste buffer. */
                    for (size_t k = 0; k < p->acc_len; k++)
                        paste_append(p, p->acc[k]);
                    paste_append(p, (char)c);
                    p->acc_len = 0;
                }
            } else {
                paste_append(p, (char)c);
            }
            break;
        }
    }
}

int hl_tui_parser_pop(HlTuiParser *p, HlTuiEvent *out)
{
    if (!p->q_count) {
        out->kind = HL_TUI_EV_NONE;
        out->key = NULL; out->text = NULL;
        return 1;
    }

    /* Move the previously-published event's heap storage (if any)
     * to its slot in the queue so it can be reused. */
    if (p->has_published) {
        free(p->published.text_buf);
        p->published.text_buf = NULL;
        p->has_published = 0;
    }

    HlTuiQueuedEvent *q = queue_pop_front(p);
    p->published = *q;
    /* The pointers inside the event still reference q's storage,
     * but since we have a copy in `published` they need to be
     * rebased. */
    if (p->published.ev.kind == HL_TUI_EV_KEY ||
        p->published.ev.kind == HL_TUI_EV_NONE) {
        p->published.ev.key = p->published.key_buf;
    }
    if (p->published.ev.kind == HL_TUI_EV_PASTE) {
        p->published.ev.text = p->published.text_buf;
    }
    /* Detach the heap buffer from the queue slot so it doesn't get
     * double-freed when the slot is reused. */
    q->text_buf = NULL;

    p->has_published = 1;
    *out = p->published.ev;
    return 0;
}

void hl_tui_parser_push_resize(HlTuiParser *p, int cols, int rows)
{
    HlTuiQueuedEvent *q = queue_push_front(p);
    if (!q) return;
    q->ev.kind = HL_TUI_EV_RESIZE;
    q->ev.cols = cols;
    q->ev.rows = rows;
}

int hl_tui_parser_flush_idle(HlTuiParser *p)
{
    /* If we've been sitting in ESC waiting for the next byte and
     * nothing arrived in the caller's idle window, commit a bare
     * escape. Other states (CSI / SS3 / OSC / PASTE) are bounded
     * by their final byte; we wait for those. */
    if (p->state == HL_TUI_PS_ESC) {
        emit_key_named(p, HL_TUI_KEY_ESCAPE, 0);
        p->state = HL_TUI_PS_GROUND;
        p->acc_len = 0;
        return 1;
    }
    return 0;
}

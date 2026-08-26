/*
 * include/hull/cap/tui.h - Terminal UI capability.
 *
 * Provides raw terminal access for full-screen interactive
 * applications: alt screen, raw mode, ANSI rendering, input event
 * decoding (keyboard, mouse, paste, resize, focus), cell-level
 * shadow-buffer diffing, OSC-based theme detection and clipboard
 * write, and singleton lifetime management with atexit / signal-
 * driven restore.
 *
 * Process invariant: at most one HlTuiCtx is live at any time. The
 * controlling tty is a per-process resource - multiple ctxs would
 * race on termios, signal handlers, and the alt screen. A second
 * acquire while another ctx is live returns -EBUSY.
 *
 * Ownership: the cap layer owns the ctx allocation and the OS-level
 * terminal state. Callers hold a pointer for as long as they need
 * cap operations and call `release` to free it. An atexit handler
 * and SIGINT/SIGTERM/SIGQUIT handlers are installed at first acquire
 * and guarantee the terminal is restored even on panic / signal.
 *
 * Concurrency: not thread-safe. All operations on a single ctx must
 * happen from the same thread.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TUI_H
#define HL_CAP_TUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HlTuiCtx HlTuiCtx;

/* ── Capability bitmask ─────────────────────────────────────────── */

#define HL_TUI_CAP_TRUECOLOR    (1u << 0)
#define HL_TUI_CAP_256COLOR     (1u << 1)
#define HL_TUI_CAP_BASIC_COLOR  (1u << 2)
#define HL_TUI_CAP_MOUSE        (1u << 3)
#define HL_TUI_CAP_FOCUS        (1u << 4)
#define HL_TUI_CAP_KITTY_KBD    (1u << 5)
#define HL_TUI_CAP_OSC52        (1u << 6)

/* ── Style attributes ───────────────────────────────────────────── */
/* Colors are 32-bit values. Top byte selects encoding:
 *   0x00rrggbb           → 24-bit truecolor
 *   0x01000000 | index   → 256-color palette index (0..255)
 *   0x02000000 | index   → 16-color basic palette (0..15)
 *   0xFFFFFFFF           → "default" (terminal default fg/bg)
 */
#define HL_TUI_COLOR_DEFAULT    0xFFFFFFFFu
#define HL_TUI_COLOR_RGB(r,g,b) ((uint32_t)(((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF))
#define HL_TUI_COLOR_256(i)     (0x01000000u | ((i) & 0xFF))
#define HL_TUI_COLOR_BASIC(i)   (0x02000000u | ((i) & 0x0F))

#define HL_TUI_ATTR_BOLD        (1u << 0)
#define HL_TUI_ATTR_DIM         (1u << 1)
#define HL_TUI_ATTR_ITALIC      (1u << 2)
#define HL_TUI_ATTR_UNDERLINE   (1u << 3)
#define HL_TUI_ATTR_REVERSE     (1u << 4)
#define HL_TUI_ATTR_STRIKE      (1u << 5)

/* ── Event types ────────────────────────────────────────────────── */

typedef enum {
    HL_TUI_EV_NONE   = 0,
    HL_TUI_EV_KEY    = 1,
    HL_TUI_EV_RESIZE = 2,
    HL_TUI_EV_MOUSE  = 3,
    HL_TUI_EV_PASTE  = 4,
    HL_TUI_EV_FOCUS  = 5,
} HlTuiEventKind;

#define HL_TUI_MOD_SHIFT  (1u << 0)
#define HL_TUI_MOD_ALT    (1u << 1)
#define HL_TUI_MOD_CTRL   (1u << 2)
#define HL_TUI_MOD_SUPER  (1u << 3)

/* Symbolic key names (passed in HlTuiEvent.key). For printable
 * characters the key field holds the UTF-8 encoding of the
 * codepoint and `codepoint` is set; for non-printable keys we use
 * stable text names below. */
#define HL_TUI_KEY_UP        "up"
#define HL_TUI_KEY_DOWN      "down"
#define HL_TUI_KEY_LEFT      "left"
#define HL_TUI_KEY_RIGHT     "right"
#define HL_TUI_KEY_HOME      "home"
#define HL_TUI_KEY_END       "end"
#define HL_TUI_KEY_PAGEUP    "pageup"
#define HL_TUI_KEY_PAGEDOWN  "pagedown"
#define HL_TUI_KEY_INSERT    "insert"
#define HL_TUI_KEY_DELETE    "delete"
#define HL_TUI_KEY_ENTER     "enter"
#define HL_TUI_KEY_TAB       "tab"
#define HL_TUI_KEY_BACKTAB   "backtab"
#define HL_TUI_KEY_ESCAPE    "escape"
#define HL_TUI_KEY_BACKSPACE "backspace"
#define HL_TUI_KEY_SPACE     "space"

/* Mouse button codes. `button` field on a mouse event. */
#define HL_TUI_MOUSE_LEFT    0
#define HL_TUI_MOUSE_MIDDLE  1
#define HL_TUI_MOUSE_RIGHT   2
#define HL_TUI_MOUSE_RELEASE 3
#define HL_TUI_MOUSE_WHEEL_UP   4
#define HL_TUI_MOUSE_WHEEL_DOWN 5

typedef struct {
    HlTuiEventKind kind;

    /* HL_TUI_EV_KEY - `key` is either a single-character UTF-8
     * sequence (printable chars; `codepoint` populated) or one of
     * the HL_TUI_KEY_* names (non-printable). Always NUL-terminated
     * and points into ctx-owned storage; valid until the next
     * call to a cap function. */
    const char *key;
    uint32_t    codepoint;
    uint32_t    mods;        /* HL_TUI_MOD_* bitmask */

    /* HL_TUI_EV_RESIZE - new dimensions. */
    int cols;
    int rows;

    /* HL_TUI_EV_MOUSE - 1-indexed cell coordinates. */
    int x;
    int y;
    int button;              /* HL_TUI_MOUSE_* */

    /* HL_TUI_EV_PASTE - bracketed-paste payload. Valid until next
     * cap call. */
    const char *text;
    size_t      text_len;

    /* HL_TUI_EV_FOCUS - 1 if gained focus, 0 if lost. */
    int focus_in;
} HlTuiEvent;

/* ── Lifecycle ──────────────────────────────────────────────────── */

/* Allocate a new ctx and enter alt screen / raw mode. Returns 0 on
 * success, -1 (errno set) on failure. Errno values:
 *   EBUSY    another ctx is already live in this process
 *   ENOTTY   stdin or stdout is not a terminal
 *   ENOMEM   allocation failed
 *
 * On success, registers a one-shot atexit handler and the SIGINT /
 * SIGTERM / SIGQUIT / SIGWINCH / SIGTSTP / SIGCONT chain.
 *
 * Performs OSC 11 background-color query (50ms timeout) for theme
 * detection - see hl_cap_tui_theme.
 */
int hl_cap_tui_acquire(HlTuiCtx **out);

/* Restore terminal, free ctx. Idempotent in the sense that calling
 * twice on the same pointer is a no-op after the first (the pointer
 * is invalidated). Pass NULL for the abandoned-pointer case. */
int hl_cap_tui_release(HlTuiCtx *ctx);

/* atexit / panic handler - restores the terminal if a ctx is live,
 * without freeing memory (process is exiting). Not for script use. */
void hl_cap_tui_force_leave(void);

/* ── Query ──────────────────────────────────────────────────────── */

/* Current terminal size in cells. Updated on SIGWINCH. */
int hl_cap_tui_size(HlTuiCtx *ctx, int *cols, int *rows);

/* Capability bitmask (HL_TUI_CAP_*). Computed at acquire from $TERM,
 * $COLORTERM, and OSC responses. */
uint32_t hl_cap_tui_caps(HlTuiCtx *ctx);

/* Theme classification: "dark" / "light" / "unknown". Cached at
 * acquire time. Pointer is to static storage; do not free. */
const char *hl_cap_tui_theme(HlTuiCtx *ctx);

/* The raw input fd (typically STDIN_FILENO). Used by binding layers
 * to register a watcher with the async backend. */
int hl_cap_tui_input_fd(HlTuiCtx *ctx);

/* ── Rendering ──────────────────────────────────────────────────── */

/* Clear the pending buffer. Next flush emits a delta from the
 * current shadow to an empty screen. */
int hl_cap_tui_clear(HlTuiCtx *ctx);

/* Zero the shadow buffer; next flush emits everything in the pending
 * buffer. Use when the screen is known to be corrupted (e.g., the
 * user switched to another app and back). */
int hl_cap_tui_invalidate(HlTuiCtx *ctx);

/* Move the pending cursor. 1-indexed; out-of-range silently clamps. */
int hl_cap_tui_move(HlTuiCtx *ctx, int x, int y);

/* Set the pending style (applied to subsequent writes until changed). */
int hl_cap_tui_style(HlTuiCtx *ctx, uint32_t fg, uint32_t bg, uint32_t attr);

/* Write a UTF-8 string at the current pending cursor. Advances the
 * cursor by the cell width of each codepoint. Wide chars consume
 * two cells; combining marks attach to the preceding cell (no
 * advance). */
int hl_cap_tui_write(HlTuiCtx *ctx, const char *s, size_t len);

/* Diff the pending buffer against the shadow and emit the minimal
 * ANSI sequence to bring the terminal in sync. After flush, the
 * shadow == pending. */
int hl_cap_tui_flush(HlTuiCtx *ctx);

/* ── Input ──────────────────────────────────────────────────────── */

/* Block until an event is available or `timeout_ms` elapses. Uses
 * poll(2) on the input fd. `timeout_ms`:
 *   -1   wait indefinitely
 *    0   non-blocking
 *   >0   wait up to N ms
 *
 * Returns 0 with *out filled on event, 1 on timeout (with
 * out->kind == HL_TUI_EV_NONE), -1 on error.
 *
 * Resize events are delivered ahead of any queued input bytes - a
 * pending SIGWINCH always wins. */
int hl_cap_tui_poll(HlTuiCtx *ctx, int timeout_ms, HlTuiEvent *out);

/* Feed raw bytes into the parser. Used by binding layers driving
 * input via the async backend's watcher callback. */
int hl_cap_tui_feed(HlTuiCtx *ctx, const char *bytes, size_t len);

/* Pop the next decoded event from the parser queue. Returns 0 with
 * *out filled if an event is available, 1 if the queue is empty. */
int hl_cap_tui_next_event(HlTuiCtx *ctx, HlTuiEvent *out);

/* Best-effort commit any in-flight parser state as an event.
 *
 * Today this only catches the lone-ESC case: if the parser has
 * received a bare ESC byte with no follow-up, this emits a synthetic
 * "escape" key event. Bindings driving input asynchronously call
 * this after a quiet window (typically 50ms since the last byte)
 * to resolve the "lone ESC vs. start of CSI" ambiguity.
 *
 * Returns 1 if a new event was queued, 0 otherwise. */
int hl_cap_tui_commit_idle(HlTuiCtx *ctx);

/* Non-blocking single-read from the input fd into the parser. Reads
 * up to `buf` bytes of stdin into a temporary buffer and feeds the
 * parser. Returns:
 *    >0  number of bytes read + fed
 *     0  fd not readable right now (no work)
 *    -1  read error (errno set)
 *
 * Bindings use this in a loop until it returns <=0, then check
 * next_event to drain decoded events. */
int hl_cap_tui_drain(HlTuiCtx *ctx);

/* Inspect whether the parser is mid-sequence (any non-GROUND state).
 * Bindings use this to decide whether to keep waiting for more
 * bytes vs. commit. */
int hl_cap_tui_parser_pending(HlTuiCtx *ctx);

/* ── Clipboard ──────────────────────────────────────────────────── */

/* Write `text` to the system clipboard via OSC 52. Returns 0 on
 * success, -ENOTSUP if HL_TUI_CAP_OSC52 isn't set. */
int hl_cap_tui_clipboard_set(HlTuiCtx *ctx, const char *text, size_t len);

/* ── Mode controls ──────────────────────────────────────────────── */

/* Toggle SGR mouse reporting (mouse events delivered via poll). */
int hl_cap_tui_enable_mouse(HlTuiCtx *ctx, int on);

/* Toggle bracketed paste. */
int hl_cap_tui_enable_paste(HlTuiCtx *ctx, int on);

/* Toggle focus tracking (focus-in / focus-out events). */
int hl_cap_tui_enable_focus(HlTuiCtx *ctx, int on);

/* Toggle Kitty keyboard protocol (richer modifier reporting). */
int hl_cap_tui_enable_kitty_kbd(HlTuiCtx *ctx, int on);

/* ── Composable-feature seam ────────────────────────────────────────
 *
 * TUI is a "subsystem feature": the whole cap layer + runtime bindings
 * live in libhull_feature-tui.a rather than the base platform lib. The
 * base ships only this weak presence hook (default 0, in cap/tui_feature.c);
 * `hull build --with=tui` composes the feature, whose strong override
 * returns 1. The resolver consults it (build_provided_caps) so a composed
 * binary admits hull/tui at app load even though HL_ENABLE_TUI was never
 * compiled into the base. A monolithic HL_ENABLE_TUI build reports TUI via
 * the compile flag instead; a plain base returns 0 and hull/tui is rejected.
 * (Parallels hl_gpu_feature_backends / hl_db_feature_backends.)
 */
int hl_tui_feature_present(void);

/* Per-runtime registration hooks for the composed tui feature. The base ships
 * weak no-ops (cap/tui_feature.c); libhull_feature-tui.a provides strong
 * overrides that register the native tui bridge (hull._tui / hull:tui). These
 * take runtime types (lua_State* / JSContext* + HlJS*), so the strong overrides
 * live in the feature archive (compiled with full runtime includes), NOT in the
 * generated feature registry. Declared here as void* to avoid pulling lua.h /
 * quickjs.h into every consumer of tui.h; the impl casts back. */
void hl_tui_feature_register_lua(void *lua_state);
int  hl_tui_feature_register_js(void *js_ctx, void *hl_js);

#ifdef __cplusplus
}
#endif

#endif /* HL_CAP_TUI_H */

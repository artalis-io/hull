/*
 * src/hull/cap/tui.c — TUI capability: lifecycle, rendering, OSC.
 *
 * Owns:
 *   • HlTuiCtx allocation + the process-singleton invariant.
 *   • Termios save/restore, alt screen, cursor-visibility, signal
 *     handlers, atexit safety net.
 *   • Shadow + pending cell buffers and cell-level diff rendering.
 *   • OSC 11 theme detection (at acquire) and OSC 52 clipboard write.
 *
 * Defers to src/hull/cap/tui_input.c for ANSI input parsing and the
 * event queue (declared via the internal header tui_internal.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tui.h"
#include "hull/cap/tui_width.h"
#include "tui_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ── Tunables ───────────────────────────────────────────────────── */

#define HL_TUI_OUT_BUF_INITIAL   4096   /* output buffer growth seed */
#define HL_TUI_OUT_FMT_TMP       64     /* out_appendf scratch */
#define HL_TUI_INPUT_CHUNK       256    /* per-read() chunk in poll */
#define HL_TUI_OSC11_TIMEOUT_MS  50     /* theme query response wait */
#define HL_TUI_OSC11_RESP_MAX    64     /* theme query reply ceiling */
#define HL_TUI_TTY_DEFAULT_COLS  80     /* fallback when ioctl returns 0 */
#define HL_TUI_TTY_DEFAULT_ROWS  24
#define HL_TUI_ESC_IDLE_MS       50     /* commit bare ESC after this idle */

/* ── Process-singleton invariant ─────────────────────────────────────
 *
 * The OS-level resources we touch (termios on the controlling tty,
 * the alt-screen + cursor state, signal handlers, atexit registration)
 * are inherently per-process. We expose an HlTuiCtx handle to keep
 * the API symmetric with HlGpuCtx / HlWasmCtx, but enforce at runtime
 * that at most one is live. A second acquire returns -EBUSY. */
static HlTuiCtx *g_singleton = NULL;
static int       g_atexit_registered = 0;

/* SIGWINCH sets this; cap_tui_poll consumes it on next call. */
static volatile sig_atomic_t g_winch_pending = 0;

/* Saved signal handlers (we chain to them after our restore). */
static struct sigaction g_prev_sigint;
static struct sigaction g_prev_sigterm;
static struct sigaction g_prev_sigquit;
static struct sigaction g_prev_sigwinch;
static struct sigaction g_prev_sigtstp;
static struct sigaction g_prev_sigcont;
static int g_signals_installed = 0;

/* ── HlTuiCtx ────────────────────────────────────────────────────── */

struct HlTuiCtx {
    /* fds */
    int in_fd;
    int out_fd;

    /* Saved tty state — restored on release / atexit. */
    struct termios saved_termios;
    int            termios_saved;

    /* Geometry */
    int cols;
    int rows;

    /* Output buffering: one write(2) per flush. */
    char  *out_buf;
    size_t out_len;
    size_t out_cap;

    /* Shadow + pending cell buffers (rows*cols each).
     * Pending = what the app has staged via tui_write/move/style.
     * Shadow = what is currently on screen (per our last flush). */
    HlTuiCell *shadow;
    HlTuiCell *pending;
    size_t     cells_alloc;       /* rows*cols at last realloc */

    /* Pending cursor + style. */
    int      cursor_x;            /* 0-indexed internally */
    int      cursor_y;
    uint32_t cur_fg;
    uint32_t cur_bg;
    uint32_t cur_attr;

    /* Capabilities + theme. */
    uint32_t    caps;
    const char *theme;            /* "dark" | "light" | "unknown" */

    /* Input parser state + event queue (see tui_input.c). */
    HlTuiParser parser;

    /* Mode toggles (mirror what we've sent the terminal). */
    int mouse_on;
    int paste_on;
    int focus_on;
    int kitty_kbd_on;
};

/* ── Forward decls ──────────────────────────────────────────────── */

static int  install_signal_handlers(void);
static void restore_signal_handlers(void);
static int  enter_alt_screen(HlTuiCtx *ctx);
static int  leave_alt_screen(HlTuiCtx *ctx);
static int  update_size(HlTuiCtx *ctx);
static int  alloc_cells(HlTuiCtx *ctx);
static int  raw_write(int fd, const char *s, size_t n);
static int  detect_caps(HlTuiCtx *ctx);
static void detect_theme(HlTuiCtx *ctx);

/* ── Output buffer ──────────────────────────────────────────────── */

static int out_reserve(HlTuiCtx *ctx, size_t extra)
{
    if (ctx->out_len + extra <= ctx->out_cap) return 0;
    size_t new_cap = ctx->out_cap ? ctx->out_cap : HL_TUI_OUT_BUF_INITIAL;
    while (new_cap < ctx->out_len + extra) {
        if (new_cap > SIZE_MAX / 2) { errno = ENOMEM; return -1; }
        new_cap *= 2;
    }
    char *nb = realloc(ctx->out_buf, new_cap);
    if (!nb) { errno = ENOMEM; return -1; }
    ctx->out_buf = nb;
    ctx->out_cap = new_cap;
    return 0;
}

static int out_append(HlTuiCtx *ctx, const char *s, size_t n)
{
    if (out_reserve(ctx, n) < 0) return -1;
    memcpy(ctx->out_buf + ctx->out_len, s, n);
    ctx->out_len += n;
    return 0;
}

__attribute__((format(printf, 2, 3)))
static int out_appendf(HlTuiCtx *ctx, const char *fmt, ...)
{
    char buf[HL_TUI_OUT_FMT_TMP];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) return -1;
    return out_append(ctx, buf, (size_t)n);
}

/* ── Signal handling ────────────────────────────────────────────── */

static void on_sigwinch(int sig)
{
    (void)sig;
    g_winch_pending = 1;
    /* Chain to whatever was installed before us. */
    if (g_prev_sigwinch.sa_handler != SIG_DFL &&
        g_prev_sigwinch.sa_handler != SIG_IGN) {
        if (g_prev_sigwinch.sa_flags & SA_SIGINFO) {
            /* sa_sigaction takes (int, siginfo_t*, void*); we don't
             * have those, so skip — chaining for sigaction-style
             * handlers is unsafe from a plain handler. */
        } else if (g_prev_sigwinch.sa_handler) {
            g_prev_sigwinch.sa_handler(sig);
        }
    }
}

static void on_fatal_signal(int sig)
{
    /* Restore terminal, then chain to previous handler (which is
     * typically the default action — coredump / terminate). */
    hl_cap_tui_force_leave();
    struct sigaction *prev = NULL;
    switch (sig) {
    case SIGINT:  prev = &g_prev_sigint;  break;
    case SIGTERM: prev = &g_prev_sigterm; break;
    case SIGQUIT: prev = &g_prev_sigquit; break;
    default: break;
    }
    if (prev) {
        if (prev->sa_handler == SIG_DFL || prev->sa_handler == SIG_IGN) {
            /* Reinstall default and re-raise. */
            struct sigaction dfl = {0};
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(sig, &dfl, NULL);
            raise(sig);
        } else if (prev->sa_handler) {
            prev->sa_handler(sig);
        }
    } else {
        _exit(128 + sig);
    }
}

static void on_sigtstp(int sig)
{
    (void)sig;
    /* Leave alt screen + restore termios so the parent shell sees a
     * normal terminal when we're suspended. */
    if (g_singleton) leave_alt_screen(g_singleton);
    /* Reinstall default SIGTSTP and re-raise so the kernel actually
     * stops us. */
    struct sigaction dfl = {0};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGTSTP, &dfl, NULL);
    raise(SIGTSTP);
}

static void on_sigcont(int sig)
{
    (void)sig;
    /* Reinstall our SIGTSTP handler (the default we used to stop has
     * to be replaced). */
    struct sigaction tstp = {0};
    tstp.sa_handler = on_sigtstp;
    sigemptyset(&tstp.sa_mask);
    sigaction(SIGTSTP, &tstp, NULL);

    if (g_singleton) {
        enter_alt_screen(g_singleton);
        hl_cap_tui_invalidate(g_singleton);
        /* Flush is the caller's job — we just mark for repaint. */
        g_winch_pending = 1; /* re-probe size, just in case */
    }
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_fatal_signal;
    if (sigaction(SIGINT,  &sa, &g_prev_sigint)  < 0) return -1;
    if (sigaction(SIGTERM, &sa, &g_prev_sigterm) < 0) return -1;
    if (sigaction(SIGQUIT, &sa, &g_prev_sigquit) < 0) return -1;

    sa.sa_handler = on_sigwinch;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGWINCH, &sa, &g_prev_sigwinch) < 0) return -1;

    sa.sa_handler = on_sigtstp;
    sa.sa_flags = 0;
    if (sigaction(SIGTSTP, &sa, &g_prev_sigtstp) < 0) return -1;

    sa.sa_handler = on_sigcont;
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCONT, &sa, &g_prev_sigcont) < 0) return -1;

    g_signals_installed = 1;
    return 0;
}

static void restore_signal_handlers(void)
{
    if (!g_signals_installed) return;
    sigaction(SIGINT,   &g_prev_sigint,   NULL);
    sigaction(SIGTERM,  &g_prev_sigterm,  NULL);
    sigaction(SIGQUIT,  &g_prev_sigquit,  NULL);
    sigaction(SIGWINCH, &g_prev_sigwinch, NULL);
    sigaction(SIGTSTP,  &g_prev_sigtstp,  NULL);
    sigaction(SIGCONT,  &g_prev_sigcont,  NULL);
    g_signals_installed = 0;
}

/* ── Termios / alt screen ───────────────────────────────────────── */

static int set_raw_mode(HlTuiCtx *ctx)
{
    if (tcgetattr(ctx->in_fd, &ctx->saved_termios) < 0) return -1;
    ctx->termios_saved = 1;

    struct termios raw = ctx->saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |=  (CS8);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(ctx->in_fd, TCSAFLUSH, &raw) < 0) return -1;
    return 0;
}

static void restore_termios(HlTuiCtx *ctx)
{
    if (!ctx || !ctx->termios_saved) return;
    tcsetattr(ctx->in_fd, TCSAFLUSH, &ctx->saved_termios);
    ctx->termios_saved = 0;
}

static int enter_alt_screen(HlTuiCtx *ctx)
{
    /* CSI ? 1049 h = save screen + enter alt
     * CSI ? 25 l   = hide cursor
     * CSI 2 J      = clear screen
     * CSI H        = home cursor                                  */
    static const char seq[] = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H";
    return raw_write(ctx->out_fd, seq, sizeof seq - 1);
}

static int leave_alt_screen(HlTuiCtx *ctx)
{
    /* Reset attributes, show cursor, leave alt screen. */
    static const char seq[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
    if (ctx->mouse_on) {
        static const char m[] = "\x1b[?1006l\x1b[?1002l\x1b[?1000l";
        raw_write(ctx->out_fd, m, sizeof m - 1);
        ctx->mouse_on = 0;
    }
    if (ctx->paste_on) {
        static const char p[] = "\x1b[?2004l";
        raw_write(ctx->out_fd, p, sizeof p - 1);
        ctx->paste_on = 0;
    }
    if (ctx->focus_on) {
        static const char f[] = "\x1b[?1004l";
        raw_write(ctx->out_fd, f, sizeof f - 1);
        ctx->focus_on = 0;
    }
    if (ctx->kitty_kbd_on) {
        static const char k[] = "\x1b[<u";
        raw_write(ctx->out_fd, k, sizeof k - 1);
        ctx->kitty_kbd_on = 0;
    }
    return raw_write(ctx->out_fd, seq, sizeof seq - 1);
}

static int raw_write(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        s += w; n -= (size_t)w;
    }
    return 0;
}

/* ── Geometry + cell buffers ────────────────────────────────────── */

static int update_size(HlTuiCtx *ctx)
{
    struct winsize ws = {0};
    if (ioctl(ctx->out_fd, TIOCGWINSZ, &ws) < 0) return -1;
    if (ws.ws_col == 0) ws.ws_col = HL_TUI_TTY_DEFAULT_COLS;
    if (ws.ws_row == 0) ws.ws_row = HL_TUI_TTY_DEFAULT_ROWS;
    ctx->cols = ws.ws_col;
    ctx->rows = ws.ws_row;
    return alloc_cells(ctx);
}

static int alloc_cells(HlTuiCtx *ctx)
{
    size_t need = (size_t)ctx->cols * (size_t)ctx->rows;
    if (need == 0) return 0;
    if (need == ctx->cells_alloc) return 0;

    HlTuiCell *new_shadow  = calloc(need, sizeof(HlTuiCell));
    HlTuiCell *new_pending = calloc(need, sizeof(HlTuiCell));
    if (!new_shadow || !new_pending) {
        free(new_shadow); free(new_pending);
        errno = ENOMEM;
        return -1;
    }
    /* On resize, the previous buffers are stale w.r.t. screen layout.
     * Drop both and let the next flush emit everything fresh. */
    free(ctx->shadow);
    free(ctx->pending);
    ctx->shadow  = new_shadow;
    ctx->pending = new_pending;
    ctx->cells_alloc = need;
    return 0;
}

static inline HlTuiCell *cell_at(HlTuiCell *buf, int cols, int x, int y)
{
    return &buf[(size_t)y * (size_t)cols + (size_t)x];
}

/* ── Capability + theme detection ────────────────────────────────── */

static int detect_caps(HlTuiCtx *ctx)
{
    uint32_t caps = HL_TUI_CAP_BASIC_COLOR;
    const char *term      = getenv("TERM");
    const char *colorterm = getenv("COLORTERM");
    const char *no_color  = getenv("NO_COLOR");

    if (term && (strstr(term, "256color") || strstr(term, "direct")))
        caps |= HL_TUI_CAP_256COLOR;
    if (colorterm && (strstr(colorterm, "truecolor") || strstr(colorterm, "24bit"))) {
        caps |= HL_TUI_CAP_TRUECOLOR | HL_TUI_CAP_256COLOR;
    }
    if (no_color && no_color[0] != '\0') {
        caps = 0;
    }

    /* OSC 52 is widely supported by modern terminals; we advertise
     * unconditionally and let the terminal silently ignore if not. */
    caps |= HL_TUI_CAP_OSC52;

    /* Mouse / focus / kitty kbd are gated on opt-in mode toggles. */
    caps |= HL_TUI_CAP_MOUSE | HL_TUI_CAP_FOCUS;

    ctx->caps = caps;
    return 0;
}

/* Issue OSC 11 query, wait up to 50ms for response, parse luminance.
 * Tolerant of no response — falls back through $COLORFGBG then a
 * "dark" default. */
static void detect_theme(HlTuiCtx *ctx)
{
    const char *no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0') { ctx->theme = "unknown"; return; }

    /* OSC 11 query. The terminal echoes back "\x1b]11;rgb:RRRR/GGGG/BBBB\x07"
     * (or "\x1b\\" terminator). */
    static const char query[] = "\x1b]11;?\x07";
    if (raw_write(ctx->out_fd, query, sizeof query - 1) < 0) goto fallback;

    struct pollfd pfd = { .fd = ctx->in_fd, .events = POLLIN };
    char  resp[HL_TUI_OSC11_RESP_MAX] = {0};
    size_t got = 0;
    while (got < sizeof resp - 1) {
        int pr = poll(&pfd, 1, HL_TUI_OSC11_TIMEOUT_MS);
        if (pr <= 0) break;
        ssize_t n = read(ctx->in_fd, resp + got, sizeof resp - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
        if (memchr(resp, '\x07', got) || memchr(resp, '\\', got)) break;
    }
    if (got == 0) goto fallback;

    /* Find "rgb:" — accept either ":RGB:RGB:RGB" or ":RRRR/GGGG/BBBB". */
    const char *p = strstr(resp, "rgb:");
    if (!p) goto fallback;
    p += 4;
    unsigned r = 0, g = 0, b = 0;
    if (sscanf(p, "%4x/%4x/%4x", &r, &g, &b) == 3) {
        /* 16-bit values; collapse to 8-bit. */
        if (r > 0xFF) r >>= 8;
        if (g > 0xFF) g >>= 8;
        if (b > 0xFF) b >>= 8;
        /* Rec. 601 luma: Y = 0.299R + 0.587G + 0.114B. */
        unsigned y = (299u*r + 587u*g + 114u*b) / 1000u;
        ctx->theme = (y < 128) ? "dark" : "light";
        return;
    }

fallback:
    {
        const char *fgbg = getenv("COLORFGBG");
        if (fgbg) {
            const char *semi = strchr(fgbg, ';');
            if (semi && semi[1] != '\0') {
                char *endptr = NULL;
                errno = 0;
                long bg = strtol(semi + 1, &endptr, 10);
                /* Only treat as a valid index if at least one digit was
                 * consumed (endptr advanced) and no overflow occurred.
                 * Anything else falls through to the default "dark". */
                if (endptr != semi + 1 && errno == 0
                        && bg >= 0 && bg <= 6) {
                    ctx->theme = "dark";
                    return;
                }
                if (endptr != semi + 1 && errno == 0
                        && bg >= 7 && bg <= 15) {
                    ctx->theme = "light";
                    return;
                }
            }
        }
    }
    ctx->theme = "dark";
}

/* ── Public API: lifecycle ──────────────────────────────────────── */

int hl_cap_tui_acquire(HlTuiCtx **out)
{
    if (!out) { errno = EINVAL; return -1; }
    if (g_singleton) { errno = EBUSY; return -1; }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        errno = ENOTTY;
        return -1;
    }

    HlTuiCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) { errno = ENOMEM; return -1; }

    ctx->in_fd  = STDIN_FILENO;
    ctx->out_fd = STDOUT_FILENO;
    ctx->cur_fg = HL_TUI_COLOR_DEFAULT;
    ctx->cur_bg = HL_TUI_COLOR_DEFAULT;
    ctx->theme  = "unknown";

    hl_tui_parser_init(&ctx->parser);

    if (set_raw_mode(ctx) < 0) goto fail;
    if (install_signal_handlers() < 0) goto fail;

    /* Probe size BEFORE entering alt screen so a failure here leaves
     * the user's primary screen untouched. */
    if (update_size(ctx) < 0) goto fail;

    detect_caps(ctx);
    detect_theme(ctx);  /* runs OSC 11 query while we still control the screen */

    if (enter_alt_screen(ctx) < 0) goto fail;

    if (!g_atexit_registered) {
        atexit(hl_cap_tui_force_leave);
        g_atexit_registered = 1;
    }

    g_singleton = ctx;
    *out = ctx;
    return 0;

fail:
    {
        int saved = errno;
        restore_signal_handlers();
        restore_termios(ctx);
        free(ctx->shadow);
        free(ctx->pending);
        free(ctx->out_buf);
        free(ctx);
        errno = saved;
        return -1;
    }
}

int hl_cap_tui_release(HlTuiCtx *ctx)
{
    if (!ctx) return 0;
    if (ctx != g_singleton) { errno = EINVAL; return -1; }

    leave_alt_screen(ctx);
    restore_signal_handlers();
    restore_termios(ctx);

    free(ctx->shadow);
    free(ctx->pending);
    free(ctx->out_buf);
    free(ctx);
    g_singleton = NULL;
    return 0;
}

void hl_cap_tui_force_leave(void)
{
    HlTuiCtx *ctx = g_singleton;
    if (!ctx) return;
    /* No locking, no allocations — we may be in a signal handler. */
    leave_alt_screen(ctx);
    restore_termios(ctx);
    /* Do not free; process is exiting. */
}

/* ── Public API: query ──────────────────────────────────────────── */

int hl_cap_tui_size(HlTuiCtx *ctx, int *cols, int *rows)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (cols) *cols = ctx->cols;
    if (rows) *rows = ctx->rows;
    return 0;
}

uint32_t hl_cap_tui_caps(HlTuiCtx *ctx)
{
    return ctx ? ctx->caps : 0;
}

const char *hl_cap_tui_theme(HlTuiCtx *ctx)
{
    return ctx ? ctx->theme : "unknown";
}

int hl_cap_tui_input_fd(HlTuiCtx *ctx)
{
    return ctx ? ctx->in_fd : -1;
}

/* ── Public API: rendering ──────────────────────────────────────── */

int hl_cap_tui_clear(HlTuiCtx *ctx)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (ctx->pending && ctx->cells_alloc) {
        memset(ctx->pending, 0, ctx->cells_alloc * sizeof(HlTuiCell));
    }
    ctx->cursor_x = 0;
    ctx->cursor_y = 0;
    return 0;
}

int hl_cap_tui_invalidate(HlTuiCtx *ctx)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (ctx->shadow && ctx->cells_alloc) {
        memset(ctx->shadow, 0, ctx->cells_alloc * sizeof(HlTuiCell));
    }
    return 0;
}

int hl_cap_tui_move(HlTuiCtx *ctx, int x, int y)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > ctx->cols) x = ctx->cols;
    if (y > ctx->rows) y = ctx->rows;
    ctx->cursor_x = x - 1;
    ctx->cursor_y = y - 1;
    return 0;
}

int hl_cap_tui_style(HlTuiCtx *ctx, uint32_t fg, uint32_t bg, uint32_t attr)
{
    if (!ctx) { errno = EINVAL; return -1; }
    ctx->cur_fg   = fg;
    ctx->cur_bg   = bg;
    ctx->cur_attr = attr;
    return 0;
}

int hl_cap_tui_write(HlTuiCtx *ctx, const char *s, size_t len)
{
    if (!ctx || !s) { errno = EINVAL; return -1; }
    if (!ctx->pending) return 0;

    size_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        int adv = hl_tui_utf8_decode(s + i, len - i, &cp);
        if (adv <= 0) break;
        i += (size_t)adv;
        if (cp == 0xFFFD) continue;

        /* Strip C0 / DEL / C1 control codepoints. These would otherwise
         * survive into the cell buffer and be re-emitted byte-for-byte
         * by the flush path, letting untrusted text (log lines, file
         * contents, error messages) inject ANSI escape sequences that
         * desync the cell-diff renderer's coordinate tracking. The
         * cap layer is the right enforcement boundary — every caller
         * (Lua, JS, stdlib middleware) routes through here. We treat
         * sanitized codepoints as no-ops rather than 0xFFFD glyphs so
         * untrusted input doesn't visually pollute the UI. */
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F))
            continue;

        int w = hl_tui_cp_width(cp);
        if (w == 0) {
            /* Combining mark — attach to preceding cell when possible. */
            if (ctx->cursor_x > 0) {
                HlTuiCell *c = cell_at(ctx->pending, ctx->cols,
                                       ctx->cursor_x - 1, ctx->cursor_y);
                if (c->combining_count < HL_TUI_MAX_COMBINING) {
                    c->combining[c->combining_count++] = cp;
                }
            }
            continue;
        }

        if (ctx->cursor_y >= ctx->rows) break;
        if (ctx->cursor_x + w > ctx->cols) {
            /* Don't split a wide char across the edge — wrap. */
            ctx->cursor_x = 0;
            ctx->cursor_y++;
            if (ctx->cursor_y >= ctx->rows) break;
        }

        HlTuiCell *c = cell_at(ctx->pending, ctx->cols,
                               ctx->cursor_x, ctx->cursor_y);
        c->codepoint = cp;
        c->width     = (uint8_t)w;
        c->fg        = ctx->cur_fg;
        c->bg        = ctx->cur_bg;
        c->attr      = ctx->cur_attr;
        c->combining_count = 0;

        /* Second cell of a wide char: continuation marker (width=0,
         * codepoint=0). Diff treats width==0 + codepoint==0 in the
         * pending buffer as a continuation. */
        if (w == 2 && ctx->cursor_x + 1 < ctx->cols) {
            HlTuiCell *cont = cell_at(ctx->pending, ctx->cols,
                                      ctx->cursor_x + 1, ctx->cursor_y);
            cont->codepoint = 0;
            cont->width     = 0;
            cont->fg        = ctx->cur_fg;
            cont->bg        = ctx->cur_bg;
            cont->attr      = ctx->cur_attr;
            cont->combining_count = 0;
        }

        ctx->cursor_x += w;
        if (ctx->cursor_x >= ctx->cols) {
            ctx->cursor_x = 0;
            ctx->cursor_y++;
        }
    }
    return 0;
}

/* Encode a codepoint as UTF-8 into `out` (at least 4 bytes). */
static size_t utf8_encode(uint32_t cp, char *out)
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

static int emit_sgr(HlTuiCtx *ctx, uint32_t fg, uint32_t bg, uint32_t attr)
{
    /* Reset first, then apply. Compact: \x1b[0;...m */
    if (out_append(ctx, "\x1b[0", 3) < 0) return -1;

    if (attr & HL_TUI_ATTR_BOLD)      if (out_append(ctx, ";1", 2) < 0) return -1;
    if (attr & HL_TUI_ATTR_DIM)       if (out_append(ctx, ";2", 2) < 0) return -1;
    if (attr & HL_TUI_ATTR_ITALIC)    if (out_append(ctx, ";3", 2) < 0) return -1;
    if (attr & HL_TUI_ATTR_UNDERLINE) if (out_append(ctx, ";4", 2) < 0) return -1;
    if (attr & HL_TUI_ATTR_REVERSE)   if (out_append(ctx, ";7", 2) < 0) return -1;
    if (attr & HL_TUI_ATTR_STRIKE)    if (out_append(ctx, ";9", 2) < 0) return -1;

    if (fg != HL_TUI_COLOR_DEFAULT) {
        uint8_t tag = (uint8_t)(fg >> 24);
        if (tag == 0x00) {
            uint8_t r = (fg >> 16) & 0xFF, g = (fg >> 8) & 0xFF, b = fg & 0xFF;
            if (out_appendf(ctx, ";38;2;%u;%u;%u", r, g, b) < 0) return -1;
        } else if (tag == 0x01) {
            if (out_appendf(ctx, ";38;5;%u", (unsigned)(fg & 0xFF)) < 0) return -1;
        } else if (tag == 0x02) {
            unsigned idx = fg & 0x0F;
            unsigned code = (idx < 8) ? (30 + idx) : (90 + idx - 8);
            if (out_appendf(ctx, ";%u", code) < 0) return -1;
        }
    }
    if (bg != HL_TUI_COLOR_DEFAULT) {
        uint8_t tag = (uint8_t)(bg >> 24);
        if (tag == 0x00) {
            uint8_t r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
            if (out_appendf(ctx, ";48;2;%u;%u;%u", r, g, b) < 0) return -1;
        } else if (tag == 0x01) {
            if (out_appendf(ctx, ";48;5;%u", (unsigned)(bg & 0xFF)) < 0) return -1;
        } else if (tag == 0x02) {
            unsigned idx = bg & 0x0F;
            unsigned code = (idx < 8) ? (40 + idx) : (100 + idx - 8);
            if (out_appendf(ctx, ";%u", code) < 0) return -1;
        }
    }
    return out_append(ctx, "m", 1);
}

static int cell_eq(const HlTuiCell *a, const HlTuiCell *b)
{
    if (a->codepoint != b->codepoint) return 0;
    if (a->width     != b->width)     return 0;
    if (a->fg        != b->fg)        return 0;
    if (a->bg        != b->bg)        return 0;
    if (a->attr      != b->attr)      return 0;
    if (a->combining_count != b->combining_count) return 0;
    for (int i = 0; i < a->combining_count; i++)
        if (a->combining[i] != b->combining[i]) return 0;
    return 1;
}

int hl_cap_tui_flush(HlTuiCtx *ctx)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (!ctx->shadow || !ctx->pending) return 0;

    ctx->out_len = 0;
    uint32_t last_fg = ~0u, last_bg = ~0u, last_attr = ~0u;
    int last_x = -2, last_y = -2;

    for (int y = 0; y < ctx->rows; y++) {
        for (int x = 0; x < ctx->cols; x++) {
            HlTuiCell *p = cell_at(ctx->pending, ctx->cols, x, y);
            HlTuiCell *s = cell_at(ctx->shadow,  ctx->cols, x, y);

            if (cell_eq(p, s)) continue;

            /* Move cursor if needed. */
            if (y != last_y || x != last_x) {
                if (out_appendf(ctx, "\x1b[%d;%dH", y + 1, x + 1) < 0) return -1;
            }

            /* Apply style if changed. */
            if (p->fg != last_fg || p->bg != last_bg || p->attr != last_attr) {
                if (emit_sgr(ctx, p->fg, p->bg, p->attr) < 0) return -1;
                last_fg = p->fg; last_bg = p->bg; last_attr = p->attr;
            }

            /* Emit the codepoint (or space if cleared). */
            uint32_t cp = p->codepoint ? p->codepoint : 0x20;
            char enc[8];
            size_t n = utf8_encode(cp, enc);
            if (out_append(ctx, enc, n) < 0) return -1;
            /* Combining marks. */
            for (int i = 0; i < p->combining_count; i++) {
                n = utf8_encode(p->combining[i], enc);
                if (out_append(ctx, enc, n) < 0) return -1;
            }

            /* Advance the "last drawn cell" tracker. Wide chars
             * occupy two columns; the continuation cell is implied. */
            int adv = p->width ? p->width : 1;
            last_x = x + adv;
            last_y = y;

            *s = *p;

            /* Skip continuation cell — pending stores width=0 in the
             * second half, but it shouldn't generate a separate
             * write. */
            if (p->width == 2 && x + 1 < ctx->cols) {
                HlTuiCell *p2 = cell_at(ctx->pending, ctx->cols, x + 1, y);
                HlTuiCell *s2 = cell_at(ctx->shadow,  ctx->cols, x + 1, y);
                *s2 = *p2;
                x++;
            }
        }
    }

    /* Position the visible cursor at the pending cursor location. */
    if (out_appendf(ctx, "\x1b[%d;%dH", ctx->cursor_y + 1, ctx->cursor_x + 1) < 0)
        return -1;

    if (ctx->out_len > 0) {
        if (raw_write(ctx->out_fd, ctx->out_buf, ctx->out_len) < 0) return -1;
        ctx->out_len = 0;
    }
    return 0;
}

/* ── Public API: input ──────────────────────────────────────────── */

/* Consume a pending SIGWINCH (if any) by re-querying tty size and
 * pushing a resize event onto the parser's queue. Returns 1 if an
 * event was enqueued, 0 otherwise. */
static int consume_winch(HlTuiCtx *ctx)
{
    if (!g_winch_pending) return 0;
    g_winch_pending = 0;
    int old_cols = ctx->cols, old_rows = ctx->rows;
    if (update_size(ctx) < 0) return 0;
    if (old_cols == ctx->cols && old_rows == ctx->rows) return 0;
    hl_tui_parser_push_resize(&ctx->parser, ctx->cols, ctx->rows);
    return 1;
}

int hl_cap_tui_feed(HlTuiCtx *ctx, const char *bytes, size_t len)
{
    if (!ctx || (!bytes && len)) { errno = EINVAL; return -1; }
    hl_tui_parser_feed(&ctx->parser, bytes, len);
    return 0;
}

int hl_cap_tui_next_event(HlTuiCtx *ctx, HlTuiEvent *out)
{
    if (!ctx || !out) { errno = EINVAL; return -1; }
    consume_winch(ctx);
    return hl_tui_parser_pop(&ctx->parser, out);
}

int hl_cap_tui_commit_idle(HlTuiCtx *ctx)
{
    if (!ctx) { errno = EINVAL; return 0; }
    return hl_tui_parser_flush_idle(&ctx->parser);
}

int hl_cap_tui_drain(HlTuiCtx *ctx)
{
    if (!ctx) { errno = EINVAL; return -1; }
    /* Non-blocking probe: poll with zero timeout. If no data, return 0
     * — that's the "fd not readable" signal the caller uses to switch
     * to event-loop suspension. */
    struct pollfd pfd = { .fd = ctx->in_fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 0);
    if (pr <= 0) return 0;

    char buf[HL_TUI_INPUT_CHUNK];
    ssize_t n = read(ctx->in_fd, buf, sizeof buf);
    if (n < 0) { if (errno == EINTR) return 0; return -1; }
    if (n == 0) return 0;  /* EOF — caller treats as no work */
    hl_tui_parser_feed(&ctx->parser, buf, (size_t)n);
    return (int)n;
}

int hl_cap_tui_parser_pending(HlTuiCtx *ctx)
{
    if (!ctx) return 0;
    return ctx->parser.state != HL_TUI_PS_GROUND;
}

int hl_cap_tui_poll(HlTuiCtx *ctx, int timeout_ms, HlTuiEvent *out)
{
    if (!ctx || !out) { errno = EINVAL; return -1; }
    out->kind = HL_TUI_EV_NONE;
    out->key = NULL; out->text = NULL;

    /* Drain any already-decoded events first. */
    consume_winch(ctx);
    if (hl_tui_parser_pop(&ctx->parser, out) == 0) return 0;

    struct pollfd pfd = { .fd = ctx->in_fd, .events = POLLIN };
    /* `remaining` is the user's wall-clock budget. We poll with the
     * smaller of `remaining` and HL_TUI_ESC_IDLE_MS so a parser stuck
     * in ESC state can commit a bare-escape after a short quiet
     * window. */
    int remaining = timeout_ms;
    for (;;) {
        int slice;
        if (remaining < 0) {
            slice = HL_TUI_ESC_IDLE_MS;
        } else if (remaining < HL_TUI_ESC_IDLE_MS) {
            slice = remaining;
        } else {
            slice = HL_TUI_ESC_IDLE_MS;
        }

        int pr = poll(&pfd, 1, slice);
        if (pr < 0) {
            if (errno == EINTR) {
                if (consume_winch(ctx) &&
                    hl_tui_parser_pop(&ctx->parser, out) == 0) return 0;
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            /* Slice elapsed with no data. Give the parser a chance
             * to commit any in-flight state (currently: bare ESC). */
            if (hl_tui_parser_flush_idle(&ctx->parser) &&
                hl_tui_parser_pop(&ctx->parser, out) == 0) return 0;
            if (consume_winch(ctx) &&
                hl_tui_parser_pop(&ctx->parser, out) == 0) return 0;
            if (remaining >= 0) {
                remaining -= slice;
                if (remaining <= 0) return 1;
            }
            continue;
        }

        /* Readable: drain. */
        char buf[HL_TUI_INPUT_CHUNK];
        ssize_t n = read(ctx->in_fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return 1;
        hl_tui_parser_feed(&ctx->parser, buf, (size_t)n);
        consume_winch(ctx);
        if (hl_tui_parser_pop(&ctx->parser, out) == 0) return 0;
        /* No complete event yet (partial CSI / ESC). Keep polling
         * with a fresh idle slice — flush_idle handles the lone-ESC
         * case below. */
    }
}

/* ── Public API: clipboard ──────────────────────────────────────── */

/* RFC 4648 Base64 encode. Writes 4 chars per 3 bytes, no newlines. */
static size_t b64_encode(const char *in, size_t n, char *out)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)(unsigned char)in[i]   << 16) |
                     ((uint32_t)(unsigned char)in[i+1] <<  8) |
                     ((uint32_t)(unsigned char)in[i+2]);
        out[o++] = tab[(v >> 18) & 0x3F];
        out[o++] = tab[(v >> 12) & 0x3F];
        out[o++] = tab[(v >>  6) & 0x3F];
        out[o++] = tab[ v        & 0x3F];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)(unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)(unsigned char)in[i+1] << 8;
        out[o++] = tab[(v >> 18) & 0x3F];
        out[o++] = tab[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? tab[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    return o;
}

int hl_cap_tui_clipboard_set(HlTuiCtx *ctx, const char *text, size_t len)
{
    if (!ctx || (!text && len)) { errno = EINVAL; return -1; }
    if (!(ctx->caps & HL_TUI_CAP_OSC52)) { errno = ENOTSUP; return -1; }

    /* OSC 52 ; c ; <b64> BEL */
    size_t b64_max = ((len + 2) / 3) * 4 + 1;
    size_t total   = 8 /*prefix*/ + b64_max + 1 /*BEL*/;
    char *buf = malloc(total);
    if (!buf) { errno = ENOMEM; return -1; }
    memcpy(buf, "\x1b]52;c;", 7);
    size_t enc_len = b64_encode(text, len, buf + 7);
    buf[7 + enc_len] = '\x07';
    int rc = raw_write(ctx->out_fd, buf, 7 + enc_len + 1);
    free(buf);
    return rc;
}

/* ── Public API: mode toggles ───────────────────────────────────── */

int hl_cap_tui_enable_mouse(HlTuiCtx *ctx, int on)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if ((!!on) == ctx->mouse_on) return 0;
    const char *seq = on
        ? "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
        : "\x1b[?1006l\x1b[?1002l\x1b[?1000l";
    if (raw_write(ctx->out_fd, seq, strlen(seq)) < 0) return -1;
    ctx->mouse_on = !!on;
    return 0;
}

int hl_cap_tui_enable_paste(HlTuiCtx *ctx, int on)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if ((!!on) == ctx->paste_on) return 0;
    const char *seq = on ? "\x1b[?2004h" : "\x1b[?2004l";
    if (raw_write(ctx->out_fd, seq, strlen(seq)) < 0) return -1;
    ctx->paste_on = !!on;
    return 0;
}

int hl_cap_tui_enable_focus(HlTuiCtx *ctx, int on)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if ((!!on) == ctx->focus_on) return 0;
    const char *seq = on ? "\x1b[?1004h" : "\x1b[?1004l";
    if (raw_write(ctx->out_fd, seq, strlen(seq)) < 0) return -1;
    ctx->focus_on = !!on;
    return 0;
}

int hl_cap_tui_enable_kitty_kbd(HlTuiCtx *ctx, int on)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if ((!!on) == ctx->kitty_kbd_on) return 0;
    /* Push (enable level 0xF) / pop (disable). */
    const char *seq = on ? "\x1b[>15u" : "\x1b[<u";
    if (raw_write(ctx->out_fd, seq, strlen(seq)) < 0) return -1;
    ctx->kitty_kbd_on = !!on;
    return 0;
}

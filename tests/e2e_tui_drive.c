/*
 * e2e_tui_drive.c - PTY-based test harness for TUI examples.
 *
 * Spawns a child process inside a fresh PTY, sends a scripted
 * sequence of keystrokes, then waits for a marker on stdout. The
 * intent is end-to-end validation that
 *   hull run examples/tui_picker/app.lua < pty | drive(arrow, enter)
 * actually produces the expected pick on stdout.
 *
 * Usage:
 *   e2e_tui_drive <expect> <input-spec> -- <cmd> [args...]
 *
 *   <expect>       - substring that must appear in the child's stdout
 *                    AFTER the input is delivered. Sets exit 0 on
 *                    match, 1 on timeout/mismatch.
 *
 *   <input-spec>   - string with embedded escape sequences. Special
 *                    forms:
 *                       %d  → ESC [ B  (down arrow)
 *                       %u  → ESC [ A  (up arrow)
 *                       %r  → \r       (enter)
 *                       %e  → ESC      (escape)
 *                       %q  → 'q'      (quit)
 *                       %sN → sleep N millis (drains output)
 *                       %%  → literal '%'
 *                    Anything else is sent verbatim.
 *
 * Drains output continuously into stdout (so the test can grep
 * after), and writes the input only after the first frame has been
 * received (heuristic: 200ms of quiet OR seeing the alt-screen
 * exit sequence).
 *
 * Exits 0 on expect-match, 1 on mismatch/timeout, 2 on usage error,
 * 3 on internal failure.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <util.h>
#define HAVE_FORKPTY 1
#endif

/* ── Tunables ─────────────────────────────────────────────────── */

#define DRAIN_QUIET_MS       200      /* idle before first input */
#define DRAIN_POLL_MS        50       /* per-poll slice in drain */
#define OVERALL_DEADLINE_MS  8000     /* hard cap on total run time */
#define READ_CHUNK_BYTES     4096

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <expect> <input-spec> -- <cmd> [args...]\n", argv0);
}

#ifndef HAVE_FORKPTY
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "SKIP: no forkpty on this platform\n");
    return 77;   /* automake "skip" convention */
}
#else

/* Walk the input spec sequentially, writing bytes and sleeping as
 * specified. This is what makes the `%s N` token mean "sleep here"
 * rather than "sleep after everything else is sent" - important when
 * the test wants the child to make progress (async ticks etc.)
 * between key presses.
 *
 * Returns 0 on success, -1 on write/sleep failure. */
static int dispatch_input(int master, const char *spec)
{
    char chunk[8];
    size_t cn;
    for (size_t i = 0; spec[i]; i++) {
        char c = spec[i];
        cn = 0;
        if (c != '%') { chunk[cn++] = c; }
        else {
            char n = spec[++i];
            if (!n) break;
            switch (n) {
            case 'd': chunk[cn++]='\x1b'; chunk[cn++]='['; chunk[cn++]='B'; break;
            case 'u': chunk[cn++]='\x1b'; chunk[cn++]='['; chunk[cn++]='A'; break;
            case 'l': chunk[cn++]='\x1b'; chunk[cn++]='['; chunk[cn++]='D'; break;
            case 'R': chunk[cn++]='\x1b'; chunk[cn++]='['; chunk[cn++]='C'; break;
            case 'r': chunk[cn++]='\r'; break;
            case 'e': chunk[cn++]='\x1b'; break;
            case 'q': chunk[cn++]='q'; break;
            case '%': chunk[cn++]='%'; break;
            case 's': {
                int ms = 0;
                while (spec[i+1] >= '0' && spec[i+1] <= '9') {
                    ms = ms * 10 + (spec[++i] - '0');
                }
                if (ms > 0) {
                    struct timespec ts = {
                        .tv_sec  = ms / 1000,
                        .tv_nsec = (ms % 1000) * 1000000L,
                    };
                    if (nanosleep(&ts, NULL) != 0 && errno != EINTR) return -1;
                }
                continue;
            }
            default: chunk[cn++] = n; break;
            }
        }
        if (cn > 0 && write(master, chunk, cn) < 0) return -1;
    }
    return 0;
}

static int millis_since_epoch_ms(long long *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    *out = (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    return 0;
}

/* Look for the alt-screen enter sequence in the accumulated buffer.
 * Reliable marker that the child has finished termios setup and is
 * ready to accept raw keystrokes - guards against the race where
 * input is delivered while stdin is still in canonical/echo mode. */
static int saw_alt_screen(const char *buf, size_t len)
{
    static const char marker[] = "\x1b[?1049h";
    if (len < sizeof marker - 1) return 0;
    for (size_t i = 0; i + sizeof marker - 1 <= len; i++) {
        if (memcmp(buf + i, marker, sizeof marker - 1) == 0) return 1;
    }
    return 0;
}

/* Drain master output into a buffer for `total_ms` (or until the
 * buffer is full). Echoes to stdout so the parent's caller can
 * grep on this program's stdout. Returns total bytes captured. */
static size_t drain_for(int master, char *buf, size_t cap, int total_ms)
{
    size_t got = 0;
    long long start = 0;
    millis_since_epoch_ms(&start);
    long long deadline = start + total_ms;

    while (got + 1 < cap) {
        long long now;
        millis_since_epoch_ms(&now);
        int remaining = (int)(deadline - now);
        if (remaining <= 0) break;
        int slice = remaining < DRAIN_POLL_MS ? remaining : DRAIN_POLL_MS;

        struct pollfd pfd = { .fd = master, .events = POLLIN };
        int pr = poll(&pfd, 1, slice);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) continue;
        char chunk[READ_CHUNK_BYTES];
        ssize_t n = read(master, chunk, sizeof chunk);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        size_t copy = (size_t)n;
        if (copy > cap - got - 1) copy = cap - got - 1;
        memcpy(buf + got, chunk, copy);
        got += copy;
        /* Echo verbatim - the caller can pipe this into their own
         * assertions. */
        (void)!write(STDOUT_FILENO, chunk, (size_t)n);
    }
    buf[got] = '\0';
    return got;
}

int main(int argc, char **argv)
{
    if (argc < 5) { usage(argv[0]); return 2; }
    const char *expect = argv[1];
    const char *input_spec = argv[2];
    if (strcmp(argv[3], "--") != 0) { usage(argv[0]); return 2; }
    char **child_argv = &argv[4];

    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) { perror("forkpty"); return 3; }
    if (pid == 0) {
        /* Child. */
        execvp(child_argv[0], child_argv);
        perror("execvp");
        _exit(127);
    }

    /* Make sure the child isn't going to outlive us if we panic. */
    fcntl(master, F_SETFD, FD_CLOEXEC);

    /* Phase 1: wait until the child has entered alt screen (i.e.
     * finished termios setup). We poll in short slices and look for
     * the CSI ?1049h marker. If we hit the overall deadline without
     * seeing it, we fall through anyway - the test will likely fail
     * but we don't deadlock here. */
    char buf[64 * 1024];
    size_t total = 0;
    {
        long long t0 = 0; millis_since_epoch_ms(&t0);
        while (total + 1 < sizeof buf) {
            long long now; millis_since_epoch_ms(&now);
            if ((int)(now - t0) > OVERALL_DEADLINE_MS) break;
            size_t got = drain_for(master, buf + total,
                                    sizeof buf - total, DRAIN_POLL_MS * 4);
            total += got;
            if (saw_alt_screen(buf, total)) {
                /* Drain a tiny bit more so the initial frame is in
                 * the captured output and any output-buffered prep
                 * has settled. */
                got = drain_for(master, buf + total,
                                 sizeof buf - total, DRAIN_POLL_MS);
                total += got;
                break;
            }
            if (got == 0 && total > 0) {
                /* No alt screen but child went quiet - maybe it's
                 * a non-TUI program (e.g. the ENOTTY refusal path).
                 * Stop waiting. */
                break;
            }
        }
    }

    /* Phase 2: deliver input sequentially. %s tokens introduce
     * real-time pauses between bytes so the child can observe
     * event-loop progress between key presses. */
    if (dispatch_input(master, input_spec) != 0) {
        perror("dispatch_input");
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 3;
    }

    /* Phase 3: drain until either the child exits (EOF / EIO on
     * master, or waitpid reaps) or the overall deadline trips. We
     * poll the master with a short slice and probe waitpid on each
     * iteration; the moment the child is reapable we stop. */
    long long t0 = 0; millis_since_epoch_ms(&t0);
    int status = 0;
    for (;;) {
        long long now; millis_since_epoch_ms(&now);
        if ((int)(now - t0) > OVERALL_DEADLINE_MS) break;

        struct pollfd pfd = { .fd = master, .events = POLLIN };
        int pr = poll(&pfd, 1, DRAIN_POLL_MS);
        if (pr > 0 && total + 1 < sizeof buf) {
            char chunk[READ_CHUNK_BYTES];
            ssize_t n = read(master, chunk, sizeof chunk);
            if (n > 0) {
                size_t copy = (size_t)n;
                if (copy > sizeof buf - total - 1) copy = sizeof buf - total - 1;
                memcpy(buf + total, chunk, copy);
                total += copy;
                buf[total] = '\0';
                (void)!write(STDOUT_FILENO, chunk, (size_t)n);
                continue;   /* keep draining while data flows */
            }
            /* n <= 0 means EOF or error (e.g., EIO when the child's
             * end of the PTY closes). Treat as child gone. */
            break;
        }

        /* No data this slice - check if the child has exited. */
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        /* Still alive and quiet - keep waiting up to OVERALL deadline. */
    }

    /* Reap if not already. */
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    close(master);

    if (strstr(buf, expect) != NULL) {
        return 0;
    }
    fprintf(stderr, "[e2e_tui_drive] expected substring '%s' not found "
                    "in %zu bytes of output\n", expect, total);
    return 1;
}

#endif /* HAVE_FORKPTY */

/*
 * e2e_build_plugin_drive.c - a fake build host for one build plugin
 * (docs/buildcontext_design.md §9.1): spawns `hull <entry>` with the channel
 * on fd 3, sends actions, answers the plugin's calls from a fixed workspace,
 * and prints a transcript: every call's operation and its canonical argument
 * encoding (hex), the bytes written, and how each action ended. The same
 * transform in Lua and in JavaScript must print the same transcript.
 *
 *   e2e_build_plugin_drive HULL ENTRY [MODE]
 *     MODE: normal (two actions, then STOP), garbage (an unknown frame),
 *           none (no action: wait for the plugin to exit)
 *
 * The fake host's operations: inputs.read (src/a.txt = "hello"; a path with
 * ".." is denied), outputs.create_blob, blob.write, blob.close,
 * outputs.declare_generated_output, diagnostics.emit; anything else is
 * unknown_op.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/build_plugin.h"
#include "hull/shared/plain_value.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* ".." anywhere in p[0..n) (memmem is not in C11; cosmocc hides it). */
static int has_dotdot(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++)
        if (p[i] == '.' && p[i + 1] == '.') return 1;
    return 0;
}

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

/* Skip one value at the reader. */
static void skip(HlPvReader *r)
{
    HlPvItem it;
    hl_pv_next(r, &it);
    if (it.type == HL_PV_ARRAY)
        for (size_t i = 0; i < it.n; i++) skip(r);
    if (it.type == HL_PV_MAP)
        for (size_t i = 0; i < it.n; i++) {
            const uint8_t *k; size_t kn;
            hl_pv_next_key(r, &k, &kn);
            skip(r);
        }
}

/* Position @p r at the value of key @p key in the map at r; 1 if found. */
static int find(HlPvReader *r, const char *key)
{
    HlPvItem it;
    if (hl_pv_next(r, &it) || it.type != HL_PV_MAP) return 0;
    for (size_t i = 0; i < it.n; i++) {
        const uint8_t *k; size_t kn;
        hl_pv_next_key(r, &k, &kn);
        if (kn == strlen(key) && memcmp(k, key, kn) == 0) return 1;
        skip(r);
    }
    return 0;
}

/* The bytes / text at @p path ("args", "path") of the value, or NULL. */
static const uint8_t *get_str(const uint8_t *buf, size_t len, const char *a,
                              const char *b, size_t *n)
{
    HlPvReader r;
    hl_pv_reader_init(&r, buf, len);
    if (!find(&r, a)) return NULL;
    if (b && !find(&r, b)) return NULL;
    HlPvItem it;
    hl_pv_next(&r, &it);
    if (it.type != HL_PV_TEXT && it.type != HL_PV_BYTES) return NULL;
    *n = it.n;
    return it.s;
}

/* The args' encoding, for the transcript. */
static void print_args(const uint8_t *buf, size_t len)
{
    HlPvReader r;
    hl_pv_reader_init(&r, buf, len);
    if (!find(&r, "args")) return;
    size_t start = r.pos;
    skip(&r);
    hex(buf + start, r.pos - start);
}

static int send_value(int fd, uint32_t type, uint32_t id, HlPvWriter *w)
{
    int rc = hl_chan_write_frame(fd, HL_BUILD_PLUGIN_MAGIC, type, id, w->buf,
                                 w->len, NULL, 0, hl_chan_mono_ms() + 10000);
    hl_pv_writer_free(w);
    return rc;
}

static void reply_ok_int(int fd, uint32_t id, int64_t v)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 1);
    hl_pv_put_key(&w, "ok", 2);
    hl_pv_put_int(&w, v);
    send_value(fd, HL_BP_REPLY, id, &w);
}

static void reply_ok_bytes(int fd, uint32_t id, const char *s)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 1);
    hl_pv_put_key(&w, "ok", 2);
    hl_pv_put_bytes(&w, s, strlen(s));
    send_value(fd, HL_BP_REPLY, id, &w);
}

static void reply_ok(int fd, uint32_t id)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 0);
    send_value(fd, HL_BP_REPLY, id, &w);
}

static void reply_err(int fd, uint32_t id, const char *token)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 1);
    hl_pv_put_key(&w, "error", 5);
    hl_pv_put_text(&w, token, strlen(token));
    send_value(fd, HL_BP_REPLY, id, &w);
}

static void send_action(int fd, int64_t id, const char *unit)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 3);
    hl_pv_put_key(&w, "config", 6);
    hl_pv_put_map(&w, 1);
    hl_pv_put_key(&w, "greeting", 8);
    hl_pv_put_text(&w, "hi", 2);
    hl_pv_put_key(&w, "id", 2);
    hl_pv_put_int(&w, id);
    hl_pv_put_key(&w, "meta", 4);
    hl_pv_put_map(&w, 1);
    hl_pv_put_key(&w, "unit", 4);
    hl_pv_put_text(&w, unit, strlen(unit));
    send_value(fd, HL_BP_ACTION, 0, &w);
}

static int env_ok(const char *kv)
{
    static const char *const keep[] = { "HOME=", "TMPDIR=", "PATH=",
                                        HL_BUILD_PLUGIN_ENV "=", NULL };
    for (int i = 0; keep[i]; i++)
        if (strncmp(kv, keep[i], strlen(keep[i])) == 0) return 1;
    return 0;
}

/* Serve one action until DONE (1), or the plugin went (0). */
static int serve_action(int fd)
{
    static char blob[4096];
    static size_t blob_len;
    blob_len = 0;
    for (;;) {
        HlChanFrame f = { .magic = HL_BUILD_PLUGIN_MAGIC, .max = HL_BP_MAX_PAYLOAD };
        int rc = hl_chan_frame_read(fd, &f, hl_chan_mono_ms() + 20000);
        if (rc != HL_CHAN_DONE) {
            printf("plugin gone (%d)\n", rc);
            hl_chan_frame_reset(&f);
            return 0;
        }
        if (hl_pv_check(f.buf, f.len) != HL_PV_OK) {
            printf("plugin sent a malformed value\n");
            hl_chan_frame_reset(&f);
            return 0;
        }
        if (f.type == HL_BP_DONE) {
            size_t n = 0;
            const uint8_t *e = get_str(f.buf, f.len, "error", NULL, &n);
            printf("written: %.*s\n", (int)blob_len, blob);
            if (e) printf("done: error %.*s\n", (int)n, e);
            else printf("done: ok\n");
            hl_chan_frame_reset(&f);
            return 1;
        }
        if (f.type != HL_BP_CALL) {
            printf("unexpected frame %u\n", f.type);
            hl_chan_frame_reset(&f);
            return 0;
        }
        size_t opn = 0;
        const uint8_t *op = get_str(f.buf, f.len, "op", NULL, &opn);
        printf("call %.*s ", (int)opn, op ? (const char *)op : "?");
        print_args(f.buf, f.len);
        printf("\n");
        char name[64] = { 0 };
        if (op && opn < sizeof name) memcpy(name, op, opn);
        if (strcmp(name, "inputs.read") == 0) {
            size_t pn = 0;
            const uint8_t *p = get_str(f.buf, f.len, "args", "path", &pn);
            if (p && has_dotdot(p, pn)) reply_err(fd, f.id, "denied");
            else if (p && pn == 9 && memcmp(p, "src/a.txt", 9) == 0)
                reply_ok_bytes(fd, f.id, "hello");
            else reply_err(fd, f.id, "absent");
        } else if (strcmp(name, "outputs.create_blob") == 0) {
            reply_ok_int(fd, f.id, 7);
        } else if (strcmp(name, "blob.write") == 0) {
            size_t bn = 0;
            const uint8_t *b = get_str(f.buf, f.len, "args", "bytes", &bn);
            if (b && blob_len + bn < sizeof blob) {
                memcpy(blob + blob_len, b, bn);
                blob_len += bn;
            }
            reply_ok(fd, f.id);
        } else if (strcmp(name, "blob.close") == 0 ||
                   strcmp(name, "outputs.declare_generated_output") == 0 ||
                   strcmp(name, "diagnostics.emit") == 0) {
            reply_ok(fd, f.id);
        } else {
            reply_err(fd, f.id, "unknown_op");
        }
        hl_chan_frame_reset(&f);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s HULL ENTRY [normal|garbage|none]\n", argv[0]);
        return 2;
    }
    const char *mode = argc > 3 ? argv[3] : "normal";
    setvbuf(stdout, NULL, _IONBF, 0);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nosigpipe(sv[0]);
    setenv(HL_BUILD_PLUGIN_ENV, "3", 1);
    char *cargv[] = { argv[1], argv[2], NULL };
    pid_t pid = hl_chan_spawn(argv[1], cargv, sv[1], env_ok);
    close(sv[1]);
    if (pid < 0) return 2;

    if (strcmp(mode, "normal") == 0) {
        send_action(sv[0], 1, "a");
        if (serve_action(sv[0])) {
            send_action(sv[0], 2, "b");
            if (serve_action(sv[0]))
                hl_chan_write_frame(sv[0], HL_BUILD_PLUGIN_MAGIC, HL_BP_STOP, 0,
                                    NULL, 0, NULL, 0, hl_chan_mono_ms() + 5000);
        }
    } else if (strcmp(mode, "garbage") == 0) {
        hl_chan_write_frame(sv[0], HL_BUILD_PLUGIN_MAGIC, 99, 0, "x", 1, NULL, 0,
                            hl_chan_mono_ms() + 5000);
    }
    int st = 0;
    if (!hl_chan_reap(pid, &st, 20000, NULL)) {
        kill(pid, SIGKILL);
        hl_chan_reap(pid, &st, 5000, NULL);
        printf("exit: killed\n");
    } else if (WIFEXITED(st)) {
        printf("exit: %d\n", WEXITSTATUS(st));
    } else {
        printf("exit: signal %d\n", WTERMSIG(st));
    }
    close(sv[0]);
    return 0;
}

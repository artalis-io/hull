/*
 * hull/cap/exec_broker.h - the command broker (docs/design/exec_broker.md §5)
 *
 * A small process, this same binary with argv[1] HL_EXEC_BROKER_ARGV1, that
 * runs the commands the manifest's `exec` section declares. The server
 * spawns it before the phase-1 sandbox (which forbids spawning), sends it
 * exactly one POLICY frame once the manifest is applied and checked, and
 * from then on only asks it to RUN a declared command by name with
 * arguments; the broker matches them again (the same hl_exec_check_call),
 * forks the child under the host's confinement (exec_platform.h), and
 * answers DONE with the output. The app process itself keeps no
 * exec / fork / clone.
 *
 * Supported on native Linux and macOS. Elsewhere hl_exec_broker_start returns
 * -1 and an app with an `exec` section is refused at start (unless
 * --allow-degraded-sandbox on a host that is not Windows, where exec.run
 * then fails).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_EXEC_BROKER_H
#define HL_CAP_EXEC_BROKER_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define HL_EXEC_BROKER_ARGV1 "__hull_exec_broker"

/* The hosts with a backend (hull/cap/exec_platform.h): native Linux
 * (cap/exec_broker_linux.c) and macOS (cap/exec_broker_macos.c). */
#if (defined(__linux__) || defined(__APPLE__)) && !defined(__COSMOPOLITAN__)
#define HL_EXEC_BROKER_SUPPORTED 1
#endif

typedef struct HlAsyncBackendCtx HlAsyncBackendCtx;

/* ── The broker process ────────────────────────────────────────────── */

/* Run this process as the broker (argv[1] == HL_EXEC_BROKER_ARGV1). Its
 * channel is fd 3. Returns the exit status. */
int hl_exec_broker_main(void);

/* ── The server's side ─────────────────────────────────────────────── */

/* Spawn the broker. Before the phase-1 sandbox and before any thread.
 * 0, or -1 (unsupported host, HULL_NO_EXEC_BROKER=1, spawn failed). */
int  hl_exec_broker_start(void);
int  hl_exec_broker_running(void);   /* spawned and READY */
int  hl_exec_broker_spawned(void);   /* spawned, policy not sent yet */
/* Close the channel and reap the broker (it kills its children first).
 * Call it while the event loop is alive (it removes its watcher). It
 * resumes nothing: pending calls are cancelled by their own teardown. */
void hl_exec_broker_stop(void);

#define HL_EXEC_POLICY_DEGRADED_OK 0x1u   /* --allow-degraded-sandbox */
#define HL_EXEC_POLICY_NO_SANDBOX  0x2u   /* --no-sandbox */

/* Send the one POLICY frame and wait for READY. @p app_dir and @p state_dir
 * (NULL = none) are the roots a command's child manifest (`manifest.fs`) is
 * resolved against, as the app's own grants are (exec_child.h); @p dbs
 * (@p n_dbs real paths: the app's SQLite database files) are files no write
 * grant may reach. hl_exec_broker_apply collects them from the registry
 * (hl_db_registry_file_paths) and @p default_db, and refuses a relative
 * child grant when the app directory is "/" or the home (checked here,
 * with the server's own environment, before the broker creates anything). 0: the broker took it and
 * locked itself down; @p report gets its startup lines (resolved paths, and
 * any guarantee it does not hold), one per line. -1: refused or lost;
 * @p report says why. Either way the policy can never be sent again. */
int hl_exec_broker_policy(const uint8_t *policy, size_t len, unsigned flags,
                          const char *app_dir, const char *state_dir,
                          const char (*dbs)[PATH_MAX], int n_dbs,
                          char *report, size_t report_size);

/* What an entry point does once its manifest is applied and checked, before
 * the phase-2 sandbox: no commands -> stop the broker; Windows -> refuse;
 * no broker -> refuse, unless @p allow_degraded (exec.run then fails);
 * else send the policy, logging the broker's report. 0, or -1 (logged:
 * refuse to start). */
typedef struct HlDbRegistry HlDbRegistry;
int hl_exec_broker_apply(const uint8_t *policy, size_t len, int allow_degraded,
                         int no_sandbox, const char *app_dir, const char *state_dir,
                         HlDbRegistry *dbs, const char *default_db);

/* One reply. Owned by the callee of HlExecDoneFn; free with
 * hl_exec_reply_free. */
#define HL_EXEC_R_DONE    0   /* the command ran: status / signal / output */
#define HL_EXEC_R_BUSY    1   /* refused by admission (busy) */
#define HL_EXEC_R_REFUSED 2   /* the broker refused the call (shape, binary) */
#define HL_EXEC_R_LOST    3   /* the broker is gone */
typedef struct HlExecReply {
    int      kind;
    int32_t  status;
    char     signal[16];          /* "" when it exited */
    uint8_t  timed_out;
    uint32_t duration_ms;
    uint8_t *out;   size_t out_len;   uint8_t out_cut;
    uint8_t *err;   size_t err_len;   uint8_t err_cut;
    char     msg[256];            /* BUSY / REFUSED / LOST: why */
} HlExecReply;

typedef void (*HlExecDoneFn)(void *ud, HlExecReply *r);

void hl_exec_reply_free(HlExecReply *r);

/* Ask the broker to run @p name with @p args, writing @p in (@p in_len
 * bytes, at most HL_EXEC_STDIN_CEIL; 0 = the child reads /dev/null) to its
 * stdin. @p fn runs on the loop thread with the reply, exactly once, unless
 * the call is cancelled first. 0 with *id_out set, or -1 (no broker: @p why
 * says so). */
int hl_exec_submit(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                   const char *const *args, const size_t *lens, size_t n,
                   const void *in, size_t in_len,
                   HlExecDoneFn fn, void *ud, uint32_t *id_out,
                   char *why, size_t why_size);

/* Cancel call @p id: the broker kills its child; @p fn will not run. */
void hl_exec_cancel(uint32_t id);

/* ── One exec.run, as an async op (both runtimes) ──────────────────── */

typedef struct HlAsyncCtx HlAsyncCtx;

/* The op's driver: the call's id while the broker has it, then the reply.
 * The ctx owns it (free_driver): freeing it while the call is still
 * pending sends KILL, so every cancel path - a client gone, a teardown, a
 * failed suspend - stops the command. */
typedef struct HlExecOp {
    uint32_t     id;
    int          pending;
    HlExecReply *reply;        /* NULL until it arrives */
} HlExecOp;

/* Submit the call and make it @p ctx's driver: the reply resumes @p ctx
 * (detached) or completes it on its connection (attached, which also gets
 * a deadline of @p timeout_ms + 5 s in case the broker never answers).
 * 0, or -1 (nothing submitted; @p why says why). The caller then sets
 * ctx->cont and suspends / tracks the ctx. */
int hl_exec_op_start(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop,
                     const char *name, size_t name_len,
                     const char *const *args, const size_t *lens, size_t n,
                     const void *in, size_t in_len,
                     uint32_t timeout_ms, char *why, size_t why_size);

/* Calls in flight (the binding's fast admission check). */
int hl_exec_inflight(void);
int hl_exec_inflight_cmd(const char *name, size_t name_len);

#endif /* HL_CAP_EXEC_BROKER_H */

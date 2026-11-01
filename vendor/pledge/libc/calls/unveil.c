/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2020 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
/*
#include "libc/assert.h"
*/
#include "libc/calls/blockcancel.internal.h"
#include "libc/calls/calls.h"
#include "libc/calls/landlock.h"
#include "libc/calls/pledge.internal.h" /* HULL PATCH 0003 */
/*
#include "libc/calls/struct/bpf.h"
#include "libc/calls/struct/filter.h"
#include "libc/calls/struct/seccomp.h"
#include "libc/calls/struct/stat.h"
#include "libc/calls/struct/stat.internal.h"
#include "libc/calls/syscall-sysv.internal.h"
*/
#include "libc/calls/syscall_support-sysv.internal.h"
/*
#include "libc/errno.h"
*/
#include "libc/fmt/conv.h"
/*
#include "libc/intrin/strace.internal.h"
*/
#include "libc/macros.internal.h"
/*
#include "libc/nexgen32e/vendor.internal.h"
#include "libc/runtime/internal.h"
*/
#include "libc/runtime/runtime.h"
#include "libc/runtime/stack.h"
#include "libc/str/path.h"
#include "libc/str/str.h"
/*
#include "libc/sysv/consts/at.h"
#include "libc/sysv/consts/audit.h"
#include "libc/sysv/consts/f.h"
#include "libc/sysv/consts/fd.h"
#include "libc/sysv/consts/nrlinux.h"
#include "libc/sysv/consts/o.h"
#include "libc/sysv/consts/pr.h"
#include "libc/sysv/consts/s.h"
#include "libc/sysv/errfuns.h"
#include "libc/thread/tls.h"
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define OFF(f) offsetof(struct seccomp_data, f)

#define UNVEIL_READ                                             \
  (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | \
   LANDLOCK_ACCESS_FS_REFER)
#define UNVEIL_WRITE \
  (LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE)
#define UNVEIL_EXEC (LANDLOCK_ACCESS_FS_EXECUTE)
#define UNVEIL_CREATE                                             \
  (LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |   \
   LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |   \
   LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
   LANDLOCK_ACCESS_FS_MAKE_SYM)

/* HULL PATCH 0002: TRUNCATE is a file right (Landlock ABI >= 3; masked out by
 * State.fs_mask below that). Without it a write grant on an existing FILE
 * could not O_TRUNC it. */
#define FILE_BITS                                                 \
  (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | \
   LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_TRUNCATE)

static struct sock_filter kUnveilBlacklistAbiVersionBelow3[] = {
#if 0  // Should we have this ? It certainly means things don't work on other
       // architectures than x86-64 as-is... (TODO check this up later when
       // relevant)
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF(arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF(nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_truncate, 1, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setxattr, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (1 & SECCOMP_RET_DATA)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

static struct sock_filter kUnveilBlacklistLatestAbi[] = {
#if 0
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF(arch)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
#endif
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, OFF(nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setxattr, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (1 & SECCOMP_RET_DATA)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
};

static int landlock_abi_version;

__attribute__((__constructor__)) void init_landlock_version() {
  landlock_abi_version =
      landlock_create_ruleset(0, 0, LANDLOCK_CREATE_RULESET_VERSION);
}

/**
 * Long living state for landlock calls.
 * fs_mask is set to use all the access rights from the latest landlock ABI.
 * On init, the current supported abi is checked and unavailable rights are
 * masked off.
 *
 * As of 6.2, the latest abi is v3.
 *
 * TODO:
 *  - Integrate with pledge and remove the file access?
 *  - Stuff state into the .protected section?
 */
_Thread_local static struct {
  uint64_t fs_mask;
  int fd;
} State;

/* HULL PATCH 0004: the Landlock domain reaches every thread (audit 42).
 *
 * landlock_restrict_self(fd, 0) restricts the CALLING thread only. A thread
 * that already existed when unveil(NULL, NULL) ran - a worker pool, a
 * watchdog, a GPU driver or DuckDB thread - kept its full filesystem reach,
 * and because threads share one address space, code running in any of them
 * voided the domain for the process. So:
 *
 *  1. Where the kernel has LANDLOCK_RESTRICT_SELF_TSYNC (Landlock ABI >= 8),
 *     the domain is applied to every thread atomically.
 *  2. Otherwise every other thread is asked to restrict ITSELF: each thread
 *     listed in /proc/self/task gets HULL_UNVEIL_SYNC_SIG (tgkill), whose
 *     handler sets no_new_privs and calls landlock_restrict_self on the same
 *     ruleset (the libpsx / glibc setxid pattern). The list is re-read until
 *     a round finds no thread it has not seen, so a thread started by an
 *     unrestricted one meanwhile is reached too. A thread that has the
 *     signal blocked, or whose call fails, does not answer within
 *     HULL_UNVEIL_SYNC_MS and is counted in hull_unveil_unsynced_threads.
 *
 * The calling thread and every thread that answered are restricted either
 * way and the seccomp filters below go to every thread (TSYNC, patch 0003);
 * the caller decides what an unsynced thread means (Hull fails closed,
 * src/hull/sandbox.c). hull_unveil_sync_mode reports what was used:
 * 0 = no other thread, 1 = Landlock TSYNC, 2 = per-thread signal. */
#define HULL_LANDLOCK_RESTRICT_SELF_TSYNC (1U << 3)
#define HULL_UNVEIL_SYNC_SIG (SIGRTMAX - 3)
#define HULL_UNVEIL_SYNC_MS 2000
#define HULL_UNVEIL_SYNC_MAX 1024
/* Test-only: -DHULL_UNVEIL_TEST_NO_TSYNC=1 takes the per-thread path even on
 * a kernel with Landlock TSYNC, so tests/e2e_sandbox.sh covers both. */
#ifndef HULL_UNVEIL_TEST_NO_TSYNC
#define HULL_UNVEIL_TEST_NO_TSYNC 0
#endif

int hull_unveil_unsynced_threads;
int hull_unveil_sync_mode;

static volatile int g_sync_fd = -1;
static int g_ack_n;
static pid_t g_ack_tid[HULL_UNVEIL_SYNC_MAX];
static int g_ack_ok[HULL_UNVEIL_SYNC_MAX];

static void unveil_sync_handler(int sig) {
  (void)sig;
  int e = errno;
  pid_t tid = (pid_t)syscall(__NR_gettid);
  int ok = 0;
  if (g_sync_fd >= 0) {
    syscall(__NR_prctl, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    ok = syscall(__NR_landlock_restrict_self, g_sync_fd, 0) == 0;
  }
  int i = __atomic_fetch_add(&g_ack_n, 1, __ATOMIC_SEQ_CST);
  if (i >= 0 && i < HULL_UNVEIL_SYNC_MAX) {
    __atomic_store_n(&g_ack_ok[i], ok, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ack_tid[i], tid, __ATOMIC_SEQ_CST);
  }
  errno = e;
}

/* The thread ids in /proc/self/task, without the caller. -1 when it cannot
 * be read (that must not pass for "single-threaded"). */
static int unveil_list_tasks(pid_t *out, int max) {
  pid_t self = (pid_t)syscall(__NR_gettid);
  DIR *d = opendir("/proc/self/task");
  if (!d) return -1;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(d))) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    pid_t t = (pid_t)strtol(de->d_name, 0, 10);
    if (t <= 0 || t == self) continue;
    if (n == max) {
      closedir(d);
      return -1;
    }
    out[n++] = t;
  }
  closedir(d);
  return n;
}

static int unveil_task_alive(pid_t tid) {
  char p[64];
  struct stat st;
  snprintf(p, sizeof p, "/proc/self/task/%d", (int)tid);
  return stat(p, &st) == 0;
}

/* Restricts every other thread through the signal handler. Returns the
 * number of threads that could not be restricted, or -1. */
static int unveil_sync_by_signal(int fd) {
  static pid_t seen[HULL_UNVEIL_SYNC_MAX], cur[HULL_UNVEIL_SYNC_MAX];
  int nseen = 0, unsynced = 0;
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = unveil_sync_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  g_sync_fd = fd;
  __atomic_store_n(&g_ack_n, 0, __ATOMIC_SEQ_CST);
  if (sigaction(HULL_UNVEIL_SYNC_SIG, &sa, &old) != 0) return -1;
  for (int round = 0; round < 16; round++) {
    int n = unveil_list_tasks(cur, HULL_UNVEIL_SYNC_MAX);
    if (n < 0) {
      unsynced = -1;
      break;
    }
    int sent0 = nseen, fresh = 0;
    for (int i = 0; i < n; i++) {
      int known = 0;
      for (int j = 0; j < nseen; j++)
        if (seen[j] == cur[i]) known = 1;
      if (known) continue;
      if (nseen == HULL_UNVEIL_SYNC_MAX) {
        unsynced = -1;
        goto out;
      }
      fresh++;
      seen[nseen++] = cur[i];
      if (syscall(__NR_tgkill, getpid(), cur[i], HULL_UNVEIL_SYNC_SIG) != 0)
        seen[nseen - 1] = 0; /* gone already (ESRCH) */
    }
    if (!fresh) break;
    /* wait until every thread signalled this round has answered */
    for (int waited = 0; waited < HULL_UNVEIL_SYNC_MS; waited++) {
      int all = 1;
      int acks = __atomic_load_n(&g_ack_n, __ATOMIC_SEQ_CST);
      if (acks > HULL_UNVEIL_SYNC_MAX) acks = HULL_UNVEIL_SYNC_MAX;
      for (int j = sent0; j < nseen && all; j++) {
        if (!seen[j]) continue;
        int got = 0;
        for (int k = 0; k < acks; k++)
          if (__atomic_load_n(&g_ack_tid[k], __ATOMIC_SEQ_CST) == seen[j])
            got = 1;
        if (!got) all = 0;
      }
      if (all) break;
      struct timespec ts = {0, 1000000};
      nanosleep(&ts, 0);
    }
  }
  /* Tally: an answered thread whose call failed, or a live thread that never
   * answered (its signal is blocked), is not restricted. */
  if (unsynced == 0) {
    int acks = __atomic_load_n(&g_ack_n, __ATOMIC_SEQ_CST);
    if (acks > HULL_UNVEIL_SYNC_MAX) acks = HULL_UNVEIL_SYNC_MAX;
    for (int j = 0; j < nseen; j++) {
      if (!seen[j]) continue;
      int got = -1;
      for (int k = 0; k < acks; k++)
        if (__atomic_load_n(&g_ack_tid[k], __ATOMIC_SEQ_CST) == seen[j])
          got = __atomic_load_n(&g_ack_ok[k], __ATOMIC_SEQ_CST);
      if (got == 1) continue;
      if (got == -1 && !unveil_task_alive(seen[j])) continue;
      unsynced++;
    }
  }
out:
  /* A late delivery (a thread unblocking the signal later) must not take
   * the default action, which for a real-time signal ends the process. */
  g_sync_fd = -1;
  sa.sa_handler = SIG_IGN;
  sigaction(HULL_UNVEIL_SYNC_SIG, &sa, 0);
  (void)old;
  return unsynced;
}

static int unveil_final(void) {
  int e, rc;
  struct sock_fprog sandbox = {
      .filter = kUnveilBlacklistLatestAbi,
      .len = ARRAYLEN(kUnveilBlacklistLatestAbi),
  };
  if (landlock_abi_version < 3) {
    sandbox = (struct sock_fprog){
        .filter = kUnveilBlacklistAbiVersionBelow3,
        .len = ARRAYLEN(kUnveilBlacklistAbiVersionBelow3),
    };
  }
  e = errno;
  prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
  errno = e;
  hull_unveil_unsynced_threads = 0;
  hull_unveil_sync_mode = 0;
  rc = -1;
  if (landlock_abi_version >= 8 && !HULL_UNVEIL_TEST_NO_TSYNC) {
    rc = landlock_restrict_self(State.fd, HULL_LANDLOCK_RESTRICT_SELF_TSYNC);
    if (rc == 0) {
      hull_unveil_sync_mode = 1;
    } else if (errno != EINVAL) {
      return rc;
    } else {
      errno = e;
    }
  }
  if (rc != 0) {
    /* Others first: once this thread is restricted it can no longer list
     * /proc/self/task. */
    pid_t probe[1];
    int others = unveil_list_tasks(probe, 1);
    if (others != 0) {
      int u = unveil_sync_by_signal(State.fd);
      hull_unveil_sync_mode = 2;
      hull_unveil_unsynced_threads = u < 0 ? -1 : u;
    }
    if ((rc = landlock_restrict_self(State.fd, 0)) == -1) return rc;
  }
  if ((rc = close(State.fd)) != -1 &&
      (rc = hull_seccomp_install(&sandbox)) != -1) {
    State.fd = 0;
  }
  return rc;
}

static int err_close(int rc, int fd) {
  int serrno = errno;
  close(fd);
  errno = serrno;
  return rc;
}

static int unveil_init(void) {
  int rc, fd;
  State.fs_mask = UNVEIL_READ | UNVEIL_WRITE | UNVEIL_EXEC | UNVEIL_CREATE;
  if (landlock_abi_version == -1) {
    if (errno == EOPNOTSUPP) {
      errno = ENOSYS;
    }
    return -1;
  }
  if (landlock_abi_version < 2) {
    State.fs_mask &= ~LANDLOCK_ACCESS_FS_REFER;
  }
  if (landlock_abi_version < 3) {
    State.fs_mask &= ~LANDLOCK_ACCESS_FS_TRUNCATE;
  }
  const struct landlock_ruleset_attr attr = {
      .handled_access_fs = State.fs_mask,
  };
  // [undocumented] landlock_create_ruleset() always returns o_cloexec
  //                assert(__sys_fcntl(rc, F_GETFD, 0) == FD_CLOEXEC);
  if ((rc = landlock_create_ruleset(&attr, sizeof(attr), 0)) < 0) return -1;
  // grant file descriptor a higher number that's less likely to interfere
  if ((fd = fcntl(rc, F_DUPFD_CLOEXEC, 100)) == -1) {
    return err_close(-1, rc);
  }
  if (close(rc) == -1) {
    return err_close(-1, fd);
  }
  State.fd = fd;
  return 0;
}

int sys_unveil_linux(const char *path, const char *permissions) {
  int rc;
  const char *dir;
  const char *last;
  const char *next;
  struct {
    char lbuf[PATH_MAX];
    char buf1[PATH_MAX];
    char buf2[PATH_MAX];
    char buf3[PATH_MAX];
    char buf4[PATH_MAX];
  } b;
  CheckLargeStackAllocation(&b, sizeof(b));

  if (!State.fd && (rc = unveil_init()) == -1) return rc;
  if ((path && !permissions) || (!path && permissions)) {
    errno = EINVAL;
    return -1;
  }
  if (!path && !permissions) return unveil_final();
  struct landlock_path_beneath_attr pb = {0};
  for (const char *c = permissions; *c != '\0'; c++) {
    switch (*c) {
      case 'r':
        pb.allowed_access |= UNVEIL_READ;
        break;
      case 'w':
        pb.allowed_access |= UNVEIL_WRITE;
        break;
      case 'x':
        pb.allowed_access |= UNVEIL_EXEC;
        break;
      case 'c':
        pb.allowed_access |= UNVEIL_CREATE;
        break;
      default:
        errno = EINVAL;
        return -1;
    }
  }
  pb.allowed_access &= State.fs_mask;

  // landlock exposes all metadata, so we only technically need to add
  // realpath(path) to the ruleset. however a corner case exists where
  // it isn't valid, e.g. /dev/stdin -> /proc/2834/fd/pipe:[51032], so
  // we'll need to work around this, by adding the path which is valid
  if (strlen(path) + 1 > PATH_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  last = path;
  next = path;
  for (int i = 0;; ++i) {
    if (i == 64) {
      // give up
      errno = ELOOP;
      return -1;
    }
    int err = errno;
    if ((rc = readlinkat(AT_FDCWD, next, b.lbuf, PATH_MAX)) != -1) {
      if (rc < PATH_MAX) {
        // we need to nul-terminate
        b.lbuf[rc] = 0;
        // last = next
        strcpy(b.buf1, next);
        last = b.buf1;
        // next = join(dirname(next), link)
        strcpy(b.buf2, next);
        dir = dirname(b.buf2);
        if ((next = joinpaths(b.buf3, PATH_MAX, dir, b.lbuf))) {
          // next now points to either: buf3, buf2, lbuf, rodata
          strcpy(b.buf4, next);
          next = b.buf4;
        } else {
          errno = ENAMETOOLONG;
          return -1;
        }
      } else {
        // symbolic link data was too long
        errno = ENAMETOOLONG;
        return -1;
      }
    } else if (errno == EINVAL) {
      // next wasn't a symbolic link
      errno = err;
      path = next;
      break;
    } else if (i && (errno == ENOENT || errno == ENOTDIR)) {
      // next is a broken symlink, use last
      errno = err;
      path = last;
      break;
    } else {
      // readlink failed for some other reason
      return -1;
    }
  }

  // now we can open the path
  BLOCK_CANCELLATIONS;
  rc = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
  ALLOW_CANCELLATIONS;
  if (rc == -1) return rc;

  pb.parent_fd = rc;
  struct stat st;
  if ((rc = fstat(pb.parent_fd, &st)) == -1) {
    return err_close(rc, pb.parent_fd);
  }
  if (!S_ISDIR(st.st_mode)) {
    pb.allowed_access &= FILE_BITS;
  }
  if ((rc = landlock_add_rule(State.fd, LANDLOCK_RULE_PATH_BENEATH, &pb, 0))) {
    return err_close(rc, pb.parent_fd);
  }
  close(pb.parent_fd);
  return rc;
}

/**
 * Makes files accessible, e.g.
 *
 *     unveil(".", "r");     // current directory + children are visible
 *     unveil("/etc", "r");  // make /etc readable too
 *     unveil(0, 0);         // commit and lock policy
 *
 * Unveiling restricts a view of the filesystem to a set of allowed
 * paths with specific privileges.
 *
 * Once you start using unveil(), the entire file system is considered
 * hidden. You then specify, by repeatedly calling unveil(), which paths
 * should become unhidden. When you're finished, you call `unveil(0,0)`
 * which commits your policy.
 *
 * This function requires OpenBSD or Linux 5.13+. We don't consider lack
 * of system support to be an ENOSYS error, because the files will still
 * become unveiled. Therefore we return 0 in such cases.
 *
 * There are some differences between unveil() on Linux versus OpenBSD.
 *
 * 1. Build your policy and lock it in one go. On OpenBSD, policies take
 *    effect immediately and may evolve as you continue to call unveil()
 *    but only in a more restrictive direction. On Linux, nothing will
 *    happen until you call `unveil(0,0)` which commits and locks.
 *
 * 2. Try not to overlap directory trees. On OpenBSD, if directory trees
 *    overlap, then the most restrictive policy will be used for a given
 *    file. On Linux overlapping may result in a less restrictive policy
 *    and possibly even undefined behavior.
 *
 * 3. OpenBSD and Linux disagree on error codes. On OpenBSD, accessing
 *    paths outside of the allowed set raises ENOENT, and accessing ones
 *    with incorrect permissions raises EACCES. On Linux, both these
 *    cases raise EACCES.
 *
 * 4. Unlike OpenBSD, Linux does nothing to conceal the existence of
 *    paths. Even with an unveil() policy in place, it's still possible
 *    to access the metadata of all files using functions like stat()
 *    and open(O_PATH), provided you know the path. A sandboxed process
 *    can always, for example, determine how many bytes of data are in
 *    /etc/passwd, even if the file isn't readable. But it's still not
 *    possible to use opendir() and go fishing for paths which weren't
 *    previously known.
 *
 * 5. Use ftruncate() rather than truncate() if you wish for portability to
 *    Linux kernels versions released before February 2022. One issue
 *    Landlock hadn't addressed as of ABI version 2 was restrictions over
 *    truncate() and setxattr() which could permit certain kinds of
 *    modifications to files outside the sandbox. When your policy is
 *    committed, we install a SECCOMP BPF filter to disable those calls,
 *    however similar trickery may be possible through other unaddressed
 *    calls like ioctl(). Using the pledge() function in addition to
 *    unveil() will solve this, since it installs a strong system call
 *    access policy. Linux 6.2 has improved this situation with Landlock
 *    ABI v3, which added the ability to control truncation operations -
 *    this means the SECCOMP BPF filter will only disable
 *    truncate() on Linux 6.1 or older
 *
 * 6. Set your process-wide policy at startup from the main thread. On
 *    OpenBSD unveil() will apply process-wide even when called from a
 *    child thread; whereas with Linux, calling unveil() from a thread
 *    will cause your ruleset to only apply to that thread in addition
 *    to any descendent threads it creates.
 *
 * 7. Always specify at least one path. OpenBSD has unclear semantics
 *    when `unveil(0,0)` is used without any previous calls.
 *
 * 8. On OpenBSD calling `unveil(0,0)` will prevent unveil() from being
 *    used again. On Linux this is allowed, because Landlock is able to
 *    do that securely, i.e. the second ruleset can only be a subset of
 *    the previous ones.
 *
 * This system call is supported natively on OpenBSD and polyfilled on
 * Linux using the Landlock LSM[1].
 *
 * @param path is the file or directory to unveil
 * @param permissions is a string consisting of zero or more of the
 *     following characters:
 *
 *     - 'r' makes `path` available for read-only path operations,
 *       corresponding to the pledge promise "rpath".
 *
 *     - `w` makes `path` available for write operations, corresponding
 *       to the pledge promise "wpath".
 *
 *     - `x` makes `path` available for execute operations,
 *       corresponding to the pledge promises "exec" and "execnative".
 *
 *     - `c` allows `path` to be created and removed, corresponding to
 *       the pledge promise "cpath".
 *
 * @return 0 on success, or -1 w/ errno
 * @raise EINVAL if one argument is set and the other is not
 * @raise EINVAL if an invalid character in `permissions` was found
 * @raise EPERM if unveil() is called after locking
 * @note on Linux this function requires Linux Kernel 5.13+ and version 6.2+
 *     to properly support truncation operations
 * @see [1] https://docs.kernel.org/userspace-api/landlock.html
 * @threadsafe
 */
int unveil(const char *path, const char *permissions) {
  int e, rc;
  e = errno;
  /* The ABI probe runs in a constructor.  Do not rely on its errno still
   * being current when unveil() is called later: unrelated startup work may
   * have changed errno.  Hull probes and reports this degraded state itself;
   * the compatibility function must consistently retain its documented
   * no-op behavior on kernels built without Landlock. */
  if (landlock_abi_version < 1) {
    errno = e;
    return 0;
  }
  /*if (IsGenuineBlink()) {
    rc = 0;  // blink doesn't support landlock
  } else if (IsLinux()) {*/
  rc = sys_unveil_linux(path, permissions);
  /*} else {
    abort();
    rc = sys_unveil(path, permissions);
  }*/
  if (rc == -1 && errno == ENOSYS) {
    errno = e;
    rc = 0;
  }
  // STRACE("unveil(%#s, %#s) → %d% m", path, permissions, rc);
  return rc;
}

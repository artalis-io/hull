/*
 * cap/exec_broker_linux.c - the command broker's Linux backend
 * (docs/design/exec_broker.md §6.2 - §6.5; interface:
 * hull/cap/exec_platform.h)
 *
 * The broker locks itself with NO_NEW_PRIVS, a Landlock domain that is the
 * union of the commands' grants and a seccomp filter that is the CEILING
 * every child inherits. Each child then narrows itself between fork and
 * exec: PR_SET_PDEATHSIG, its own Landlock ruleset (prepared before the
 * broker locked), and a stacked filter that refuses process creation,
 * credential changes and (unless unix_sockets) AF_UNIX. It execs the very
 * descriptor the broker checked (execveat, AT_EMPTY_PATH).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* O_PATH, AT_EMPTY_PATH (glibc); before any header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "hull/cap/exec_platform.h"

#if defined(HL_EXEC_BROKER_SUPPORTED) && defined(__linux__)

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/sched.h>

/* ── Landlock (raw syscalls: no header dependency on the build host) ── */

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule       445
#define __NR_landlock_restrict_self  446
#endif

#define LL_EXECUTE     (1ull << 0)
#define LL_WRITE_FILE  (1ull << 1)
#define LL_READ_FILE   (1ull << 2)
#define LL_READ_DIR    (1ull << 3)
#define LL_REMOVE_FILE (1ull << 5)
#define LL_MAKE_REG    (1ull << 8)
#define LL_TRUNCATE    (1ull << 14)
#define LL_IOCTL_DEV   (1ull << 15)
#define LL_FILE_RIGHTS (LL_EXECUTE | LL_WRITE_FILE | LL_READ_FILE | LL_TRUNCATE | \
                        LL_IOCTL_DEV)
#define LL_NET_BIND_TCP    (1ull << 0)
#define LL_NET_CONNECT_TCP (1ull << 1)
#define LL_SCOPE_ABSTRACT_UNIX (1ull << 0)
#define LL_SCOPE_SIGNAL        (1ull << 1)

struct ll_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
    uint64_t scoped;
};

struct ll_path_beneath {
    uint64_t allowed_access;
    int32_t  parent_fd;
} __attribute__((packed));

static int g_ll_abi;            /* 0 = none */
static uint64_t g_ll_fs_mask;   /* every fs right this ABI knows */

static void ll_probe(void)
{
    long abi = syscall(__NR_landlock_create_ruleset, NULL, 0, 1u);
    g_ll_abi = abi > 0 ? (int)abi : 0;
    g_ll_fs_mask = 0;
    if (g_ll_abi >= 1) g_ll_fs_mask = (1ull << 13) - 1;
    if (g_ll_abi >= 2) g_ll_fs_mask |= 1ull << 13;     /* REFER */
    if (g_ll_abi >= 3) g_ll_fs_mask |= LL_TRUNCATE;
    if (g_ll_abi >= 5) g_ll_fs_mask |= LL_IOCTL_DEV;
}

static int ll_ruleset(void)
{
    struct ll_ruleset_attr a = { g_ll_fs_mask, 0, 0 };
    size_t sz = sizeof a.handled_access_fs;
    if (g_ll_abi >= 4) {
        a.handled_access_net = LL_NET_BIND_TCP | LL_NET_CONNECT_TCP;
        sz = 2 * sizeof(uint64_t);
    }
    if (g_ll_abi >= 6) {
        a.scoped = LL_SCOPE_ABSTRACT_UNIX | LL_SCOPE_SIGNAL;
        sz = sizeof a;
    }
    return (int)syscall(__NR_landlock_create_ruleset, &a, sz, 0u);
}

/* Grant @p access under @p path (a file gets the file rights only). A path
 * that does not exist is skipped. 0, or -1 when the rule was refused. */
static int ll_allow(int rs, const char *path, uint64_t access)
{
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (!S_ISDIR(st.st_mode)) access &= LL_FILE_RIGHTS;
    access &= g_ll_fs_mask;
    int rc = 0;
    if (access) {
        struct ll_path_beneath pb = { access, fd };
        rc = (int)syscall(__NR_landlock_add_rule, rs, 1 /* PATH_BENEATH */, &pb, 0u);
    }
    close(fd);
    return rc;
}

/* The trees every command may read (§6.2); never the home directories, the
 * app directory, /srv or /var. */
static const char *const READ_SET[] = { "/usr", "/lib", "/lib64", "/etc",
                                        "/proc", "/sys", "/dev", NULL };

/* The grants one command needs (§6.3). */
static int ll_grant_cmd(int rs, const HlExecBrokerCmd *cmd)
{
    int rc = 0;
    rc |= ll_allow(rs, cmd->real, LL_EXECUTE | LL_READ_FILE);
    if (cmd->interp[0]) rc |= ll_allow(rs, cmd->interp, LL_EXECUTE | LL_READ_FILE);
    for (int i = 0; READ_SET[i]; i++)
        rc |= ll_allow(rs, READ_SET[i], LL_READ_FILE | LL_READ_DIR);
    rc |= ll_allow(rs, "/dev/null", LL_READ_FILE | LL_WRITE_FILE);
    const uint8_t *cur = cmd->c.fs_read;
    for (unsigned i = 0; i < cmd->c.n_fs_read; i++)
        rc |= ll_allow(rs, hl_exec_next_str(&cur, NULL), LL_READ_FILE | LL_READ_DIR);
    cur = cmd->c.fs_write;
    for (unsigned i = 0; i < cmd->c.n_fs_write; i++)
        rc |= ll_allow(rs, hl_exec_next_str(&cur, NULL),
                       LL_READ_FILE | LL_WRITE_FILE | LL_TRUNCATE | LL_IOCTL_DEV |
                       LL_READ_DIR | LL_MAKE_REG | LL_REMOVE_FILE);
    return rc;
}

/* ── seccomp ───────────────────────────────────────────────────────── */

#if defined(__x86_64__)
#define HX_AUDIT_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define HX_AUDIT_ARCH AUDIT_ARCH_AARCH64
#endif

#define F_LD_NR   BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))
#define F_LD_ARG0 BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]))
#define F_RET(v)  BPF_STMT(BPF_RET | BPF_K, (v))
#define F_ALLOW   F_RET(SECCOMP_RET_ALLOW)
#define F_EPERM   F_RET(SECCOMP_RET_ERRNO | EPERM)

#define HX_CLONE_NS (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
                     CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)

typedef struct { struct sock_filter f[96]; unsigned n; } Prog;

static void p_add(Prog *p, struct sock_filter s) { if (p->n < 96) p->f[p->n++] = s; }

/* nr == x -> return @p ret (the accumulator holds nr). */
static void p_nr_ret(Prog *p, uint32_t nr, uint32_t ret)
{
    p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
    p_add(p, (struct sock_filter)F_RET(ret));
}

static void p_head(Prog *p)
{
#ifdef HX_AUDIT_ARCH
    p_add(p, (struct sock_filter)BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                                          offsetof(struct seccomp_data, arch)));
    p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, HX_AUDIT_ARCH, 1, 0));
    p_add(p, (struct sock_filter)F_EPERM);              /* another ABI */
#endif
    p_add(p, (struct sock_filter)F_LD_NR);
#if defined(__x86_64__)
    p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 0x40000000u, 0, 1));
    p_add(p, (struct sock_filter)F_EPERM);              /* x32 */
#endif
}

/* An argument-0 test for syscall @p nr: reload nr afterwards. */
static void p_arg0(Prog *p, uint32_t nr, int jset, uint32_t k, int match_on,
                   uint32_t ret)
{
    /* if (nr != NR) skip the block */
    p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 4));
    p_add(p, (struct sock_filter)F_LD_ARG0);
    if (jset)
        p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, k,
                                              match_on ? 0 : 1, match_on ? 1 : 0));
    else
        p_add(p, (struct sock_filter)BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k,
                                              match_on ? 0 : 1, match_on ? 1 : 0));
    p_add(p, (struct sock_filter)F_RET(ret));
    p_add(p, (struct sock_filter)F_LD_NR);
}

static int p_install(Prog *p)
{
    p_add(p, (struct sock_filter)F_ALLOW);
    if (p->n >= 96) return -1;
    struct sock_fprog fp = { (unsigned short)p->n, p->f };
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp) == 0 ? 0 : -1;
}

/* The broker's filter: the CEILING every child inherits (§6.2), so it
 * admits what the supported commands need - AF_UNIX sockets included. */
static int seccomp_broker(void)
{
    Prog p = { .n = 0 };
    p_head(&p);
    static const uint32_t refused[] = {
        __NR_ptrace, __NR_process_vm_readv, __NR_process_vm_writev,
        __NR_io_uring_setup, __NR_io_uring_enter, __NR_io_uring_register,
        __NR_bpf, __NR_userfaultfd, __NR_perf_event_open, __NR_kexec_load,
#ifdef __NR_kexec_file_load
        __NR_kexec_file_load,
#endif
        __NR_mount, __NR_umount2, __NR_pivot_root, __NR_unshare, __NR_setns,
        __NR_keyctl, __NR_add_key, __NR_request_key, __NR_open_by_handle_at,
        __NR_init_module, __NR_finit_module, __NR_delete_module,
#ifdef __NR_fsopen
        __NR_fsopen, __NR_fsmount, __NR_move_mount, __NR_open_tree, __NR_fsconfig,
#endif
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++)
        p_nr_ret(&p, refused[i], SECCOMP_RET_ERRNO | EPERM);
    /* clone3 keeps its flags in memory no filter can read: ENOSYS makes
     * glibc fall back to clone, whose flags it can. */
    p_nr_ret(&p, __NR_clone3, SECCOMP_RET_ERRNO | ENOSYS);
    /* clone with a namespace flag */
    p_arg0(&p, __NR_clone, 1, HX_CLONE_NS, 1, SECCOMP_RET_ERRNO | EPERM);
    /* socket: AF_UNIX only */
    p_arg0(&p, __NR_socket, 0, AF_UNIX, 0, SECCOMP_RET_ERRNO | EACCES);
    return p_install(&p);
}

/* A child's own filter, stacked on the broker's (§5.3): it only removes. */
static int seccomp_child(int unix_sockets)
{
    Prog p = { .n = 0 };
    p_head(&p);
    /* No process creation: a grandchild would escape PDEATHSIG. Threads
     * (clone with CLONE_THREAD) stay. */
#ifdef __NR_fork
    p_nr_ret(&p, __NR_fork, SECCOMP_RET_ERRNO | EPERM);
#endif
#ifdef __NR_vfork
    p_nr_ret(&p, __NR_vfork, SECCOMP_RET_ERRNO | EPERM);
#endif
    p_arg0(&p, __NR_clone, 1, CLONE_THREAD, 0, SECCOMP_RET_ERRNO | EPERM);
    /* Nothing that clears or replaces PDEATHSIG (§5.3 step 1). */
    static const uint32_t creds[] = {
        __NR_setuid, __NR_setgid, __NR_setreuid, __NR_setregid, __NR_setresuid,
        __NR_setresgid, __NR_setfsuid, __NR_setfsgid, __NR_setgroups, __NR_capset,
    };
    for (size_t i = 0; i < sizeof creds / sizeof creds[0]; i++)
        p_nr_ret(&p, creds[i], SECCOMP_RET_ERRNO | EPERM);
    p_arg0(&p, __NR_prctl, 0, PR_SET_PDEATHSIG, 1, SECCOMP_RET_ERRNO | EPERM);
    if (!unix_sockets) p_nr_ret(&p, __NR_socket, SECCOMP_RET_ERRNO | EACCES);
    return p_install(&p);
}

/* ── The interface ─────────────────────────────────────────────────── */

int hl_exec_plat_broker_init(int *watch_fd)
{
    /* Die with the server (it spawned this from its main thread, before
     * any other thread existed): the kernel ends the broker itself. */
    *watch_fd = -1;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() == 1) return -1;
    return 0;
}

int hl_exec_plat_leaf_check(int fd, const char *real, char *why, size_t why_size)
{
    /* File capabilities raise privilege without NNP and clear PDEATHSIG on
     * exec (§5.3). */
    if (fgetxattr(fd, "security.capability", NULL, 0) >= 0) {
        snprintf(why, why_size, "%s carries file capabilities", real);
        return -1;
    }
    return 0;
}

int hl_exec_plat_binary_check(int fd, const char *real, char *interp,
                              size_t isz, char *why, size_t why_size)
{
    interp[0] = '\0';
    unsigned char id[EI_NIDENT];
    if (pread(fd, id, sizeof id, 0) != (ssize_t)sizeof id ||
        memcmp(id, ELFMAG, SELFMAG) != 0) {
        snprintf(why, why_size, "%s is not an ELF executable (a script would "
                 "bring its interpreter)", real);
        return -1;
    }
    if (id[EI_CLASS] != ELFCLASS64 || id[EI_DATA] != ELFDATA2LSB) {
        snprintf(why, why_size, "%s is not a 64-bit little-endian ELF", real);
        return -1;
    }
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum > 256) {
        snprintf(why, why_size, "%s has a malformed ELF header", real);
        return -1;
    }
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph;
        if (pread(fd, &ph, sizeof ph, (off_t)(eh.e_phoff + i * sizeof ph)) !=
            (ssize_t)sizeof ph) {
            snprintf(why, why_size, "%s has a malformed program header", real);
            return -1;
        }
        if (ph.p_type != PT_INTERP) continue;
        if (ph.p_filesz < 2 || ph.p_filesz >= isz) {
            snprintf(why, why_size, "%s has a malformed PT_INTERP", real);
            return -1;
        }
        if (pread(fd, interp, ph.p_filesz, (off_t)ph.p_offset) != (ssize_t)ph.p_filesz ||
            interp[ph.p_filesz - 1] != '\0' || interp[0] != '/') {
            snprintf(why, why_size, "%s has a malformed PT_INTERP", real);
            interp[0] = '\0';
            return -1;
        }
        return 0;
    }
    return 0;
}

int hl_exec_plat_lock(HlExecBrokerCmd *cmds, int n, unsigned flags,
                      const char *self, HlExecReport *r)
{
    (void)self;
    /* The rights each guarantee needs (§6.5). */
    ll_probe();
    int sandbox = !(flags & HL_EXEC_POLICY_NO_SANDBOX);
    if (sandbox) {
        static const struct { int abi; const char *what; } need[] = {
            { 1, "no Landlock: exec is not confined to the declared binaries" },
            { 2, "no REFER (ABI 2): rename / link out of a granted tree not refused" },
            { 3, "no TRUNCATE (ABI 3): truncation outside fs.write not refused" },
            { 5, "no IOCTL_DEV (ABI 5): device ioctls not narrowed per command" },
            { 6, "no scopes (ABI 6): signals and abstract sockets not scoped" },
        };
        int missing = 0;
        for (size_t k = 0; k < sizeof need / sizeof need[0]; k++) {
            if (g_ll_abi >= need[k].abi) continue;
            missing = 1;
            hl_exec_report_line(r, "exec: degraded: %s", need[k].what);
        }
        if (missing && !(flags & HL_EXEC_POLICY_DEGRADED_OK)) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", r->buf);
            hl_exec_report_set(r, "exec: this kernel's Landlock (ABI %d) lacks a "
                               "right the exec sandbox needs (ABI 6); pass "
                               "--allow-degraded-sandbox to run without it\n%s",
                               g_ll_abi, tmp);
            return -1;
        }
        /* One ruleset per command, prepared before the broker locks itself. */
        if (g_ll_abi >= 1) {
            for (int i = 0; i < n; i++) {
                int rs = ll_ruleset();
                if (rs < 0 || ll_grant_cmd(rs, &cmds[i]) != 0) {
                    if (rs >= 0) close(rs);
                    hl_exec_report_set(r, "exec.%s: its Landlock ruleset could not "
                                       "be built: %s", cmds[i].c.name, strerror(errno));
                    return -1;
                }
                (void)fcntl(rs, F_SETFD, FD_CLOEXEC);
                cmds[i].plat = rs;
            }
        }
    } else {
        /* The last line: nothing is appended after it. */
        hl_exec_report_line(r, "exec: no sandbox (--no-sandbox)");
    }

    /* Lock down (§6.2): NNP, the broker's domain (the union of the
     * commands' grants), the inherited seccomp ceiling. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        hl_exec_report_set(r, "exec: no_new_privs: %s", strerror(errno));
        return -1;
    }
    if (!sandbox) return 0;
    if (g_ll_abi >= 1) {
        int rs = ll_ruleset();
        int bad = rs < 0;
        for (int i = 0; i < n && !bad; i++)
            bad = ll_grant_cmd(rs, &cmds[i]) != 0;
        if (bad || syscall(__NR_landlock_restrict_self, rs, 0u) != 0) {
            hl_exec_report_set(r, "exec: the broker's Landlock domain could "
                               "not be applied: %s", strerror(errno));
            if (rs >= 0) close(rs);
            return -1;
        }
        close(rs);
    }
    if (seccomp_broker() != 0) {
        hl_exec_report_set(r, "exec: the broker's seccomp filter could not be "
                           "applied: %s", strerror(errno));
        return -1;
    }
    return 0;
}

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

int hl_exec_plat_exit_watch(pid_t pid)
{
    /* Readable the moment it exits (Linux 5.3+). */
    return (int)syscall(__NR_pidfd_open, pid, 0u);
}

int hl_exec_plat_child_death(void)
{
    return prctl(PR_SET_PDEATHSIG, SIGKILL);
}

int hl_exec_plat_child_confine(const HlExecBrokerCmd *cmd, unsigned flags)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return HL_EXEC_STAGE_PRIVS;
    if (!(flags & HL_EXEC_POLICY_NO_SANDBOX) && cmd->plat >= 0 &&
        syscall(__NR_landlock_restrict_self, cmd->plat, 0u) != 0)
        return HL_EXEC_STAGE_CONFINE;
    return 0;
}

int hl_exec_plat_child_filter(const HlExecBrokerCmd *cmd, unsigned flags)
{
    if (flags & HL_EXEC_POLICY_NO_SANDBOX) return 0;
    return seccomp_child(cmd->c.flags & HL_EXEC_CMD_UNIX_SOCKETS);
}

void hl_exec_plat_child_exec(const HlExecBrokerCmd *cmd, int bfd,
                             char *const argv[], char *const envp[])
{
    (void)cmd;
    syscall(__NR_execveat, bfd, "", argv, envp, AT_EMPTY_PATH);
}

#else
typedef int hl_exec_broker_linux_empty;   /* ISO C: no empty translation unit */
#endif /* HL_EXEC_BROKER_SUPPORTED && __linux__ */

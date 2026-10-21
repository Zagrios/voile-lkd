/* test_task_seccomp.c: seccomp(2), prctl(PR_SET_SECCOMP/PR_GET_SECCOMP), the classic BPF checks and the
 * actions a filter takes on the syscalls of lkd_syscall. Models kernel/seccomp.c and net/core/filter.c of
 * Linux v6.12; the Wine case is the filter Proton's ntdll installs (dlls/ntdll/unix/signal_x86_64.c,
 * install_bpf). Kill actions end the process, so they run in a forked child. */
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "sig.h"
#include "task.h"

#define P(x) ((uint64_t)(uintptr_t)(x))

#define BPF_STMT(code, k) { (uint16_t)(code), 0, 0, (uint32_t)(k) }
#define BPF_JUMP(code, k, jt, jf) { (uint16_t)(code), (uint8_t)(jt), (uint8_t)(jf), (uint32_t)(k) }
#define LD_ABS 0x20     /* BPF_LD | BPF_W | BPF_ABS */
#define LD_LEN 0x80     /* BPF_LD | BPF_W | BPF_LEN */
#define LD_MEM 0x60     /* BPF_LD | BPF_MEM */
#define ST 0x02
#define RET_K 0x06
#define RET_A 0x16
#define JEQ_K 0x15
#define JGT_K 0x25
#define JGE_K 0x35
#define JA 0x05
#define DIV_K 0x34
#define DIV_X 0x3c
#define LD_H_ABS 0x28   /* BPF_LD | BPF_H | BPF_ABS: refused by seccomp */
#define OFF_NR offsetof(struct lx_seccomp_data, nr)
#define OFF_ARCH offsetof(struct lx_seccomp_data, arch)
#define OFF_IP offsetof(struct lx_seccomp_data, instruction_pointer)
#define OFF_ARG0 offsetof(struct lx_seccomp_data, args)

static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    lkd_sysreq r = { nr, { a0, a1, a2, 0, 0, 0 } };
    return lkd_dispatch(t, &r);
}

/* The syscall as the engine makes it: through lkd_syscall, from the instruction after SYSCALL at rip. */
static int64_t call_at(lkd_task *t, uint64_t rip, unsigned nr, uint64_t a0)
{
    fake_cpu_of(t)->regs.rip = rip;
    lkd_sysreq r = { nr, { a0, 0, 0, 0, 0, 0 } };
    int64_t rax = 0;
    (void)lkd_syscall(t, &r, &rax);
    return rax;
}

static int64_t install(lkd_task *t, unsigned flags, struct lx_sock_filter *f, unsigned len)
{
    struct lx_sock_fprog prog = { (uint16_t)len, { 0 }, P(f) };
    return sc(t, LKD_NR_seccomp, LX_SECCOMP_SET_MODE_FILTER, flags, P(&prog));
}

static lkd_task *mk(void)
{
    lkd_task *t = fake_make_task();
    fake_cpu_of(t)->regs.rsp = 0x7000000;
    LT_EQ(sc(t, LKD_NR_prctl, LX_PR_SET_NO_NEW_PRIVS, 1, 0), 0);
    return t;
}

/* seccomp_prepare_filter: without no_new_privs (and without CAP_SYS_ADMIN) a filter is refused with
 * EACCES; an empty or too long program is EINVAL; an unreadable one EFAULT. */
LT_TEST(install_rules)
{
    lkd_task *t = fake_make_task();
    struct lx_sock_filter allow[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW) };
    LT_ERRNO(install(t, 0, allow, 1), LX_EACCES);
    LT_EQ(sc(t, LKD_NR_prctl, LX_PR_SET_NO_NEW_PRIVS, 1, 0), 0);
    LT_ERRNO(install(t, 0, allow, 0), LX_EINVAL);
    LT_ERRNO(install(t, 0, allow, LX_BPF_MAXINSNS + 1), LX_EINVAL);
    LT_ERRNO(install(t, 0, (struct lx_sock_filter *)8, 1), LX_EFAULT);
    LT_ERRNO(install(t, LX_SECCOMP_FILTER_FLAG_NEW_LISTENER, allow, 1), LX_EINVAL);
    LT_ERRNO(install(t, 1u << 9, allow, 1), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_seccomp, 7, 0, 0), LX_EINVAL);
    LT_EQ(sc(t, LKD_NR_prctl, LX_PR_GET_SECCOMP, 0, 0), LX_SECCOMP_MODE_DISABLED);
    LT_EQ(install(t, LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW, allow, 1), 0);
    LT_EQ(sc(t, LKD_NR_prctl, LX_PR_GET_SECCOMP, 0, 0), LX_SECCOMP_MODE_FILTER);
    /* seccomp_may_assign_mode: a filtered thread cannot switch to the strict mode */
    LT_ERRNO(sc(t, LKD_NR_seccomp, LX_SECCOMP_SET_MODE_STRICT, 0, 0), LX_EINVAL);
    int mode = -1, filters = -1;
    lkd_seccomp_status(t, &mode, &filters);
    LT_EQ(mode, LX_SECCOMP_MODE_FILTER);
    LT_EQ(filters, 1);
}

/* seccomp_check_filter and bpf_check_classic refuse what the kernel refuses. */
LT_TEST(program_checks)
{
    lkd_task *t = mk();
    struct lx_sock_filter no_ret[] = { BPF_STMT(LD_ABS, OFF_NR) };
    LT_ERRNO(install(t, 0, no_ret, 1), LX_EINVAL);
    struct lx_sock_filter misaligned[] = { BPF_STMT(LD_ABS, 2), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, misaligned, 2), LX_EINVAL);
    struct lx_sock_filter beyond[] = { BPF_STMT(LD_ABS, 64), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, beyond, 2), LX_EINVAL);
    struct lx_sock_filter half[] = { BPF_STMT(LD_H_ABS, OFF_NR), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, half, 2), LX_EINVAL);
    struct lx_sock_filter far_jump[] = { BPF_JUMP(JEQ_K, 0, 1, 0), BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW) };
    LT_ERRNO(install(t, 0, far_jump, 2), LX_EINVAL);
    struct lx_sock_filter far_ja[] = { BPF_STMT(JA, 1), BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW) };
    LT_ERRNO(install(t, 0, far_ja, 2), LX_EINVAL);
    struct lx_sock_filter div_zero[] = { BPF_STMT(DIV_K, 0), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, div_zero, 2), LX_EINVAL);
    struct lx_sock_filter unset_cell[] = { BPF_STMT(LD_MEM, 3), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, unset_cell, 2), LX_EINVAL);
    /* a cell written on one branch only is still unset where the branches meet */
    struct lx_sock_filter one_branch[] = {
        BPF_JUMP(JEQ_K, 0, 0, 1), BPF_STMT(ST, 3), BPF_STMT(LD_MEM, 3), BPF_STMT(RET_A, 0) };
    LT_ERRNO(install(t, 0, one_branch, 4), LX_EINVAL);
    struct lx_sock_filter both[] = { BPF_STMT(ST, 3), BPF_STMT(LD_MEM, 3), BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW) };
    LT_EQ(install(t, 0, both, 3), 0);
    LT_EQ(sc(t, LKD_NR_prctl, LX_PR_GET_SECCOMP, 0, 0), LX_SECCOMP_MODE_FILTER);
}

/* SECCOMP_RET_ERRNO answers -data, at most MAX_ERRNO, without running the call; the length of seccomp_data
 * loads as 64; a division by a zero X makes the filter return 0, which kills (checked in a child below). */
LT_TEST(errno_action_and_loads)
{
    lkd_task *t = mk();
    struct lx_sock_filter f[] = {
        BPF_STMT(LD_LEN, 0),
        BPF_JUMP(JEQ_K, sizeof(struct lx_seccomp_data), 1, 0),
        BPF_STMT(RET_K, LX_SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(LD_ABS, OFF_NR),
        BPF_JUMP(JEQ_K, LKD_NR_getppid, 0, 1),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ERRNO | 9999),
        BPF_JUMP(JEQ_K, LKD_NR_getuid, 0, 1),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ERRNO | LX_EPERM),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW),
    };
    LT_EQ(install(t, 0, f, 9), 0);
    LT_EQ(call_at(t, 0x7f0000001000, LKD_NR_getuid, 0), -LX_EPERM);
    LT_EQ(call_at(t, 0x7f0000001000, LKD_NR_getppid, 0), -4095);
    LT_EQ(call_at(t, 0x7f0000001000, LKD_NR_getpid, 0), t->proc->pid);
}

/* Filters run newest first and the action of lowest precedence value wins: ERRNO over LOG and ALLOW,
 * TRAP over ERRNO (seccomp_run_filters, ACTION_ONLY). */
LT_TEST(precedence)
{
    lkd_task *t = mk();
    struct lx_sock_filter errno_f[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_ERRNO | 5) };
    struct lx_sock_filter log_f[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_LOG) };
    LT_EQ(install(t, 0, errno_f, 1), 0);
    LT_EQ(install(t, LX_SECCOMP_FILTER_FLAG_LOG, log_f, 1), 0);
    LT_EQ(call_at(t, 0x7f0000001000, LKD_NR_getpid, 0), -5);
    int mode, filters;
    lkd_seccomp_status(t, &mode, &filters);
    LT_EQ(filters, 2);
}

/* Proton's ntdll filter: syscalls from the native libraries, above 0x700000000000, run; the ones Windows
 * code makes below trap. SECCOMP_RET_TRAP skips the call, leaves its number in RAX (syscall_rollback) and
 * forces SIGSYS with SYS_SECCOMP, the address after SYSCALL, the number and the arch to the handler. */
LT_TEST(wine_trap_filter)
{
    lkd_task *t = mk();
    struct lx_sock_filter f[] = {
        BPF_STMT(LD_ABS, OFF_IP + 4),
        BPF_JUMP(JGT_K, 0x700000000000ull >> 32, 0, 1),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW),
        BPF_STMT(LD_ABS, OFF_ARCH),
        BPF_JUMP(JEQ_K, LX_AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW),
        BPF_STMT(LD_ABS, OFF_IP),
        BPF_JUMP(JGE_K, 0x7d400000, 1, 0),
        BPF_STMT(RET_K, LX_SECCOMP_RET_TRAP),
        BPF_JUMP(JGE_K, 0x7d402000, 0, 1),
        BPF_STMT(RET_K, LX_SECCOMP_RET_TRAP),
        BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW),
    };
    LT_EQ(install(t, LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW, f, 12), 0);
    LT_EQ(call_at(t, 0x7efff5dc1234ull, LKD_NR_getpid, 0), t->proc->pid);
    LT_EQ(call_at(t, 0x7d400010ull, LKD_NR_getpid, 0), t->proc->pid); /* the preloader */
    LT_EQ(lkd_signal_pending(t), 0);

    struct lx_sigaction a = { 0x401000, LX_SA_SIGINFO | LX_SA_RESTORER, 0x2000, 0 };
    lkd_sysreq act = { LKD_NR_rt_sigaction, { LX_SIGSYS, P(&a), 0, 8, 0, 0 } };
    LT_EQ(lkd_dispatch(t, &act), 0);

    /* NtAllocateVirtualMemory is 0x18 in Wine, sched_yield in Linux: it must never run */
    LT_EQ(call_at(t, 0x14027fb09ull, 0x18, 0xffffffffffffffffull), 0x18);
    fake_engine_reset();
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_frame_build_count, 1);
    LT_EQ(fake_last_delivery.signo, LX_SIGSYS);
    LT_EQ(fake_last_delivery.handler, 0x401000);
    LT_EQ(fake_last_delivery.info.si_code, LX_SYS_SECCOMP);
    LT_EQ(fake_last_delivery.info.si_errno, 0);
    LT_EQ(fake_last_delivery.info.u.sys.call_addr, 0x14027fb09ull);
    LT_EQ(fake_last_delivery.info.u.sys.syscall, 0x18);
    LT_EQ(fake_last_delivery.info.u.sys.arch, LX_AUDIT_ARCH_X86_64);
}

/* copy_seccomp: threads and children created after a filter run it; SECCOMP_FILTER_FLAG_TSYNC gives it to
 * the threads created before, and refuses (with the tid, or ESRCH) when one runs a filter that is not an
 * ancestor of the caller's. */
LT_TEST(inheritance_and_tsync)
{
    lkd_task *t = mk();
    lkd_task *before = fake_make_thread(t);
    lkd_set_current(t);
    struct lx_sock_filter e1[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_ERRNO | 1) };
    LT_EQ(install(t, 0, e1, 1), 0);
    lkd_task *after = fake_make_thread(t);
    lkd_set_current(t);
    LT_EQ(call_at(after, 0x7f0000001000, LKD_NR_getpid, 0), -1);
    LT_EQ(call_at(before, 0x7f0000001000, LKD_NR_getpid, 0), t->proc->pid);

    struct lx_sock_filter e2[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_ERRNO | 2) };
    LT_EQ(install(after, 0, e2, 1), 0);   /* after now runs a filter t's chain does not hold */
    LT_EQ(install(t, LX_SECCOMP_FILTER_FLAG_TSYNC, e2, 1), after->tid);
    LT_ERRNO(install(t, LX_SECCOMP_FILTER_FLAG_TSYNC | LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH, e2, 1), LX_ESRCH);

    lkd_task *u = mk();
    lkd_task *old = fake_make_thread(u);
    lkd_set_current(u);
    LT_EQ(install(u, LX_SECCOMP_FILTER_FLAG_TSYNC, e1, 1), 0);
    LT_EQ(call_at(old, 0x7f0000001000, LKD_NR_getpid, 0), -1);
    int mode, filters;
    lkd_seccomp_status(old, &mode, &filters);
    LT_EQ(mode, LX_SECCOMP_MODE_FILTER);
    LT_EQ(filters, 1);
}

/* SECCOMP_GET_ACTION_AVAIL and SECCOMP_GET_NOTIF_SIZES. No listener can exist: USER_NOTIF is not offered. */
LT_TEST(queries)
{
    lkd_task *t = mk();
    uint32_t action = LX_SECCOMP_RET_TRAP;
    LT_EQ(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_ACTION_AVAIL, 0, P(&action)), 0);
    action = LX_SECCOMP_RET_KILL_PROCESS;
    LT_EQ(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_ACTION_AVAIL, 0, P(&action)), 0);
    action = LX_SECCOMP_RET_USER_NOTIF;
    LT_ERRNO(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_ACTION_AVAIL, 0, P(&action)), LX_EOPNOTSUPP);
    action = 0x00010000;
    LT_ERRNO(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_ACTION_AVAIL, 0, P(&action)), LX_EOPNOTSUPP);
    LT_ERRNO(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_ACTION_AVAIL, 1, P(&action)), LX_EINVAL);
    struct lx_seccomp_notif_sizes sizes;
    LT_EQ(sc(t, LKD_NR_seccomp, LX_SECCOMP_GET_NOTIF_SIZES, 0, P(&sizes)), 0);
    LT_EQ(sizes.seccomp_data, 64);
}

/* Kills, each in a forked child: the strict mode kills anything but read, write, exit and rt_sigreturn
 * with SIGKILL; SECCOMP_RET_KILL_PROCESS and a filter that returns 0 (a division by a zero X) end the
 * process by SIGSYS. The wait status carries the signal. */
static int child_status(int (*body)(void))
{
    pid_t pid = fork();
    if (pid == 0) _exit(body());
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

static int64_t kill_then_report(lkd_task *t, unsigned nr)
{
    int64_t rax = call_at(t, 0x7f0000001000, nr, 0);
    int st = 0;
    if (rax == 0 && lkd_task_release(t, &st)) _exit(st & 0x7f ? 128 + (st & 0x7f) : 3);
    return rax;
}

static int strict_child(void)
{
    lkd_task *t = mk();
    if (sc(t, LKD_NR_seccomp, LX_SECCOMP_SET_MODE_STRICT, 0, 0) != 0) return 1;
    if (call_at(t, 0x7f0000001000, LKD_NR_read, (uint64_t)-1) != -LX_EBADF) return 2;
    (void)kill_then_report(t, LKD_NR_getpid);
    return 4;
}

static int kill_process_child(void)
{
    lkd_task *t = mk();
    struct lx_sock_filter f[] = { BPF_STMT(RET_K, LX_SECCOMP_RET_KILL_PROCESS) };
    if (install(t, 0, f, 1) != 0) return 1;
    (void)kill_then_report(t, LKD_NR_getpid);
    return 4;
}

static int div_zero_child(void)
{
    lkd_task *t = mk();
    struct lx_sock_filter g[] = { BPF_STMT(0x01, 0) /* LDX IMM 0 */, BPF_STMT(DIV_X, 0),
                                  BPF_STMT(RET_K, LX_SECCOMP_RET_ALLOW) };
    if (install(t, 0, g, 3) != 0) return 1;
    (void)kill_then_report(t, LKD_NR_getpid);
    return 4;
}

LT_TEST(kills)
{
    int st = child_status(strict_child);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 128 + LX_SIGKILL);
    st = child_status(kill_process_child);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 128 + LX_SIGSYS);
    st = child_status(div_zero_child);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 128 + LX_SIGSYS);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    LT_RUN(install_rules);
    LT_RUN(program_checks);
    LT_RUN(errno_action_and_loads);
    LT_RUN(precedence);
    LT_RUN(wine_trap_filter);
    LT_RUN(inheritance_and_tsync);
    LT_RUN(queries);
    LT_RUN(kills);
    return lt_finish();
}

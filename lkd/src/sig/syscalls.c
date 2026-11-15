/* syscalls.c: signal system calls. Owner: sig. Models kernel/signal.c v6.12 (SYSCALL_DEFINEs of
 * rt_sigaction, rt_sigprocmask, rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo, rt_tgsigqueueinfo,
 * rt_sigsuspend, sigaltstack, kill, tkill, tgkill, pause, pidfd_send_signal) and
 * arch/x86/kernel/signal_64.c rt_sigreturn. */
#include "sig_priv.h"

int sig_kill(lkd_task *t, int32_t pid, int sig, const struct lx_siginfo *info);
int sig_tkill(lkd_task *t, int32_t tgid, int32_t tid, int sig);

#define KSIGINFO_BYTES 48 /* sizeof(struct kernel_siginfo): the rest of siginfo_t is not kept */

static int copy_siginfo_in(uint64_t gva, int sig, struct lx_siginfo *info)
{
    if (lkd_user_read(gva, info, sizeof *info) < 0) return -LX_EFAULT;
    memset((char *)info + KSIGINFO_BYTES, 0, sizeof *info - KSIGINFO_BYTES);
    info->si_signo = sig;
    return 0;
}

LKD_SYSCALL(rt_sigaction)
{
    int sig = LKD_ARGI(0);
    uint64_t uact = LKD_ARGP(1), uoact = LKD_ARGP(2);
    if (LKD_ARGU(3) != LX_SIGSET_BYTES) return -LX_EINVAL;
    struct lx_sigaction na, oa;
    if (uact && lkd_user_read(uact, &na, sizeof na) < 0) return -LX_EFAULT;
    if (sig < 1 || sig > LX_NSIG || (uact && (sig == LX_SIGKILL || sig == LX_SIGSTOP))) return -LX_EINVAL;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct lkd_sighand *h = sig_hand(t);
    sig_lock(sp);
    pthread_mutex_lock(&h->lock);
    oa = h->act[sig - 1];
    oa.flags &= SIG_UAPI_SA_FLAGS;
    if (uact) {
        int was_ignored = h->act[sig - 1].handler == LX_SIG_IGN;
        (void)was_ignored;
        na.flags &= SIG_UAPI_SA_FLAGS;
        na.mask &= ~LX_SIG_UNBLOCKABLE;
        h->act[sig - 1] = na;
        sig_hand_update_masks(h);
        /* POSIX 3.3.1.3: setting a signal to be ignored discards it from every pending set */
        uint64_t hd = na.handler;
        if (hd == LX_SIG_IGN || (hd == LX_SIG_DFL && (SIG_KERNEL_IGNORE_MASK & LX_SIGBIT(sig))))
            sig_flush_process_locked(t->proc, sp, LX_SIGBIT(sig));
    }
    pthread_mutex_unlock(&h->lock);
    sig_unlock(sp);
    if (uoact && lkd_user_write(uoact, &oa, sizeof oa) < 0) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(rt_sigprocmask)
{
    int how = LKD_ARGI(0);
    uint64_t uset = LKD_ARGP(1), uoset = LKD_ARGP(2);
    if (LKD_ARGU(3) != LX_SIGSET_BYTES) return -LX_EINVAL;
    uint64_t old = atomic_load(&t->sigmask);
    if (uset) {
        uint64_t set;
        if (lkd_user_read(uset, &set, sizeof set) < 0) return -LX_EFAULT;
        set &= ~LX_SIG_UNBLOCKABLE;
        uint64_t nm;
        switch (how) {
        case LX_SIG_BLOCK: nm = old | set; break;
        case LX_SIG_UNBLOCK: nm = old & ~set; break;
        case LX_SIG_SETMASK: nm = set; break;
        default: return -LX_EINVAL;
        }
        lkd_sig_set_mask(t, nm);
    }
    if (uoset && lkd_user_write(uoset, &old, sizeof old) < 0) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(rt_sigpending)
{
    uint64_t size = LKD_ARGU(1);
    if (size > LX_SIGSET_BYTES) return -LX_EINVAL;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    sig_lock(sp);
    uint64_t set = (atomic_load(&t->pending.bits) | atomic_load(&sp->shared_bits)) & atomic_load(&t->sigmask);
    sig_unlock(sp);
    if (lkd_user_write(LKD_ARGP(0), &set, size) < 0) return -LX_EFAULT;
    return 0;
}

int sig_read_timeout(uint64_t uts, int64_t *rel_ns)
{
    struct lx_timespec ts;
    if (lkd_user_read(uts, &ts, sizeof ts) < 0) return -LX_EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -LX_EINVAL;
    if (ts.tv_sec > 1000000000ll) ts.tv_sec = 1000000000ll; /* 31 years, so that the ns count fits */
    *rel_ns = ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return 0;
}

/* do_sigtimedwait: the waited set is unblocked during the wait; real_blocked keeps the old mask. */
LKD_SYSCALL(rt_sigtimedwait)
{
    uint64_t uset = LKD_ARGP(0), uinfo = LKD_ARGP(1), uts = LKD_ARGP(2);
    if (LKD_ARGU(3) != LX_SIGSET_BYTES) return -LX_EINVAL;
    uint64_t set;
    if (lkd_user_read(uset, &set, sizeof set) < 0) return -LX_EFAULT;
    int64_t rel = -1;
    if (uts) { int e = sig_read_timeout(uts, &rel); if (e < 0) return e; }
    set &= ~LX_SIG_UNBLOCKABLE;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct lx_siginfo info;
    int r = 0;
    sig_lock(sp);
    int sig = sig_dequeue_locked(t, sp, set, &info);
    sig_recalc_locked(t, sp);
    if (!sig && rel != 0) {
        struct sig_task *st = sig_task_of(t);
        st->real_blocked = atomic_load(&t->sigmask);
        atomic_store(&t->sigmask, st->real_blocked & ~set);
        sig_recalc_locked(t, sp);
        sig_unlock(sp);
        r = lkd_block_sleep(t, rel < 0 ? -1 : lkd_host_mono_ns() + rel);
        struct sig_wakeset ws = { 0 };
        sig_lock(sp);
        uint64_t rb = st->real_blocked;
        st->real_blocked = 0;
        sig_set_blocked_locked(t, sp, rb, &ws);
        sig = sig_dequeue_locked(t, sp, set, &info);
        sig_recalc_locked(t, sp);
        sig_unlock(sp);
        sig_wakeset_run(&ws);
    } else {
        sig_unlock(sp);
    }
    if (sig) {
        if (uinfo && lkd_user_write(uinfo, &info, sizeof info) < 0) return -LX_EFAULT;
        return sig;
    }
    return (rel != 0 && r < 0) ? -LX_EINTR : -LX_EAGAIN;
}

LKD_SYSCALL(rt_sigsuspend)
{
    if (LKD_ARGU(1) != LX_SIGSET_BYTES) return -LX_EINVAL;
    uint64_t mask;
    if (lkd_user_read(LKD_ARGP(0), &mask, sizeof mask) < 0) return -LX_EFAULT;
    lkd_sig_set_temp_mask(t, mask);
    while (lkd_block_sleep(t, -1) == 0) { }
    return -LX_ERESTARTNOHAND;
}

LKD_SYSCALL(pause)
{
    (void)rq;
    while (lkd_block_sleep(t, -1) == 0) { }
    return -LX_ERESTARTNOHAND;
}

LKD_SYSCALL(sigaltstack)
{
    uint64_t uss = LKD_ARGP(0), uoss = LKD_ARGP(1);
    struct lx_stack_t ss, oss;
    if (uss && lkd_user_read(uss, &ss, sizeof ss) < 0) return -LX_EFAULT;
    int e = sig_do_sigaltstack(t, uss ? &ss : NULL, uoss ? &oss : NULL, sig_current_sp(t));
    if (!e && uoss && lkd_user_write(uoss, &oss, sizeof oss) < 0) return -LX_EFAULT;
    return e;
}

LKD_SYSCALL(rt_sigreturn)
{
    (void)rq;
    const lkd_engine_ops *ops = lkd_engine();
    uint64_t mask = 0;
    struct lx_stack_t st;
    memset(&st, 0, sizeof st);
    t->restart.fn = NULL;
    int e = ops && ops->frame_restore ? ops->frame_restore(t, &mask, &st) : -LX_EFAULT;
    if (e < 0) {
        /* badframe: signal_fault -> force_sig(SIGSEGV) */
        struct lx_siginfo info;
        memset(&info, 0, sizeof info);
        info.si_signo = LX_SIGSEGV;
        info.si_code = LX_SI_KERNEL;
        sig_send_task_full(t, LX_SIGSEGV, &info, 1, 1);
        return 0;
    }
    lkd_sig_set_mask(t, mask);
    (void)sig_do_sigaltstack(t, &st, NULL, sig_current_sp(t)); /* restore_altstack squashes errors */
    return LKD_RET_CONTEXT_REPLACED;
}

LKD_SYSCALL(kill)
{
    int32_t pid = (int32_t)LKD_ARGI(0);
    int sig = LKD_ARGI(1);
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    return sig_kill(t, pid, sig, NULL);
}

LKD_SYSCALL(tkill)
{
    int32_t tid = LKD_ARGI(0);
    int sig = LKD_ARGI(1);
    if (tid <= 0) return -LX_EINVAL;
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    return sig_tkill(t, 0, tid, sig);
}

LKD_SYSCALL(tgkill)
{
    int32_t tgid = LKD_ARGI(0), tid = LKD_ARGI(1);
    int sig = LKD_ARGI(2);
    if (tid <= 0 || tgid <= 0) return -LX_EINVAL;
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    return sig_tkill(t, tgid, tid, sig);
}

LKD_SYSCALL(rt_sigqueueinfo)
{
    int32_t pid = LKD_ARGI(0);
    int sig = LKD_ARGI(1);
    struct lx_siginfo info;
    if (copy_siginfo_in(LKD_ARGP(2), sig, &info) < 0) return -LX_EFAULT;
    if ((info.si_code >= 0 || info.si_code == LX_SI_TKILL) && pid != t->proc->pid) return -LX_EPERM;
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    if (pid <= 0) return -LX_ESRCH; /* kill_proc_info: find_vpid of a non-positive pid finds nothing */
    return lkd_sig_send_pid(t, pid, sig, &info);
}

LKD_SYSCALL(rt_tgsigqueueinfo)
{
    int32_t tgid = LKD_ARGI(0), tid = LKD_ARGI(1);
    int sig = LKD_ARGI(2);
    struct lx_siginfo info;
    if (copy_siginfo_in(LKD_ARGP(3), sig, &info) < 0) return -LX_EFAULT;
    if (tid <= 0 || tgid <= 0) return -LX_EINVAL;
    if ((info.si_code >= 0 || info.si_code == LX_SI_TKILL) && tid != t->tid) return -LX_EPERM;
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    return lkd_sig_send_tid(t, tgid, tid, sig, &info);
}

/* pidfd objects belong to the task module: ops->name "pidfd", priv starts with the int32 pid
 * (docs/requests/sig-2.md). Anything else is not a pidfd: EBADF. */
LKD_SYSCALL(pidfd_send_signal)
{
    int fd = LKD_ARGI(0);
    int sig = LKD_ARGI(1);
    uint64_t uinfo = LKD_ARGP(2);
    unsigned flags = LKD_ARGUI(3);
    if (flags & ~0x7u) return -LX_EINVAL; /* PIDFD_SIGNAL_THREAD | _THREAD_GROUP | _PROCESS_GROUP */
    if (flags & (flags - 1)) return -LX_EINVAL;
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int is_pidfd = o->ops && o->ops->name && strcmp(o->ops->name, "pidfd") == 0 && o->priv;
    int32_t pid = is_pidfd ? *(int32_t *)o->priv : 0;
    lkd_fdobj_put(o);
    if (!is_pidfd) return -LX_EBADF;
    if (sig < 0 || sig > LX_NSIG) return -LX_EINVAL;
    struct lx_siginfo info, *ip = NULL;
    if (uinfo) {
        if (lkd_user_read(uinfo, &info, sizeof info) < 0) return -LX_EFAULT;
        if (info.si_signo != sig) return -LX_EINVAL;
        memset((char *)&info + KSIGINFO_BYTES, 0, sizeof info - KSIGINFO_BYTES);
        if ((pid != t->proc->pid || (flags & 4u)) && (info.si_code >= 0 || info.si_code == LX_SI_TKILL)) return -LX_EPERM;
        ip = &info;
    }
    if (flags & 1u) return lkd_sig_send_tid(t, 0, pid, sig, ip);
    if (flags & 4u) { int r = lkd_sig_send_pgrp(t, pid, sig, ip); return r < 0 ? r : 0; }
    return lkd_sig_send_pid(t, pid, sig, ip);
}

void sig_syscalls_register(void)
{
    LKD_REGISTER(rt_sigaction);
    LKD_REGISTER(rt_sigprocmask);
    LKD_REGISTER(rt_sigreturn);
    LKD_REGISTER(rt_sigpending);
    LKD_REGISTER(rt_sigtimedwait);
    LKD_REGISTER(rt_sigqueueinfo);
    LKD_REGISTER(rt_sigsuspend);
    LKD_REGISTER(rt_tgsigqueueinfo);
    LKD_REGISTER(sigaltstack);
    LKD_REGISTER(kill);
    LKD_REGISTER(tkill);
    LKD_REGISTER(tgkill);
    LKD_REGISTER(pause);
    LKD_REGISTER(pidfd_send_signal);
}

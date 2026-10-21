/* deliver.c: the delivery protocol with the engine, restart decisions, job-control stops, faults.
 * Owner: sig. Models kernel/signal.c v6.12 get_signal, do_signal_stop, signal_delivered,
 * force_sig_info_to_task, do_sigaltstack, and arch/x86/kernel/signal.c handle_signal,
 * arch_do_signal_or_restart, get_sigframe. */
#include <poll.h>
#include <unistd.h>

#include "sig_priv.h"

/* ---- lock-free checks ------------------------------------------------------------------------- */

static uint64_t shared_bits_of(const lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc_peek(t->proc);
    return sp ? atomic_load(&sp->shared_bits) : 0;
}

int lkd_sig_interrupted(const lkd_task *t)
{
    uint32_t f = atomic_load(&((lkd_task *)t)->flags);
    if (f & (LKD_TF_FATAL | LKD_TF_SIGPENDING | LKD_TF_GROUP_STOP | LKD_TF_TRACE_STOP)) return 1;
    uint64_t bits = atomic_load(&((lkd_task *)t)->pending.bits) | shared_bits_of(t);
    return (bits & ~atomic_load(&((lkd_task *)t)->sigmask)) != 0;
}

int lkd_signal_pending(const lkd_task *t)
{
    return lkd_sig_interrupted(t) || t->restore_sigmask;
}

/* ---- masks ------------------------------------------------------------------------------------ */

void lkd_sig_set_mask(lkd_task *t, uint64_t mask)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    sig_set_blocked_locked(t, sp, mask, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

void lkd_sig_set_temp_mask(lkd_task *t, uint64_t mask)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    if (!t->restore_sigmask) t->saved_sigmask = atomic_load(&t->sigmask);
    t->restore_sigmask = 1;
    sig_set_blocked_locked(t, sp, mask, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

/* ---- alternate stack -------------------------------------------------------------------------- */

static int on_sig_stack(lkd_task *t, uint64_t sp)
{
    if ((uint32_t)t->altstack.ss_flags & LX_SS_AUTODISARM) return 0;
    return sp > t->altstack.ss_sp && sp - t->altstack.ss_sp <= t->altstack.ss_size;
}

int sig_altstack_flags(lkd_task *t, uint64_t sp)
{
    if (!t->altstack.ss_size) return LX_SS_DISABLE;
    return on_sig_stack(t, sp) ? LX_SS_ONSTACK : 0;
}

uint64_t sig_current_sp(lkd_task *t)
{
    const lkd_engine_ops *ops = lkd_engine();
    lkd_regs r;
    if (!ops || !ops->get_regs || ops->get_regs(t, &r) < 0) return 0;
    return r.rsp;
}

/* do_sigaltstack (kernel/signal.c): returns 0, -EPERM (on the stack), -EINVAL, -ENOMEM. */
int sig_do_sigaltstack(lkd_task *t, const struct lx_stack_t *ss, struct lx_stack_t *oss, uint64_t sp)
{
    if (oss) {
        memset(oss, 0, sizeof *oss);
        oss->ss_sp = t->altstack.ss_sp;
        oss->ss_size = t->altstack.ss_size;
        oss->ss_flags = sig_altstack_flags(t, sp) | (int32_t)((uint32_t)t->altstack.ss_flags & LX_SS_AUTODISARM);
    }
    if (ss) {
        uint32_t flags = (uint32_t)ss->ss_flags;
        if (on_sig_stack(t, sp)) return -LX_EPERM;
        uint32_t mode = flags & ~LX_SS_AUTODISARM;
        if (mode != LX_SS_DISABLE && mode != LX_SS_ONSTACK && mode != 0) return -LX_EINVAL;
        uint64_t ssp = ss->ss_sp, size = ss->ss_size;
        if (t->altstack.ss_sp == ssp && t->altstack.ss_size == size && (uint32_t)t->altstack.ss_flags == flags) return 0;
        if (mode == LX_SS_DISABLE) { size = 0; ssp = 0; }
        else if (size < LX_MINSIGSTKSZ) return -LX_ENOMEM;
        t->altstack.ss_sp = ssp;
        t->altstack.ss_size = size;
        t->altstack.ss_flags = (int32_t)flags;
    }
    return 0;
}

/* ---- the process dies ------------------------------------------------------------------------- */

static int die(lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    int run = 0, status = 0;
    sig_lock(sp);
    if (sp->group_exit && !sp->group_exit_done) {
        sp->group_exit_done = 1;
        run = 1;
        status = sp->group_exit_status;
    }
    sig_unlock(sp);
    atomic_fetch_or(&t->flags, LKD_TF_FATAL);
    if (run) (void)lkd_task_do_exit_group(t, status);
    return -1;
}

static int core_dumps(struct lkd_proc *p)
{
    /* No core file is written. The wait status reports a dump when Linux would attempt one with a
     * file core_pattern: RLIMIT_CORE soft limit not zero and the process dumpable. */
    return p->rlimits[LX_RLIMIT_CORE].rlim_cur != 0 && p->dumpable;
}

static int start_group_exit(lkd_task *t, int status)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    sig_lock(sp);
    if (!sp->group_exit) {
        sp->group_exit = 1;
        sp->group_exit_status = status;
    }
    sig_unlock(sp);
    return die(t);
}

void sig_trace_death(lkd_task *t, int sig, const struct lx_siginfo *info, int current)
{
    int code = info ? info->si_code : LX_SI_KERNEL;
    int fault = code > 0 && code != LX_SI_KERNEL &&
                (sig == LX_SIGSEGV || sig == LX_SIGBUS || sig == LX_SIGILL || sig == LX_SIGFPE || sig == LX_SIGTRAP);
    if (fault) {
        const lkd_engine_ops *ops = lkd_engine();
        lkd_regs r;
        if (current && ops && ops->get_regs && ops->get_regs(t, &r) == 0)
            lkd_log("pid %d dies of signal %d in tid %d: fault at %#llx (si_code %d), rip %#llx", t->proc->pid, sig,
                    t->tid, (unsigned long long)info->u.fault.addr, code, (unsigned long long)r.rip);
        else
            lkd_log("pid %d dies of signal %d in tid %d: fault at %#llx (si_code %d)", t->proc->pid, sig, t->tid,
                    (unsigned long long)info->u.fault.addr, code);
    } else if (info && code <= 0) {
        lkd_log("pid %d dies of signal %d in tid %d: sent by pid %d (si_code %d)", t->proc->pid, sig, t->tid,
                info->u.kill.pid, code);
    } else {
        lkd_log("pid %d dies of signal %d in tid %d (si_code %d)", t->proc->pid, sig, t->tid, code);
    }
}

/* ---- job control ------------------------------------------------------------------------------ */

/* do_signal_stop: the first stop signal of a group stop marks every thread; each parks at its next
 * safe point; the last one to park reports CLD_STOPPED to the parent. */
static void initiate_stop(lkd_task *t, struct lkd_sigproc *sp, int sig, struct sig_wakeset *ws)
{
    if (sp->stop_state != SP_RUNNING || sp->group_exit) return;
    sp->stop_state = SP_STOPPING;
    sp->stop_signo = sig;
    sp->stop_count = 0;
    pthread_mutex_lock(&t->proc->lock);
    for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc) {
        if (atomic_load(&x->flags) & LKD_TF_EXITING) continue;
        atomic_fetch_or(&x->flags, LKD_TF_GROUP_STOP);
        sp->stop_count++;
        if (x != t) sig_wakeset_add(ws, x);
    }
    pthread_mutex_unlock(&t->proc->lock);
}

/* Block until the stop ends (SIGCONT) or the task must die. Returns -1 if fatal. */
static int park(lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    int notify = 0, signo = 0;
    sig_lock(sp);
    atomic_fetch_and(&t->flags, ~LKD_TF_GROUP_STOP);
    if (sp->stop_state == SP_RUNNING) { sig_unlock(sp); return 0; }
    struct sig_task *st = sig_task_of(t);
    st->parked = 1;
    if (--sp->stop_count <= 0 && sp->stop_state == SP_STOPPING) {
        sp->stop_state = SP_STOPPED;
        notify = 1;
    }
    signo = sp->stop_signo;
    sig_unlock(sp);
    lkd_registry_publish(t);
    if (notify) {
        lkd_task_notify_stop(t, signo);   /* the wait4(WUNTRACED) report (request task-2) */
        sig_notify_parent(t->proc, LX_CLD_STOPPED, signo);
    }
    int traced = (atomic_load(&t->ptrace.flags) & LKD_PT_TRACED) != 0;
    int fatal = 0;
    if (traced) {
        /* A traced task reports the group stop to its tracer and stays until resumed. */
        int rs = 0;
        if (lkd_ptrace_stop(t, LKD_STOP_GROUP, signo, &rs) < 0) fatal = 1;
    } else {
        for (;;) {
            if (atomic_load(&t->flags) & LKD_TF_FATAL) { fatal = 1; break; }
            sig_lock(sp);
            int stopped = sp->stop_state != SP_RUNNING;
            sig_unlock(sp);
            if (!stopped) break;
            struct pollfd p = { .fd = t->wake_rd, .events = POLLIN, .revents = 0 };
            int r = poll(&p, 1, 200);
            if (r > 0) { char buf[64]; while (read(t->wake_rd, buf, sizeof buf) > 0) { } }
        }
    }
    sig_lock(sp);
    st->parked = 0;
    sig_unlock(sp);
    lkd_registry_publish(t);
    return fatal ? -1 : 0;
}

/* 'T' while parked in a group stop, 0 otherwise (for the registry and /proc). ANY. */
int lkd_sig_task_stopped(lkd_task *t)
{
    struct sig_task *st = __atomic_load_n((struct sig_task **)&t->pending.queue, __ATOMIC_ACQUIRE);
    return st && st->parked ? LKD_PS_STOPPED : 0;
}

/* ---- delivery --------------------------------------------------------------------------------- */

static void force_sigsegv(lkd_task *t, int failed_sig)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    struct lx_siginfo info;
    memset(&info, 0, sizeof info);
    info.si_signo = LX_SIGSEGV;
    info.si_code = LX_SI_KERNEL;
    sig_lock(sp);
    if (failed_sig == LX_SIGSEGV) {
        struct lkd_sighand *h = sig_hand(t);
        pthread_mutex_lock(&h->lock);
        h->act[LX_SIGSEGV - 1].handler = LX_SIG_DFL;
        sig_hand_update_masks(h);
        pthread_mutex_unlock(&h->lock);
    }
    sig_force_locked(t, sp, LX_SIGSEGV, &info, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

/* lkd_signal_next honors LKD_TF_TRACE_STOP (request ptrace-1); ptrace has a weak 0. */
int lkd_sig_trace_stop_supported(void) { return 1; }

int lkd_signal_next(lkd_task *t)
{
    const lkd_engine_ops *ops = lkd_engine();
    struct lkd_proc *p = t->proc;
    struct lkd_sigproc *sp = sig_proc(p);
    for (;;) {
        if (atomic_load(&t->flags) & LKD_TF_FATAL) return die(t);
        sig_lock(sp);
        if (sp->cld_notify) {
            int code = sp->cld_notify, status = sp->cld_status;
            sp->cld_notify = 0;
            sig_unlock(sp);
            if (code == LX_CLD_CONTINUED) lkd_task_notify_continue(t);   /* wait4(WCONTINUED), task-2 */
            sig_notify_parent(p, code, status);
            continue;
        }
        if (atomic_load(&t->flags) & LKD_TF_TRACE_STOP) {
            /* PTRACE_INTERRUPT (or the trap of an auto-attached seized child): a PTRACE_EVENT_STOP at this
             * safe point, before any signal (get_signal: do_jobctl_trap, kernel/signal.c v6.12). */
            sig_unlock(sp);
            atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP);
            if (lkd_ptrace_trap_stop(t) < 0) return die(t);
            continue;
        }
        if (atomic_load(&t->flags) & LKD_TF_GROUP_STOP) {
            sig_unlock(sp);
            if (park(t) < 0) return die(t);
            continue;
        }
        struct lx_siginfo info;
        int sig = sig_dequeue_locked(t, sp, ~atomic_load(&t->sigmask), &info);
        if (!sig) {
            if (t->restore_sigmask) {
                /* Nothing was delivered: the temporary mask of sigsuspend/ppoll/pselect ends here. */
                t->restore_sigmask = 0;
                struct sig_wakeset ws = { 0 };
                sig_set_blocked_locked(t, sp, t->saved_sigmask, &ws);
                sig_unlock(sp);
                sig_wakeset_run(&ws);
                continue;
            }
            sig_recalc_locked(t, sp);
            sig_unlock(sp);
            return 0;
        }
        sig_recalc_locked(t, sp);
        sig_unlock(sp);

        if ((atomic_load(&t->ptrace.flags) & LKD_PT_TRACED) && sig != LX_SIGKILL) {
            int ns = lkd_ptrace_signal_stop(t, sig, &info);
            if (ns < 0) { atomic_fetch_or(&t->flags, LKD_TF_FATAL); return die(t); }
            if (ns == 0) continue;
            if (ns != sig) {
                /* ptrace_signal: a new signal from the tracer gets a fresh SI_USER siginfo */
                memset(&info, 0, sizeof info);
                info.si_signo = ns;
                info.si_code = LX_SI_USER;
                lkd_proc_refresh_ids(p);
                info.u.kill.pid = t->ptrace.tracer_tid ? 0 : p->ppid;
                sig = ns;
            }
            if (sig < 1 || sig > LX_NSIG) continue;
            if (atomic_load(&t->sigmask) & LX_SIGBIT(sig)) {
                /* blocked now: requeue it for later (send_signal_locked to current) */
                sig_send_task_full(t, sig, &info, 1, 0);
                continue;
            }
        }

        sig_lock(sp);
        struct lkd_sighand *h = sig_hand(t);
        struct lx_sigaction act = h->act[sig - 1];
        if (act.handler != LX_SIG_IGN && act.handler != LX_SIG_DFL && (act.flags & LX_SA_RESETHAND)) {
            pthread_mutex_lock(&h->lock);
            h->act[sig - 1].handler = LX_SIG_DFL;
            sig_hand_update_masks(h);
            pthread_mutex_unlock(&h->lock);
        }
        if (act.handler == LX_SIG_IGN) { sig_unlock(sp); continue; }
        if (act.handler == LX_SIG_DFL || sig == LX_SIGKILL || sig == LX_SIGSTOP) {
            enum lx_sig_default d = sig == LX_SIGKILL ? LX_SIGDFL_TERM : lx_sig_default_action(sig);
            if (d == LX_SIGDFL_IGN || d == LX_SIGDFL_CONT) { sig_unlock(sp); continue; }
            if (d == LX_SIGDFL_STOP) {
                struct sig_wakeset ws = { 0 };
                initiate_stop(t, sp, sig, &ws);
                sig_unlock(sp);
                sig_wakeset_run(&ws);
                continue; /* the next turn parks this thread */
            }
            sig_unlock(sp);
            int status = sig;
            if (d == LX_SIGDFL_CORE && core_dumps(p)) status |= 0x80;
            if (lkd_trace_level()) sig_trace_death(t, sig, &info, 1);
            return start_group_exit(t, status);
        }

        /* A handler runs: build the delivery description (handle_signal + signal_delivered). */
        lkd_delivery d;
        memset(&d, 0, sizeof d);
        d.signo = sig;
        d.info = info;
        d.handler = act.handler;
        d.flags = (uint32_t)act.flags;
        d.restorer = act.restorer;
        uint64_t cur = atomic_load(&t->sigmask);
        d.old_mask = t->restore_sigmask ? t->saved_sigmask : cur;
        uint64_t nm = cur | act.mask;
        if (!(act.flags & LX_SA_NODEFER)) nm |= LX_SIGBIT(sig);
        nm &= ~LX_SIG_UNBLOCKABLE;
        d.new_mask = nm;
        sig_unlock(sp);

        uint64_t rsp = sig_current_sp(t);
        d.use_altstack = (act.flags & LX_SA_ONSTACK) && sig_altstack_flags(t, rsp - LX_RED_ZONE) == 0;
        d.altstack = t->altstack;
        if (!d.altstack.ss_size && !d.altstack.ss_flags) d.altstack.ss_flags = LX_SS_DISABLE;

        int fr = ops && ops->frame_build ? ops->frame_build(t, &d) : -LX_EFAULT;
        if (fr < 0) {
            force_sigsegv(t, sig); /* signal_setup_done(failed) */
            continue;
        }
        struct sig_wakeset ws = { 0 };
        sig_lock(sp);
        t->restore_sigmask = 0;
        sig_set_blocked_locked(t, sp, nm, &ws);
        if ((uint32_t)t->altstack.ss_flags & LX_SS_AUTODISARM) {
            t->altstack.ss_sp = 0;
            t->altstack.ss_size = 0;
            t->altstack.ss_flags = LX_SS_DISABLE;
        }
        t->restart.fn = NULL;
        sig_unlock(sp);
        sig_wakeset_run(&ws);
        return 1;
    }
}

int lkd_sig_restart_decision(lkd_task *t, int restart_errno)
{
    if (atomic_load(&t->flags) & LKD_TF_FATAL) return LKD_RESTART_NONE;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    int handler = 0, fatal = 0;
    uint64_t hflags = 0;
    sig_lock(sp);
    uint64_t bits = (atomic_load(&t->pending.bits) | atomic_load(&sp->shared_bits)) & ~atomic_load(&t->sigmask);
    struct lkd_sighand *h = sig_hand(t);
    while (bits) {
        int s = sig_next_signal(bits);
        bits &= ~LX_SIGBIT(s);
        const struct lx_sigaction *a = &h->act[s - 1];
        if (s == LX_SIGKILL) { fatal = 1; break; }
        if (a->handler == LX_SIG_IGN) continue;
        if (a->handler == LX_SIG_DFL || s == LX_SIGSTOP) {
            enum lx_sig_default d = lx_sig_default_action(s);
            if (d == LX_SIGDFL_IGN || d == LX_SIGDFL_CONT || d == LX_SIGDFL_STOP) continue;
            fatal = 1;
            break;
        }
        handler = 1;
        hflags = a->flags;
        break;
    }
    sig_unlock(sp);
    if (fatal) return LKD_RESTART_NONE;
    if (!handler) return restart_errno == LX_ERESTART_RESTARTBLOCK ? LKD_RESTART_BLOCK : LKD_RESTART_SAME;
    switch (restart_errno) {
    case LX_ERESTARTNOINTR: return LKD_RESTART_SAME;
    case LX_ERESTARTSYS: return (hflags & LX_SA_RESTART) ? LKD_RESTART_SAME : LKD_RESTART_NONE;
    default: return LKD_RESTART_NONE; /* ERESTARTNOHAND, ERESTART_RESTARTBLOCK */
    }
}

void lkd_signal_returned(lkd_task *t, uint64_t restored_mask, const struct lx_stack_t *restored_stack)
{
    lkd_sig_set_mask(t, restored_mask);
    t->restart.fn = NULL;
    if (restored_stack) (void)sig_do_sigaltstack(t, restored_stack, NULL, sig_current_sp(t));
}

/* ---- synchronous faults ------------------------------------------------------------------------ */

void lkd_fault(lkd_task *t, int signo, int si_code, uint64_t addr, uint64_t err, uint64_t trapno)
{
    if (!t || signo < 1 || signo > LX_NSIG) return;
    struct lx_siginfo info;
    memset(&info, 0, sizeof info);
    info.si_signo = signo;
    info.si_code = si_code;
    /* force_sig_fault fills si_addr; force_sig (SI_KERNEL: int3, #GP) leaves pid/uid at 0 */
    if (si_code != LX_SI_KERNEL && si_code > 0) info.u.fault.addr = addr;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    struct sig_task *st = sig_task_of(t);
    st->fault_err = err;
    st->fault_trapno = trapno;
    st->fault_addr = addr;
    sig_force_locked(t, sp, signo, &info, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

void lkd_sig_force_sync(lkd_task *t, const struct lx_siginfo *info)
{
    if (!t || !info || info->si_signo < 1 || info->si_signo > LX_NSIG) return;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    struct sig_task *st = sig_task_of(t);
    st->fault_err = 0;
    st->fault_trapno = 0;
    st->fault_addr = 0;
    sig_force_locked(t, sp, info->si_signo, info, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

/* ---- /proc status ----------------------------------------------------------------------------- */

int lkd_sig_status(lkd_task *t, struct lkd_sig_status *out)
{
    memset(out, 0, sizeof *out);
    struct lkd_sigproc *sp = sig_proc(t->proc);
    sig_lock(sp);
    struct lkd_sighand *h = sig_hand(t);
    out->pnd = atomic_load(&t->pending.bits);
    out->shdpnd = atomic_load(&sp->shared_bits);
    out->blk = atomic_load(&t->sigmask);
    out->ign = atomic_load(&h->ign);
    out->cgt = atomic_load(&h->cgt);
    out->queued = sp->queued;
    uint64_t lim = t->proc->rlimits[LX_RLIMIT_SIGPENDING].rlim_cur;
    out->queue_max = lim > SIG_QUEUE_HARD_MAX ? SIG_QUEUE_HARD_MAX : (uint32_t)lim;
    sig_unlock(sp);
    return 0;
}

int sig_wait_interruptible(lkd_task *t, int64_t deadline_ns)
{
    return lkd_block_sleep(t, deadline_ns);
}

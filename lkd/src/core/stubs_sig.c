/* stubs_sig.c: weak placeholders for the sig module (signals and futex). Owner: core until sig
 * overrides them. Nothing is ever queued; LKD_TF_FATAL and LKD_TF_SIGPENDING set by a test are honored
 * so that the interruption protocol of the blocking helpers can be exercised. */
#include "futex.h"
#include "sig.h"

LKD_WEAK int lkd_sig_send_pid(lkd_task *s, int32_t pid, int signo, const struct lx_siginfo *i)
{ (void)s; (void)pid; (void)signo; (void)i; return -LX_ENOSYS; }
LKD_WEAK int lkd_sig_send_tid(lkd_task *s, int32_t tgid, int32_t tid, int signo, const struct lx_siginfo *i)
{ (void)s; (void)tgid; (void)tid; (void)signo; (void)i; return -LX_ENOSYS; }
LKD_WEAK int lkd_sig_send_pgrp(lkd_task *s, int32_t pgrp, int signo, const struct lx_siginfo *i)
{ (void)s; (void)pgrp; (void)signo; (void)i; return -LX_ENOSYS; }
LKD_WEAK void lkd_sig_send_task(lkd_task *target, int signo, const struct lx_siginfo *info, int thread_directed)
{ (void)info; (void)thread_directed; if (signo >= 1 && signo <= LX_NSIG) {
    atomic_fetch_or(&target->pending.bits, LX_SIGBIT(signo));
    atomic_fetch_or(&target->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(target); } }
LKD_WEAK int lkd_sig_child_event(int32_t pp, int32_t cp, uint32_t uid, int code, int status)
{ (void)pp; (void)cp; (void)uid; (void)code; (void)status; return 0; }

LKD_WEAK int lkd_sig_interrupted(const lkd_task *t)
{
    return (atomic_load(&t->flags) & (LKD_TF_FATAL | LKD_TF_SIGPENDING)) != 0;
}
LKD_WEAK int lkd_sig_restart_decision(lkd_task *t, int restart_errno)
{
    if (lkd_sig_interrupted(t)) return LKD_RESTART_NONE; /* placeholder: any pending signal means EINTR */
    return restart_errno == LX_ERESTART_RESTARTBLOCK ? LKD_RESTART_BLOCK : LKD_RESTART_SAME;
}
LKD_WEAK void lkd_sig_set_temp_mask(lkd_task *t, uint64_t mask)
{
    t->saved_sigmask = atomic_load(&t->sigmask);
    t->restore_sigmask = 1;
    atomic_store(&t->sigmask, mask & ~LX_SIG_UNBLOCKABLE);
}
LKD_WEAK void lkd_sig_set_mask(lkd_task *t, uint64_t mask) { atomic_store(&t->sigmask, mask & ~LX_SIG_UNBLOCKABLE); }
LKD_WEAK struct lkd_sighand *lkd_sighand_clone(struct lkd_sighand *parent, uint64_t clone_flags) { (void)clone_flags; return parent; }
LKD_WEAK void lkd_sighand_put(struct lkd_sighand *h) { (void)h; }
LKD_WEAK void lkd_sig_exec_reset(lkd_task *t) { (void)t; }
LKD_WEAK void lkd_sig_kill_all_threads(lkd_task *t)
{
    pthread_mutex_lock(&t->proc->lock);
    for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc) {
        atomic_fetch_or(&x->flags, LKD_TF_FATAL);
        lkd_task_wake(x);
        if (x != t && lkd_engine()) lkd_engine()->kick(x);
    }
    pthread_mutex_unlock(&t->proc->lock);
}
LKD_WEAK int lkd_sig_status(lkd_task *t, struct lkd_sig_status *out)
{
    memset(out, 0, sizeof *out);
    out->pnd = atomic_load(&t->pending.bits);
    out->blk = atomic_load(&t->sigmask);
    return 0;
}

/* Public engine hooks implemented by sig. */
LKD_WEAK int lkd_signal_pending(const lkd_task *t)
{
    return (atomic_load(&t->flags) & (LKD_TF_FATAL | LKD_TF_SIGPENDING)) != 0;
}
LKD_WEAK int lkd_signal_next(lkd_task *t) { return (atomic_load(&t->flags) & LKD_TF_FATAL) ? -1 : 0; }
LKD_WEAK void lkd_signal_returned(lkd_task *t, uint64_t mask, const struct lx_stack_t *st)
{ lkd_sig_set_mask(t, mask); if (st) t->altstack = *st; }
LKD_WEAK void lkd_fault(lkd_task *t, int signo, int si_code, uint64_t addr, uint64_t err, uint64_t trapno)
{
    lkd_log("guest fault with no sig module: tid %d signal %d code %d addr 0x%llx err 0x%llx trapno %llu",
            t ? t->tid : 0, signo, si_code, (unsigned long long)addr, (unsigned long long)err, (unsigned long long)trapno);
    abort();
}

/* futex */
LKD_WEAK int lkd_futex_wake_addr(lkd_task *t, uint64_t gva, int nr, int private_hint)
{ (void)t; (void)gva; (void)nr; (void)private_hint; return 0; }
LKD_WEAK void lkd_futex_exit_robust_list(lkd_task *t) { (void)t; }
LKD_WEAK void lkd_sig_wake_hook(lkd_task *t) { (void)t; }
LKD_WEAK int lkd_sig_task_stopped(lkd_task *t) { (void)t; return 0; }
LKD_WEAK size_t lkd_sig_exec_export(lkd_task *t, void *buf, size_t cap) { (void)t; (void)buf; (void)cap; return 0; }
LKD_WEAK int lkd_sig_exec_import(lkd_task *t, const void *buf, size_t len) { (void)t; (void)buf; return len ? -LX_EINVAL : 0; }
LKD_WEAK void lkd_sig_force_sync(lkd_task *t, const struct lx_siginfo *info)
{
    lkd_log("forced signal with no sig module: tid %d signal %d code %d", t ? t->tid : 0,
            info ? info->si_signo : 0, info ? info->si_code : 0);
    abort();
}

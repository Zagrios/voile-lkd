/* state.c: lazy signal state, actions objects, wakeups, module hooks (init, fork, exec, exit). Owner: sig. */
#include <unistd.h>

#include "sig_priv.h"

static pthread_mutex_t g_lazy = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_nprocs;

void sig_lock(struct lkd_sigproc *sp) { pthread_mutex_lock(&sp->lock); }
void sig_unlock(struct lkd_sigproc *sp) { pthread_mutex_unlock(&sp->lock); }

static struct lkd_sighand *hand_new(void)
{
    struct lkd_sighand *h = calloc(1, sizeof *h);
    if (!h) { lkd_log("sig: %s", "out of memory for signal actions"); abort(); }
    atomic_init(&h->refs, 1);
    pthread_mutex_init(&h->lock, NULL);
    return h;
}

void sig_hand_update_masks(struct lkd_sighand *h)
{
    uint64_t ign = 0, cgt = 0;
    for (int s = 1; s <= LX_NSIG; s++) {
        uint64_t hd = h->act[s - 1].handler;
        if (hd == LX_SIG_IGN) ign |= LX_SIGBIT(s);
        else if (hd != LX_SIG_DFL) cgt |= LX_SIGBIT(s);
    }
    atomic_store(&h->ign, ign);
    atomic_store(&h->cgt, cgt);
}

struct lkd_sigproc *sig_proc_peek(const struct lkd_proc *p)
{
    return __atomic_load_n(&p->sigproc, __ATOMIC_ACQUIRE);
}

struct lkd_sigproc *sig_proc(struct lkd_proc *p)
{
    struct lkd_sigproc *sp = sig_proc_peek(p);
    if (sp) return sp;
    pthread_mutex_lock(&g_lazy);
    sp = p->sigproc;
    if (!sp) {
        sp = calloc(1, sizeof *sp);
        if (!sp) { lkd_log("sig: %s", "out of memory for process signal state"); abort(); }
        pthread_mutex_init(&sp->lock, NULL);
        sp->dfl_hand = hand_new();
        sq_init(&sp->shared_q);
        __atomic_store_n(&p->sigproc, sp, __ATOMIC_RELEASE);
        atomic_fetch_add(&g_nprocs, 1);
    }
    pthread_mutex_unlock(&g_lazy);
    if (!lkd_famsvc_running()) (void)lkd_famsvc_start();
    return sp;
}

struct sig_task *sig_task_of(lkd_task *t)
{
    struct sig_task *st = __atomic_load_n((struct sig_task **)&t->pending.queue, __ATOMIC_ACQUIRE);
    if (st) return st;
    st = calloc(1, sizeof *st);
    if (!st) { lkd_log("sig: %s", "out of memory for thread signal state"); abort(); }
    sq_init(&st->q);
    __atomic_store_n((struct sig_task **)&t->pending.queue, st, __ATOMIC_RELEASE);
    return st;
}

struct lkd_sighand *sig_hand(lkd_task *t)
{
    if (t->sighand) return t->sighand;
    return sig_proc(t->proc)->dfl_hand;
}

/* ---- actions objects (sig.h) ------------------------------------------------------------------ */

struct lkd_sighand *lkd_sighand_clone(struct lkd_sighand *parent, uint64_t clone_flags)
{
    if (clone_flags & LX_CLONE_SIGHAND) {
        if (!parent) return NULL; /* the threads then share the process default */
        atomic_fetch_add(&parent->refs, 1);
        return parent;
    }
    struct lkd_sighand *h = hand_new();
    if (parent) {
        pthread_mutex_lock(&parent->lock);
        memcpy(h->act, parent->act, sizeof h->act);
        pthread_mutex_unlock(&parent->lock);
    }
    if (clone_flags & LX_CLONE_CLEAR_SIGHAND) {
        /* kernel/fork.c copy_sighand: flush_signal_handlers(tsk, 0) keeps ignored signals */
        for (int s = 1; s <= LX_NSIG; s++) {
            struct lx_sigaction *a = &h->act[s - 1];
            uint64_t hd = a->handler;
            memset(a, 0, sizeof *a);
            if (hd == LX_SIG_IGN) a->handler = LX_SIG_IGN;
        }
    }
    sig_hand_update_masks(h);
    return h;
}

void lkd_sighand_put(struct lkd_sighand *h)
{
    if (!h) return;
    if (atomic_fetch_sub(&h->refs, 1) == 1) {
        pthread_mutex_destroy(&h->lock);
        free(h);
    }
}

/* ---- wakeups ---------------------------------------------------------------------------------- */

void sig_wakeset_add(struct sig_wakeset *ws, lkd_task *t)
{
    if (!ws || !t) return;
    for (int i = 0; i < ws->n; i++) if (ws->t[i] == t) return;
    if (ws->n < SIG_WAKESET_MAX) ws->t[ws->n++] = lkd_task_get(t);
}

void sig_wakeset_run(struct sig_wakeset *ws)
{
    const lkd_engine_ops *ops = lkd_engine();
    lkd_task *cur = lkd_current();
    for (int i = 0; i < ws->n; i++) {
        lkd_task *t = ws->t[i];
        lkd_task_wake(t);                 /* rings a shared futex sleep too (lkd_sig_wake_hook) */
        if (t != cur && ops && ops->kick) ops->kick(t);
        lkd_task_put(t);
    }
    ws->n = 0;
}

/* ---- exec ------------------------------------------------------------------------------------- */

/* kernel/signal.c flush_signal_handlers(t, 0): handlers become SIG_DFL, ignored stay ignored, flags,
 * masks and restorers are cleared. fs/exec.c begin_new_exec: the alternate stack is dropped. Pending
 * signals and the blocked mask survive exec. */
void lkd_sig_exec_reset(lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct lkd_sighand *h = sig_hand(t);
    sig_lock(sp);
    pthread_mutex_lock(&h->lock);
    for (int s = 1; s <= LX_NSIG; s++) {
        struct lx_sigaction *a = &h->act[s - 1];
        uint64_t hd = a->handler;
        memset(a, 0, sizeof *a);
        if (hd == LX_SIG_IGN) a->handler = LX_SIG_IGN;
    }
    sig_hand_update_masks(h);
    pthread_mutex_unlock(&h->lock);
    t->altstack.ss_sp = 0;
    t->altstack.ss_size = 0;
    t->altstack.ss_flags = LX_SS_DISABLE;
    sig_unlock(sp);
}

void lkd_sig_exec_commit(lkd_task *t) { lkd_sig_exec_reset(t); }

/* ---- exit ------------------------------------------------------------------------------------- */

void lkd_sig_task_exit(lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    int notify = 0, signo = 0;
    sig_lock(sp);
    struct sig_task *st = sig_task_of(t);
    /* kernel/signal.c exit_signals: a thread that leaves during a group stop counts as stopped, and
     * process signals it could have taken go to another thread. */
    if ((atomic_load(&t->flags) & LKD_TF_GROUP_STOP) && !st->parked && sp->stop_state == SP_STOPPING) {
        atomic_fetch_and(&t->flags, ~LKD_TF_GROUP_STOP);
        if (--sp->stop_count <= 0) {
            sp->stop_state = SP_STOPPED;
            notify = 1;
            signo = sp->stop_signo;
        }
    }
    atomic_fetch_or(&t->flags, LKD_TF_EXITING);
    uint64_t shared = atomic_load(&sp->shared_bits) & ~atomic_load(&t->sigmask);
    if (shared && !sp->group_exit) {
        pthread_mutex_lock(&t->proc->lock);
        for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc) {
            if (x == t || (atomic_load(&x->flags) & LKD_TF_EXITING)) continue;
            if (shared & ~atomic_load(&x->sigmask)) {
                atomic_fetch_or(&x->flags, LKD_TF_SIGPENDING);
                sig_wakeset_add(&ws, x);
                break;
            }
        }
        pthread_mutex_unlock(&t->proc->lock);
    }
    /* Thread-directed pending signals die with the thread (flush_sigqueue(&tsk->pending)). */
    sq_flush(&st->q, ~0ull, &sp->queued);
    atomic_store(&t->pending.bits, 0);
    atomic_store(&st->fx, NULL);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
    if (notify) sig_notify_parent(t->proc, LX_CLD_STOPPED, signo);
}

void lkd_sig_proc_exit(lkd_task *t)
{
    sig_sem_proc_exit(t);
    struct lkd_sigproc *sp = sig_proc_peek(t->proc);
    if (sp) {
        sig_lock(sp);
        sq_flush(&sp->shared_q, ~0ull, &sp->queued);
        atomic_store(&sp->shared_bits, 0);
        sig_unlock(sp);
    }
    /* The last guest process of this host process stops the family service (unit tests run several
     * guest processes in one host process; the engine runs one). */
    if (t->proc->host_pid == getpid()) {
        int left = sp ? atomic_fetch_sub(&g_nprocs, 1) - 1 : atomic_load(&g_nprocs);
        if (left <= 0) {
            sig_ntsync_host_exit();
            lkd_famsvc_shutdown();
        }
    }
}

/* ---- fork ------------------------------------------------------------------------------------- */

static struct lkd_sigproc *g_fork_sp;
static struct lkd_sighand *g_fork_hand;

void lkd_sig_fork_prepare(lkd_task *t, uint64_t clone_flags)
{
    (void)clone_flags;
    pthread_mutex_lock(&g_lazy);
    g_fork_sp = t ? sig_proc_peek(t->proc) : NULL;
    g_fork_hand = NULL;
    if (g_fork_sp) {
        sig_lock(g_fork_sp);
        g_fork_hand = t->sighand ? t->sighand : g_fork_sp->dfl_hand;
        pthread_mutex_lock(&g_fork_hand->lock);
    }
    lkd_famsvc_fork_prepare();
}

void lkd_sig_fork_parent(lkd_task *t, pid_t host_child)
{
    (void)t; (void)host_child;
    lkd_famsvc_fork_parent();
    if (g_fork_hand) pthread_mutex_unlock(&g_fork_hand->lock);
    if (g_fork_sp) sig_unlock(g_fork_sp);
    g_fork_sp = NULL;
    g_fork_hand = NULL;
    pthread_mutex_unlock(&g_lazy);
}

/* The child is a new process: kernel/fork.c copy_process clears the pending sets (init_sigpending)
 * and keeps the mask, the actions and the alternate stack. */
void lkd_sig_fork_child(lkd_task *t)
{
    lkd_fork_reinit_mutex(&g_lazy, 0);
    if (g_fork_hand) lkd_fork_reinit_mutex(&g_fork_hand->lock, 0);
    struct lkd_sigproc *sp = t ? sig_proc_peek(t->proc) : NULL;
    if (t && !sp && g_fork_sp) {
        /* task gave the child a new struct lkd_proc: it inherits the copy of the parent's state */
        __atomic_store_n(&t->proc->sigproc, g_fork_sp, __ATOMIC_RELEASE);
        sp = g_fork_sp;
    }
    if (sp) {
        sig_sfd_fork_child(sp, t->proc);
        lkd_fork_reinit_mutex(&sp->lock, 0);
        sq_flush(&sp->shared_q, ~0ull, &sp->queued);
        sp->queued = 0;
        atomic_store(&sp->shared_bits, 0);
        sp->group_exit = sp->group_exit_status = sp->group_exit_done = 0;
        sp->stop_state = SP_RUNNING;
        sp->stop_signo = sp->stop_count = 0;
        sp->cld_notify = sp->cld_status = 0;
        atomic_store(&g_nprocs, 1);
    } else {
        atomic_store(&g_nprocs, 0);
    }
    if (t) {
        struct sig_task *st = __atomic_load_n((struct sig_task **)&t->pending.queue, __ATOMIC_ACQUIRE);
        if (st) {
            struct sq_node *n = st->q.head;
            while (n) { struct sq_node *x = n->next; free(n); n = x; }
            sq_init(&st->q);
            st->real_blocked = 0;
            st->parked = 0;
            atomic_store(&st->fx, NULL);
        }
        atomic_store(&t->pending.bits, 0);
        atomic_fetch_and(&t->flags, ~(LKD_TF_SIGPENDING | LKD_TF_GROUP_STOP | LKD_TF_FATAL));
    }
    g_fork_sp = NULL;
    g_fork_hand = NULL;
    sig_ntsync_fork_child();
    sig_futex_fork_child();
    lkd_famsvc_fork_child();
}

/* ---- init ------------------------------------------------------------------------------------- */

void lkd_sig_init(void)
{
    sig_syscalls_register();
    sig_futex_register();
    sig_sfd_register();
    sig_sem_register();
    sig_send_init();
    int e = lkd_famsvc_start();
    if (e < 0) lkd_log("sig: family service not started (%d): cross-process signals answer ESRCH", e);
}

/* ---- host re-exec ----------------------------------------------------------------------------- */

/* State that Linux keeps across execve and that a host re-exec of the runtime would lose: the blocked
 * mask, the ignored dispositions, the pending sets with their queued siginfo (thread first, then the
 * process). The task module carries the blob through the family shared memory (docs/requests/sig-2.md). */
#define SIG_EXEC_MAGIC 0x53474558u /* "SGEX" */
struct sig_exec_rec { int32_t shared; int32_t _pad; struct lx_siginfo info; };
struct sig_exec_blob {
    uint32_t magic, version;
    uint64_t mask, ign, tbits, sbits;
    uint32_t nrec, _pad;
    struct sig_exec_rec rec[];
};

size_t lkd_sig_exec_export(lkd_task *t, void *buf, size_t cap)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    sig_lock(sp);
    struct sig_task *st = sig_task_of(t);
    uint32_t n = 0;
    for (struct sq_node *x = st->q.head; x; x = x->next) n++;
    for (struct sq_node *x = sp->shared_q.head; x; x = x->next) n++;
    size_t need = sizeof(struct sig_exec_blob) + (size_t)n * sizeof(struct sig_exec_rec);
    if (buf && cap >= need) {
        struct sig_exec_blob *b = buf;
        memset(b, 0, sizeof *b);
        b->magic = SIG_EXEC_MAGIC;
        b->version = 1;
        b->mask = atomic_load(&t->sigmask);
        b->ign = atomic_load(&sig_hand(t)->ign);
        b->tbits = atomic_load(&t->pending.bits);
        b->sbits = atomic_load(&sp->shared_bits);
        b->nrec = n;
        uint32_t i = 0;
        for (struct sq_node *x = st->q.head; x; x = x->next, i++) { b->rec[i].shared = 0; b->rec[i].info = x->info; }
        for (struct sq_node *x = sp->shared_q.head; x; x = x->next, i++) { b->rec[i].shared = 1; b->rec[i].info = x->info; }
    }
    sig_unlock(sp);
    return need;
}

int lkd_sig_exec_import(lkd_task *t, const void *buf, size_t len)
{
    const struct sig_exec_blob *b = buf;
    if (!b || len < sizeof *b || b->magic != SIG_EXEC_MAGIC || b->version != 1 ||
        len < sizeof *b + (size_t)b->nrec * sizeof(struct sig_exec_rec))
        return -LX_EINVAL;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct lkd_sighand *h = sig_hand(t);
    sig_lock(sp);
    struct sig_task *st = sig_task_of(t);
    pthread_mutex_lock(&h->lock);
    for (int s = 1; s <= LX_NSIG; s++) {
        memset(&h->act[s - 1], 0, sizeof h->act[s - 1]);
        if ((b->ign & LX_SIGBIT(s)) && s != LX_SIGKILL && s != LX_SIGSTOP) h->act[s - 1].handler = LX_SIG_IGN;
    }
    sig_hand_update_masks(h);
    pthread_mutex_unlock(&h->lock);
    atomic_store(&t->sigmask, b->mask & ~LX_SIG_UNBLOCKABLE);
    for (uint32_t i = 0; i < b->nrec; i++) {
        struct sq_node *n = malloc(sizeof *n);
        if (!n) break;
        n->info = b->rec[i].info;
        n->next = NULL;
        struct sq *q = b->rec[i].shared ? &sp->shared_q : &st->q;
        *q->tail = n;
        q->tail = &n->next;
        sp->queued++;
    }
    atomic_fetch_or(&t->pending.bits, b->tbits);
    atomic_fetch_or(&sp->shared_bits, b->sbits);
    sig_recalc_locked(t, sp);
    sig_unlock(sp);
    return 0;
}

/* sig_priv.h: private structures of the sig module (signals, futex, signalfd, famsvc). Owner: sig.
 *
 * Locking. One mutex per process, struct lkd_sigproc.lock (LKD_RANK_SIG), protects the process-wide
 * pending set and queue, every per-thread queue (struct sig_task), the job-control and group-exit state
 * and the signalfd list. Actions live in struct lkd_sighand (shared per CLONE_SIGHAND) and change only
 * with the process lock AND the sighand lock held; readers of the same process hold either one. The
 * pending bitmaps (task.h lkd_sigpending.bits, sigproc.shared_bits) and the masks are atomics so that
 * the lock-free checks (lkd_signal_pending, lkd_sig_interrupted) can read them. proc->lock (rank 50) may
 * be taken under the sig lock (rank 40) to walk the thread list. Engine callbacks (kick, frame_build,
 * get_regs) and lkd_task_* exit calls are made with no sig lock held: wakeups are collected in a
 * struct sig_wakeset and performed after unlocking.
 */
#ifndef LKD_SIG_PRIV_H
#define LKD_SIG_PRIV_H

#include "famsvc.h"
#include "fd.h"
#include "futex.h"
#include "net.h"
#include "sig.h"
#include "task.h"

#define SIG_SYNC_MASK (LX_SIGBIT(LX_SIGSEGV) | LX_SIGBIT(LX_SIGBUS) | LX_SIGBIT(LX_SIGILL) | \
                       LX_SIGBIT(LX_SIGTRAP) | LX_SIGBIT(LX_SIGFPE) | LX_SIGBIT(LX_SIGSYS))
#define SIG_STOP_MASK (LX_SIGBIT(LX_SIGSTOP) | LX_SIGBIT(LX_SIGTSTP) | LX_SIGBIT(LX_SIGTTIN) | LX_SIGBIT(LX_SIGTTOU))
#define SIG_KERNEL_IGNORE_MASK (LX_SIGBIT(LX_SIGCONT) | LX_SIGBIT(LX_SIGCHLD) | LX_SIGBIT(LX_SIGWINCH) | LX_SIGBIT(LX_SIGURG))
#define SIG_COREDUMP_MASK (LX_SIGBIT(LX_SIGQUIT) | LX_SIGBIT(LX_SIGILL) | LX_SIGBIT(LX_SIGTRAP) | LX_SIGBIT(LX_SIGABRT) | \
                           LX_SIGBIT(LX_SIGFPE) | LX_SIGBIT(LX_SIGSEGV) | LX_SIGBIT(LX_SIGBUS) | LX_SIGBIT(LX_SIGSYS) | \
                           LX_SIGBIT(LX_SIGXCPU) | LX_SIGBIT(LX_SIGXFSZ))
/* rt_sigaction keeps only these flag bits (kernel/signal.c UAPI_SA_FLAGS for x86-64). */
#define SIG_UAPI_SA_FLAGS (LX_SA_NOCLDSTOP | LX_SA_NOCLDWAIT | LX_SA_SIGINFO | LX_SA_ONSTACK | LX_SA_RESTART | \
                           LX_SA_NODEFER | LX_SA_RESETHAND | LX_SA_EXPOSE_TAGBITS | LX_SA_RESTORER)
/* Queued siginfo per process when RLIMIT_SIGPENDING is unlimited (memory bound). */
#define SIG_QUEUE_HARD_MAX 16384

/* A queue of siginfo records (FIFO). */
struct sq_node { struct sq_node *next; struct lx_siginfo info; };
struct sq { struct sq_node *head; struct sq_node **tail; };

struct lkd_sighand {
    _Atomic int refs;
    pthread_mutex_t lock;
    struct lx_sigaction act[LX_NSIG];
    _Atomic uint64_t ign;   /* SIG_IGN set, for SigIgn */
    _Atomic uint64_t cgt;   /* handler set, for SigCgt */
};

enum { SP_RUNNING = 0, SP_STOPPING = 1, SP_STOPPED = 2 };

struct sfd; /* signalfd object, signalfd.c */

struct lkd_sigproc {
    pthread_mutex_t lock;
    struct lkd_sighand *dfl_hand; /* actions of tasks that carry no sighand (lazy) */
    _Atomic uint64_t shared_bits; /* process-directed pending set */
    struct sq shared_q;
    uint32_t queued;              /* siginfo records queued in the process (thread and shared queues) */
    int group_exit;               /* a fatal signal or exit_group decided the death of the process */
    int group_exit_status;        /* wait status for lkd_task_do_exit_group */
    int group_exit_done;          /* lkd_task_do_exit_group already called (or task started it) */
    int stop_state;               /* SP_* */
    int stop_signo;
    int stop_count;               /* threads that still have to park */
    int cld_notify;               /* CLD_STOPPED or CLD_CONTINUED owed to the parent, 0 none */
    int cld_status;
    struct sfd *sfds;             /* signalfd objects of this process */
};

/* Per-thread state, hung on task->pending.queue. */
struct fx_sleeper;
struct sig_task {
    struct sq q;                  /* thread-directed queue */
    uint64_t real_blocked;        /* rt_sigtimedwait: mask before the wait (ignored test uses it) */
    struct fx_sleeper *_Atomic fx; /* futex wait in progress, rung by signal generation */
    int parked;                   /* parked in a group stop */
    uint64_t fault_err, fault_trapno, fault_addr; /* last synchronous fault reported by the engine */
};

/* Tasks to wake (and kick) after the sig lock is released. Each holds a task reference. */
#define SIG_WAKESET_MAX 512
struct sig_wakeset { int n; int kick_self; lkd_task *t[SIG_WAKESET_MAX]; };

/* ---- futex.c: shared sleepers and the robust spin lock, for ntsync.c ------------------------------
 * A shared sleeper lives in the family's futex.shm: any process of the family can wake it by index, and
 * signals to its task ring it. sig_shared_sleep returns 1 woken, 0 deadline (lkd_host_mono_ns domain,
 * -1 none), -1 interrupted. The spin lock holds the holder's host pid and is taken over from a dead
 * holder. */
struct fx_sleeper;
struct fx_sleeper *sig_shared_sleeper_get(lkd_task *t);   /* NULL: no family file or no free sleeper */
uint32_t sig_shared_sleeper_index(struct fx_sleeper *s); /* 1-based */
void sig_shared_sleeper_wake(uint32_t index);
int sig_shared_sleep(lkd_task *t, struct fx_sleeper *s, int64_t deadline);
void sig_shared_sleeper_put(struct fx_sleeper *s);
void sig_spin_lock(_Atomic uint32_t *l);
void sig_spin_unlock(_Atomic uint32_t *l);

/* ---- sem.c ------------------------------------------------------------------------------------ */
void sig_sem_register(void);
void sig_sem_proc_exit(lkd_task *t); /* the guest process of t exits: apply its SEM_UNDO adjustments */

/* ---- ntsync.c --------------------------------------------------------------------------------- */
void sig_ntsync_host_exit(void);  /* last guest process of this host process: release its ntsync slots */
void sig_ntsync_fork_child(void);

/* ---- state.c ---------------------------------------------------------------------------------- */
struct lkd_sigproc *sig_proc(struct lkd_proc *p);       /* lazy, never NULL except ENOMEM (aborts) */
struct lkd_sigproc *sig_proc_peek(const struct lkd_proc *p); /* NULL when not created yet, lock-free */
struct sig_task *sig_task_of(lkd_task *t);              /* lazy; call with the process sig lock held */
struct lkd_sighand *sig_hand(lkd_task *t);              /* t->sighand or the process default */
void sig_lock(struct lkd_sigproc *sp);
void sig_unlock(struct lkd_sigproc *sp);
void sig_wakeset_add(struct sig_wakeset *ws, lkd_task *t);
void sig_wakeset_run(struct sig_wakeset *ws);           /* wake pipe + futex ring + kick, drop refs */
void sig_ring_futex(lkd_task *t);                       /* end a futex sleep of t (futex.c) */
void sig_hand_update_masks(struct lkd_sighand *h);

/* ---- queue.c ---------------------------------------------------------------------------------- */
void sq_init(struct sq *q);
void sq_flush(struct sq *q, uint64_t mask, uint32_t *queued);   /* drop records of signals in mask */
int sig_next_signal(uint64_t bits);
/* Generation (Linux __send_signal_locked + prepare_signal + complete_signal). Called with sp locked.
 * target is the thread for thread_directed, or a preferred thread of the process. force: the fault
 * path (never ignored, may not be dropped). Returns 0 or -LX_EAGAIN (RT queue overflow). */
int sig_generate_locked(lkd_task *target, struct lkd_sigproc *sp, int sig, const struct lx_siginfo *info,
                        int thread_directed, struct sig_wakeset *ws);
/* Dequeue one signal of `allowed` from t's queue then the process queue. Returns signo or 0. */
int sig_dequeue_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t allowed, struct lx_siginfo *info);
/* Peek without dequeuing: next signo of allowed (thread first), 0 if none; skip excludes signals. */
int sig_peek_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t allowed);
/* Flush given signals from every pending set of the process. */
void sig_flush_process_locked(struct lkd_proc *p, struct lkd_sigproc *sp, uint64_t mask);
/* Change the blocked mask of t (SIGKILL/SIGSTOP removed); hands process signals that t now blocks to
 * another thread. */
void sig_set_blocked_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t mask, struct sig_wakeset *ws);
/* Recompute LKD_TF_SIGPENDING for t. */
void sig_recalc_locked(lkd_task *t, struct lkd_sigproc *sp);
int sig_is_ignored_locked(lkd_task *t, int sig);
/* force_sig_info_to_task: unblock and reset an ignored or blocked action to SIG_DFL, then queue. */
void sig_force_locked(lkd_task *t, struct lkd_sigproc *sp, int sig, const struct lx_siginfo *info, struct sig_wakeset *ws);
void sig_send_task_full(lkd_task *target, int sig, const struct lx_siginfo *info, int thread_directed, int force);

/* ---- deliver.c -------------------------------------------------------------------------------- */
/* With LKD_TRACE on, one stderr line for the signal that ends t's process: the thread it hit, then the faulting
 * address (and rip when t is the calling thread) or the sender's pid. info may be NULL. */
void sig_trace_death(lkd_task *t, int sig, const struct lx_siginfo *info, int current);

/* ---- send.c ----------------------------------------------------------------------------------- */
struct sig_sender {               /* the identity checked by kill permission */
    int32_t pid, tid, sid;
    uint32_t uid, euid;
    int cap_kill;
    int is_kernel;                /* no sender: kernel-originated, no permission check */
};
void sig_sender_of(lkd_task *t, struct sig_sender *s);
void sig_info_user(struct lx_siginfo *info, int sig, int code, const struct sig_sender *s);
void sig_send_init(void);

/* ---- deliver.c -------------------------------------------------------------------------------- */
int sig_altstack_flags(lkd_task *t, uint64_t sp);       /* sas_ss_flags */
int sig_do_sigaltstack(lkd_task *t, const struct lx_stack_t *ss, struct lx_stack_t *oss, uint64_t sp);
uint64_t sig_current_sp(lkd_task *t);
void sig_notify_parent(struct lkd_proc *p, int code, int status);
int sig_wait_interruptible(lkd_task *t, int64_t deadline_ns); /* 0 deadline, -ERESTARTSYS interrupted */

/* ---- signalfd.c ------------------------------------------------------------------------------- */
void sig_sfd_notify_locked(struct lkd_sigproc *sp, int sig);
void sig_sfd_register(void);
void sig_sfd_fork_child(struct lkd_sigproc *sp, struct lkd_proc *p); /* objects now belong to p */

/* ---- futex.c ---------------------------------------------------------------------------------- */
void sig_futex_register(void);
void sig_futex_fork_child(void);
/* Test hook: replace the futex key function (NULL restores lkd_mm_futex_key). */
void lkd_futex_set_key_fn(int (*fn)(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key));

/* ---- syscalls.c ------------------------------------------------------------------------------- */
void sig_syscalls_register(void);

#endif /* LKD_SIG_PRIV_H */

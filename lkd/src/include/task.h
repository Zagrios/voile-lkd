/* task.h: tasks (guest threads), processes and the family registry. OWNER of the implementation: task.
 * Core provides weak placeholders (src/core/stubs_task.c) so other modules can create fake tasks in
 * unit tests before the task module exists; the task module overrides them with strong symbols.
 *
 * Model. A guest process is a host process. A guest thread is a host thread. One struct lkd_task per
 * guest thread, one struct lkd_proc per guest process (shared by its threads). Everything another
 * process of the family may need to know (name, parent, state, exit status, signal masks, tracer) is
 * also published in a shared-memory registry (lkd_proc_info / lkd_tid_info snapshots).
 */
#ifndef LKD_TASK_H
#define LKD_TASK_H

#include "lkd_internal.h"
#include "ptrace.h"

#define LKD_TASK_MAGIC 0x4c4b4454u /* 'LKDT' */
#define LKD_COMM_LEN 16
#define LKD_MAX_GROUPS 32
#define LKD_REG_PATH_MAX 512
#define LKD_REG_CMDLINE_MAX 4096
#define LKD_PID_MAX 0x3fffffff /* FUTEX_TID_MASK: a tid must fit in the futex owner field */

struct lkd_mm;       /* mm.h */
struct lkd_fdtable;  /* fd.h */
struct lkd_fsctx;    /* vfs.h */
struct lkd_sighand;  /* sig.h */
struct lkd_sigproc;  /* sig.h */
struct lkd_waitstate; /* task-private */

/* Linux task state letters as shown in /proc/<pid>/stat and status. */
enum { LKD_PS_RUNNING = 'R', LKD_PS_SLEEPING = 'S', LKD_PS_DISK = 'D', LKD_PS_STOPPED = 'T',
       LKD_PS_TRACING_STOP = 't', LKD_PS_ZOMBIE = 'Z', LKD_PS_DEAD = 'X' };

struct lkd_creds {
    uint32_t uid, euid, suid, fsuid;
    uint32_t gid, egid, sgid, fsgid;
    uint32_t ngroups;
    uint32_t groups[LKD_MAX_GROUPS];
    uint64_t cap_inh, cap_perm, cap_eff, cap_bset, cap_amb;
    uint32_t securebits;
};

/* Pending signals: a bitmap the lock-free check reads, plus the sig module's queue of siginfo. */
struct lkd_sigpending {
    _Atomic uint64_t bits;  /* bit (sig-1) set = pending */
    void *queue;            /* sig-owned; protected by the sig lock (LKD_RANK_SIGQ) */
};

/* Task flags (lkd_task.flags). Set with atomic OR, cleared with atomic AND. */
#define LKD_TF_SIGPENDING 0x01u  /* hint: something may be deliverable (see lkd_signal_pending) */
#define LKD_TF_FATAL      0x02u  /* the task must die at its next safe point (SIGKILL, exit_group) */
#define LKD_TF_EXITING    0x04u  /* exit processing started */
#define LKD_TF_GROUP_STOP 0x08u  /* a job-control stop is pending for this task */
#define LKD_TF_TRACE_STOP 0x10u  /* a ptrace stop was requested (INTERRUPT, SIGSTOP from the tracer) */
#define LKD_TF_IN_BLOCK   0x20u  /* inside an interruptible blocking wait (informational) */
#define LKD_TF_EXEC       0x40u  /* exec in progress */

/* Per-process state, shared by the threads of a guest process. Fields marked [lock] need
 * proc->lock (rank LKD_RANK_TASKS); [reg] fields are caches of the shared registry. */
struct lkd_proc {
    _Atomic int refs;
    pthread_mutex_t lock;
    int32_t pid;                   /* == tgid == tid of the main thread, family-unique */
    int32_t ppid, pgid, sid;       /* [reg] refreshed (with exit_signal) through lkd_proc_refresh_ids */
    int32_t host_pid;              /* getpid() of the macOS process */
    struct lkd_creds cred;         /* [lock] simplification: per process, not per thread */
    struct lx_rlimit rlimits[LX_RLIM_NLIMITS]; /* [lock] */
    uint32_t personality;          /* [lock] */
    uint8_t dumpable;              /* [lock] 0, 1, 2 as PR_SET_DUMPABLE */
    uint8_t no_new_privs;
    uint8_t child_subreaper;
    int32_t exit_signal;           /* [reg] signal to the parent when the process dies (SIGCHLD once reparented) */
    char *exe_path;                /* guest path (malloc), target of /proc/<pid>/exe */
    char *cmdline; size_t cmdline_len;   /* NUL-separated snapshot at exec (malloc) */
    char *environ_snap; size_t environ_len;
    struct lkd_proc_layout *layout; /* set by lkd_proc_set_layout after exec */
    struct lkd_task *threads;      /* [lock] list of live threads, linked by next_in_proc */
    int nthreads;                  /* [lock] */
    struct lkd_sigproc *sigproc;   /* sig-owned: process-wide pending, itimers, group-stop state */
    struct lkd_waitstate *wait;    /* task-owned: children and zombies */
    uint64_t start_mono_ns;        /* lkd_host_mono_ns() at creation */
    uint32_t exit_code;            /* [lock] wait status once the process is a zombie */
    uint8_t exiting;
};

/* Memory layout facts the engine reports after exec (for /proc/<pid>/stat, auxv, maps). */
struct lkd_proc_layout {
    uint64_t start_code, end_code, start_data, end_data, start_brk, start_stack;
    uint64_t arg_start, arg_end, env_start, env_end, entry;
    uint8_t *auxv; size_t auxv_len;  /* the auxiliary vector as placed on the stack (copy, malloc) */
};

struct lkd_task {
    uint32_t magic;                /* LKD_TASK_MAGIC */
    _Atomic int refs;              /* lkd_task_get/put */
    int32_t tid;                   /* family-unique; main thread: tid == pid */
    struct lkd_proc *proc;         /* never NULL */
    struct lkd_task *next_in_proc; /* proc->lock */
    void *engine_data;             /* engine's thread object */

    /* Objects shared according to the CLONE_* flags; each is refcounted and owned by its module. */
    struct lkd_mm *mm;
    struct lkd_fdtable *fdt;
    struct lkd_fsctx *fsctx;       /* cwd, root, umask */
    struct lkd_sighand *sighand;   /* signal actions */

    /* Signals (sig module owns the semantics). */
    _Atomic uint64_t sigmask;      /* blocked set, SIGKILL/SIGSTOP never set */
    uint64_t saved_sigmask;        /* [owner] temporary mask of sigsuspend/ppoll/epoll_pwait/pselect */
    int restore_sigmask;           /* [owner] saved_sigmask is to be restored after the syscall */
    struct lkd_sigpending pending; /* thread-directed signals */
    struct lx_stack_t altstack;    /* [owner] sigaltstack */
    _Atomic uint32_t flags;        /* LKD_TF_* */

    /* Thread bookkeeping (task module). */
    char comm[LKD_COMM_LEN];       /* [owner] */
    uint64_t clear_child_tid;      /* guest address (set_tid_address, CLONE_CHILD_CLEARTID) */
    uint64_t robust_list_head;     /* guest address, 0 if unset */
    uint64_t robust_list_len;
    uint64_t start_fs_base;        /* initial TLS bases for spawn; the engine's registers are authoritative */
    uint64_t start_gs_base;
    int32_t exit_signal_to_parent; /* clone low byte, for the thread-group leader */
    int32_t pdeathsig;             /* PR_SET_PDEATHSIG */
    int32_t nice;
    int32_t sched_policy;
    int32_t sched_priority;
    uint64_t affinity;             /* cpu mask presented to the guest */
    uint64_t nvcsw, nivcsw;        /* context switch counters for /proc status */

    /* Blocking support (core, src/core/block.c). */
    int wake_rd, wake_wr;          /* wake pipe; created by lkd_task_alloc, nonblocking, CLOEXEC */

    /* The syscall in flight, for /proc/<pid>/syscall, ptrace GET_SYSCALL_INFO and tracing. */
    _Atomic int64_t cur_nr;        /* -1 outside a syscall */
    uint64_t cur_args[6];

    /* Restart block for ERESTART_RESTARTBLOCK syscalls (nanosleep, clock_nanosleep, poll, futex): the
     * handler stores a continuation, core's restart_syscall runs it. */
    struct { int64_t (*fn)(lkd_task *t); uint64_t arg[6]; } restart;

    struct lkd_ptrace_task ptrace; /* ptrace link, see ptrace.h */
    int exit_code;                 /* argument of sys_exit for this thread */

    /* seccomp (task module, src/task/seccomp.c): the filters this thread runs, newest first. A filter is
     * shared, refcounted, with the threads and children created after it was installed. */
    struct lkd_seccomp_filter *_Atomic seccomp_filter;
    _Atomic uint8_t seccomp_mode;  /* LX_SECCOMP_MODE_* */
};

/* ---- Registry snapshots (readable for any process of the family) ------------------------------- */

typedef struct lkd_proc_info {
    int32_t pid, ppid, pgid, sid;
    int32_t host_pid;
    char state;                    /* LKD_PS_* of the main thread, 'Z' for a zombie not yet reaped */
    int32_t nthreads;
    int32_t wait_status;           /* valid when state == 'Z' */
    int32_t exit_signal;
    uint32_t uid, euid, gid, egid;
    uint8_t dumpable;
    uint8_t child_subreaper;
    uint64_t start_mono_ns;
    uint64_t sigign, sigcgt;       /* ignored and caught signal sets, published by sig */
    uint64_t shdpnd;               /* process-directed pending set, published by sig */
    char comm[LKD_COMM_LEN];       /* comm of the main thread */
    char exe[LKD_REG_PATH_MAX];
    uint32_t cmdline_len;
    char cmdline[LKD_REG_CMDLINE_MAX];
} lkd_proc_info;

typedef struct lkd_tid_info {
    int32_t tid, tgid;
    char state;
    char comm[LKD_COMM_LEN];
    int32_t tracer_tid;            /* 0 = not traced; else tid of the tracer (TracerPid) */
    uint64_t sigblk, sigpnd;       /* blocked and thread-pending sets, published by sig */
    int64_t syscall_nr;            /* -1 when running guest code */
    uint64_t syscall_args[6];
    uint64_t syscall_sp, syscall_pc;
    uint64_t nvcsw, nivcsw;
} lkd_tid_info;

/* ---- API owned by the task module -------------------------------------------------------------- */

/* Family. The first process of a run creates the shared registry and counter; a fork child inherits
 * the shared mapping; a host re-exec (guest execve) re-attaches through the exported fd.
 *   lkd_family_create: returns 0, -errno. first_pid is the first pid handed out (>= 2).
 *   lkd_family_export: returns a dup'ed fd (inheritable, not CLOEXEC) of the shared region and sets
 *                      LKD_FAMILY_FD in the environment for the re-executed runtime; -errno on failure.
 *   lkd_family_attach: map the region from fd (or from getenv("LKD_FAMILY_FD") when fd < 0).
 *   lkd_pid_alloc:     family-wide unique id for a new process or thread, 1 .. LKD_PID_MAX, never reused
 *                      during a run, async-safe, lock-free (shared atomic). Returns -LX_EAGAIN when full. */
int lkd_family_create(uint32_t first_pid);
int lkd_family_export(void);
int lkd_family_attach(int fd);
int32_t lkd_pid_alloc(void);

/* Task objects. lkd_task_alloc builds a task (and its proc unless CLONE_THREAD is in clone_flags) from
 * `parent` (NULL for the very first task), allocates the tid, the wake pipe, inherits the signal mask,
 * comm, creds and rlimits. It does NOT make the task visible and does NOT touch the shared objects
 * (mm, fdt, fsctx, sighand): the clone path sets those, in this order:
 *
 *     child = lkd_task_alloc(parent, flags);
 *     child->mm      = lkd_mm_clone(parent->mm, flags);          // CLONE_VM shares; fork uses host fork
 *     child->fdt     = lkd_fdtable_clone(parent->fdt, flags);    // CLONE_FILES shares, else copies
 *     child->fsctx   = lkd_fsctx_clone(parent->fsctx, flags);    // CLONE_FS shares
 *     child->sighand = lkd_sighand_clone(parent->sighand, flags);// CLONE_SIGHAND shares
 *     lkd_ptrace_task_init(child, parent, flags);                // PTRACE_O_TRACECLONE inheritance
 *     lkd_task_publish(child);                                   // visible, counted in the registry
 *     err = lkd_engine()->spawn_thread(child, &regs);            // else lkd_task_unpublish + put
 *
 * Reference counting: alloc returns one reference; lkd_task_get/put adjust; the last put frees the task
 * (its proc when the last thread is gone). lkd_task_lookup returns a counted reference. */
lkd_task *lkd_task_alloc(lkd_task *parent, uint64_t clone_flags);
void lkd_task_publish(lkd_task *t);
void lkd_task_unpublish(lkd_task *t);
lkd_task *lkd_task_get(lkd_task *t);
void lkd_task_put(lkd_task *t);
/* Thread of THIS host process with that tid, or NULL. ANY, takes the registry lock briefly. */
lkd_task *lkd_task_lookup(int32_t tid);
/* Any process of the family. Returns 0 and fills *out, or -LX_ESRCH. */
int lkd_proc_lookup(int32_t pid, lkd_proc_info *out);
int lkd_tid_lookup(int32_t tid, lkd_tid_info *out);
/* All processes (live and zombie), ascending pid; fills up to max entries, returns the count. */
int lkd_proc_list(lkd_proc_info *out, int max);
/* tids of the threads of process tgid, ascending; returns the count or -LX_ESRCH. */
int lkd_tid_list(int32_t tgid, int32_t *tids, int max);
/* Re-publish the registry record of t (state, comm, masks, tracer, syscall). Called by task, sig and
 * ptrace when the corresponding data changes. Takes the registry lock; do not hold ranks > 5a. */
void lkd_registry_publish(lkd_task *t);
void lkd_proc_refresh_ids(struct lkd_proc *p);
/* Record the layout the engine established at exec (engine calls it from ops->exec_image). */
void lkd_proc_set_layout(lkd_task *t, const struct lkd_proc_layout *l);

/* Wake every thread blocked in wait4/waitid of process pid (any process of the family). Called when a
 * child changes state: by task (exit), sig (stop, continue) and ptrace (a tracee stops). ANY. */
void lkd_task_wake_waiters(int32_t pid);
/* Job control (request task-1): sig calls notify_stop when the group stop of t's process completes and
 * notify_continue when SIGCONT ends it. task publishes 'T' and the one-time report that wait4(WUNTRACED)
 * and waitid(WSTOPPED/WCONTINUED) return, and wakes the parent's waiters. OWNER, no LKD lock held. */
void lkd_task_notify_stop(lkd_task *t, int signo);
void lkd_task_notify_continue(lkd_task *t);
/* uname(2) strings, shared with procfs (/proc/version, /proc/sys/kernel/osrelease). Returns 0. */
struct lx_utsname;
int lkd_task_uname(struct lx_utsname *out);
/* 1 for a guest process living in the host process of another one (vfork or posix_spawn child before its
 * exec): its proc_exit hooks must not stop services global to the host process. */
int lkd_task_shares_host(const lkd_task *t);
/* Now on the guest clock clockid (LX_CLOCK_*), as clock_gettime(2) answers it, in ns; -1 for an unknown
 * clock. sig converts absolute futex deadlines with it (request sig-2 point 3: guest MONOTONIC is Darwin
 * CLOCK_UPTIME_RAW, which stops during sleep, not lkd_host_mono_ns). ANY. */
int64_t lkd_guest_clock_ns(int clockid);
/* The next id the family counter will hand out (pids and tids, never reused); 0 without a family. procfs
 * prints "processes" of /proc/stat from it (request procfs-4 point 7). ANY, lock-free. */
uint32_t lkd_pid_peek(void);

/* Exit. lkd_task_do_exit ends one thread (sys_exit): clear_child_tid + futex wake, robust list,
 * module task_exit hooks, ptrace exit stop; it returns the disposition the syscall handler must return.
 * lkd_task_do_exit_group ends the process with the given wait status (exit code << 8, or terminating
 * signal | 0x80 for core): marks every thread fatal, kicks them, publishes the zombie record, sends
 * SIGCHLD to the parent through the sig module. sig calls it for fatal signals. Both run on the
 * exiting thread. */
int64_t lkd_task_do_exit(lkd_task *t, int exit_code);
int64_t lkd_task_do_exit_group(lkd_task *t, int wait_status);

/* seccomp (task module), as __secure_computing in kernel/seccomp.c. lkd_seccomp_enter runs t's filters on
 * the syscall rq before its handler, OWNER, at the instruction pointer the engine reports. It returns 0 when
 * the call runs, 1 when it is skipped: *ret is then what the syscall returns (a -errno, the syscall number
 * for SECCOMP_RET_TRAP, whose forced SIGSYS is queued, or LKD_RET_EXIT_THREAD/GROUP when the filter kills).
 * lkd_seccomp_fork gives a new task its creator's mode and filters (copy_seccomp); lkd_seccomp_release drops
 * a freed task's filters; lkd_seccomp_status reports the mode and filter count of /proc/<pid>/status. */
int lkd_seccomp_enter(lkd_task *t, const lkd_sysreq *rq, int64_t *ret);
void lkd_seccomp_fork(lkd_task *child, lkd_task *parent);
void lkd_seccomp_release(lkd_task *t);
void lkd_seccomp_status(lkd_task *t, int *mode, int *filters);

/* One wait notification, produced by the task module for zombies and by the ptrace module for stops. */
struct lkd_wait_event {
    int32_t pid;
    int32_t status;   /* wait status as wait4 reports it */
    int32_t code;     /* CLD_EXITED, CLD_KILLED, CLD_DUMPED, CLD_TRAPPED, CLD_STOPPED, CLD_CONTINUED */
    uint32_t uid;
    struct lx_rusage ru;
};

#endif /* LKD_TASK_H */

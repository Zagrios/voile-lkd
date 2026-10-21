/* task_priv.h: private interfaces of the task module (src/task). Not for other modules.
 *
 * Layout of the module:
 *   family.c   shared family registry (file-backed MAP_SHARED region, fcntl byte lock), pid counter
 *   tasks.c    task and proc objects, local task table, publish, create_main, release, exit
 *   ids.c      identity syscalls (pid, pgid, sid, uid/gid family, groups, caps, personality)
 *   sys.c      uname, rlimits, rusage, sysinfo, sched_*, getcpu, getrandom, prctl, arch_prctl, misc
 *   time.c     clocks, sleeps, itimers
 *   clone.c    clone, clone3, fork, vfork and the fork hooks
 *   wait.c     wait4, waitid, zombies, reparenting, SIGCHLD
 *   exec.c     execve, execveat, lkd_exec_prepare, exec hooks
 *   seccomp.c  seccomp, prctl(PR_SET_SECCOMP/PR_GET_SECCOMP) and the filters every syscall runs through
 */
#ifndef LKD_TASK_PRIV_H
#define LKD_TASK_PRIV_H

#include "lkd_internal.h"
#include "task.h"

/* Defined in src/core/api.c and declared in core_priv.h; the module needs uid, gid, first_pid and
 * hostname (request docs/requests/task-1.md asks core to expose it in lkd_internal.h). */
const lkd_config *lkd_config_get(void);

/* Machine-safety caps (docs/CONVENTIONS.md 10.5). Linux answers -EAGAIN at its own limits. */
#define TASK_MAX_THREADS_PER_PROC 300
#define TASK_MAX_CHILDREN 64
#define TASK_MAX_LIVE_PROCS 96 /* lead, 2026-10-02: 332 leaked host processes panicked the Mac (VM map entry zone) */

/* ---- Shared family registry (family.c) -------------------------------------------------------- */

#define FAM_MAX_PROCS 1024
#define FAM_MAX_TIDS 8192

/* fam_proc.flags */
#define RF_ZOMBIE       0x001u /* the process is dead, wait_status valid, waiting to be reaped */
#define RF_DID_EXEC     0x002u /* execve succeeded at least once (setpgid answers EACCES) */
#define RF_VFORK_DONE   0x004u /* vfork child released its parent (exec or exit) */
#define RF_STOP_PENDING 0x008u /* a stop not yet reported by wait (WUNTRACED) */
#define RF_CONT_PENDING 0x010u /* a continue not yet reported by wait (WCONTINUED) */
#define RF_COHOSTED     0x020u /* lives as threads inside another guest process' host process */
#define RF_REAPING      0x040u /* a waiter is reaping it (WNOWAIT readers skip nothing) */

struct fam_proc {
    lkd_proc_info info;            /* info.pid == 0: free slot */
    _Atomic uint32_t flags;        /* RF_* */
    int32_t creator_tid;           /* thread that created it (__WNOTHREAD, pdeathsig on thread exit) */
    int32_t pdeathsig;             /* PR_SET_PDEATHSIG of the process (Linux: of its threads) */
    int32_t stop_status;           /* wait status of a pending stop report */
    int32_t cld_code;              /* CLD_* for the zombie */
    int32_t host_parent;           /* host pid of the process that forked it, 0 if unknown */
    struct lx_rlimit rlim[LX_RLIM_NLIMITS];
    struct lx_rusage ru;           /* own usage, published at death */
    struct lx_rusage cru;          /* usage of reaped children */
};

struct fam_tid {
    lkd_tid_info info;             /* info.tid == 0: free slot */
    uint64_t affinity;
    uint64_t robust_head, robust_len;
    int32_t nice;
};

int fam_ready(void);               /* creates the family on first use; 0 or -errno */
void fam_lock(void);               /* rank 51: local mutex then fcntl byte lock */
void fam_unlock(void);
uint32_t fam_seq(void);            /* change counter for waiters, lock-free */
void fam_bump(void);
/* All the following need fam_lock. */
struct fam_proc *fam_proc_find(int32_t pid);
struct fam_proc *fam_proc_new(int32_t pid);   /* zeroed slot with info.pid set, or NULL when full */
void fam_proc_free(struct fam_proc *r);       /* also frees the tid records of that tgid */
struct fam_tid *fam_tid_find(int32_t tid);
struct fam_tid *fam_tid_new(int32_t tid, int32_t tgid);
void fam_tid_free(struct fam_tid *r);
struct fam_proc *fam_proc_at(int i);          /* slot i (0 .. FAM_MAX_PROCS-1) */
struct fam_tid *fam_tid_at(int i);
int fam_count_children(int32_t ppid);         /* live (non-zombie) records with that ppid */
/* The family's init, Linux's child_reaper of a pid namespace: the first process registered with no parent in the
 * family (the program the engine started, pid first_pid). Orphans without a live subreaper ancestor go to it. LKD
 * hands out no pid 1 by default, so it keeps its own pid. Set once; 0 before. */
int32_t fam_child_reaper(void);
void fam_set_child_reaper(int32_t pid);
int fam_count_live(void);
/* Shared UTS strings (sethostname is EPERM for the guest, the runtime sets them once). */
const char *fam_hostname(void);
const char *fam_domainname(void);
void fam_fork_prepare(void);
void fam_fork_parent(void);
void fam_fork_child(void);

/* ---- Per-process extension (struct lkd_waitstate is task-owned, see task.h) ------------------- */

struct itimer_state;

struct lkd_waitstate {
    struct fam_proc *rec;          /* this process' registry slot (stable while the process lives) */
    int cohosted;                  /* guest process living inside another process' host process */
    int group_exit;                /* exit_group or fatal signal in progress */
    int group_status;              /* wait status of the group exit */
    int zombie_done;               /* zombie record published */
    int migrated;                  /* the process moved to another host process (exec of a vfork child) */
    int exec_killing;              /* de_thread in progress */
    int keepcaps, thp_disable, mce_kill, timing;
    uint64_t timerslack_ns;
    struct itimer_state *itimers;  /* time.c, NULL until first setitimer */
};

/* ---- Local task table (tasks.c) --------------------------------------------------------------- */

#define LT_EXIT_DONE  0x01u        /* per-thread exit work done */
#define LT_IN_WAIT    0x02u        /* blocked in wait4/waitid */
#define LT_MIGRATED   0x04u        /* identity moved to a forked host process: release silently */
#define LT_STOPPED    0x08u        /* group stop reported through lkd_task_notify_stop */

void tasks_lock(void);             /* rank 50 */
void tasks_unlock(void);
uint32_t task_xflags(lkd_task *t);
void task_set_xflags(lkd_task *t, uint32_t set, uint32_t clear);
struct fam_tid *task_tidrec(lkd_task *t);     /* needs fam_lock */
char task_state_char(lkd_task *t);
/* Publish records of t (and of its process when t is the leader) with fam_lock held. */
void task_publish_locked(lkd_task *t);
void proc_publish_locked(struct lkd_proc *p, lkd_task *leader);
/* Process-wide exit: publish the zombie, reparent children, notify the parent. status is the wait
 * status. Idempotent. Returns nonzero when the host process must exit afterwards. */
int proc_die(lkd_task *t, int status);
/* Per-thread exit work (robust list, clear_child_tid, module task_exit hooks). Idempotent. */
void task_exit_work(lkd_task *t);
/* Robust list and clear_child_tid only (exec_mm_release semantics). */
void task_mm_release(lkd_task *t);
/* New proc for a forked or cloned child, copied from parent; pid given. */
struct lkd_proc *proc_new_from(lkd_task *parent, int32_t pid, uint64_t clone_flags);
void proc_free(struct lkd_proc *p);
/* Create the registry records of a new process (proc + tid of its leader). fam_lock held. */
int proc_register_locked(struct lkd_proc *p, lkd_task *leader_or_null, int32_t leader_tid, const char *comm,
                         int32_t creator_tid);
/* Local table manipulation for fork/exec. */
void tasks_reset_after_fork(lkd_task *keep);
void tasks_forget(lkd_task *t);              /* drop from the local table without registry changes */
int tasks_count_proc(struct lkd_proc *p);
/* Kill every other thread of t's process and wait for them to be released (exec). */
int task_de_thread(lkd_task *t, int32_t *old_tid);
/* Wake local threads of process pid that wait in wait4. */
void tasks_wake_waiters_local(int32_t pid);

/* famsvc opcodes of task (LKD_FAMSVC_OP_TASK_FIRST .. LAST), answered by the host process that owns the task. */
#define TASK_OP_WAKE_WAITERS (LKD_FAMSVC_OP_TASK_FIRST + 0) /* tasks.c: wake the wait4 callers of a process */
#define TASK_OP_SET_AFFINITY (LKD_FAMSVC_OP_TASK_FIRST + 1) /* sys.c: sched_setaffinity of one of its threads */

/* ---- wait.c ----------------------------------------------------------------------------------- */
void wait_note_host_child(pid_t host_pid, int32_t guest_pid);
void wait_forget_host_children(void);       /* fork child: the parent's host children are not ours */
void wait_vfork_parent(lkd_task *t, int32_t child_pid);
void wait_rusage_now(struct lx_rusage *ru);

/* ---- time.c ----------------------------------------------------------------------------------- */
void itimers_fork_child(struct lkd_proc *p);
void itimers_proc_exit(struct lkd_proc *p);
void itimers_fork_prepare(void);
void itimers_fork_parent(void);
int64_t lx_clock_now_ns(int clk);           /* 0 .. 11 static clocks; -1 when unsupported */

/* ---- exec.c ----------------------------------------------------------------------------------- */
/* Pure helpers, exposed for unit tests. */
enum { EXEC_KIND_ELF64 = 1, EXEC_KIND_SCRIPT = 2 };
int exec_classify(const unsigned char *buf, size_t n);  /* EXEC_KIND_* or -LX_ENOEXEC */
/* Parse a "#!" line (buf holds the first n <= 256 bytes of the file). Fills interp and arg (arg[0] = 0
 * when absent). Returns 0 or -LX_ENOEXEC. Models fs/binfmt_script.c load_script(). */
int exec_parse_shebang(const unsigned char *buf, size_t n, char *interp, size_t icap, char *arg, size_t acap);

/* ---- Small helpers ---------------------------------------------------------------------------- */
static inline void lx_rusage_add(struct lx_rusage *a, const struct lx_rusage *b)
{
    a->ru_utime.tv_sec += b->ru_utime.tv_sec;
    a->ru_utime.tv_usec += b->ru_utime.tv_usec;
    a->ru_stime.tv_sec += b->ru_stime.tv_sec;
    a->ru_stime.tv_usec += b->ru_stime.tv_usec;
    while (a->ru_utime.tv_usec >= 1000000) { a->ru_utime.tv_usec -= 1000000; a->ru_utime.tv_sec++; }
    while (a->ru_stime.tv_usec >= 1000000) { a->ru_stime.tv_usec -= 1000000; a->ru_stime.tv_sec++; }
    if (b->ru_maxrss > a->ru_maxrss) a->ru_maxrss = b->ru_maxrss;
    a->ru_minflt += b->ru_minflt; a->ru_majflt += b->ru_majflt;
    a->ru_inblock += b->ru_inblock; a->ru_oublock += b->ru_oublock;
    a->ru_nvcsw += b->ru_nvcsw; a->ru_nivcsw += b->ru_nivcsw;
}

/* New API proposed to core for task.h (docs/requests/task-2.md); implemented here. */

/* ---- seccomp (seccomp.c) ------------------------------------------------------------------------- */

void task_seccomp_register(void);
int64_t task_seccomp_prctl_get(lkd_task *t);
int64_t task_seccomp_prctl_set(lkd_task *t, uint64_t mode, uint64_t ufilter);

#endif /* LKD_TASK_PRIV_H */

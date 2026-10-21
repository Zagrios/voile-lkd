/* tasks.c: task and process objects, the local task table, publication in the family registry,
 * lkd_task_create_main, exit and lkd_task_release.
 *
 * A guest thread is a host thread, a guest process normally a host process. A process created with
 * CLONE_VM but without CLONE_THREAD (vfork, posix_spawn, some runtimes) cannot be a separate host
 * process (memory must stay shared), so it lives as host threads inside its parent's host process
 * ("cohosted") until it calls execve, where it moves to a host fork() child (clone.c, exec.c).
 */
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "fd.h"
#include "famsvc.h"
#include "futex.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task_priv.h"
#include "vfs.h"

struct ltask {
    lkd_task *t;
    uint32_t xflags;
};

#define LTAB_MAX 2048
static pthread_mutex_t g_tasks = PTHREAD_MUTEX_INITIALIZER;
static struct ltask g_ltab[LTAB_MAX];
static int g_ln;

void tasks_lock(void) { pthread_mutex_lock(&g_tasks); }
void tasks_unlock(void) { pthread_mutex_unlock(&g_tasks); }

static struct ltask *lt_find(lkd_task *t)
{
    for (int i = 0; i < g_ln; i++) if (g_ltab[i].t == t) return &g_ltab[i];
    return NULL;
}

uint32_t task_xflags(lkd_task *t)
{
    tasks_lock();
    struct ltask *e = lt_find(t);
    uint32_t f = e ? e->xflags : 0;
    tasks_unlock();
    return f;
}

void task_set_xflags(lkd_task *t, uint32_t set, uint32_t clear)
{
    tasks_lock();
    struct ltask *e = lt_find(t);
    if (e) e->xflags = (e->xflags | set) & ~clear;
    tasks_unlock();
}

/* ---- Defaults ----------------------------------------------------------------------------------- */

static uint64_t host_sysctl_u64(const char *name, uint64_t dflt)
{
    uint64_t v64 = 0;
    size_t len = sizeof v64;
    if (sysctlbyname(name, &v64, &len, NULL, 0) < 0) return dflt;
    if (len == sizeof(uint32_t)) return (uint32_t)v64;
    return v64;
}

static void rlimit_defaults(struct lx_rlimit *rl)
{
    for (int i = 0; i < LX_RLIM_NLIMITS; i++) rl[i].rlim_cur = rl[i].rlim_max = LX_RLIM_INFINITY;
    /* Linux defaults (include/asm-generic/resource.h INIT_RLIMITS) where the host has no counterpart,
     * the host's own limit where it bounds what LKD can deliver. */
    rl[LX_RLIMIT_STACK].rlim_cur = 8ull << 20;               /* _STK_LIM */
    rl[LX_RLIMIT_CORE].rlim_cur = 0;                          /* distributions ship 0 */
    struct rlimit hr;
    if (getrlimit(RLIMIT_NPROC, &hr) == 0) {
        rl[LX_RLIMIT_NPROC].rlim_cur = hr.rlim_cur == RLIM_INFINITY ? LX_RLIM_INFINITY : (uint64_t)hr.rlim_cur;
        rl[LX_RLIMIT_NPROC].rlim_max = hr.rlim_max == RLIM_INFINITY ? LX_RLIM_INFINITY : (uint64_t)hr.rlim_max;
    }
    rl[LX_RLIMIT_NOFILE].rlim_cur = 1024;                     /* INR_OPEN_CUR */
    rl[LX_RLIMIT_NOFILE].rlim_max = host_sysctl_u64("kern.maxfilesperproc", 4096); /* host fds back guest fds */
    rl[LX_RLIMIT_MEMLOCK].rlim_cur = rl[LX_RLIMIT_MEMLOCK].rlim_max = 8ull << 20; /* MLOCK_LIMIT */
    uint64_t maxproc = host_sysctl_u64("kern.maxproc", 4096);
    rl[LX_RLIMIT_SIGPENDING].rlim_cur = rl[LX_RLIMIT_SIGPENDING].rlim_max = maxproc;
    rl[LX_RLIMIT_MSGQUEUE].rlim_cur = rl[LX_RLIMIT_MSGQUEUE].rlim_max = 819200; /* MQ_BYTES_MAX */
    rl[LX_RLIMIT_NICE].rlim_cur = rl[LX_RLIMIT_NICE].rlim_max = 0;
    rl[LX_RLIMIT_RTPRIO].rlim_cur = rl[LX_RLIMIT_RTPRIO].rlim_max = 0;
}

static uint64_t all_cpus_mask(void)
{
    uint64_t n = host_sysctl_u64("hw.ncpu", 1);
    if (n >= 64) return ~0ull;
    return (1ull << n) - 1;
}

static char *dup_block(const char *s, size_t n)
{
    if (!s || !n) return NULL;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- Process objects ---------------------------------------------------------------------------- */

struct lkd_proc *proc_new_from(lkd_task *parent, int32_t pid, uint64_t clone_flags)
{
    struct lkd_proc *p = calloc(1, sizeof *p);
    struct lkd_waitstate *w = calloc(1, sizeof *w);
    if (!p || !w) { free(p); free(w); return NULL; }
    atomic_init(&p->refs, 1);
    pthread_mutex_init(&p->lock, NULL);
    p->wait = w;
    w->timerslack_ns = 50000; /* PR_GET_TIMERSLACK default: 50 us */
    p->pid = pid;
    p->host_pid = getpid();
    p->start_mono_ns = (uint64_t)lkd_host_mono_ns();
    p->exit_signal = (int32_t)(clone_flags & 0xff);
    if (parent) {
        struct lkd_proc *pp = parent->proc;
        lkd_proc_refresh_ids(pp);
        p->ppid = (clone_flags & LX_CLONE_PARENT) ? pp->ppid : pp->pid;
        p->pgid = pp->pgid;
        p->sid = pp->sid;
        pthread_mutex_lock(&pp->lock);
        p->cred = pp->cred;
        memcpy(p->rlimits, pp->rlimits, sizeof p->rlimits);
        p->personality = pp->personality;
        p->dumpable = pp->dumpable;
        p->no_new_privs = pp->no_new_privs;
        p->exe_path = pp->exe_path ? strdup(pp->exe_path) : NULL;
        p->cmdline = dup_block(pp->cmdline, pp->cmdline_len);
        p->cmdline_len = p->cmdline ? pp->cmdline_len : 0;
        p->environ_snap = dup_block(pp->environ_snap, pp->environ_len);
        p->environ_len = p->environ_snap ? pp->environ_len : 0;
        if (pp->layout) {
            p->layout = malloc(sizeof *p->layout);
            if (p->layout) {
                *p->layout = *pp->layout;
                p->layout->auxv = (uint8_t *)dup_block((const char *)pp->layout->auxv, pp->layout->auxv_len);
                if (!p->layout->auxv) p->layout->auxv_len = 0;
            }
        }
        w->keepcaps = 0; /* PR_SET_KEEPCAPS is cleared on fork */
        w->thp_disable = pp->wait ? pp->wait->thp_disable : 0;
        w->timerslack_ns = pp->wait ? pp->wait->timerslack_ns : 50000;
        pthread_mutex_unlock(&pp->lock);
    } else {
        const lkd_config *c = lkd_config_get();
        p->ppid = 1;
        p->pgid = p->sid = pid;
        p->cred.uid = p->cred.euid = p->cred.suid = p->cred.fsuid = c->uid;
        p->cred.gid = p->cred.egid = p->cred.sgid = p->cred.fsgid = c->gid;
        p->cred.ngroups = 1;
        p->cred.groups[0] = c->gid;
        p->cred.cap_bset = (1ull << 41) - 1; /* CAP_LAST_CAP == 40: a non-root task keeps the full bounding set */
        rlimit_defaults(p->rlimits);
        p->dumpable = 1;
    }
    return p;
}

void proc_free(struct lkd_proc *p)
{
    if (!p) return;
    if (p->wait) itimers_proc_exit(p);
    free(p->exe_path);
    free(p->cmdline);
    free(p->environ_snap);
    if (p->layout) { free(p->layout->auxv); free(p->layout); }
    free(p->wait);
    pthread_mutex_destroy(&p->lock);
    free(p);
}

static int make_wake_pipe(lkd_task *t)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    t->wake_rd = p[0];
    t->wake_wr = p[1];
    return 0;
}

static lkd_task *task_alloc_pid(lkd_task *parent, uint64_t clone_flags, int32_t forced_pid)
{
    if (fam_ready() < 0) return NULL;
    lkd_task *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->magic = LKD_TASK_MAGIC;
    atomic_init(&t->refs, 1);
    atomic_init(&t->cur_nr, -1);
    t->wake_rd = t->wake_wr = -1;
    if (make_wake_pipe(t) < 0) { free(t); return NULL; }
    if (parent && (clone_flags & LX_CLONE_THREAD)) {
        int32_t tid = lkd_pid_alloc();
        if (tid < 0) goto fail;
        t->proc = parent->proc;
        atomic_fetch_add(&t->proc->refs, 1);
        t->tid = tid;
        t->exit_signal_to_parent = -1; /* threads never signal the parent */
    } else {
        int32_t pid = forced_pid > 0 ? forced_pid : lkd_pid_alloc();
        if (pid < 0) goto fail;
        t->proc = proc_new_from(parent, pid, clone_flags);
        if (!t->proc) goto fail;
        t->tid = pid;
        t->exit_signal_to_parent = (int32_t)(clone_flags & 0xff);
    }
    t->affinity = all_cpus_mask();
    if (parent) {
        atomic_store(&t->sigmask, atomic_load(&parent->sigmask));
        memcpy(t->comm, parent->comm, sizeof t->comm);
        t->affinity = parent->affinity;
        t->nice = parent->nice;
        t->sched_policy = parent->sched_policy;
        t->sched_priority = parent->sched_priority;
        /* copy_process: the alternate signal stack is dropped for CLONE_VM without CLONE_VFORK. */
        if ((clone_flags & (LX_CLONE_VM | LX_CLONE_VFORK)) != LX_CLONE_VM) t->altstack = parent->altstack;
        else { t->altstack.ss_sp = 0; t->altstack.ss_size = 0; t->altstack.ss_flags = 2; /* SS_DISABLE */ }
        lkd_seccomp_fork(t, parent); /* copy_seccomp */
    } else {
        t->altstack.ss_flags = 2; /* SS_DISABLE */
    }
    pthread_mutex_lock(&t->proc->lock);
    t->next_in_proc = t->proc->threads;
    t->proc->threads = t;
    t->proc->nthreads++;
    pthread_mutex_unlock(&t->proc->lock);
    return t;
fail:
    close(t->wake_rd);
    close(t->wake_wr);
    free(t);
    return NULL;
}

lkd_task *lkd_task_alloc(lkd_task *parent, uint64_t clone_flags)
{
    return task_alloc_pid(parent, clone_flags, 0);
}

/* Remove t from its process thread list. Returns the number of threads left, -1 if t was not listed. */
static int proc_unlist(lkd_task *t)
{
    struct lkd_proc *p = t->proc;
    int left = -1;
    pthread_mutex_lock(&p->lock);
    for (lkd_task **pp = &p->threads; *pp; pp = &(*pp)->next_in_proc)
        if (*pp == t) {
            *pp = t->next_in_proc;
            t->next_in_proc = NULL;
            p->nthreads--;
            left = p->nthreads;
            break;
        }
    pthread_mutex_unlock(&p->lock);
    return left;
}

lkd_task *lkd_task_get(lkd_task *t)
{
    atomic_fetch_add(&t->refs, 1);
    return t;
}

void lkd_task_put(lkd_task *t)
{
    if (!t || atomic_fetch_sub(&t->refs, 1) != 1) return;
    struct lkd_proc *pr = t->proc;
    proc_unlist(t);
    if (atomic_fetch_sub(&pr->refs, 1) == 1) proc_free(pr);
    if (t->wake_rd >= 0) close(t->wake_rd);
    if (t->wake_wr >= 0) close(t->wake_wr);
    lkd_seccomp_release(t);
    t->magic = 0;
    free(t);
}

/* ---- Registry records ---------------------------------------------------------------------------- */

static void copy_comm(char *dst, const char *src)
{
    memset(dst, 0, LKD_COMM_LEN);
    if (src) strncpy(dst, src, LKD_COMM_LEN - 1);
}

char task_state_char(lkd_task *t)
{
    uint32_t pf = atomic_load(&t->ptrace.flags);
    if (pf & LKD_PT_STOPPED) return LKD_PS_TRACING_STOP;
    uint32_t xf = 0;
    struct ltask *e = lt_find(t); /* caller may or may not hold tasks lock: read-only racy access is fine */
    if (e) xf = e->xflags;
    if (xf & LT_STOPPED) return LKD_PS_STOPPED;
    if ((atomic_load(&t->flags) & LKD_TF_IN_BLOCK) || (xf & LT_IN_WAIT)) return LKD_PS_SLEEPING;
    return LKD_PS_RUNNING;
}

struct fam_tid *task_tidrec(lkd_task *t) { return fam_tid_find(t->tid); }

void proc_publish_locked(struct lkd_proc *p, lkd_task *leader)
{
    struct fam_proc *r = p->wait ? p->wait->rec : NULL;
    if (!r || r->info.pid != p->pid) return;
    lkd_proc_info *i = &r->info;
    i->host_pid = p->host_pid;
    if (!(atomic_load(&r->flags) & RF_ZOMBIE)) {
        if (leader) i->state = task_state_char(leader);
        i->nthreads = p->nthreads;
    }
    i->uid = p->cred.uid; i->euid = p->cred.euid; i->gid = p->cred.gid; i->egid = p->cred.egid;
    i->dumpable = p->dumpable;
    i->child_subreaper = p->child_subreaper;
    i->start_mono_ns = p->start_mono_ns;
    if (leader) memcpy(i->comm, leader->comm, LKD_COMM_LEN);
    snprintf(i->exe, sizeof i->exe, "%s", p->exe_path ? p->exe_path : "");
    size_t n = p->cmdline_len < sizeof i->cmdline ? p->cmdline_len : sizeof i->cmdline;
    if (p->cmdline && n) memcpy(i->cmdline, p->cmdline, n);
    i->cmdline_len = (uint32_t)n;
    memcpy(r->rlim, p->rlimits, sizeof r->rlim);
}

void task_publish_locked(lkd_task *t)
{
    struct fam_tid *r = fam_tid_find(t->tid);
    if (r) {
        lkd_tid_info *i = &r->info;
        i->tgid = t->proc->pid;
        i->state = task_state_char(t);
        memcpy(i->comm, t->comm, LKD_COMM_LEN);
        i->tracer_tid = t->ptrace.tracer_tid;
        i->sigblk = atomic_load(&t->sigmask);
        i->sigpnd = atomic_load(&t->pending.bits);
        i->syscall_nr = atomic_load(&t->cur_nr);
        memcpy(i->syscall_args, t->cur_args, sizeof i->syscall_args);
        i->nvcsw = t->nvcsw;
        i->nivcsw = t->nivcsw;
        r->affinity = t->affinity;
        r->robust_head = t->robust_list_head;
        r->robust_len = t->robust_list_len;
        r->nice = t->nice;
    }
    if (t->tid == t->proc->pid) proc_publish_locked(t->proc, t);
}

int proc_register_locked(struct lkd_proc *p, lkd_task *leader, int32_t leader_tid, const char *comm, int32_t creator_tid)
{
    struct fam_proc *r = fam_proc_find(p->pid);
    if (!r) r = fam_proc_new(p->pid);
    if (!r) return -LX_EAGAIN;
    r->info.ppid = p->ppid;
    r->info.pgid = p->pgid;
    r->info.sid = p->sid;
    r->info.state = LKD_PS_RUNNING;
    /* the record keeps the exit signal from here on: reparenting changes it (wait.c) */
    r->info.exit_signal = p->exit_signal;
    r->creator_tid = creator_tid;
    /* no parent in the family: the program the engine started, the family's init if it is the first */
    if (!fam_proc_find(p->ppid)) fam_set_child_reaper(p->pid);
    if (p->wait && p->wait->cohosted) atomic_fetch_or(&r->flags, RF_COHOSTED);
    p->wait->rec = r;
    if (comm) copy_comm(r->info.comm, comm);
    proc_publish_locked(p, leader);
    if (!fam_tid_new(leader_tid, p->pid)) return -LX_EAGAIN;
    struct fam_tid *tr = fam_tid_find(leader_tid);
    tr->info.state = LKD_PS_RUNNING;
    if (comm) copy_comm(tr->info.comm, comm);
    return 0;
}

void lkd_task_publish(lkd_task *t)
{
    tasks_lock();
    if (!lt_find(t) && g_ln < LTAB_MAX) {
        g_ltab[g_ln].t = lkd_task_get(t);
        g_ltab[g_ln].xflags = 0;
        g_ln++;
    }
    tasks_unlock();
    fam_lock();
    struct lkd_proc *p = t->proc;
    if (t->tid == p->pid && (!p->wait->rec || p->wait->rec->info.pid != p->pid))
        proc_register_locked(p, t, t->tid, t->comm, lkd_current() ? lkd_current()->tid : 0);
    if (!fam_tid_find(t->tid)) fam_tid_new(t->tid, p->pid);
    task_publish_locked(t);
    fam_unlock();
    fam_bump();
}

void tasks_forget(lkd_task *t)
{
    lkd_task *drop = NULL;
    tasks_lock();
    for (int i = 0; i < g_ln; i++)
        if (g_ltab[i].t == t) { drop = t; g_ltab[i] = g_ltab[--g_ln]; break; }
    tasks_unlock();
    if (drop) lkd_task_put(drop);
}

void lkd_task_unpublish(lkd_task *t)
{
    tasks_forget(t);
    fam_lock();
    fam_tid_free(fam_tid_find(t->tid));
    struct fam_proc *r = t->proc->wait ? t->proc->wait->rec : NULL;
    if (t->tid == t->proc->pid && r && r->info.pid == t->proc->pid && !(atomic_load(&r->flags) & RF_ZOMBIE)) {
        fam_proc_free(r);
        t->proc->wait->rec = NULL;
    }
    fam_unlock();
    fam_bump();
}

lkd_task *lkd_task_lookup(int32_t tid)
{
    lkd_task *r = NULL;
    tasks_lock();
    for (int i = 0; i < g_ln; i++)
        if (g_ltab[i].t->tid == tid) { r = lkd_task_get(g_ltab[i].t); break; }
    tasks_unlock();
    return r;
}

int tasks_count_proc(struct lkd_proc *p)
{
    int n = 0;
    tasks_lock();
    for (int i = 0; i < g_ln; i++) if (g_ltab[i].t->proc == p) n++;
    tasks_unlock();
    return n;
}

void tasks_wake_waiters_local(int32_t pid)
{
    tasks_lock();
    for (int i = 0; i < g_ln; i++)
        if (g_ltab[i].t->proc->pid == pid && (g_ltab[i].xflags & LT_IN_WAIT)) lkd_task_wake(g_ltab[i].t);
    tasks_unlock();
}

void lkd_registry_publish(lkd_task *t)
{
    struct lkd_sig_status ss;
    memset(&ss, 0, sizeof ss);
    lkd_sig_status(t, &ss);
    if (fam_ready() < 0) return;
    fam_lock();
    task_publish_locked(t);
    struct fam_proc *r = t->proc->wait ? t->proc->wait->rec : NULL;
    if (r && r->info.pid == t->proc->pid) {
        r->info.sigign = ss.ign;
        r->info.sigcgt = ss.cgt;
        r->info.shdpnd = ss.shdpnd;
    }
    fam_unlock();
}

void lkd_proc_refresh_ids(struct lkd_proc *p)
{
    struct fam_proc *r = p && p->wait ? p->wait->rec : NULL;
    if (!r || __atomic_load_n(&r->info.pid, __ATOMIC_ACQUIRE) != p->pid) return;
    p->ppid = __atomic_load_n(&r->info.ppid, __ATOMIC_ACQUIRE);
    p->pgid = __atomic_load_n(&r->info.pgid, __ATOMIC_ACQUIRE);
    p->sid = __atomic_load_n(&r->info.sid, __ATOMIC_ACQUIRE);
    p->exit_signal = __atomic_load_n(&r->info.exit_signal, __ATOMIC_ACQUIRE);
}

void lkd_proc_set_layout(lkd_task *t, const struct lkd_proc_layout *l)
{
    struct lkd_proc *p = t->proc;
    struct lkd_proc_layout *n = calloc(1, sizeof *n);
    if (!n) return;
    *n = *l;
    n->auxv = NULL;
    n->auxv_len = 0;
    if (l->auxv && l->auxv_len) {
        n->auxv = malloc(l->auxv_len);
        if (n->auxv) { memcpy(n->auxv, l->auxv, l->auxv_len); n->auxv_len = l->auxv_len; }
    }
    pthread_mutex_lock(&p->lock);
    struct lkd_proc_layout *old = p->layout;
    p->layout = n;
    pthread_mutex_unlock(&p->lock);
    if (old) { free(old->auxv); free(old); }
}

/* TASK_OP_WAKE_WAITERS: wake the wait4/waitid callers of a process living in the receiver. */
static void op_wake_waiters(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    int32_t pid = 0;
    if (req->len != sizeof pid) { resp->status = -LX_EPROTO; return; }
    memcpy(&pid, req->data, sizeof pid);
    tasks_wake_waiters_local(pid);
    resp->status = 0;
}

void task_wake_register(void) { lkd_famsvc_register(TASK_OP_WAKE_WAITERS, op_wake_waiters); }

/* The waiters of pid sleep on their wake pipes (lkd_block_wait_wake in do_wait). A process of another
 * host process is reached through its family service; without it the waiter would only see the change
 * counter at its next 10 ms poll (ptrace-2: the death or stop of a child in another host process). */
void lkd_task_wake_waiters(int32_t pid)
{
    fam_bump();
    int32_t host = 0;
    if (pid > 0 && fam_ready() == 0) {
        fam_lock();
        struct fam_proc *r = fam_proc_find(pid);
        if (r && !(atomic_load(&r->flags) & RF_ZOMBIE)) host = r->info.host_pid;
        fam_unlock();
    }
    if (host <= 0 || host == getpid()) { tasks_wake_waiters_local(pid); return; }
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = TASK_OP_WAKE_WAITERS;
    req.target_pid = pid;
    req.data = &pid;
    req.len = sizeof pid;
    (void)lkd_famsvc_call(host, &req, &resp, 1000); /* best effort: the counter covers a failure */
    lkd_famsvc_msg_free(&resp);
}

void lkd_task_notify_stop(lkd_task *t, int signo)
{
    task_set_xflags(t, LT_STOPPED, 0);
    int32_t ppid = 0;
    fam_lock();
    struct fam_proc *r = t->proc->wait ? t->proc->wait->rec : NULL;
    if (r && r->info.pid == t->proc->pid) {
        r->stop_status = LX_W_STOPCODE(signo);
        atomic_fetch_or(&r->flags, RF_STOP_PENDING);
        atomic_fetch_and(&r->flags, ~RF_CONT_PENDING);
        r->info.state = LKD_PS_STOPPED;
        ppid = r->info.ppid;
    }
    fam_unlock();
    if (ppid) lkd_task_wake_waiters(ppid);
}

void lkd_task_notify_continue(lkd_task *t)
{
    task_set_xflags(t, 0, LT_STOPPED);
    int32_t ppid = 0;
    fam_lock();
    struct fam_proc *r = t->proc->wait ? t->proc->wait->rec : NULL;
    if (r && r->info.pid == t->proc->pid) {
        atomic_fetch_or(&r->flags, RF_CONT_PENDING);
        atomic_fetch_and(&r->flags, ~RF_STOP_PENDING);
        r->info.state = LKD_PS_RUNNING;
        ppid = r->info.ppid;
    }
    fam_unlock();
    if (ppid) lkd_task_wake_waiters(ppid);
}

/* ---- Main task ----------------------------------------------------------------------------------- */

static void set_snapshot(char **dst, size_t *len, const char *const *v)
{
    free(*dst);
    *dst = NULL;
    *len = 0;
    if (!v) return;
    size_t n = 0;
    for (int i = 0; v[i]; i++) n += strlen(v[i]) + 1;
    if (!n) return;
    char *b = malloc(n), *q = b;
    if (!b) return;
    for (int i = 0; v[i]; i++) { size_t l = strlen(v[i]) + 1; memcpy(q, v[i], l); q += l; }
    *dst = b;
    *len = n;
}

static const char *base_name(const char *p)
{
    const char *s = p ? strrchr(p, '/') : NULL;
    return s ? s + 1 : (p ? p : "");
}

lkd_task *lkd_task_create_main(const lkd_main_spec *spec)
{
    if (!spec || spec->size < offsetof(lkd_main_spec, engine_data)) { errno = EINVAL; return NULL; }
    if (fam_ready() < 0) { errno = ENOMEM; return NULL; }
    int32_t pid = spec->pid > 0 ? spec->pid : 0;
    struct fam_proc adopt;
    int have_adopt = 0;
    if (pid) {
        fam_lock();
        struct fam_proc *r = fam_proc_find(pid);
        if (r) {
            if (r->info.host_pid == getpid() && !(atomic_load(&r->flags) & RF_ZOMBIE)) {
                adopt = *r; /* the same process after a host re-exec */
                have_adopt = 1;
            } else {
                fam_unlock();
                errno = EEXIST;
                return NULL;
            }
        }
        fam_unlock();
    }
    lkd_task *t = task_alloc_pid(NULL, 0, pid);
    if (!t) { errno = ENOMEM; return NULL; }
    struct lkd_proc *p = t->proc;
    if (have_adopt) {
        p->ppid = adopt.info.ppid;
        p->pgid = adopt.info.pgid;
        p->sid = adopt.info.sid;
        p->dumpable = adopt.info.dumpable;
        p->cred.uid = p->cred.suid = p->cred.fsuid = adopt.info.uid;
        p->cred.euid = adopt.info.euid;
        p->cred.gid = p->cred.sgid = p->cred.fsgid = adopt.info.gid;
        p->cred.egid = adopt.info.egid;
        p->child_subreaper = adopt.info.child_subreaper;
        memcpy(p->rlimits, adopt.rlim, sizeof p->rlimits);
    } else if (spec->ppid > 0) {
        p->ppid = spec->ppid;
    }
    if (spec->exe_path) p->exe_path = strdup(spec->exe_path);
    set_snapshot(&p->cmdline, &p->cmdline_len, spec->argv);
    set_snapshot(&p->environ_snap, &p->environ_len, spec->envp);
    copy_comm(t->comm, base_name(spec->exe_path));
    t->mm = lkd_mm_create();
    t->fdt = lkd_fdtable_new();
    t->fsctx = lkd_fsctx_new();
    t->sighand = lkd_sighand_clone(NULL, 0);
    t->engine_data = spec->engine_data;
    if (have_adopt) {
        fam_lock();
        struct fam_proc *r = fam_proc_find(pid);
        if (r) p->wait->rec = r;
        fam_unlock();
    }
    lkd_task_publish(t);
    return t;
}

/* Does t's guest process share its host process with another guest process (a vfork/posix_spawn
 * child before exec)? Modules must then not stop host-wide services in their proc_exit hooks. */
int lkd_task_shares_host(const lkd_task *t)
{
    return t && t->proc && t->proc->wait && t->proc->wait->cohosted;
}

/* ---- Exit ------------------------------------------------------------------------------------------ */

void task_mm_release(lkd_task *t)
{
    /* kernel/fork.c mm_release(): robust list first (futex_exit_release), then clear_child_tid when
     * the address space has other users: write 0 and FUTEX_WAKE 1 (shared). */
    lkd_futex_exit_robust_list(t);
    t->robust_list_head = 0;
    t->robust_list_len = 0;
    if (t->clear_child_tid) {
        int shared;
        pthread_mutex_lock(&t->proc->lock);
        shared = t->proc->nthreads > 1 || (t->proc->wait && t->proc->wait->cohosted);
        pthread_mutex_unlock(&t->proc->lock);
        if (shared) {
            uint32_t zero = 0;
            if (lkd_user_write(t->clear_child_tid, &zero, sizeof zero) == 0)
                lkd_futex_wake_addr(t, t->clear_child_tid, 1, 0);
        }
        t->clear_child_tid = 0;
    }
}

static void thread_forget_children(lkd_task *t)
{
    /* kernel/exit.c forget_original_parent(): children created by an exiting thread go to another live
     * thread of the group and receive their parent-death signal. */
    int32_t heir = 0;
    pthread_mutex_lock(&t->proc->lock);
    for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc)
        if (x != t && !(atomic_load(&x->flags) & LKD_TF_EXITING)) { heir = x->tid; break; }
    pthread_mutex_unlock(&t->proc->lock);
    if (!heir) return;
    int32_t pids[TASK_MAX_CHILDREN], sigs[TASK_MAX_CHILDREN];
    int n = 0;
    fam_lock();
    for (int i = 0; i < FAM_MAX_PROCS && n < TASK_MAX_CHILDREN; i++) {
        struct fam_proc *c = fam_proc_at(i);
        if (!c->info.pid || c->info.ppid != t->proc->pid || c->creator_tid != t->tid) continue;
        c->creator_tid = heir;
        if (c->pdeathsig && !(atomic_load(&c->flags) & RF_ZOMBIE)) { pids[n] = c->info.pid; sigs[n] = c->pdeathsig; n++; }
    }
    fam_unlock();
    for (int i = 0; i < n; i++) lkd_sig_send_pid(t, pids[i], sigs[i], NULL);
}

void task_exit_work(lkd_task *t)
{
    tasks_lock();
    struct ltask *e = lt_find(t);
    int done = e ? (e->xflags & LT_EXIT_DONE) : 0;
    if (e) e->xflags |= LT_EXIT_DONE;
    tasks_unlock();
    if (done) return;
    atomic_fetch_or(&t->flags, LKD_TF_EXITING);
    task_mm_release(t);
    lkd_hooks_task_exit(t);
    thread_forget_children(t);
}

int64_t lkd_task_do_exit(lkd_task *t, int exit_code)
{
    /* kernel/exit.c SYSCALL_DEFINE1(exit): do_exit((error_code & 0xff) << 8). */
    t->exit_code = (exit_code & 0xff) << 8;
    task_exit_work(t);
    return LKD_RET_EXIT_THREAD;
}

int64_t lkd_task_do_exit_group(lkd_task *t, int wait_status)
{
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    /* do_group_exit(): a group exit already in progress keeps its status. */
    if (!p->wait->group_exit) {
        p->wait->group_exit = 1;
        p->wait->group_status = wait_status;
        p->exit_code = (uint32_t)wait_status;
        p->exiting = 1;
    }
    pthread_mutex_unlock(&p->lock);
    t->exit_code = p->wait->group_status;
    lkd_sig_kill_all_threads(t);
    task_exit_work(t);
    return LKD_RET_EXIT_GROUP;
}

int lkd_task_release(lkd_task *t, int *exit_status_out)
{
    struct lkd_proc *p = t->proc;
    if (exit_status_out) *exit_status_out = 0;
    if (task_xflags(t) & LT_MIGRATED) {
        /* The identity continues in a forked host process: nothing to publish here. Drop this host
         * process' references to the shared objects (a leaked fd table copy would keep pipe ends open). */
        if (t->fdt) { lkd_fdtable_put(t->fdt); t->fdt = NULL; }
        if (t->fsctx) { lkd_fsctx_put(t->fsctx); t->fsctx = NULL; }
        if (t->sighand) { lkd_sighand_put(t->sighand); t->sighand = NULL; }
        if (t->mm) { lkd_mm_put(t->mm); t->mm = NULL; }
        proc_unlist(t);
        tasks_forget(t);
        lkd_task_put(t);
        return 0;
    }
    task_exit_work(t);
    int left = proc_unlist(t);
    int last = left <= 0;
    pthread_mutex_lock(&p->lock);
    int group = p->wait->group_exit;
    int status = group ? p->wait->group_status : t->exit_code;
    pthread_mutex_unlock(&p->lock);
    if (!last && t->tid != p->pid) {
        fam_lock();
        fam_tid_free(fam_tid_find(t->tid));
        fam_unlock();
    } else if (!last) {
        fam_lock();
        struct fam_tid *r = fam_tid_find(t->tid);
        if (r) r->info.state = LKD_PS_ZOMBIE; /* zombie leader of a live thread group */
        fam_unlock();
    }
    int host_exit = 0;
    if (last) lkd_hooks_proc_exit(t);
    if (last || group) host_exit = proc_die(t, status);
    tasks_forget(t);
    if (exit_status_out)
        *exit_status_out = LX_WIFEXITED(status) ? LX_WEXITSTATUS(status) : 128 + LX_WTERMSIG(status);
    lkd_task_put(t);
    return host_exit;
}

/* ---- de_thread for exec -------------------------------------------------------------------------- */

int task_de_thread(lkd_task *t, int32_t *old_tid)
{
    struct lkd_proc *p = t->proc;
    *old_tid = t->tid;
    const lkd_engine_ops *ops = lkd_engine();
    pthread_mutex_lock(&p->lock);
    if (p->wait->group_exit) { pthread_mutex_unlock(&p->lock); return -LX_EAGAIN; }
    p->wait->exec_killing = 1;
    for (lkd_task *x = p->threads; x; x = x->next_in_proc) {
        if (x == t) continue;
        atomic_fetch_or(&x->flags, LKD_TF_FATAL);
        lkd_task_wake(x);
        if (ops) ops->kick(x);
    }
    pthread_mutex_unlock(&p->lock);
    int64_t start = lkd_host_mono_ns(), next_log = start + 2000000000ll;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        int n = p->nthreads, gx = p->wait->group_exit;
        pthread_mutex_unlock(&p->lock);
        if (n <= 1) break;
        if (gx || (atomic_load(&t->flags) & LKD_TF_FATAL)) {
            p->wait->exec_killing = 0;
            return -LX_EAGAIN; /* de_thread(): another group action wins */
        }
        if (lkd_host_mono_ns() > next_log) {
            lkd_log("exec of tid %d waits for %d other threads to exit", t->tid, n - 1);
            next_log += 2000000000ll;
        }
        usleep(500);
    }
    p->wait->exec_killing = 0;
    if (t->tid != p->pid) {
        /* A non-leader thread that execs takes over the leader's tid (fs/exec.c de_thread). */
        int32_t old = t->tid;
        fam_lock();
        struct fam_tid *lr = fam_tid_find(p->pid);
        struct fam_tid *mr = fam_tid_find(old);
        if (lr) fam_tid_free(lr);
        if (mr) mr->info.tid = p->pid;
        else fam_tid_new(p->pid, p->pid);
        t->tid = p->pid;
        task_publish_locked(t);
        fam_unlock();
    }
    return 0;
}

/* ---- fork support ---------------------------------------------------------------------------------- */

void tasks_reset_after_fork(lkd_task *keep)
{
    lkd_fork_reinit_mutex(&g_tasks, 0);
    uint32_t keepx = 0;
    for (int i = 0; i < g_ln; i++) {
        lkd_task *x = g_ltab[i].t;
        if (x == keep) { keepx = g_ltab[i].xflags; continue; }
        /* Inherited copies of other threads' wake pipes: close them, the objects stay (other modules may
         * still point at them; the memory is a private copy). */
        if (x->wake_rd >= 0) close(x->wake_rd);
        if (x->wake_wr >= 0) close(x->wake_wr);
        x->wake_rd = x->wake_wr = -1;
    }
    g_ln = 0;
    if (keep) {
        g_ltab[0].t = keep;
        g_ltab[0].xflags = keepx & ~(LT_IN_WAIT | LT_STOPPED | LT_MIGRATED);
        g_ln = 1;
    }
}

/* Re-create the wake pipe of t in a fork child (the inherited one is shared with the parent's task). */
int task_new_wake_pipe(lkd_task *t)
{
    if (t->wake_rd >= 0) close(t->wake_rd);
    if (t->wake_wr >= 0) close(t->wake_wr);
    t->wake_rd = t->wake_wr = -1;
    return make_wake_pipe(t);
}

void lkd_task_fork_prepare(lkd_task *t, uint64_t clone_flags)
{
    (void)clone_flags;
    tasks_lock();
    /* Every process object of this host process: their locks must be free in the child. */
    struct lkd_proc *seen[64];
    int ns = 0;
    for (int i = 0; i < g_ln && ns < 64; i++) {
        struct lkd_proc *p = g_ltab[i].t->proc;
        int dup = 0;
        for (int j = 0; j < ns; j++) if (seen[j] == p) dup = 1;
        if (!dup) seen[ns++] = p;
    }
    if (t && t->proc) {
        int dup = 0;
        for (int j = 0; j < ns; j++) if (seen[j] == t->proc) dup = 1;
        if (!dup && ns < 64) seen[ns++] = t->proc;
    }
    /* ascending address order: one consistent order for equal-rank locks */
    for (int i = 1; i < ns; i++)
        for (int j = i; j > 0 && seen[j] < seen[j - 1]; j--) { struct lkd_proc *x = seen[j]; seen[j] = seen[j - 1]; seen[j - 1] = x; }
    for (int i = 0; i < ns; i++) pthread_mutex_lock(&seen[i]->lock);
    fam_fork_prepare();
    itimers_fork_prepare();
    /* the set of locked procs is recomputed identically by fork_parent */
}

static int locked_procs(lkd_task *t, struct lkd_proc **seen)
{
    int ns = 0;
    for (int i = 0; i < g_ln && ns < 64; i++) {
        struct lkd_proc *p = g_ltab[i].t->proc;
        int dup = 0;
        for (int j = 0; j < ns; j++) if (seen[j] == p) dup = 1;
        if (!dup) seen[ns++] = p;
    }
    if (t && t->proc) {
        int dup = 0;
        for (int j = 0; j < ns; j++) if (seen[j] == t->proc) dup = 1;
        if (!dup && ns < 64) seen[ns++] = t->proc;
    }
    return ns;
}

void lkd_task_fork_parent(lkd_task *t, pid_t host_child)
{
    (void)host_child;
    itimers_fork_parent();
    fam_fork_parent();
    struct lkd_proc *seen[64];
    int ns = locked_procs(t, seen);
    for (int i = 0; i < ns; i++) pthread_mutex_unlock(&seen[i]->lock);
    tasks_unlock();
}

/* lkd_task_fork_child is in clone.c (it switches identities). Helper for it: */
void tasks_fork_child_unlock_all(lkd_task *t)
{
    struct lkd_proc *seen[64];
    int ns = locked_procs(t, seen);
    for (int i = 0; i < ns; i++) lkd_fork_reinit_mutex(&seen[i]->lock, 0);
    fam_fork_child();
}

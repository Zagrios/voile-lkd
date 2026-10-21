/* stubs_task.c: weak placeholder task registry. Owner: core (until the task module overrides it).
 *
 * Just enough for other modules to build fake tasks in unit tests: in-memory tasks and processes, a
 * family-wide id counter in an anonymous MAP_SHARED page (inherited by fork), local lookups. No shared
 * registry, no wait4, no real exit. Every symbol here is weak.
 */
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core_priv.h"
#include "fd.h"
#include "mm.h"
#include "sig.h"
#include "task.h"
#include "vfs.h"

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static lkd_task *reg_head;          /* published tasks of this process, linked through a side array */
#define REG_MAX 4096
static lkd_task *reg_tab[REG_MAX];
static int reg_n;

static _Atomic int32_t *id_counter;

LKD_WEAK int lkd_family_create(uint32_t first_pid)
{
    if (id_counter) return 0;
    void *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return LKD_HOST_ERR();
    id_counter = p;
    atomic_store(id_counter, (int32_t)(first_pid ? first_pid : 2));
    return 0;
}
LKD_WEAK int lkd_family_export(void) { return -LX_ENOSYS; }
LKD_WEAK int lkd_family_attach(int fd) { (void)fd; return -LX_ENOSYS; }

LKD_WEAK int32_t lkd_pid_alloc(void)
{
    if (!id_counter && lkd_family_create(lkd_config_get()->first_pid) < 0) return -LX_EAGAIN;
    int32_t id = atomic_fetch_add(id_counter, 1);
    return id > LKD_PID_MAX ? -LX_EAGAIN : id;
}

LKD_WEAK lkd_task *lkd_task_alloc(lkd_task *parent, uint64_t clone_flags)
{
    lkd_task *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->magic = LKD_TASK_MAGIC;
    atomic_init(&t->refs, 1);
    atomic_init(&t->cur_nr, -1);
    t->wake_rd = t->wake_wr = -1;
    int p[2];
    if (pipe(p) < 0) { free(t); return NULL; }
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    t->wake_rd = p[0];
    t->wake_wr = p[1];
    if (parent && (clone_flags & LX_CLONE_THREAD)) {
        t->proc = parent->proc;
        atomic_fetch_add(&t->proc->refs, 1);
        t->tid = lkd_pid_alloc();
    } else {
        struct lkd_proc *pr = calloc(1, sizeof *pr);
        if (!pr) { close(p[0]); close(p[1]); free(t); return NULL; }
        atomic_init(&pr->refs, 1);
        pthread_mutex_init(&pr->lock, NULL);
        pr->pid = lkd_pid_alloc();
        pr->ppid = parent ? parent->proc->pid : 1;
        pr->pgid = parent ? parent->proc->pgid : pr->pid;
        pr->sid = parent ? parent->proc->sid : pr->pid;
        pr->host_pid = getpid();
        pr->cred.uid = pr->cred.euid = pr->cred.suid = pr->cred.fsuid = 1000;
        pr->cred.gid = pr->cred.egid = pr->cred.sgid = pr->cred.fsgid = 1000;
        for (int i = 0; i < LX_RLIM_NLIMITS; i++) pr->rlimits[i].rlim_cur = pr->rlimits[i].rlim_max = LX_RLIM_INFINITY;
        pr->rlimits[LX_RLIMIT_NOFILE].rlim_cur = 1024;
        pr->rlimits[LX_RLIMIT_NOFILE].rlim_max = 65536;
        pr->dumpable = 1;
        pr->start_mono_ns = (uint64_t)lkd_host_mono_ns();
        if (parent) {
            pr->cred = parent->proc->cred;
            memcpy(pr->rlimits, parent->proc->rlimits, sizeof pr->rlimits);
        }
        t->proc = pr;
        t->tid = pr->pid;
    }
    if (parent) {
        atomic_store(&t->sigmask, atomic_load(&parent->sigmask));
        memcpy(t->comm, parent->comm, sizeof t->comm);
    }
    pthread_mutex_lock(&t->proc->lock);
    t->next_in_proc = t->proc->threads;
    t->proc->threads = t;
    t->proc->nthreads++;
    pthread_mutex_unlock(&t->proc->lock);
    return t;
}

LKD_WEAK void lkd_task_publish(lkd_task *t)
{
    pthread_mutex_lock(&reg_lock);
    if (reg_n < REG_MAX) { reg_tab[reg_n++] = t; lkd_task_get(t); }
    pthread_mutex_unlock(&reg_lock);
    (void)reg_head;
}

LKD_WEAK void lkd_task_unpublish(lkd_task *t)
{
    int found = 0;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n; i++)
        if (reg_tab[i] == t) { reg_tab[i] = reg_tab[--reg_n]; found = 1; break; }
    pthread_mutex_unlock(&reg_lock);
    if (found) lkd_task_put(t);
}

LKD_WEAK lkd_task *lkd_task_get(lkd_task *t)
{
    atomic_fetch_add(&t->refs, 1);
    return t;
}

LKD_WEAK void lkd_task_put(lkd_task *t)
{
    if (atomic_fetch_sub(&t->refs, 1) != 1) return;
    struct lkd_proc *pr = t->proc;
    pthread_mutex_lock(&pr->lock);
    for (struct lkd_task **pp = &pr->threads; *pp; pp = &(*pp)->next_in_proc)
        if (*pp == t) { *pp = t->next_in_proc; break; }
    pr->nthreads--;
    pthread_mutex_unlock(&pr->lock);
    if (atomic_fetch_sub(&pr->refs, 1) == 1) {
        free(pr->exe_path);
        free(pr->cmdline);
        free(pr->environ_snap);
        pthread_mutex_destroy(&pr->lock);
        free(pr);
    }
    if (t->wake_rd >= 0) close(t->wake_rd);
    if (t->wake_wr >= 0) close(t->wake_wr);
    t->magic = 0;
    free(t);
}

LKD_WEAK lkd_task *lkd_task_lookup(int32_t tid)
{
    lkd_task *r = NULL;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n; i++)
        if (reg_tab[i]->tid == tid) { r = lkd_task_get(reg_tab[i]); break; }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

static void fill_proc(const lkd_task *t, lkd_proc_info *o)
{
    const struct lkd_proc *p = t->proc;
    memset(o, 0, sizeof *o);
    o->pid = p->pid; o->ppid = p->ppid; o->pgid = p->pgid; o->sid = p->sid;
    o->host_pid = p->host_pid; o->state = LKD_PS_RUNNING; o->nthreads = p->nthreads;
    o->uid = p->cred.uid; o->euid = p->cred.euid; o->gid = p->cred.gid; o->egid = p->cred.egid;
    o->dumpable = p->dumpable; o->start_mono_ns = p->start_mono_ns;
    memcpy(o->comm, t->comm, sizeof o->comm);
    if (p->exe_path) snprintf(o->exe, sizeof o->exe, "%s", p->exe_path);
}

LKD_WEAK int lkd_proc_lookup(int32_t pid, lkd_proc_info *out)
{
    int r = -LX_ESRCH;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n; i++)
        if (reg_tab[i]->tid == pid && reg_tab[i]->proc->pid == pid) { fill_proc(reg_tab[i], out); r = 0; break; }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

LKD_WEAK int lkd_tid_lookup(int32_t tid, lkd_tid_info *out)
{
    int r = -LX_ESRCH;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n; i++) {
        lkd_task *t = reg_tab[i];
        if (t->tid != tid) continue;
        memset(out, 0, sizeof *out);
        out->tid = t->tid; out->tgid = t->proc->pid; out->state = LKD_PS_RUNNING;
        memcpy(out->comm, t->comm, sizeof out->comm);
        out->tracer_tid = t->ptrace.tracer_tid;
        out->sigblk = atomic_load(&t->sigmask);
        out->sigpnd = atomic_load(&t->pending.bits);
        out->syscall_nr = atomic_load(&t->cur_nr);
        r = 0;
        break;
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

LKD_WEAK int lkd_proc_list(lkd_proc_info *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n && n < max; i++)
        if (reg_tab[i]->tid == reg_tab[i]->proc->pid) fill_proc(reg_tab[i], &out[n++]);
    pthread_mutex_unlock(&reg_lock);
    return n;
}

LKD_WEAK int lkd_tid_list(int32_t tgid, int32_t *tids, int max)
{
    int n = 0;
    pthread_mutex_lock(&reg_lock);
    for (int i = 0; i < reg_n && n < max; i++)
        if (reg_tab[i]->proc->pid == tgid) tids[n++] = reg_tab[i]->tid;
    pthread_mutex_unlock(&reg_lock);
    return n ? n : -LX_ESRCH;
}

LKD_WEAK void lkd_registry_publish(lkd_task *t) { (void)t; }
LKD_WEAK void lkd_proc_refresh_ids(struct lkd_proc *p) { (void)p; }
LKD_WEAK void lkd_proc_set_layout(lkd_task *t, const struct lkd_proc_layout *l) { (void)t; (void)l; }
LKD_WEAK void lkd_task_wake_waiters(int32_t pid) { (void)pid; }

LKD_WEAK int64_t lkd_task_do_exit(lkd_task *t, int exit_code)
{
    t->exit_code = exit_code;
    atomic_fetch_or(&t->flags, LKD_TF_EXITING);
    return LKD_RET_EXIT_THREAD;
}

LKD_WEAK int64_t lkd_task_do_exit_group(lkd_task *t, int wait_status)
{
    t->proc->exit_code = (uint32_t)wait_status;
    t->proc->exiting = 1;
    lkd_sig_kill_all_threads(t);
    return LKD_RET_EXIT_GROUP;
}

LKD_WEAK lkd_task *lkd_task_create_main(const lkd_main_spec *spec)
{
    if (!spec) { errno = EINVAL; return NULL; }
    lkd_task *t = lkd_task_alloc(NULL, 0);
    if (!t) { errno = ENOMEM; return NULL; }
    if (spec->exe_path) t->proc->exe_path = strdup(spec->exe_path);
    t->mm = lkd_mm_create();
    t->fdt = lkd_fdtable_new();
    t->fsctx = lkd_fsctx_new();
    t->sighand = lkd_sighand_clone(NULL, 0);
    t->engine_data = spec->engine_data;
    lkd_task_publish(t);
    return t;
}

LKD_WEAK int lkd_task_release(lkd_task *t, int *exit_status_out)
{
    int last;
    pthread_mutex_lock(&t->proc->lock);
    last = t->proc->nthreads == 1;
    pthread_mutex_unlock(&t->proc->lock);
    if (exit_status_out) *exit_status_out = (int)t->proc->exit_code;
    lkd_task_unpublish(t);
    lkd_task_put(t);
    return last;
}

LKD_WEAK int lkd_exec_prepare(lkd_task *t, const char *path, char *const argv[], char *const envp[], lkd_exec_image *img)
{
    (void)t; (void)path; (void)argv; (void)envp; (void)img;
    return -LX_ENOSYS;
}
LKD_WEAK void lkd_exec_finish(lkd_task *t) { (void)t; }
LKD_WEAK void lkd_task_notify_stop(lkd_task *t, int signo) { (void)t; (void)signo; }
LKD_WEAK void lkd_task_notify_continue(lkd_task *t) { (void)t; }
LKD_WEAK int lkd_task_uname(struct lx_utsname *out) { (void)out; return -LX_ENOSYS; }
LKD_WEAK int lkd_task_shares_host(const lkd_task *t) { (void)t; return 0; }
LKD_WEAK int64_t lkd_guest_clock_ns(int clockid)
{
    struct timespec ts;
    clock_gettime(clockid == LX_CLOCK_REALTIME ? CLOCK_REALTIME : CLOCK_UPTIME_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
LKD_WEAK uint32_t lkd_pid_peek(void) { return 0; }
LKD_WEAK int lkd_seccomp_enter(lkd_task *t, const lkd_sysreq *rq, int64_t *ret) { (void)t; (void)rq; (void)ret; return 0; }
LKD_WEAK void lkd_seccomp_fork(lkd_task *child, lkd_task *parent) { (void)child; (void)parent; }
LKD_WEAK void lkd_seccomp_release(lkd_task *t) { (void)t; }
LKD_WEAK void lkd_seccomp_status(lkd_task *t, int *mode, int *filters) { (void)t; *mode = 0; *filters = 0; }

/* sys.c: uname, resource limits and usage, system information, scheduling, getrandom, prctl,
 * arch_prctl, thread bookkeeping (set_tid_address, robust lists), exit, and the small privileged calls
 * that answer -EPERM for a user without capabilities. Behaviors follow kernel/sys.c, kernel/sched/
 * syscalls.c, arch/x86/kernel/process_64.c and drivers/char/random.c of Linux v6.12. */
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "famsvc.h"
#include "fd.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task_priv.h"
#include "vfs.h"

/* The release string is shared with procfs (/proc/sys/kernel/osrelease, /proc/version) through
 * lkd_task_uname(). It names the ABI LKD models (Linux v6.12) and says it is LKD. */
#define LKD_UNAME_RELEASE "6.12.0-lkd"
#define LKD_UNAME_VERSION "#1 SMP PREEMPT_DYNAMIC LKD"

int lkd_task_uname(struct lx_utsname *u)
{
    memset(u, 0, sizeof *u);
    fam_ready();
    snprintf(u->sysname, sizeof u->sysname, "Linux");
    snprintf(u->nodename, sizeof u->nodename, "%s", fam_hostname());
    snprintf(u->release, sizeof u->release, "%s", LKD_UNAME_RELEASE);
    snprintf(u->version, sizeof u->version, "%s", LKD_UNAME_VERSION);
    snprintf(u->machine, sizeof u->machine, "x86_64");
    snprintf(u->domainname, sizeof u->domainname, "%s", fam_domainname());
    return 0;
}

LKD_SYSCALL(uname)
{
    struct lx_utsname u;
    lkd_task_uname(&u);
    return lkd_user_write(LKD_ARGP(0), &u, sizeof u) ? -LX_EFAULT : 0;
}

/* sethostname/setdomainname need CAP_SYS_ADMIN in the UTS namespace: -EPERM for the guest user. */
LKD_SYSCALL(sethostname) { return -LX_EPERM; }
LKD_SYSCALL(setdomainname) { return -LX_EPERM; }

/* ---- rlimits ------------------------------------------------------------------------------------ */

#define NR_OPEN_MAX 1048576u /* fs.nr_open default */

static int rlimit_get(lkd_task *t, int32_t pid, unsigned res, struct lx_rlimit *out)
{
    if (pid == 0 || pid == t->proc->pid) {
        pthread_mutex_lock(&t->proc->lock);
        *out = t->proc->rlimits[res];
        pthread_mutex_unlock(&t->proc->lock);
        return 0;
    }
    int r = -LX_ESRCH;
    fam_lock();
    struct fam_proc *p = fam_proc_find(pid);
    if (!p) {
        struct fam_tid *tr = fam_tid_find(pid);
        if (tr) p = fam_proc_find(tr->info.tgid);
    }
    if (p && !(atomic_load(&p->flags) & RF_ZOMBIE)) { *out = p->rlim[res]; r = 0; }
    fam_unlock();
    return r;
}

static int rlimit_set(lkd_task *t, unsigned res, const struct lx_rlimit *n)
{
    if (n->rlim_cur > n->rlim_max) return -LX_EINVAL;
    if (res == LX_RLIMIT_NOFILE && n->rlim_max > NR_OPEN_MAX) return -LX_EPERM;
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    struct lx_rlimit *o = &p->rlimits[res];
    /* do_prlimit(): raising the hard limit needs CAP_SYS_RESOURCE. */
    if (n->rlim_max > o->rlim_max && p->cred.euid != 0) { pthread_mutex_unlock(&p->lock); return -LX_EPERM; }
    *o = *n;
    pthread_mutex_unlock(&p->lock);
    fam_lock();
    proc_publish_locked(p, NULL);
    fam_unlock();
    return 0;
}

LKD_SYSCALL(getrlimit)
{
    unsigned res = LKD_ARGUI(0);
    if (res >= LX_RLIM_NLIMITS) return -LX_EINVAL;
    struct lx_rlimit r;
    rlimit_get(t, 0, res, &r);
    return lkd_user_write(LKD_ARGP(1), &r, sizeof r) ? -LX_EFAULT : 0;
}

LKD_SYSCALL(setrlimit)
{
    unsigned res = LKD_ARGUI(0);
    if (res >= LX_RLIM_NLIMITS) return -LX_EINVAL;
    struct lx_rlimit r;
    if (lkd_user_read(LKD_ARGP(1), &r, sizeof r)) return -LX_EFAULT;
    return rlimit_set(t, res, &r);
}

LKD_SYSCALL(prlimit64)
{
    int32_t pid = LKD_ARGI(0);
    unsigned res = LKD_ARGUI(1);
    uint64_t newp = LKD_ARGP(2), oldp = LKD_ARGP(3);
    struct lx_rlimit n, o;
    if (newp && lkd_user_read(newp, &n, sizeof n)) return -LX_EFAULT;
    if (res >= LX_RLIM_NLIMITS) return -LX_EINVAL;
    if (newp && n.rlim_cur > n.rlim_max) return -LX_EINVAL;
    int self = pid == 0 || pid == t->proc->pid;
    if (!self) {
        lkd_proc_info pi;
        int e = rlimit_get(t, pid, res, &o);
        if (e < 0) return e;
        /* check_prlimit_permission(): same real, effective and saved ids. */
        if (lkd_proc_lookup(pid, &pi) == 0 && (pi.uid != t->proc->cred.uid || pi.euid != t->proc->cred.euid))
            return -LX_EPERM;
        if (newp) {
            lkd_log_once("prlimit64 on another process (pid %d) is not supported", pid);
            return -LX_ENOSYS;
        }
    } else {
        rlimit_get(t, 0, res, &o);
        if (newp) {
            int e = rlimit_set(t, res, &n);
            if (e < 0) return e;
        }
    }
    if (oldp && lkd_user_write(oldp, &o, sizeof o)) return -LX_EFAULT;
    return 0;
}

/* ---- rusage, times, sysinfo ---------------------------------------------------------------------- */

static void tv_from_host(struct lx_timeval *d, struct timeval s) { d->tv_sec = s.tv_sec; d->tv_usec = s.tv_usec; }

void wait_rusage_now(struct lx_rusage *ru)
{
    struct rusage h;
    memset(ru, 0, sizeof *ru);
    if (getrusage(RUSAGE_SELF, &h) < 0) return;
    tv_from_host(&ru->ru_utime, h.ru_utime);
    tv_from_host(&ru->ru_stime, h.ru_stime);
    ru->ru_maxrss = h.ru_maxrss / 1024; /* Darwin reports bytes, Linux KiB */
    ru->ru_minflt = h.ru_minflt; ru->ru_majflt = h.ru_majflt;
    ru->ru_inblock = h.ru_inblock; ru->ru_oublock = h.ru_oublock;
    ru->ru_nvcsw = h.ru_nvcsw; ru->ru_nivcsw = h.ru_nivcsw;
}

/* ru_maxrss: the host process's resident high-water mark (MACH_TASK_BASIC_INFO, which does not walk the
 * threads), sampled at most every 10 ms. getrusage(RUSAGE_SELF) of the host walks every thread of the host
 * process to sum CPU times: RUSAGE_THREAD only needs the calling thread's times (request of the lead, menu
 * profile: a guest thread spins on getrusage(RUSAGE_THREAD)). */
static _Atomic int64_t maxrss_kib, maxrss_ns;
static int64_t host_maxrss_kib(void)
{
    int64_t now = lkd_host_mono_ns(), at = atomic_load(&maxrss_ns);
    if (at && now - at < 10000000) return atomic_load(&maxrss_kib);
    mach_task_basic_info_data_t bi;
    mach_msg_type_number_t cnt = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&bi, &cnt) == KERN_SUCCESS) {
        int64_t v = (int64_t)(bi.resident_size_max / 1024);
        if (v > atomic_load(&maxrss_kib)) atomic_store(&maxrss_kib, v);
        atomic_store(&maxrss_ns, now);
    }
    return atomic_load(&maxrss_kib);
}

static void thread_rusage(struct lx_rusage *ru)
{
    memset(ru, 0, sizeof *ru);
    thread_basic_info_data_t bi;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) == KERN_SUCCESS) {
        ru->ru_utime.tv_sec = bi.user_time.seconds; ru->ru_utime.tv_usec = bi.user_time.microseconds;
        ru->ru_stime.tv_sec = bi.system_time.seconds; ru->ru_stime.tv_usec = bi.system_time.microseconds;
    }
    ru->ru_maxrss = host_maxrss_kib(); /* RUSAGE_THREAD reports the process maxrss */
}

static void children_rusage(lkd_task *t, struct lx_rusage *ru)
{
    memset(ru, 0, sizeof *ru);
    fam_lock();
    struct fam_proc *r = t->proc->wait ? t->proc->wait->rec : NULL;
    if (r && r->info.pid == t->proc->pid) *ru = r->cru;
    fam_unlock();
}

LKD_SYSCALL(getrusage)
{
    int who = LKD_ARGI(0);
    struct lx_rusage ru;
    if (who == LX_RUSAGE_SELF) wait_rusage_now(&ru);
    else if (who == LX_RUSAGE_THREAD) thread_rusage(&ru);
    else if (who == LX_RUSAGE_CHILDREN) children_rusage(t, &ru);
    else return -LX_EINVAL;
    return lkd_user_write(LKD_ARGP(1), &ru, sizeof ru) ? -LX_EFAULT : 0;
}

#define USER_HZ 100
#define KERNEL_HZ 250

static int64_t tv_ticks(const struct lx_timeval *tv) { return tv->tv_sec * USER_HZ + tv->tv_usec / (1000000 / USER_HZ); }

LKD_SYSCALL(times)
{
    if (LKD_ARGP(0)) {
        struct lx_rusage self, kids;
        wait_rusage_now(&self);
        children_rusage(t, &kids);
        struct lx_tms tm = { tv_ticks(&self.ru_utime), tv_ticks(&self.ru_stime), tv_ticks(&kids.ru_utime), tv_ticks(&kids.ru_stime) };
        if (lkd_user_write(LKD_ARGP(0), &tm, sizeof tm)) return -LX_EFAULT;
    }
    /* jiffies_64_to_clock_t(get_jiffies_64()): jiffies start at INITIAL_JIFFIES (-300 s, 32-bit wrap)
     * and count while the machine is awake (CLOCK_MONOTONIC). */
    uint64_t mono = (uint64_t)lx_clock_now_ns(LX_CLOCK_MONOTONIC);
    uint64_t jiffies = (uint64_t)(uint32_t)(-300 * KERNEL_HZ) + mono / (1000000000ull / KERNEL_HZ);
    return (int64_t)(jiffies * USER_HZ / KERNEL_HZ);
}

LKD_SYSCALL(sysinfo)
{
    struct lx_sysinfo si;
    memset(&si, 0, sizeof si);
    int64_t boot = lx_clock_now_ns(LX_CLOCK_BOOTTIME);
    si.uptime = boot / 1000000000ll + (boot % 1000000000ll ? 1 : 0); /* do_sysinfo rounds up */
    double la[3] = { 0, 0, 0 };
    if (getloadavg(la, 3) == 3)
        for (int i = 0; i < 3; i++) si.loads[i] = (uint64_t)(la[i] * 65536.0); /* SI_LOAD_SHIFT */
    uint64_t memsize = 0;
    size_t len = sizeof memsize;
    sysctlbyname("hw.memsize", &memsize, &len, NULL, 0);
    si.totalram = memsize;
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) == KERN_SUCCESS)
        si.freeram = (uint64_t)vs.free_count * vm_page_size;
    struct xsw_usage sw;
    len = sizeof sw;
    if (sysctlbyname("vm.swapusage", &sw, &len, NULL, 0) == 0) { si.totalswap = sw.xsu_total; si.freeswap = sw.xsu_avail; }
    int threads = 0;
    fam_lock();
    for (int i = 0; i < FAM_MAX_TIDS; i++) if (fam_tid_at(i)->info.tid) threads++;
    fam_unlock();
    si.procs = (uint16_t)threads; /* nr_threads of the guest system (the family) */
    si.mem_unit = 1;
    return lkd_user_write(LKD_ARGP(0), &si, sizeof si) ? -LX_EFAULT : 0;
}

/* ---- scheduling ---------------------------------------------------------------------------------- */

static int ncpus(void)
{
    int n = 1;
    size_t len = sizeof n;
    sysctlbyname("hw.ncpu", &n, &len, NULL, 0);
    return n < 1 ? 1 : n;
}

static uint64_t online_mask(void) { int n = ncpus(); return n >= 64 ? ~0ull : (1ull << n) - 1; }

/* Find a thread for the sched calls. Returns a local task (counted) or NULL with *remote filled. */
static lkd_task *find_target(lkd_task *t, int32_t pid, struct fam_tid *remote, int *err)
{
    *err = 0;
    if (pid < 0) { *err = -LX_EINVAL; return NULL; }
    if (pid == 0 || pid == t->tid) return lkd_task_get(t);
    lkd_task *x = lkd_task_lookup(pid);
    if (x) return x;
    fam_lock();
    struct fam_tid *r = fam_tid_find(pid);
    if (r) *remote = *r;
    fam_unlock();
    if (!r) *err = -LX_ESRCH;
    return NULL;
}

LKD_SYSCALL(sched_getaffinity)
{
    uint64_t len = LKD_ARGU(1);
    if (len * 8 < (uint64_t)ncpus()) return -LX_EINVAL;
    if (len & 7) return -LX_EINVAL;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    uint64_t mask = x ? x->affinity : remote.affinity;
    if (x) lkd_task_put(x);
    mask &= online_mask();
    return lkd_user_write(LKD_ARGP(2), &mask, sizeof mask) ? -LX_EFAULT : (int64_t)sizeof mask;
}

static int set_affinity_local(lkd_task *x, uint64_t mask)
{
    if (!(mask & online_mask())) return -LX_EINVAL;
    /* Stored and reported; macOS offers no thread placement API, so it is not enforced. */
    x->affinity = mask & online_mask();
    lkd_registry_publish(x);
    return 0;
}

/* TASK_OP_SET_AFFINITY: the mask of a thread of the receiver, from a caller in another host process. wineserver
 * gives a Windows thread's SetThreadAffinityMask to the game's thread this way, and a program such as Wwise gives up
 * on a thread whose mask cannot be set. */
struct affinity_req { int32_t tid; int32_t pad; uint64_t mask; };

static void op_set_affinity(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    struct affinity_req a;
    if (req->len != sizeof a) { resp->status = -LX_EPROTO; return; }
    memcpy(&a, req->data, sizeof a);
    lkd_task *x = a.tid > 0 ? lkd_task_lookup(a.tid) : NULL;
    if (!x) { resp->status = -LX_ESRCH; return; }
    resp->status = set_affinity_local(x, a.mask);
    lkd_task_put(x);
}

LKD_SYSCALL(sched_setaffinity)
{
    uint64_t len = LKD_ARGU(1), mask = 0;
    /* get_user_cpu_mask(): shorter masks are zero-extended, longer ones truncated */
    if (lkd_user_read(LKD_ARGP(2), &mask, len < 8 ? len : 8)) return -LX_EFAULT;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    if (x) {
        int r = set_affinity_local(x, mask);
        lkd_task_put(x);
        return r;
    }
    /* A thread of another host process: the process that owns it sets the mask and publishes it. */
    if (!(mask & online_mask())) return -LX_EINVAL;
    struct affinity_req a = { remote.info.tid, 0, mask };
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = TASK_OP_SET_AFFINITY;
    req.target_pid = remote.info.tgid;
    req.target_tid = remote.info.tid;
    req.data = &a;
    req.len = sizeof a;
    int r = lkd_famsvc_call_pid(remote.info.tgid, &req, &resp, 0);
    lkd_famsvc_msg_free(&resp);
    return r;
}

LKD_SYSCALL(sched_yield) { sched_yield(); return 0; }

static int valid_policy(int pol)
{
    return pol == LX_SCHED_OTHER || pol == LX_SCHED_FIFO || pol == LX_SCHED_RR || pol == LX_SCHED_BATCH ||
           pol == LX_SCHED_IDLE || pol == LX_SCHED_DEADLINE;
}

#define SCHED_RESET_ON_FORK 0x40000000

static int64_t set_sched(lkd_task *t, int32_t pid, int policy, int prio, int keep_policy)
{
    if (pid < 0) return -LX_EINVAL;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, pid, &remote, &err);
    if (err) return err;
    if (!x) {
        lkd_log_once("sched_setscheduler on a thread of another host process is not supported");
        return -LX_ENOSYS;
    }
    if (keep_policy) policy = x->sched_policy;
    policy &= ~SCHED_RESET_ON_FORK;
    int64_t r = 0;
    if (!valid_policy(policy) || policy == LX_SCHED_DEADLINE) r = -LX_EINVAL;
    else if ((policy == LX_SCHED_FIFO || policy == LX_SCHED_RR) ? (prio < 1 || prio > 99) : prio != 0) r = -LX_EINVAL;
    else if (policy == LX_SCHED_FIFO || policy == LX_SCHED_RR) {
        /* user_check_sched_setscheduler(): needs RLIMIT_RTPRIO >= prio without CAP_SYS_NICE */
        if ((uint64_t)prio > t->proc->rlimits[LX_RLIMIT_RTPRIO].rlim_cur) r = -LX_EPERM;
    }
    if (!r) { x->sched_policy = policy; x->sched_priority = prio; }
    lkd_task_put(x);
    return r;
}

LKD_SYSCALL(sched_setscheduler)
{
    int policy = LKD_ARGI(1);
    if (!LKD_ARGP(2) || LKD_ARGI(0) < 0) return -LX_EINVAL;
    struct lx_sched_param sp;
    if (lkd_user_read(LKD_ARGP(2), &sp, sizeof sp)) return -LX_EFAULT;
    if (policy < 0) return -LX_EINVAL;
    return set_sched(t, LKD_ARGI(0), policy, sp.sched_priority, 0);
}

LKD_SYSCALL(sched_setparam)
{
    if (!LKD_ARGP(1) || LKD_ARGI(0) < 0) return -LX_EINVAL;
    struct lx_sched_param sp;
    if (lkd_user_read(LKD_ARGP(1), &sp, sizeof sp)) return -LX_EFAULT;
    return set_sched(t, LKD_ARGI(0), 0, sp.sched_priority, 1);
}

LKD_SYSCALL(sched_getscheduler)
{
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    int pol = x ? x->sched_policy : LX_SCHED_OTHER; /* remote threads: only their own changes exist */
    if (x) lkd_task_put(x);
    return pol;
}

LKD_SYSCALL(sched_getparam)
{
    if (!LKD_ARGP(1) || LKD_ARGI(0) < 0) return -LX_EINVAL;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    struct lx_sched_param sp = { x ? x->sched_priority : 0 };
    if (x) lkd_task_put(x);
    return lkd_user_write(LKD_ARGP(1), &sp, sizeof sp) ? -LX_EFAULT : 0;
}

LKD_SYSCALL(sched_get_priority_max)
{
    int p = LKD_ARGI(0);
    if (p == LX_SCHED_FIFO || p == LX_SCHED_RR) return 99;
    if (p == LX_SCHED_OTHER || p == LX_SCHED_BATCH || p == LX_SCHED_IDLE || p == LX_SCHED_DEADLINE) return 0;
    return -LX_EINVAL;
}

LKD_SYSCALL(sched_get_priority_min)
{
    int p = LKD_ARGI(0);
    if (p == LX_SCHED_FIFO || p == LX_SCHED_RR) return 1;
    if (p == LX_SCHED_OTHER || p == LX_SCHED_BATCH || p == LX_SCHED_IDLE || p == LX_SCHED_DEADLINE) return 0;
    return -LX_EINVAL;
}

LKD_SYSCALL(sched_rr_get_interval)
{
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    int pol = x ? x->sched_policy : LX_SCHED_OTHER;
    if (x) lkd_task_put(x);
    /* RR: RR_TIMESLICE (100 ms). FIFO: 0. Fair class: NS_TO_JIFFIES(base slice 3.75 ms) is 0 at
     * HZ=250, so 0 as well (INFERE: depends on the modeled HZ). */
    struct lx_timespec ts = { 0, pol == LX_SCHED_RR ? 100000000 : 0 };
    return lkd_user_write(LKD_ARGP(1), &ts, sizeof ts) ? -LX_EFAULT : 0;
}

struct lx_sched_attr {
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
#define SCHED_ATTR_SIZE_VER0 48u

LKD_SYSCALL(sched_getattr)
{
    uint32_t usize = LKD_ARGUI(2);
    if (!LKD_ARGP(1) || usize > 4096 || usize < SCHED_ATTR_SIZE_VER0 || LKD_ARGUI(3) || LKD_ARGI(0) < 0) return -LX_EINVAL;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    struct lx_sched_attr a;
    memset(&a, 0, sizeof a);
    a.sched_policy = x ? (uint32_t)x->sched_policy : 0;
    a.sched_nice = x ? x->nice : remote.nice;
    a.sched_priority = x ? (uint32_t)x->sched_priority : 0;
    a.sched_util_max = 1024; /* SCHED_CAPACITY_SCALE: uclamp default */
    if (x) lkd_task_put(x);
    a.size = usize < sizeof a ? usize : (uint32_t)sizeof a;
    return lkd_user_write(LKD_ARGP(1), &a, a.size) ? -LX_EFAULT : 0;
}

static int can_nice(lkd_task *t, int nice)
{
    /* can_nice(): 20 - nice must be within RLIMIT_NICE */
    return (uint64_t)(20 - nice) <= t->proc->rlimits[LX_RLIMIT_NICE].rlim_cur;
}

LKD_SYSCALL(sched_setattr)
{
    if (!LKD_ARGP(1) || LKD_ARGI(0) < 0 || LKD_ARGUI(2)) return -LX_EINVAL;
    struct lx_sched_attr a;
    memset(&a, 0, sizeof a);
    uint32_t size;
    if (lkd_user_read(LKD_ARGP(1), &size, 4)) return -LX_EFAULT;
    if (!size) size = SCHED_ATTR_SIZE_VER0;
    if (size < SCHED_ATTR_SIZE_VER0 || size > 4096) {
        uint32_t want = sizeof a;
        lkd_user_write(LKD_ARGP(1), &want, 4);
        return -LX_E2BIG;
    }
    if (lkd_user_read(LKD_ARGP(1), &a, size < sizeof a ? size : sizeof a)) return -LX_EFAULT;
    int pol = (int)a.sched_policy;
    if (!valid_policy(pol)) return -LX_EINVAL;
    if (pol == LX_SCHED_DEADLINE) return -LX_EPERM; /* needs CAP_SYS_NICE */
    if (a.sched_flags & ~0x7full) return -LX_EINVAL;
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, LKD_ARGI(0), &remote, &err);
    if (err) return err;
    if (!x) {
        lkd_log_once("sched_setattr on a thread of another host process is not supported");
        return -LX_ENOSYS;
    }
    int64_t r;
    if (pol == LX_SCHED_FIFO || pol == LX_SCHED_RR) {
        lkd_task_put(x);
        return set_sched(t, LKD_ARGI(0), pol, (int)a.sched_priority, 0);
    }
    int nice = a.sched_nice < -20 ? -20 : a.sched_nice > 19 ? 19 : a.sched_nice;
    if (a.sched_priority != 0) r = -LX_EINVAL;
    else if (nice < x->nice && !can_nice(t, nice)) r = -LX_EPERM;
    else { x->sched_policy = pol; x->sched_priority = 0; x->nice = nice; r = 0; }
    lkd_task_put(x);
    return r;
}

#define PRIO_PROCESS 0
#define PRIO_PGRP 1
#define PRIO_USER 2

LKD_SYSCALL(getpriority)
{
    int which = LKD_ARGI(0);
    int32_t who = LKD_ARGI(1);
    if (which < PRIO_PROCESS || which > PRIO_USER) return -LX_EINVAL;
    if (which == PRIO_PROCESS) {
        struct fam_tid remote;
        int err;
        lkd_task *x = find_target(t, who, &remote, &err);
        if (err) return err == -LX_EINVAL ? -LX_ESRCH : err;
        int nice = x ? x->nice : remote.nice;
        if (x) lkd_task_put(x);
        return 20 - nice; /* the syscall returns 20 - nice so that it is never negative */
    }
    lkd_proc_refresh_ids(t->proc);
    if (!who) who = which == PRIO_PGRP ? t->proc->pgid : (int32_t)t->proc->cred.uid;
    int best = -1;
    fam_lock();
    for (int i = 0; i < FAM_MAX_TIDS; i++) {
        struct fam_tid *tr = fam_tid_at(i);
        if (!tr->info.tid) continue;
        struct fam_proc *p = fam_proc_find(tr->info.tgid);
        if (!p || (atomic_load(&p->flags) & RF_ZOMBIE)) continue;
        int match = which == PRIO_PGRP ? p->info.pgid == who : (int32_t)p->info.uid == who;
        if (match && 20 - tr->nice > best) best = 20 - tr->nice;
    }
    fam_unlock();
    return best < 0 ? -LX_ESRCH : best;
}

LKD_SYSCALL(setpriority)
{
    int which = LKD_ARGI(0);
    int32_t who = LKD_ARGI(1);
    int nice = LKD_ARGI(2);
    if (which < PRIO_PROCESS || which > PRIO_USER) return -LX_EINVAL;
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    if (which != PRIO_PROCESS) {
        lkd_log_once("setpriority(PRIO_PGRP/PRIO_USER) is not supported");
        return -LX_ENOSYS;
    }
    struct fam_tid remote;
    int err;
    lkd_task *x = find_target(t, who, &remote, &err);
    if (err) return err == -LX_EINVAL ? -LX_ESRCH : err;
    if (!x) {
        lkd_log_once("setpriority on a thread of another host process is not supported");
        return -LX_ENOSYS;
    }
    int64_t r = 0;
    if (nice < x->nice && !can_nice(t, nice)) r = -LX_EACCES; /* set_one_prio() */
    else x->nice = nice; /* recorded; not applied to the host thread (no per-thread nice on Darwin) */
    if (!r) lkd_registry_publish(x);
    lkd_task_put(x);
    return r;
}

LKD_SYSCALL(getcpu)
{
    size_t cpu = 0;
    if (pthread_cpu_number_np(&cpu) != 0) cpu = 0;
    uint32_t c = (uint32_t)cpu, node = 0;
    if (LKD_ARGP(0) && lkd_user_write(LKD_ARGP(0), &c, 4)) return -LX_EFAULT;
    if (LKD_ARGP(1) && lkd_user_write(LKD_ARGP(1), &node, 4)) return -LX_EFAULT;
    return 0;
}

/* ---- getrandom ----------------------------------------------------------------------------------- */

LKD_SYSCALL(getrandom)
{
    uint64_t buf = LKD_ARGP(0), len = LKD_ARGU(1);
    unsigned flags = LKD_ARGUI(2);
    if (flags & ~(unsigned)(LX_GRND_NONBLOCK | LX_GRND_RANDOM | LX_GRND_INSECURE)) return -LX_EINVAL;
    if ((flags & (LX_GRND_INSECURE | LX_GRND_RANDOM)) == (LX_GRND_INSECURE | LX_GRND_RANDOM)) return -LX_EINVAL;
    if (len > 0x7ffff000) len = 0x7ffff000; /* MAX_RW_COUNT */
    uint8_t chunk[256];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done < sizeof chunk ? (size_t)(len - done) : sizeof chunk;
        arc4random_buf(chunk, n); /* the host CSPRNG, always seeded */
        if (lkd_user_write(buf + done, chunk, n)) { memset(chunk, 0, sizeof chunk); return done ? (int64_t)done : -LX_EFAULT; }
        done += n;
        /* get_random_bytes_user(): a pending signal ends a large request with what was copied */
        if (done < len && (done & 4095) == 0 && lkd_sig_interrupted(t)) break;
    }
    memset(chunk, 0, sizeof chunk);
    return (int64_t)done;
}

/* ---- thread bookkeeping ---------------------------------------------------------------------------- */

LKD_SYSCALL(set_tid_address)
{
    t->clear_child_tid = LKD_ARGP(0);
    return t->tid;
}

LKD_SYSCALL(set_robust_list)
{
    if (LKD_ARGU(1) != sizeof(struct lx_robust_list_head)) return -LX_EINVAL;
    t->robust_list_head = LKD_ARGP(0);
    t->robust_list_len = LKD_ARGU(1);
    return 0;
}

LKD_SYSCALL(get_robust_list)
{
    int32_t pid = LKD_ARGI(0);
    uint64_t head, len;
    if (pid == 0 || pid == t->tid) { head = t->robust_list_head; len = sizeof(struct lx_robust_list_head); }
    else {
        lkd_task *x = lkd_task_lookup(pid);
        int32_t tgid;
        if (x) { head = x->robust_list_head; tgid = x->proc->pid; lkd_task_put(x); }
        else {
            fam_lock();
            struct fam_tid *r = fam_tid_find(pid);
            head = r ? r->robust_head : 0;
            tgid = r ? r->info.tgid : 0;
            fam_unlock();
            if (!tgid) return -LX_ESRCH;
        }
        len = sizeof(struct lx_robust_list_head);
        /* ptrace_may_access(PTRACE_MODE_READ_REALCREDS) */
        if (tgid != t->proc->pid && lkd_ptrace_check_access(t, tgid, LKD_PTRACE_MODE_READ) < 0) return -LX_EPERM;
    }
    if (lkd_user_write(LKD_ARGP(2), &len, sizeof len)) return -LX_EFAULT;
    if (lkd_user_write(LKD_ARGP(1), &head, sizeof head)) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(exit) { return lkd_task_do_exit(t, LKD_ARGI(0)); }

LKD_SYSCALL(exit_group)
{
    /* SYSCALL_DEFINE1(exit_group): do_group_exit((error_code & 0xff) << 8) */
    return lkd_task_do_exit_group(t, (LKD_ARGI(0) & 0xff) << 8);
}

/* ---- arch_prctl ---------------------------------------------------------------------------------- */

#define TASK_SIZE_MAX 0x00007ffffffff000ull
#define ARCH_GET_XCOMP_SUPP 0x1021
#define ARCH_GET_XCOMP_PERM 0x1022
#define ARCH_REQ_XCOMP_PERM 0x1023
#define ARCH_GET_XCOMP_GUEST_PERM 0x1024
#define ARCH_REQ_XCOMP_GUEST_PERM 0x1025
#define ARCH_GET_UNTAG_MASK 0x4001
#define ARCH_ENABLE_TAGGED_ADDR 0x4002
#define ARCH_GET_MAX_TAG_BITS 0x4003
#define ARCH_FORCE_TAGGED_SVA 0x4004
#define ARCH_SHSTK_ENABLE 0x5001
#define ARCH_SHSTK_DISABLE 0x5002
#define ARCH_SHSTK_LOCK 0x5003
#define ARCH_SHSTK_UNLOCK 0x5004
#define ARCH_SHSTK_STATUS 0x5005

LKD_SYSCALL(arch_prctl)
{
    int opt = LKD_ARGI(0);
    uint64_t arg = LKD_ARGU(1);
    const lkd_engine_ops *ops = lkd_engine();
    lkd_regs r;
    switch (opt) {
    case LX_ARCH_SET_FS:
    case LX_ARCH_SET_GS:
        if (arg >= TASK_SIZE_MAX) return -LX_EPERM; /* x86_fsbase_write_task(): must be a user address */
        if (!ops || ops->get_regs(t, &r) < 0) return -LX_ESRCH;
        if (opt == LX_ARCH_SET_FS) { r.fs_base = arg; r.fs = 0; } else { r.gs_base = arg; r.gs = 0; }
        if (ops->set_regs(t, &r) < 0) return -LX_ESRCH;
        if (opt == LX_ARCH_SET_FS) t->start_fs_base = arg; else t->start_gs_base = arg;
        return 0;
    case LX_ARCH_GET_FS:
    case LX_ARCH_GET_GS: {
        if (!ops || ops->get_regs(t, &r) < 0) return -LX_ESRCH;
        uint64_t v = opt == LX_ARCH_GET_FS ? r.fs_base : r.gs_base;
        return lkd_user_write(arg, &v, sizeof v) ? -LX_EFAULT : 0;
    }
    case LX_ARCH_GET_CPUID:
        return 1; /* CPUID is not faulting */
    case LX_ARCH_SET_CPUID:
        return -LX_ENODEV; /* no CPUID faulting support in the engine */
    case ARCH_GET_UNTAG_MASK: {
        uint64_t m = ~0ull; /* no LAM: every bit is significant */
        return lkd_user_write(arg, &m, sizeof m) ? -LX_EFAULT : 0;
    }
    case ARCH_GET_MAX_TAG_BITS: {
        uint64_t z = 0;
        return lkd_user_write(arg, &z, sizeof z) ? -LX_EFAULT : 0;
    }
    case ARCH_ENABLE_TAGGED_ADDR:
        return -LX_ENODEV; /* no X86_FEATURE_LAM */
    case ARCH_FORCE_TAGGED_SVA:
        return -LX_EINVAL;
    case ARCH_SHSTK_STATUS: {
        uint64_t z = 0; /* no user shadow stack feature enabled */
        return lkd_user_write(arg, &z, sizeof z) ? -LX_EFAULT : 0;
    }
    case ARCH_SHSTK_LOCK:
        return 0;
    case ARCH_SHSTK_UNLOCK:
        return -LX_EINVAL; /* only through ptrace */
    case ARCH_SHSTK_ENABLE:
    case ARCH_SHSTK_DISABLE:
        if (__builtin_popcountll(arg) > 1) return -LX_EINVAL;
        if (arg & 3) return -LX_EOPNOTSUPP; /* !cpu_feature_enabled(X86_FEATURE_USER_SHSTK) */
        return -LX_EINVAL;
    case ARCH_GET_XCOMP_SUPP:
    case ARCH_GET_XCOMP_PERM:
    case ARCH_REQ_XCOMP_PERM:
    case ARCH_GET_XCOMP_GUEST_PERM:
    case ARCH_REQ_XCOMP_GUEST_PERM:
        /* The supported XSAVE feature set belongs to the engine; no op reports it yet. */
        lkd_log_once("arch_prctl(0x%x) needs the engine's XSAVE feature set: -EINVAL", opt);
        return -LX_EINVAL;
    default:
        return -LX_EINVAL; /* includes ARCH_CET_STATUS (never merged upstream) and ARCH_MAP_VDSO_* */
    }
}

/* ---- prctl --------------------------------------------------------------------------------------- */

#define PR_GET_UNALIGN 5
#define PR_SET_UNALIGN 6
#define PR_GET_FPEMU 9
#define PR_SET_FPEMU 10
#define PR_GET_FPEXC 11
#define PR_SET_FPEXC 12
#define PR_GET_TIMING 13
#define PR_SET_TIMING 14
#define PR_GET_ENDIAN 19
#define PR_SET_ENDIAN 20
#define PR_GET_TSC 25
#define PR_SET_TSC 26
#define PR_TASK_PERF_EVENTS_DISABLE 31
#define PR_TASK_PERF_EVENTS_ENABLE 32
#define PR_MCE_KILL 33
#define PR_MCE_KILL_GET 34
#define PR_SET_MM 35
#define PR_GET_SPECULATION_CTRL 52
#define PR_SET_SPECULATION_CTRL 53
#define PR_SET_IO_FLUSHER 57
#define PR_GET_IO_FLUSHER 58
#define PR_SET_SYSCALL_USER_DISPATCH 59
#define PR_SCHED_CORE 62
#define PR_SET_MEMORY_MERGE 67
#define PR_GET_MEMORY_MERGE 68
#define PR_GET_AUXV 0x41555856
#define PR_SET_PTRACER 0x59616d61
#define PR_SET_VMA_ANON_NAME 0
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#define CAP_LAST_CAP 40
#define PR_TSC_ENABLE 1
#define PR_TSC_SIGSEGV 2

LKD_SYSCALL(prctl)
{
    int opt = LKD_ARGI(0);
    uint64_t a2 = LKD_ARGU(1), a3 = LKD_ARGU(2), a4 = LKD_ARGU(3), a5 = LKD_ARGU(4);
    struct lkd_proc *p = t->proc;
    struct lkd_waitstate *w = p->wait;
    switch (opt) {
    case LX_PR_SET_PDEATHSIG: {
        if (a2 > LX_NSIG) return -LX_EINVAL; /* valid_signal() */
        t->pdeathsig = (int32_t)a2;
        fam_lock();
        if (w->rec && w->rec->info.pid == p->pid) w->rec->pdeathsig = (int32_t)a2;
        fam_unlock();
        return 0;
    }
    case LX_PR_GET_PDEATHSIG: {
        int32_t v = t->pdeathsig;
        return lkd_user_write(a2, &v, sizeof v) ? -LX_EFAULT : 0;
    }
    case LX_PR_GET_DUMPABLE:
        return p->dumpable;
    case LX_PR_SET_DUMPABLE:
        if (a2 != 0 && a2 != 1) return -LX_EINVAL; /* SUID_DUMP_ROOT (2) is not settable */
        p->dumpable = (uint8_t)a2;
        lkd_registry_publish(t);
        return 0;
    case PR_GET_UNALIGN: case PR_SET_UNALIGN: case PR_GET_FPEMU: case PR_SET_FPEMU:
    case PR_GET_FPEXC: case PR_SET_FPEXC: case PR_GET_ENDIAN: case PR_SET_ENDIAN:
        return -LX_EINVAL; /* not defined on x86 */
    case LX_PR_GET_KEEPCAPS:
        return w->keepcaps;
    case LX_PR_SET_KEEPCAPS:
        if (a2 > 1) return -LX_EINVAL;
        w->keepcaps = (int)a2;
        return 0;
    case PR_GET_TIMING:
        return 0; /* PR_TIMING_STATISTICAL */
    case PR_SET_TIMING:
        return a2 == 0 ? 0 : -LX_EINVAL;
    case LX_PR_SET_NAME: {
        char name[LKD_COMM_LEN];
        memset(name, 0, sizeof name);
        int64_t n = lkd_user_strn(a2, name, sizeof name);
        if (n == -LX_EFAULT) return -LX_EFAULT;
        name[LKD_COMM_LEN - 1] = 0; /* strncpy_from_user of 15 bytes: longer names are truncated */
        memcpy(t->comm, name, LKD_COMM_LEN);
        lkd_registry_publish(t);
        return 0;
    }
    case LX_PR_GET_NAME:
        return lkd_user_write(a2, t->comm, LKD_COMM_LEN) ? -LX_EFAULT : 0;
    case LX_PR_GET_SECCOMP:
        return task_seccomp_prctl_get(t);
    case LX_PR_SET_SECCOMP:
        return task_seccomp_prctl_set(t, a2, a3);
    case LX_PR_CAPBSET_READ:
        if (a2 > CAP_LAST_CAP) return -LX_EINVAL;
        return (p->cred.cap_bset >> a2) & 1;
    case LX_PR_CAPBSET_DROP:
        if (a2 > CAP_LAST_CAP) return -LX_EINVAL;
        return -LX_EPERM; /* needs CAP_SETPCAP */
    case PR_GET_TSC: {
        int32_t v = PR_TSC_ENABLE;
        return lkd_user_write(a2, &v, sizeof v) ? -LX_EFAULT : 0;
    }
    case PR_SET_TSC:
        if (a2 == PR_TSC_ENABLE) return 0;
        if (a2 == PR_TSC_SIGSEGV) {
            lkd_log_once("prctl(PR_SET_TSC, PR_TSC_SIGSEGV): RDTSC trapping is not available in the engine");
            return -LX_EINVAL;
        }
        return -LX_EINVAL;
    case LX_PR_GET_SECUREBITS:
        return p->cred.securebits;
    case LX_PR_SET_SECUREBITS:
        return -LX_EPERM; /* needs CAP_SETPCAP */
    case LX_PR_SET_TIMERSLACK:
        w->timerslack_ns = a2 ? a2 : 50000; /* 0 resets to the default */
        return 0;
    case LX_PR_GET_TIMERSLACK:
        return (int64_t)w->timerslack_ns;
    case PR_TASK_PERF_EVENTS_DISABLE:
    case PR_TASK_PERF_EVENTS_ENABLE:
        return 0; /* no perf events exist */
    case PR_MCE_KILL:
        if (a4 | a5) return -LX_EINVAL;
        if (a2 == 0) { w->mce_kill = 0; return 0; }   /* PR_MCE_KILL_CLEAR */
        if (a2 == 1 && a3 <= 2) { w->mce_kill = (int)a3 + 1; return 0; } /* PR_MCE_KILL_SET */
        return -LX_EINVAL;
    case PR_MCE_KILL_GET:
        if (a2 | a3 | a4 | a5) return -LX_EINVAL;
        return w->mce_kill ? w->mce_kill - 1 : 2; /* PR_MCE_KILL_DEFAULT */
    case PR_SET_MM:
        return -LX_EPERM; /* CAP_SYS_RESOURCE */
    case LX_PR_SET_CHILD_SUBREAPER:
        p->child_subreaper = a2 ? 1 : 0;
        lkd_registry_publish(t);
        return 0;
    case LX_PR_GET_CHILD_SUBREAPER: {
        int32_t v = p->child_subreaper;
        return lkd_user_write(a2, &v, sizeof v) ? -LX_EFAULT : 0;
    }
    case LX_PR_SET_NO_NEW_PRIVS:
        if (a2 != 1 || a3 || a4 || a5) return -LX_EINVAL;
        p->no_new_privs = 1;
        return 0;
    case LX_PR_GET_NO_NEW_PRIVS:
        if (a2 || a3 || a4 || a5) return -LX_EINVAL;
        return p->no_new_privs;
    case LX_PR_GET_TID_ADDRESS:
        return lkd_user_write(a2, &t->clear_child_tid, sizeof(uint64_t)) ? -LX_EFAULT : 0;
    case LX_PR_SET_THP_DISABLE:
        if (a3 || a4 || a5) return -LX_EINVAL;
        w->thp_disable = a2 ? 1 : 0;
        return 0;
    case LX_PR_GET_THP_DISABLE:
        if (a2 || a3 || a4 || a5) return -LX_EINVAL;
        return w->thp_disable;
    case LX_PR_CAP_AMBIENT:
        if (a2 == PR_CAP_AMBIENT_CLEAR_ALL) {
            if (a3 || a4 || a5) return -LX_EINVAL;
            p->cred.cap_amb = 0;
            return 0;
        }
        if (a4 || a5 || a3 > CAP_LAST_CAP) return -LX_EINVAL;
        if (a2 == PR_CAP_AMBIENT_IS_SET) return (p->cred.cap_amb >> a3) & 1;
        if (a2 == PR_CAP_AMBIENT_LOWER) { p->cred.cap_amb &= ~(1ull << a3); return 0; }
        if (a2 == PR_CAP_AMBIENT_RAISE) {
            if (!((p->cred.cap_perm >> a3) & 1) || !((p->cred.cap_inh >> a3) & 1)) return -LX_EPERM;
            p->cred.cap_amb |= 1ull << a3;
            return 0;
        }
        return -LX_EINVAL;
    case PR_GET_SPECULATION_CTRL:
        if (a3 || a4 || a5) return -LX_EINVAL;
        if (a2 > 2) return -LX_ENODEV;
        return 0; /* PR_SPEC_NOT_AFFECTED: the translated CPU exposes no x86 speculation controls */
    case PR_SET_SPECULATION_CTRL:
        if (a4 || a5) return -LX_EINVAL;
        if (a2 > 2) return -LX_ENODEV;
        return -LX_ENXIO; /* control not possible: not affected */
    case PR_SET_IO_FLUSHER:
    case PR_SET_MEMORY_MERGE:
        return -LX_EPERM; /* CAP_SYS_RESOURCE */
    case PR_GET_IO_FLUSHER:
        return -LX_EPERM; /* CAP_SYS_RESOURCE (also checked for GET) */
    case PR_GET_MEMORY_MERGE:
        return 0;
    case PR_SET_SYSCALL_USER_DISPATCH:
        lkd_log_once("prctl(PR_SET_SYSCALL_USER_DISPATCH) is not supported (no SIGSYS dispatch in LKD)");
        return -LX_EINVAL;
    case PR_SCHED_CORE:
        return -LX_EINVAL;
    case LX_PR_SET_MDWE:
    case LX_PR_GET_MDWE:
        lkd_log_once("prctl(PR_SET_MDWE/PR_GET_MDWE) is not supported");
        return -LX_EINVAL;
    case LX_PR_SET_VMA: {
        if (a2 != PR_SET_VMA_ANON_NAME) return -LX_EINVAL;
        char name[80];
        const char *np = NULL;
        if (a5) {
            int64_t n = lkd_user_strn(a5, name, sizeof name);
            if (n == -LX_EFAULT) return -LX_EFAULT;
            if (n < 0) return -LX_EINVAL; /* ANON_VMA_NAME_MAX_LEN 80 */
            for (int64_t i = 0; i < n; i++)
                if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e || name[i] == '\\' ||
                    name[i] == '`' || name[i] == '$' || name[i] == '[' || name[i] == ']')
                    return -LX_EINVAL;
            np = name;
        }
        return lkd_mm_set_vma_name(t->mm, a3, a4, np ? np : "");
    }
    case PR_GET_AUXV: {
        if (a4 || a5) return -LX_EINVAL;
        pthread_mutex_lock(&p->lock);
        size_t n = p->layout ? p->layout->auxv_len : 0;
        uint8_t *copy = n ? malloc(n) : NULL;
        if (copy) memcpy(copy, p->layout->auxv, n);
        pthread_mutex_unlock(&p->lock);
        if (!n) return -LX_EINVAL; /* the engine has not reported the auxiliary vector (lkd_proc_set_layout) */
        size_t c = a3 < n ? (size_t)a3 : n;
        int64_t r = (c && lkd_user_write(a2, copy, c)) ? -LX_EFAULT : (int64_t)n;
        free(copy);
        return r;
    }
    case PR_SET_PTRACER:
        return -LX_EINVAL; /* modeled kernel without the Yama LSM; see the ptrace module */
    default:
        return -LX_EINVAL;
    }
}

/* ---- seccomp, namespaces, I/O privileges ------------------------------------------------------- */

LKD_SYSCALL(iopl)
{
    unsigned level = LKD_ARGUI(0);
    if (level > 3) return -LX_EINVAL;
    if (level == 0) return 0;
    return -LX_EPERM; /* CAP_SYS_RAWIO */
}

LKD_SYSCALL(ioperm)
{
    uint64_t from = LKD_ARGU(0), num = LKD_ARGU(1);
    if (from + num <= from || from + num > 65536) return -LX_EINVAL; /* IO_BITMAP_BITS */
    if (LKD_ARGI(2)) return -LX_EPERM;
    return 0;
}

#define UNSHARE_OK (LX_CLONE_THREAD | LX_CLONE_FS | LX_CLONE_NEWNS | LX_CLONE_SIGHAND | LX_CLONE_VM | LX_CLONE_FILES | \
                    LX_CLONE_SYSVSEM | LX_CLONE_NEWUTS | LX_CLONE_NEWIPC | LX_CLONE_NEWNET | LX_CLONE_NEWUSER | \
                    LX_CLONE_NEWPID | LX_CLONE_NEWCGROUP | LX_CLONE_NEWTIME)
#define NEW_NS (LX_CLONE_NEWNS | LX_CLONE_NEWUTS | LX_CLONE_NEWIPC | LX_CLONE_NEWNET | LX_CLONE_NEWUSER | \
                LX_CLONE_NEWPID | LX_CLONE_NEWCGROUP | LX_CLONE_NEWTIME)

LKD_SYSCALL(unshare)
{
    uint64_t f = LKD_ARGU(0);
    if (f & ~(uint64_t)UNSHARE_OK) return -LX_EINVAL;
    if (f & (LX_CLONE_THREAD | LX_CLONE_SIGHAND | LX_CLONE_VM)) {
        pthread_mutex_lock(&t->proc->lock);
        int n = t->proc->nthreads;
        pthread_mutex_unlock(&t->proc->lock);
        if (n > 1) return -LX_EINVAL;
    }
    if (f & NEW_NS) {
        /* Namespaces need CAP_SYS_ADMIN; user namespaces are not provided either (documented). */
        return -LX_EPERM;
    }
    if (f & LX_CLONE_FILES) {
        lkd_fdtable *old = t->fdt, *n = lkd_fdtable_clone(old, 0);
        if (old && !n) return -LX_ENOMEM;
        t->fdt = n;
        lkd_fdtable_put(old);
    }
    if (f & LX_CLONE_FS) {
        lkd_fsctx *old = t->fsctx, *n = lkd_fsctx_clone(old, 0);
        if (old && !n) return -LX_ENOMEM;
        t->fsctx = n;
        lkd_fsctx_put(old);
    }
    return 0;
}

LKD_SYSCALL(setns)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    lkd_fdobj_put(o);
    return -LX_EINVAL; /* no namespace file exists in LKD */
}

void task_sys_register(void)
{
    LKD_REGISTER(uname); LKD_REGISTER(sethostname); LKD_REGISTER(setdomainname);
    LKD_REGISTER(getrlimit); LKD_REGISTER(setrlimit); LKD_REGISTER(prlimit64);
    LKD_REGISTER(getrusage); LKD_REGISTER(times); LKD_REGISTER(sysinfo);
    LKD_REGISTER(sched_getaffinity); LKD_REGISTER(sched_setaffinity); LKD_REGISTER(sched_yield);
    LKD_REGISTER(sched_setscheduler); LKD_REGISTER(sched_setparam); LKD_REGISTER(sched_getscheduler);
    LKD_REGISTER(sched_getparam); LKD_REGISTER(sched_get_priority_max); LKD_REGISTER(sched_get_priority_min);
    LKD_REGISTER(sched_rr_get_interval); LKD_REGISTER(sched_getattr); LKD_REGISTER(sched_setattr);
    LKD_REGISTER(getpriority); LKD_REGISTER(setpriority); LKD_REGISTER(getcpu); LKD_REGISTER(getrandom);
    LKD_REGISTER(set_tid_address); LKD_REGISTER(set_robust_list); LKD_REGISTER(get_robust_list);
    LKD_REGISTER(exit); LKD_REGISTER(exit_group); LKD_REGISTER(arch_prctl); LKD_REGISTER(prctl);
    LKD_REGISTER(iopl); LKD_REGISTER(ioperm); LKD_REGISTER(unshare); LKD_REGISTER(setns);
    lkd_famsvc_register(TASK_OP_SET_AFFINITY, op_set_affinity);
}

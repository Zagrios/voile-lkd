/* test_task_ids.c: identity, uname, limits, scheduling, prctl, arch_prctl and thread bookkeeping
 * syscalls of the task module. Each case cites the Linux v6.12 behavior it models. Guest pointers are
 * host pointers (lkd_guest_base == 0, permissive range check). */
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "task.h"

static int allow_all(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }

static lkd_task *T;

static int64_t sc6(unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(T, &rq);
}
#define SC0(n) sc6(LKD_NR_##n, 0, 0, 0, 0, 0, 0)
#define SC1(n, a) sc6(LKD_NR_##n, (uint64_t)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0, 0)
#define SC5(n, a, b, c, d, e) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), 0)
#define P(x) ((uint64_t)(uintptr_t)(x))

/* getpid(2), gettid(2): the main thread's tid is the pid; getppid of the first process is init (1)
 * when nothing else is configured (kernel/sys.c, lkd_main_spec.ppid == 0). */
LT_TEST(pid_tid_ppid)
{
    LT_EQ(SC0(getpid), T->proc->pid);
    LT_EQ(SC0(gettid), T->tid);
    LT_EQ(SC0(gettid), SC0(getpid));
    LT_EQ(SC0(getppid), 1);
    LT_CHECK(T->proc->pid >= 2);   /* never pid 1 unless configured */
    lkd_task *th = fake_make_thread(T);
    lkd_task *save = T;
    T = th;
    LT_EQ(SC0(getpid), save->proc->pid);  /* a thread reports the group id */
    LT_NE(SC0(gettid), save->tid);
    T = save;
}

/* setuid(2) and friends for an unprivileged user (kernel/sys.c __sys_setuid, __sys_setreuid,
 * __sys_setresuid, __sys_setfsuid): only ids already held are allowed. */
LT_TEST(uid_gid_rules)
{
    LT_EQ(SC0(getuid), 1000);
    LT_EQ(SC0(geteuid), 1000);
    LT_EQ(SC0(getgid), 1000);
    LT_EQ(SC0(getegid), 1000);
    LT_ERRNO(SC1(setuid, 0), LX_EPERM);
    LT_ERRNO(SC1(setuid, 0xffffffffu), LX_EINVAL);
    LT_EQ(SC1(setuid, 1000), 0);
    LT_EQ(SC1(setgid, 1000), 0);
    LT_ERRNO(SC2(setreuid, 0, 0xffffffffu), LX_EPERM);
    LT_EQ(SC2(setreuid, 0xffffffffu, 0xffffffffu), 0);
    LT_ERRNO(SC3(setresuid, 0xffffffffu, 0, 0xffffffffu), LX_EPERM);
    LT_EQ(SC3(setresuid, 0xffffffffu, 0xffffffffu, 0xffffffffu), 0);
    LT_EQ(SC3(setresgid, 1000, 1000, 1000), 0);
    uint32_t r = 7, e = 7, s = 7;
    LT_EQ(SC3(getresuid, P(&r), P(&e), P(&s)), 0);
    LT_EQ(r, 1000); LT_EQ(e, 1000); LT_EQ(s, 1000);
    LT_ERRNO(SC3(getresgid, 8, P(&e), P(&s)), LX_EFAULT);
    LT_EQ(SC1(setfsuid, 0), 1000);        /* returns the old fsuid, refuses the change */
    LT_EQ(SC1(setfsuid, 0xffffffffu), 1000); /* the probe idiom: still 1000 */
    LT_EQ(SC1(setfsgid, 0), 1000);
    LT_EQ(SC2(getgroups, 0, 0), 1);
    uint32_t g[4] = { 0 };
    LT_EQ(SC2(getgroups, 4, P(g)), 1);
    LT_EQ(g[0], 1000);
    LT_ERRNO(SC2(getgroups, -1, P(g)), LX_EINVAL);
    LT_ERRNO(SC2(setgroups, 1, P(g)), LX_EPERM);
}

/* capget(2)/capset(2): a non-root task holds no capability; a wrong version is answered with the
 * preferred one (cap_validate_magic) and EINVAL, or 0 when dataptr is NULL. */
LT_TEST(capabilities)
{
    struct lx_cap_header h = { 0x20080522u, 0 };
    struct lx_cap_data d[2];
    memset(d, 0xff, sizeof d);
    LT_EQ(SC2(capget, P(&h), P(d)), 0);
    LT_EQ(d[0].effective, 0); LT_EQ(d[0].permitted, 0); LT_EQ(d[1].inheritable, 0);
    struct lx_cap_header bad = { 0x12345678u, 0 };
    LT_ERRNO(SC2(capget, P(&bad), P(d)), LX_EINVAL);
    LT_EQ(bad.version, 0x20080522u);
    bad.version = 1;
    LT_EQ(SC2(capget, P(&bad), 0), 0);
    memset(d, 0, sizeof d);
    LT_EQ(SC2(capset, P(&h), P(d)), 0);
    d[0].permitted = 1;
    LT_ERRNO(SC2(capset, P(&h), P(d)), LX_EPERM);
    struct lx_cap_header other = { 0x20080522u, 999999 };
    memset(d, 0, sizeof d);
    LT_ERRNO(SC2(capset, P(&other), P(d)), LX_EPERM);
    LT_ERRNO(SC2(capget, P(&other), P(d)), LX_ESRCH);
}

/* personality(2): 0xffffffff queries, any value is stored and the old one returned. */
LT_TEST(personality_query_and_set)
{
    LT_EQ(SC1(personality, 0xffffffffu), 0);
    LT_EQ(SC1(personality, 0x0040000), 0);        /* ADDR_NO_RANDOMIZE */
    LT_EQ(SC1(personality, 0xffffffffu), 0x0040000);
    LT_EQ(SC1(personality, 0), 0x0040000);
}

/* uname(2): sysname Linux, machine x86_64, release of the modeled kernel, hostname of the family. */
LT_TEST(uname_fields)
{
    struct lx_utsname u;
    memset(&u, 'x', sizeof u);
    LT_EQ(SC1(uname, P(&u)), 0);
    LT_STREQ(u.sysname, "Linux");
    LT_STREQ(u.machine, "x86_64");
    LT_STREQ(u.release, "6.12.0-lkd");
    LT_STREQ(u.nodename, "obelisk");
    LT_STREQ(u.domainname, "(none)");
    LT_ERRNO(SC1(uname, 8), LX_EFAULT);
    LT_ERRNO(SC2(sethostname, P("x"), 1), LX_EPERM);   /* CAP_SYS_ADMIN */
    LT_ERRNO(SC2(setdomainname, P("x"), 1), LX_EPERM);
}

/* getrlimit(2)/setrlimit(2)/prlimit64(2): defaults of include/asm-generic/resource.h, soft <= hard,
 * hard limits only go down without CAP_SYS_RESOURCE, nr_open caps NOFILE. */
LT_TEST(rlimits)
{
    struct lx_rlimit r;
    LT_EQ(SC2(getrlimit, LX_RLIMIT_STACK, P(&r)), 0);
    LT_EQ(r.rlim_cur, 8ull << 20);
    LT_EQ(r.rlim_max, LX_RLIM_INFINITY);
    LT_EQ(SC2(getrlimit, LX_RLIMIT_NOFILE, P(&r)), 0);
    LT_EQ(r.rlim_cur, 1024);
    LT_CHECK(r.rlim_max >= 1024);
    LT_ERRNO(SC2(getrlimit, 16, P(&r)), LX_EINVAL);
    struct lx_rlimit n = { 4096, 1ull << 30 };
    LT_ERRNO(SC2(setrlimit, LX_RLIMIT_NOFILE, P(&n)), LX_EPERM);   /* raises the hard limit */
    n.rlim_cur = 512; n.rlim_max = r.rlim_max;
    LT_EQ(SC2(setrlimit, LX_RLIMIT_NOFILE, P(&n)), 0);
    struct lx_rlimit o;
    LT_EQ(SC4(prlimit64, 0, LX_RLIMIT_NOFILE, 0, P(&o)), 0);
    LT_EQ(o.rlim_cur, 512);
    n.rlim_cur = n.rlim_max + 1;
    LT_ERRNO(SC2(setrlimit, LX_RLIMIT_NOFILE, P(&n)), LX_EINVAL);  /* soft above hard */
    n.rlim_cur = 1024;
    LT_EQ(SC4(prlimit64, T->proc->pid, LX_RLIMIT_NOFILE, P(&n), P(&o)), 0);
    LT_EQ(o.rlim_cur, 512);                                       /* old value returned */
    LT_ERRNO(SC4(prlimit64, 0x3ffffff0, LX_RLIMIT_NOFILE, 0, P(&o)), LX_ESRCH);
    struct lx_rlimit lower = { 1ull << 20, 1ull << 20 };
    LT_EQ(SC2(setrlimit, LX_RLIMIT_CORE, P(&lower)), 0);
    struct lx_rlimit up = { 1ull << 21, 1ull << 21 };
    LT_ERRNO(SC2(setrlimit, LX_RLIMIT_CORE, P(&up)), LX_EPERM);
    /* the published limits are visible to the family registry */
    LT_EQ(SC2(getrlimit, LX_RLIMIT_CORE, P(&r)), 0);
    LT_EQ(r.rlim_max, 1ull << 20);
}

/* getrusage(2), times(2), sysinfo(2): real host figures in Linux units. */
LT_TEST(usage_and_sysinfo)
{
    volatile uint64_t x = 0;
    for (int i = 0; i < 20000000; i++) x += (uint64_t)i;
    struct lx_rusage ru;
    LT_EQ(SC2(getrusage, LX_RUSAGE_SELF, P(&ru)), 0);
    LT_CHECK(ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);
    LT_CHECK(ru.ru_maxrss > 100 && ru.ru_maxrss < (64ll << 20)); /* KiB, not bytes */
    LT_EQ(SC2(getrusage, LX_RUSAGE_THREAD, P(&ru)), 0);
    LT_CHECK(ru.ru_utime.tv_sec > 0 || ru.ru_utime.tv_usec > 0);
    LT_EQ(SC2(getrusage, LX_RUSAGE_CHILDREN, P(&ru)), 0);
    LT_ERRNO(SC2(getrusage, -2, P(&ru)), LX_EINVAL);
    struct lx_tms tm;
    int64_t a = SC1(times, P(&tm)), b;
    LT_CHECK(a > 0);
    LT_CHECK(tm.tms_utime >= 0);
    usleep(30000);
    b = SC1(times, 0);
    LT_CHECK(b - a >= 2 && b - a < 100);  /* clock ticks at USER_HZ = 100 */
    struct lx_sysinfo si;
    LT_EQ(SC1(sysinfo, P(&si)), 0);
    uint64_t mem = 0;
    size_t len = sizeof mem;
    sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
    LT_EQ(si.totalram, mem);
    LT_EQ(si.mem_unit, 1);
    LT_CHECK(si.uptime > 0);
    LT_CHECK(si.freeram > 0 && si.freeram < si.totalram);
    LT_CHECK(si.procs >= 1);
}

/* sched_getaffinity(2): len must cover nr_cpu_ids and be a multiple of sizeof(long); returns the
 * cpumask size; the mask is the host's online CPUs. sched_setaffinity rejects an empty mask. */
LT_TEST(affinity_and_sched)
{
    int ncpu = 0;
    size_t len = sizeof ncpu;
    sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);
    uint64_t m = 0;
    LT_EQ(SC3(sched_getaffinity, 0, 8, P(&m)), 8);
    LT_EQ(m, ncpu >= 64 ? ~0ull : (1ull << ncpu) - 1);
    LT_ERRNO(SC3(sched_getaffinity, 0, 4, P(&m)), LX_EINVAL);
    LT_ERRNO(SC3(sched_getaffinity, 0x3ffffff0, 8, P(&m)), LX_ESRCH);
    uint64_t one = 1;
    LT_EQ(SC3(sched_setaffinity, 0, 8, P(&one)), 0);
    LT_EQ(SC3(sched_getaffinity, 0, 8, P(&m)), 8);
    LT_EQ(m, 1);
    uint64_t none = 0;
    LT_ERRNO(SC3(sched_setaffinity, 0, 8, P(&none)), LX_EINVAL);
    uint64_t all = ~0ull;
    LT_EQ(SC3(sched_setaffinity, 0, 8, P(&all)), 0);
    LT_EQ(SC0(sched_yield), 0);
    LT_EQ(SC1(sched_get_priority_max, LX_SCHED_FIFO), 99);
    LT_EQ(SC1(sched_get_priority_min, LX_SCHED_RR), 1);
    LT_EQ(SC1(sched_get_priority_max, LX_SCHED_OTHER), 0);
    LT_ERRNO(SC1(sched_get_priority_max, 4), LX_EINVAL);
    struct lx_sched_param sp = { 10 };
    LT_ERRNO(SC3(sched_setscheduler, 0, LX_SCHED_FIFO, P(&sp)), LX_EPERM); /* RLIMIT_RTPRIO 0 */
    LT_ERRNO(SC3(sched_setscheduler, 0, LX_SCHED_OTHER, P(&sp)), LX_EINVAL); /* priority must be 0 */
    sp.sched_priority = 0;
    LT_EQ(SC3(sched_setscheduler, 0, LX_SCHED_BATCH, P(&sp)), 0);
    LT_EQ(SC1(sched_getscheduler, 0), LX_SCHED_BATCH);
    LT_EQ(SC3(sched_setscheduler, 0, LX_SCHED_OTHER, P(&sp)), 0);
    LT_ERRNO(SC1(sched_getscheduler, -1), LX_EINVAL);
    LT_EQ(SC2(sched_getparam, 0, P(&sp)), 0);
    LT_EQ(sp.sched_priority, 0);
    struct lx_timespec ts = { 9, 9 };
    LT_EQ(SC2(sched_rr_get_interval, 0, P(&ts)), 0);
    LT_EQ(ts.tv_sec, 0);
    uint8_t attr[56];
    memset(attr, 0xff, sizeof attr);
    LT_EQ(SC4(sched_getattr, 0, P(attr), 56, 0), 0);
    uint32_t asz;
    memcpy(&asz, attr, 4);
    LT_EQ(asz, 56);
    LT_ERRNO(SC4(sched_getattr, 0, P(attr), 40, 0), LX_EINVAL);
}

/* getpriority(2) returns 20 - nice; raising priority (lower nice) needs RLIMIT_NICE (set_one_prio:
 * EACCES); lowering it is always allowed. */
LT_TEST(nice_values)
{
    LT_EQ(SC2(getpriority, 0, 0), 20);
    LT_EQ(SC3(setpriority, 0, 0, 5), 0);
    LT_EQ(SC2(getpriority, 0, 0), 15);
    LT_ERRNO(SC3(setpriority, 0, 0, 0), LX_EACCES);
    LT_EQ(SC3(setpriority, 0, 0, 40), 0);               /* clamped to 19 */
    LT_EQ(SC2(getpriority, 0, 0), 1);
    LT_ERRNO(SC2(getpriority, 3, 0), LX_EINVAL);
    LT_ERRNO(SC2(getpriority, 0, 0x3ffffff0), LX_ESRCH);
    uint32_t cpu = 0xffff, node = 0xffff;
    LT_EQ(SC3(getcpu, P(&cpu), P(&node), 0), 0);
    LT_CHECK(cpu < 1024);
    LT_EQ(node, 0);
}

/* getrandom(2): unknown flags and INSECURE|RANDOM are EINVAL, the full length is returned. */
LT_TEST(getrandom_flags_and_content)
{
    uint8_t a[64], b[64];
    memset(a, 0, sizeof a);
    memset(b, 0, sizeof b);
    LT_EQ(SC3(getrandom, P(a), 64, 0), 64);
    LT_EQ(SC3(getrandom, P(b), 64, LX_GRND_NONBLOCK), 64);
    LT_CHECK(memcmp(a, b, 64) != 0);
    int zeros = 0;
    for (int i = 0; i < 64; i++) zeros += a[i] == 0;
    LT_CHECK(zeros < 16);
    LT_ERRNO(SC3(getrandom, P(a), 8, 8), LX_EINVAL);
    LT_ERRNO(SC3(getrandom, P(a), 8, LX_GRND_INSECURE | LX_GRND_RANDOM), LX_EINVAL);
    LT_EQ(SC3(getrandom, P(a), 0, 0), 0);
    LT_ERRNO(SC3(getrandom, 8, 16, 0), LX_EFAULT);
}

/* prctl(2) options of the module's scope; unknown options EINVAL (kernel/sys.c default case). */
LT_TEST(prctl_options)
{
    char name[32];
    LT_EQ(SC2(prctl, LX_PR_SET_NAME, P("abcdefghijklmnopqrstuvwxyz")), 0);
    memset(name, 'z', sizeof name);
    LT_EQ(SC2(prctl, LX_PR_GET_NAME, P(name)), 0);
    LT_STREQ(name, "abcdefghijklmno");                   /* TASK_COMM_LEN 16 */
    LT_EQ(SC2(prctl, LX_PR_SET_NAME, P("wine")), 0);
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup(T->tid, &ti), 0);
    LT_STREQ(ti.comm, "wine");
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(T->proc->pid, &pi), 0);
    LT_STREQ(pi.comm, "wine");
    LT_EQ(SC1(prctl, LX_PR_GET_DUMPABLE), 1);
    LT_EQ(SC2(prctl, LX_PR_SET_DUMPABLE, 0), 0);
    LT_EQ(SC1(prctl, LX_PR_GET_DUMPABLE), 0);
    LT_EQ(lkd_proc_lookup(T->proc->pid, &pi), 0);
    LT_EQ(pi.dumpable, 0);
    LT_ERRNO(SC2(prctl, LX_PR_SET_DUMPABLE, 2), LX_EINVAL);
    LT_EQ(SC2(prctl, LX_PR_SET_DUMPABLE, 1), 0);
    int32_t v = -1;
    LT_ERRNO(SC2(prctl, LX_PR_SET_PDEATHSIG, 65), LX_EINVAL);
    LT_EQ(SC2(prctl, LX_PR_SET_PDEATHSIG, LX_SIGKILL), 0);
    LT_EQ(SC2(prctl, LX_PR_GET_PDEATHSIG, P(&v)), 0);
    LT_EQ(v, LX_SIGKILL);
    LT_EQ(SC2(prctl, LX_PR_SET_PDEATHSIG, 0), 0);
    LT_EQ(SC2(prctl, LX_PR_SET_CHILD_SUBREAPER, 1), 0);
    LT_EQ(SC2(prctl, LX_PR_GET_CHILD_SUBREAPER, P(&v)), 0);
    LT_EQ(v, 1);
    LT_EQ(SC2(prctl, LX_PR_SET_CHILD_SUBREAPER, 0), 0);
    LT_ERRNO(SC2(prctl, LX_PR_SET_NO_NEW_PRIVS, 0), LX_EINVAL);
    LT_EQ(SC1(prctl, LX_PR_GET_NO_NEW_PRIVS), 0);
    LT_EQ(SC2(prctl, LX_PR_SET_NO_NEW_PRIVS, 1), 0);
    LT_EQ(SC1(prctl, LX_PR_GET_NO_NEW_PRIVS), 1);
    LT_ERRNO(SC2(prctl, LX_PR_GET_NO_NEW_PRIVS, 1), LX_EINVAL);
    uint64_t addr = 0x1234;
    T->clear_child_tid = 0xdead0;
    LT_EQ(SC2(prctl, LX_PR_GET_TID_ADDRESS, P(&addr)), 0);
    LT_EQ(addr, 0xdead0);
    T->clear_child_tid = 0;
    LT_EQ(SC1(prctl, LX_PR_GET_SECCOMP), LX_SECCOMP_MODE_DISABLED); /* test_task_seccomp.c sets the modes */
    LT_ERRNO(SC2(prctl, LX_PR_SET_SECCOMP, 3), LX_EINVAL);
    LT_EQ(SC2(prctl, LX_PR_CAPBSET_READ, 0), 1);
    LT_ERRNO(SC2(prctl, LX_PR_CAPBSET_READ, 41), LX_EINVAL);
    LT_ERRNO(SC2(prctl, LX_PR_CAPBSET_DROP, 0), LX_EPERM);
    LT_EQ(SC1(prctl, LX_PR_GET_TIMERSLACK), 50000);
    LT_EQ(SC2(prctl, LX_PR_SET_TIMERSLACK, 1000), 0);
    LT_EQ(SC1(prctl, LX_PR_GET_TIMERSLACK), 1000);
    LT_EQ(SC1(prctl, LX_PR_GET_KEEPCAPS), 0);
    LT_ERRNO(SC1(prctl, 0x7777), LX_EINVAL);
    LT_ERRNO(SC1(prctl, 6 /* PR_SET_UNALIGN */), LX_EINVAL);
    LT_ERRNO(SC2(prctl, 0x41555856 /* PR_GET_AUXV */, P(name)), LX_EINVAL); /* no layout reported */
}

/* arch_prctl(2): FS/GS bases go through the engine registers; an address above TASK_SIZE_MAX is
 * EPERM; ARCH_CET_STATUS (never merged) is EINVAL; CPUID faulting is unavailable (ENODEV). */
LT_TEST(arch_prctl_bases)
{
    LT_EQ(SC2(arch_prctl, LX_ARCH_SET_FS, 0x7f0000001000ull), 0);
    LT_EQ(fake_cpu_of(T)->regs.fs_base, 0x7f0000001000ull);
    uint64_t v = 0;
    LT_EQ(SC2(arch_prctl, LX_ARCH_GET_FS, P(&v)), 0);
    LT_EQ(v, 0x7f0000001000ull);
    LT_EQ(SC2(arch_prctl, LX_ARCH_SET_GS, 0x5000), 0);
    LT_EQ(SC2(arch_prctl, LX_ARCH_GET_GS, P(&v)), 0);
    LT_EQ(v, 0x5000);
    LT_ERRNO(SC2(arch_prctl, LX_ARCH_SET_FS, 0x00007ffffffff000ull), LX_EPERM);
    LT_ERRNO(SC2(arch_prctl, LX_ARCH_CET_STATUS, P(&v)), LX_EINVAL);
    LT_EQ(SC2(arch_prctl, LX_ARCH_GET_CPUID, 0), 1);
    LT_ERRNO(SC2(arch_prctl, LX_ARCH_SET_CPUID, 0), LX_ENODEV);
    LT_ERRNO(SC2(arch_prctl, LX_ARCH_GET_FS, 8), LX_EFAULT);
    LT_ERRNO(SC2(arch_prctl, 0x9999, 0), LX_EINVAL);
    fake_cpu_of(T)->running = 1;                     /* an engine that cannot read the registers */
    LT_ERRNO(SC2(arch_prctl, LX_ARCH_GET_FS, P(&v)), LX_ESRCH);
    fake_cpu_of(T)->running = 0;
}

/* set_tid_address(2) returns the tid; set_robust_list(2) wants exactly sizeof(struct
 * robust_list_head) (24); get_robust_list(2) returns what was set. */
LT_TEST(tid_address_and_robust_list)
{
    uint32_t word = 5;
    LT_EQ(SC1(set_tid_address, P(&word)), T->tid);
    LT_EQ(T->clear_child_tid, P(&word));
    T->clear_child_tid = 0;
    struct lx_robust_list_head head;
    LT_ERRNO(SC2(set_robust_list, P(&head), 23), LX_EINVAL);
    LT_EQ(SC2(set_robust_list, P(&head), 24), 0);
    uint64_t h = 0, l = 0;
    LT_EQ(SC3(get_robust_list, 0, P(&h), P(&l)), 0);
    LT_EQ(h, P(&head));
    LT_EQ(l, 24);
    LT_ERRNO(SC3(get_robust_list, 0x3ffffff0, P(&h), P(&l)), LX_ESRCH);
    T->robust_list_head = 0;
}

/* iopl(2)/ioperm(2) (arch/x86/kernel/ioport.c), unshare(2), setns(2) for a task without capabilities. */
LT_TEST(privileged_calls)
{
    LT_EQ(SC1(iopl, 0), 0);
    LT_ERRNO(SC1(iopl, 3), LX_EPERM);
    LT_ERRNO(SC1(iopl, 4), LX_EINVAL);
    LT_ERRNO(SC3(ioperm, 0x3f8, 8, 1), LX_EPERM);
    LT_EQ(SC3(ioperm, 0x3f8, 8, 0), 0);
    LT_ERRNO(SC3(ioperm, 0, 65537, 0), LX_EINVAL);
    LT_EQ(SC1(unshare, 0), 0);
    LT_ERRNO(SC1(unshare, 0x11), LX_EINVAL);            /* CSIGNAL bits are not unshare flags */
    LT_ERRNO(SC1(unshare, LX_CLONE_NEWUSER), LX_EPERM);
    LT_ERRNO(SC1(unshare, LX_CLONE_NEWNS), LX_EPERM);
    LT_ERRNO(SC2(setns, 9999, 0), LX_EBADF);
}

/* setsid(2) fails for a process-group leader; getsid/getpgid of 0 are the caller's; setpgid(2) of a
 * session leader is EPERM; a negative pgid is EINVAL. */
LT_TEST(sessions_and_groups)
{
    int32_t me = T->proc->pid;
    LT_EQ(SC0(getpgrp), me);
    LT_EQ(SC1(getpgid, 0), me);
    LT_EQ(SC1(getsid, 0), me);
    LT_EQ(SC1(getsid, me), me);
    LT_ERRNO(SC1(getsid, 0x3ffffff0), LX_ESRCH);
    LT_ERRNO(SC0(setsid), LX_EPERM);
    LT_ERRNO(SC2(setpgid, 0, 0), LX_EPERM);             /* session leader */
    LT_ERRNO(SC2(setpgid, 0, -1), LX_EINVAL);
    LT_ERRNO(SC2(setpgid, 0x3ffffff0, 0), LX_ESRCH);
}

/* exit(2) and exit_group(2) encode (code & 0xff) << 8 and end the thread / the process. */
LT_TEST(exit_dispositions)
{
    lkd_task *th = fake_make_thread(T);
    lkd_task *save = T;
    T = th;
    LT_EQ(SC1(exit, 0x1ff), LKD_RET_EXIT_THREAD);
    LT_EQ(th->exit_code, 0xff00);
    T = save;
    LT_EQ(lkd_task_release(th, NULL), 0);                /* not the last thread */
    int32_t tids[8];
    LT_EQ(lkd_tid_list(T->proc->pid, tids, 8) >= 1, 1);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    lkd_usermem_set_check_fn(allow_all);
    T = fake_make_task();
    LT_RUN(pid_tid_ppid);
    LT_RUN(uid_gid_rules);
    LT_RUN(capabilities);
    LT_RUN(personality_query_and_set);
    LT_RUN(uname_fields);
    LT_RUN(rlimits);
    LT_RUN(usage_and_sysinfo);
    LT_RUN(affinity_and_sched);
    LT_RUN(nice_values);
    LT_RUN(getrandom_flags_and_content);
    LT_RUN(prctl_options);
    LT_RUN(arch_prctl_bases);
    LT_RUN(tid_address_and_robust_list);
    LT_RUN(privileged_calls);
    LT_RUN(sessions_and_groups);
    LT_RUN(exit_dispositions);
    return lt_finish();
}

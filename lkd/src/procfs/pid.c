/* pid.c: the files of /proc/<pid> and /proc/<pid>/task/<tid>.
 *
 * Every value comes from the runtime state of the task: the task module's structures (credentials,
 * rlimits, layout, comm), the mm module (VMAs, Vm* counters), the sig module (signal sets), the fs
 * module (descriptors, cwd, root), the ptrace module (TracerPid, access rules). A process that lives in
 * another host process of the family is rendered by THAT process (famsvc, remote.c) with its own local
 * state; the viewer only decides the access rules (ptrace_may_access) and sends the verdict.
 *
 * Linux references (v6.12): fs/proc/array.c (proc_pid_status, do_task_stat, proc_pid_statm),
 * fs/proc/task_mmu.c (task_mem, task_statm), fs/proc/base.c (limits, syscall, comm, cmdline, environ,
 * auxv, mem, links, task_dump_owner, proc_fd_access_allowed), fs/proc/fd.c (fdinfo).
 */
#include <mach/mach.h>
#include <sys/resource.h>
#include <unistd.h>

#include "procfs_priv.h"

/* ---- Access rules ------------------------------------------------------------------------------ */

int pf_permission(lkd_task *viewer, uint32_t mode, uint32_t uid, uint32_t gid, int want)
{
    if (!viewer || !viewer->proc) return 0;           /* the family service thread: no guest identity */
    struct lkd_creds c;
    pthread_mutex_lock(&viewer->proc->lock);
    c = viewer->proc->cred;
    pthread_mutex_unlock(&viewer->proc->lock);
    uint32_t bits;
    if (c.fsuid == uid) bits = (mode >> 6) & 7;
    else {
        int ingrp = c.fsgid == gid;
        for (uint32_t i = 0; i < c.ngroups && !ingrp; i++) ingrp = c.groups[i] == gid;
        bits = ingrp ? (mode >> 3) & 7 : mode & 7;
    }
    if ((bits & (uint32_t)want) == (uint32_t)want) return 0;
    /* capable_wrt_inode_uidgid: CAP_DAC_OVERRIDE (bit 1) for r/w, and for x on directories or when any x bit
     * is set; CAP_DAC_READ_SEARCH (bit 2) for read and directory search (fs/namei.c generic_permission). */
    int isdir = LX_S_ISDIR(mode);
    uint64_t cap = c.cap_eff;
    if (cap & (1ull << 1)) {
        if (!(want & 1) || isdir || (mode & 0111)) return 0;
    }
    if ((cap & (1ull << 2)) && !(want & 2)) {
        if (!(want & 1) || isdir) return 0;
    }
    return -LX_EACCES;
}

int pf_ptrace_ok(lkd_task *viewer, int32_t pid, unsigned mode)
{
    if (!viewer || !viewer->proc) return 0;
    if (viewer->proc->pid == pid) return 0;           /* same_thread_group: always allowed */
    return pf_be.access(viewer, pid, mode);
}

/* ---- Target ------------------------------------------------------------------------------------ */

int pf_target_get(int32_t pid, int32_t tid, int per_thread, struct pf_target *out)
{
    memset(out, 0, sizeof *out);
    out->pid = pid;
    out->tid = tid ? tid : pid;
    out->per_thread = per_thread;
    if (pf_be.proc_lookup(pid, &out->pi) < 0) return -LX_ESRCH;
    out->zombie = out->pi.state == LKD_PS_ZOMBIE || out->pi.state == LKD_PS_DEAD;
    if (!out->zombie) out->tk = lkd_task_lookup(out->tid);
    return 0;
}

void pf_target_put(struct pf_target *tg)
{
    if (tg->tk) lkd_task_put(tg->tk);
    tg->tk = NULL;
}

/* ---- Small helpers ----------------------------------------------------------------------------- */

static const char *state_str(char s)
{
    switch (s) {
    case 'R': return "R (running)";
    case 'S': return "S (sleeping)";
    case 'D': return "D (disk sleep)";
    case 'T': return "T (stopped)";
    case 't': return "t (tracing stop)";
    case 'X': return "X (dead)";
    case 'Z': return "Z (zombie)";
    default: return "S (sleeping)";
    }
}

/* seq_escape_str(ESCAPE_SPACE | ESCAPE_SPECIAL, "\n\\") */
static void put_comm_escaped(struct pbuf *b, const char *comm)
{
    for (const char *p = comm; *p && p - comm < LKD_COMM_LEN; p++) {
        if (*p == '\n') pb_puts(b, "\\n");
        else if (*p == '\\') pb_puts(b, "\\\\");
        else pb_putc(b, *p);
    }
}

static void put_sigset(struct pbuf *b, const char *hdr, uint64_t set)
{
    pb_printf(b, "%s%016llx\n", hdr, (unsigned long long)set);
}

/* Everything the renderers need about a local task, copied under the proper locks. */
struct snap {
    lkd_task *tk;
    struct lkd_creds cred;
    struct lx_rlimit rl[LX_RLIM_NLIMITS];
    uint32_t personality;
    uint8_t no_new_privs, dumpable;
    int32_t pid, ppid, pgid, sid, nthreads, exit_signal;
    uint64_t start_mono_ns;
    int has_layout;
    struct lkd_proc_layout lay;
    uint8_t *auxv;
    size_t auxv_len;
    char *cmdline; size_t cmdline_len;
    char *environ_snap; size_t environ_len;
    char *exe;
    char comm[LKD_COMM_LEN + 1];
    int exiting;
    char state;
    lkd_tid_info ti;
    int have_ti;
};

static void snap_get(lkd_task *tk, struct snap *s)
{
    memset(s, 0, sizeof *s);
    s->tk = tk;
    struct lkd_proc *p = tk->proc;
    /* ppid, pgid, sid and the exit signal live in the family registry: a parent that died, a setpgid by the
     * parent, change them there, and the process' own copy follows only when it refreshes it */
    lkd_proc_refresh_ids(p);
    pthread_mutex_lock(&p->lock);
    s->cred = p->cred;
    memcpy(s->rl, p->rlimits, sizeof s->rl);
    s->personality = p->personality;
    s->no_new_privs = p->no_new_privs;
    s->dumpable = p->dumpable;
    s->pid = p->pid; s->ppid = p->ppid; s->pgid = p->pgid; s->sid = p->sid;
    s->nthreads = p->nthreads;
    s->exit_signal = p->exit_signal;
    s->start_mono_ns = p->start_mono_ns;
    s->exiting = p->exiting;
    if (p->layout) {
        s->has_layout = 1;
        s->lay = *p->layout;
        s->lay.auxv = NULL;
        if (p->layout->auxv && p->layout->auxv_len) {
            s->auxv = malloc(p->layout->auxv_len);
            if (s->auxv) { memcpy(s->auxv, p->layout->auxv, p->layout->auxv_len); s->auxv_len = p->layout->auxv_len; }
        }
    }
    if (p->cmdline && p->cmdline_len) {
        s->cmdline = malloc(p->cmdline_len);
        if (s->cmdline) { memcpy(s->cmdline, p->cmdline, p->cmdline_len); s->cmdline_len = p->cmdline_len; }
    }
    if (p->environ_snap && p->environ_len) {
        s->environ_snap = malloc(p->environ_len);
        if (s->environ_snap) { memcpy(s->environ_snap, p->environ_snap, p->environ_len); s->environ_len = p->environ_len; }
    }
    if (p->exe_path) s->exe = strdup(p->exe_path);
    pthread_mutex_unlock(&p->lock);
    memcpy(s->comm, tk->comm, LKD_COMM_LEN);
    s->comm[LKD_COMM_LEN] = 0;
    s->have_ti = lkd_tid_lookup(tk->tid, &s->ti) == 0;
    s->state = s->have_ti ? s->ti.state : LKD_PS_RUNNING;
}

static void snap_free(struct snap *s)
{
    free(s->auxv); free(s->cmdline); free(s->environ_snap); free(s->exe);
}

static uint64_t affinity_of(const lkd_task *tk)
{
    int n = lkd_procfs_ncpu();
    if (n > 64) n = 64;
    uint64_t all = n >= 64 ? ~0ull : ((1ull << n) - 1);
    return tk->affinity ? (tk->affinity & all) : all;
}

/* Descriptor numbers of a local task, ascending (malloc). */
static int local_fd_list(lkd_task *tk, int **out)
{
    *out = NULL;
    if (!tk->fdt) return 0;
    int cap = 1024;
    for (;;) {
        int *a = malloc((size_t)cap * sizeof *a);
        if (!a) return -LX_ENOMEM;
        int n = lkd_fd_list(tk, a, cap);
        if (n < cap || cap >= (1 << 19)) { *out = a; return n; }
        free(a);
        cap *= 4;
    }
}

/* fdtable max_fds: 64 initially, then powers of two (fs/file.c alloc_fdtable). */
static int fdsize_of(lkd_task *tk)
{
    if (!tk->fdt) return 0;
    int *fds = NULL, n = local_fd_list(tk, &fds), max = -1;
    for (int i = 0; i < n; i++) if (fds[i] > max) max = fds[i];
    free(fds);
    int sz = 64;
    while (sz <= max) sz *= 2;
    return sz;
}

/* ---- status ------------------------------------------------------------------------------------ */

static void render_status(const struct lkd_procfs_wire *rq, const struct snap *s, struct pbuf *b)
{
    lkd_task *tk = s->tk;
    pb_puts(b, "Name:\t");
    put_comm_escaped(b, s->comm);
    pb_putc(b, '\n');
    if (tk->fsctx) pb_printf(b, "Umask:\t%#04o\n", lkd_vfs_umask(tk));
    pb_printf(b, "State:\t%s\n", state_str(s->state));
    pb_printf(b, "Tgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n", s->pid, tk->tid, s->ppid);
    pb_printf(b, "TracerPid:\t%d\n", (int)lkd_ptrace_tracer_of(tk->tid));
    pb_printf(b, "Uid:\t%u\t%u\t%u\t%u\n", s->cred.uid, s->cred.euid, s->cred.suid, s->cred.fsuid);
    pb_printf(b, "Gid:\t%u\t%u\t%u\t%u\n", s->cred.gid, s->cred.egid, s->cred.sgid, s->cred.fsgid);
    pb_printf(b, "FDSize:\t%d\nGroups:\t", fdsize_of(tk));
    for (uint32_t i = 0; i < s->cred.ngroups; i++) pb_printf(b, "%s%u", i ? " " : "", s->cred.groups[i]);
    pb_putc(b, ' ');   /* Linux appends a space after the list (fs/proc/array.c) */
    pb_printf(b, "\nNStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\nKthread:\t0\n", s->pid, tk->tid, s->pgid, s->sid);
    if (tk->mm) {
        struct lkd_mm_stats st;
        if (lkd_mm_stats(tk->mm, &st) == 0) {
            /* VmPeak and VmHWM: the mm high-water marks (request procfs-2; VmHWM is a lower bound,
             * see struct lkd_mm_stats). */
            uint64_t rss = st.rss_anon + st.rss_file + st.rss_shmem;
            uint64_t text = 0;
            if (s->has_layout && s->lay.end_code > s->lay.start_code)
                text = ((s->lay.end_code + 4095) & ~4095ull) - (s->lay.start_code & ~4095ull);
            if (text > st.exec_vm) text = st.exec_vm;
            uint64_t lib = st.exec_vm - text;
            pb_udec_w(b, "VmPeak:\t", st.hiwater_vm >> 10, 8);
            pb_udec_w(b, " kB\nVmSize:\t", st.total_vm >> 10, 8);
            pb_udec_w(b, " kB\nVmLck:\t", st.locked_vm >> 10, 8);
            pb_udec_w(b, " kB\nVmPin:\t", 0, 8);
            pb_udec_w(b, " kB\nVmHWM:\t", st.hiwater_rss >> 10, 8);
            pb_udec_w(b, " kB\nVmRSS:\t", rss >> 10, 8);
            pb_udec_w(b, " kB\nRssAnon:\t", st.rss_anon >> 10, 8);
            pb_udec_w(b, " kB\nRssFile:\t", st.rss_file >> 10, 8);
            pb_udec_w(b, " kB\nRssShmem:\t", st.rss_shmem >> 10, 8);
            pb_udec_w(b, " kB\nVmData:\t", st.data_vm >> 10, 8);
            pb_udec_w(b, " kB\nVmStk:\t", st.stack_vm >> 10, 8);
            pb_udec_w(b, " kB\nVmExe:\t", text >> 10, 8);
            pb_udec_w(b, " kB\nVmLib:\t", lib >> 10, 8);
            pb_udec_w(b, " kB\nVmPTE:\t", st.pte_bytes >> 10, 8);
            pb_udec_w(b, " kB\nVmSwap:\t", 0, 8);
            pb_puts(b, " kB\n");
        }
        pb_puts(b, "CoreDumping:\t0\nTHP_enabled:\t1\nuntag_mask:\t0xffffffffffffff\n");
    }
    struct lkd_sig_status ss;
    memset(&ss, 0, sizeof ss);
    lkd_sig_status(tk, &ss);
    pb_printf(b, "Threads:\t%d\nSigQ:\t%u/%llu\n", s->nthreads, ss.queued,
              (unsigned long long)s->rl[LX_RLIMIT_SIGPENDING].rlim_cur);
    put_sigset(b, "SigPnd:\t", ss.pnd);
    put_sigset(b, "ShdPnd:\t", ss.shdpnd);
    put_sigset(b, "SigBlk:\t", ss.blk);
    put_sigset(b, "SigIgn:\t", ss.ign);
    put_sigset(b, "SigCgt:\t", ss.cgt);
    pb_printf(b, "CapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\nCapBnd:\t%016llx\nCapAmb:\t%016llx\n",
              (unsigned long long)s->cred.cap_inh, (unsigned long long)s->cred.cap_perm,
              (unsigned long long)s->cred.cap_eff, (unsigned long long)s->cred.cap_bset,
              (unsigned long long)s->cred.cap_amb);
    /* No Seccomp lines: the modeled kernel has no CONFIG_SECCOMP (docs/requests/task-2.md point 9).
     * prctl(PR_GET_SPECULATION_CTRL) answers "not affected" in the task module. */
    pb_printf(b, "NoNewPrivs:\t%d\n", s->no_new_privs ? 1 : 0);
    pb_puts(b, "Speculation_Store_Bypass:\tnot vulnerable\nSpeculationIndirectBranch:\tnot affected\n");
    pb_puts(b, "Cpus_allowed:\t");
    int ncpu = lkd_procfs_ncpu();
    pb_mask(b, affinity_of(tk), ncpu > 64 ? 64 : ncpu);
    pb_puts(b, "\nCpus_allowed_list:\t");
    pb_mask_list(b, affinity_of(tk), ncpu > 64 ? 64 : ncpu);
    pb_puts(b, "\nMems_allowed:\t1\nMems_allowed_list:\t0\n");
    pb_printf(b, "voluntary_ctxt_switches:\t%llu\nnonvoluntary_ctxt_switches:\t%llu\n",
              (unsigned long long)tk->nvcsw, (unsigned long long)tk->nivcsw);
    (void)rq;
}

/* ---- stat -------------------------------------------------------------------------------------- */

static int64_t ticks_of(int64_t sec, int64_t usec) { return sec * 100 + usec / 10000; }

static void thread_ticks(const lkd_task *tk, int whole, int64_t *ut, int64_t *st, int64_t *cut, int64_t *cst,
                         int64_t *minflt, int64_t *majflt)
{
    struct rusage ru, rc;
    getrusage(RUSAGE_SELF, &ru);
    getrusage(RUSAGE_CHILDREN, &rc);
    *cut = ticks_of(rc.ru_utime.tv_sec, rc.ru_utime.tv_usec);
    *cst = ticks_of(rc.ru_stime.tv_sec, rc.ru_stime.tv_usec);
    *ut = *st = 0;
    *minflt = ru.ru_minflt; *majflt = ru.ru_majflt;
    if (whole) {
        *ut = ticks_of(ru.ru_utime.tv_sec, ru.ru_utime.tv_usec);
        *st = ticks_of(ru.ru_stime.tv_sec, ru.ru_stime.tv_usec);
    } else if (lkd_current() == tk) {
        /* Per-thread times exist only for the calling thread (a Mach thread port of another guest thread is
         * not available): other threads report 0 (documented). */
        thread_basic_info_data_t ti;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        mach_port_t th = mach_thread_self();
        if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&ti, &cnt) == KERN_SUCCESS) {
            *ut = ticks_of(ti.user_time.seconds, ti.user_time.microseconds);
            *st = ticks_of(ti.system_time.seconds, ti.system_time.microseconds);
        }
        mach_port_deallocate(mach_task_self(), th);
    }
}

static void render_stat(const struct lkd_procfs_wire *rq, const struct snap *s, struct pbuf *b)
{
    lkd_task *tk = s->tk;
    int whole = !rq->per_thread;
    int permitted = rq->permitted;
    struct lkd_mm_stats ms;
    int have_mm = tk->mm && lkd_mm_stats(tk->mm, &ms) == 0;
    struct lkd_sig_status ss;
    memset(&ss, 0, sizeof ss);
    lkd_sig_status(tk, &ss);
    int64_t ut, st, cut, cst, minflt, majflt;
    thread_ticks(tk, whole, &ut, &st, &cut, &cst, &minflt, &majflt);
    int prio = s->tk->sched_policy == 1 || s->tk->sched_policy == 2 ? -1 - s->tk->sched_priority : 20 + s->tk->nice;
    uint64_t rss_pages = have_mm ? (ms.rss_anon + ms.rss_file + ms.rss_shmem) / 4096 : 0;
    const struct lkd_proc_layout *l = s->has_layout ? &s->lay : NULL;
    size_t cpu = 0;
    if (lkd_current() != tk || pthread_cpu_number_np(&cpu) != 0) cpu = 0;
    pb_printf(b, "%d (%.15s) %c %d %d %d 0 -1 %u %lld 0 %lld 0 %lld %lld %lld %lld %d %d %d 0 %llu %llu %llu %llu",
              tk->tid, s->comm, s->state, s->ppid, s->pgid, s->sid,
              s->exiting ? 4u : 0u, (long long)minflt, (long long)majflt, (long long)ut, (long long)st,
              (long long)cut, (long long)cst, prio, tk->nice, s->nthreads,
              (unsigned long long)(s->start_mono_ns / 10000000ull),
              (unsigned long long)(have_mm ? ms.total_vm : 0), (unsigned long long)rss_pages,
              (unsigned long long)s->rl[LX_RLIMIT_RSS].rlim_cur);
    pb_printf(b, " %llu %llu %llu 0 0", (unsigned long long)(have_mm ? (permitted ? (l ? l->start_code : 0) : 1) : 0),
              (unsigned long long)(have_mm ? (permitted ? (l ? l->end_code : 0) : 1) : 0),
              (unsigned long long)(permitted && have_mm && l ? l->start_stack : 0));
    pb_printf(b, " %llu %llu %llu %llu %d 0 0 %d %d %u %u 0 0 0",
              (unsigned long long)(ss.pnd & 0x7fffffffull), (unsigned long long)(tk->sigmask & 0x7fffffffull),
              (unsigned long long)(ss.ign & 0x7fffffffull), (unsigned long long)(ss.cgt & 0x7fffffffull),
              permitted && s->state != 'R' ? 1 : 0, s->exit_signal, (int)cpu,
              (unsigned)(s->tk->sched_priority > 0 ? s->tk->sched_priority : 0), (unsigned)s->tk->sched_policy);
    if (have_mm && permitted && l)
        pb_printf(b, " %llu %llu %llu %llu %llu %llu %llu", (unsigned long long)l->start_data,
                  (unsigned long long)l->end_data, (unsigned long long)l->start_brk, (unsigned long long)l->arg_start,
                  (unsigned long long)l->arg_end, (unsigned long long)l->env_start, (unsigned long long)l->env_end);
    else
        pb_puts(b, " 0 0 0 0 0 0 0");
    pb_printf(b, " %d\n", permitted ? tk->exit_code : 0);
}

static void render_statm(const struct snap *s, struct pbuf *b)
{
    struct lkd_mm_stats ms;
    if (!s->tk->mm || lkd_mm_stats(s->tk->mm, &ms) != 0) { pb_puts(b, "0 0 0 0 0 0 0\n"); return; }
    uint64_t shared = (ms.rss_file + ms.rss_shmem) / 4096;
    uint64_t resident = shared + ms.rss_anon / 4096;
    uint64_t text = 0;
    if (s->has_layout && s->lay.end_code > s->lay.start_code)
        text = (((s->lay.end_code + 4095) & ~4095ull) - (s->lay.start_code & ~4095ull)) / 4096;
    pb_printf(b, "%llu %llu %llu %llu 0 %llu 0\n", (unsigned long long)(ms.total_vm / 4096),
              (unsigned long long)resident, (unsigned long long)shared, (unsigned long long)text,
              (unsigned long long)((ms.data_vm + ms.stack_vm) / 4096));
}

/* ---- limits, syscall, misc --------------------------------------------------------------------- */

static void render_limits(const struct snap *s, struct pbuf *b)
{
    static const struct { const char *name, *unit; } ln[LX_RLIM_NLIMITS] = {
        [LX_RLIMIT_CPU] = { "Max cpu time", "seconds" }, [LX_RLIMIT_FSIZE] = { "Max file size", "bytes" },
        [LX_RLIMIT_DATA] = { "Max data size", "bytes" }, [LX_RLIMIT_STACK] = { "Max stack size", "bytes" },
        [LX_RLIMIT_CORE] = { "Max core file size", "bytes" }, [LX_RLIMIT_RSS] = { "Max resident set", "bytes" },
        [LX_RLIMIT_NPROC] = { "Max processes", "processes" }, [LX_RLIMIT_NOFILE] = { "Max open files", "files" },
        [LX_RLIMIT_MEMLOCK] = { "Max locked memory", "bytes" }, [LX_RLIMIT_AS] = { "Max address space", "bytes" },
        [LX_RLIMIT_LOCKS] = { "Max file locks", "locks" }, [LX_RLIMIT_SIGPENDING] = { "Max pending signals", "signals" },
        [LX_RLIMIT_MSGQUEUE] = { "Max msgqueue size", "bytes" }, [LX_RLIMIT_NICE] = { "Max nice priority", NULL },
        [LX_RLIMIT_RTPRIO] = { "Max realtime priority", NULL }, [LX_RLIMIT_RTTIME] = { "Max realtime timeout", "us" },
    };
    pb_puts(b, "Limit                     Soft Limit           Hard Limit           Units     \n");
    for (int i = 0; i < LX_RLIM_NLIMITS; i++) {
        char soft[32], hard[32];
        if (s->rl[i].rlim_cur == LX_RLIM_INFINITY) snprintf(soft, sizeof soft, "unlimited");
        else snprintf(soft, sizeof soft, "%llu", (unsigned long long)s->rl[i].rlim_cur);
        if (s->rl[i].rlim_max == LX_RLIM_INFINITY) snprintf(hard, sizeof hard, "unlimited");
        else snprintf(hard, sizeof hard, "%llu", (unsigned long long)s->rl[i].rlim_max);
        pb_printf(b, "%-25s %-20s %-20s ", ln[i].name, soft, hard);
        if (ln[i].unit) pb_printf(b, "%-10s\n", ln[i].unit);
        else pb_putc(b, '\n');
    }
}

static void render_syscall(const struct snap *s, struct pbuf *b)
{
    lkd_task *tk = s->tk;
    int64_t nr = atomic_load(&tk->cur_nr);
    if (nr >= 0) {
        pb_printf(b, "%lld", (long long)nr);
        for (int i = 0; i < 6; i++) pb_printf(b, " 0x%llx", (unsigned long long)tk->cur_args[i]);
    } else {
        if (s->state == 'R') { pb_puts(b, "running\n"); return; }
        pb_puts(b, "-1");
    }
    pb_printf(b, " 0x%llx 0x%llx\n", (unsigned long long)(s->have_ti ? s->ti.syscall_sp : 0),
              (unsigned long long)(s->have_ti ? s->ti.syscall_pc : 0));
}

/* cmdline: NUL separated snapshot taken at exec (Linux reads arg_start..arg_end of the live mm, which a
 * program can rewrite: setproctitle is not reflected, documented). */
static void render_cmdline(const struct snap *s, struct pbuf *b)
{
    if (s->cmdline && s->cmdline_len) pb_write(b, s->cmdline, s->cmdline_len);
}

static int fd_describe(lkd_task *tk, int fd, char *buf, size_t cap)
{
    lkd_fdobj *o = tk->fdt ? lkd_fd_get(tk, fd, NULL) : NULL;
    if (!o) return -LX_ENOENT;
    int n = lkd_fs_describe(o, buf, cap);
    lkd_fdobj_put(o);
    return n;
}

static int render_fdinfo(const struct lkd_procfs_wire *rq, const struct snap *s, struct pbuf *b)
{
    lkd_task *tk = s->tk;
    int cx = 0;
    lkd_fdobj *o = tk->fdt ? lkd_fd_get(tk, rq->fd, &cx) : NULL;
    if (!o) return -LX_ENOENT;
    int64_t pos = 0;
    uint32_t fl = 0;
    int mnt = 0;
    lkd_fs_fdinfo_basic(tk, o, &pos, &fl, &mnt);
    if (cx) fl |= LX_O_CLOEXEC;
    pb_printf(b, "pos:\t%lld\nflags:\t0%o\nmnt_id:\t%d\nino:\t%llu\n", (long long)pos, fl, mnt, (unsigned long long)o->ino);
    if (o->ops && o->ops->fdinfo) {
        char *x = malloc(4096);
        if (x) {
            int n = o->ops->fdinfo(o, x, 4096);
            if (n > 0) pb_write(b, x, (size_t)n < 4096 ? (size_t)n : 4095);
            free(x);
        }
    }
    lkd_fdobj_put(o);
    return 0;
}

/* ---- Local rendering --------------------------------------------------------------------------- */

static int render_link_text(const struct lkd_procfs_wire *rq, const struct snap *s, struct pbuf *b)
{
    lkd_task *tk = s->tk;
    char buf[4096];
    int n;
    switch (rq->kind) {
    case PK_EXE:
        if (!s->exe) return -LX_ENOENT;
        pb_puts(b, s->exe);
        return 0;
    case PK_CWD:
        if (!tk->fsctx) return -LX_ENOENT;
        n = lkd_vfs_getcwd(tk, buf, sizeof buf);
        if (n < 0) return n;
        pb_write(b, buf, strlen(buf));
        return 0;
    case PK_ROOTLNK:
        if (!tk->fsctx) return -LX_ENOENT;
        fs_get_root(tk, buf, sizeof buf);
        pb_puts(b, buf);
        return 0;
    case PK_FD:
        n = fd_describe(tk, rq->fd, buf, sizeof buf - 1);
        if (n < 0) return n;
        pb_write(b, buf, (size_t)n);
        return 0;
    }
    return -LX_EINVAL;
}

int pf_render_local(const struct lkd_procfs_wire *rq, lkd_task *tk, struct pbuf *b)
{
    struct snap s;
    snap_get(tk, &s);
    int rc = 0;
    switch (rq->kind) {
    case PK_STATUS: render_status(rq, &s, b); break;
    case PK_STAT: render_stat(rq, &s, b); break;
    case PK_STATM: render_statm(&s, b); break;
    case PK_LIMITS: render_limits(&s, b); break;
    case PK_COMM: pb_puts(b, s.comm); pb_putc(b, '\n'); break;
    case PK_SYSCALL: render_syscall(&s, b); break;
    case PK_WCHAN: pb_putc(b, '0'); break;   /* no kernel symbols: the "0" Linux prints without symbol access */
    case PK_CMDLINE: render_cmdline(&s, b); break;
    case PK_ENVIRON: if (s.environ_snap) pb_write(b, s.environ_snap, s.environ_len); break;
    case PK_AUXV: if (s.auxv) pb_write(b, s.auxv, s.auxv_len); break;
    case PK_PERSONALITY: pb_printf(b, "%08x\n", s.personality); break;
    case PK_CGROUP: pb_puts(b, "0::/\n"); break;
    case PK_MOUNTS: case PK_MOUNTINFO: {
        size_t len = 0;
        char *t = lkd_vfs_render_mounts(tk, rq->kind == PK_MOUNTINFO, &len);
        if (t) { pb_write(b, t, len); free(t); } else rc = -LX_ENOMEM;
        break;
    }
    case PK_MAPS: case PK_SMAPS: {
        size_t len = 0;
        char *t = tk->mm ? (rq->kind == PK_MAPS ? lkd_mm_render_maps(tk->mm, &len) : lkd_mm_render_smaps(tk->mm, &len)) : NULL;
        if (t) { pb_write(b, t, len); free(t); } else if (tk->mm) rc = -LX_ENOMEM;
        break;
    }
    case PK_FDINFO: rc = render_fdinfo(rq, &s, b); break;
    case PK_EXE: case PK_CWD: case PK_ROOTLNK: case PK_FD: rc = render_link_text(rq, &s, b); break;
    case PK_PNET:   /* net/dev: the header only, no interface is translated from the host (docs/status/procfs.md) */
        if (rq->fd == 0)
            pb_puts(b, "Inter-|   Receive                                                |  Transmit\n"
                       " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
        if (rq->fd == 2 || rq->fd == 3) {   /* net/route, net/ipv6_route: the routes of the interfaces net shows */
            size_t len = 0;
            char *t = lkd_net_render_routes(rq->fd == 3, &len);
            if (t) { pb_write(b, t, len); free(t); } else rc = -LX_ENOMEM;
        }
        break;
    case PK_WIRE_FDLIST: {
        int *fds = NULL, n = local_fd_list(tk, &fds);
        if (n < 0) rc = n;
        else { pb_write(b, fds, (size_t)n * sizeof(int)); }
        free(fds);
        break;
    }
    default: rc = -LX_EINVAL;
    }
    snap_free(&s);
    if (rc == 0 && b->oom) rc = -LX_ENOMEM;
    return rc;
}

/* A process that is a zombie: only the registry knows it. No mm, no files, no signals (the real kernel
 * prints the same reduced status for a zombie: no Vm lines, empty cmdline). */
int pf_render_zombie(const struct lkd_procfs_wire *rq, const lkd_proc_info *pi, struct pbuf *b)
{
    switch (rq->kind) {
    case PK_STATUS:
        pb_puts(b, "Name:\t");
        put_comm_escaped(b, pi->comm);
        pb_printf(b, "\nState:\tZ (zombie)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n", pi->pid, pi->pid, pi->ppid);
        pb_printf(b, "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t0\nGroups:\t \n", pi->uid, pi->euid, pi->euid,
                  pi->euid, pi->gid, pi->egid, pi->egid, pi->egid);
        pb_printf(b, "NStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\nKthread:\t0\nThreads:\t%d\n", pi->pid, pi->pid,
                  pi->pgid, pi->sid, pi->nthreads);
        return 0;
    case PK_STAT:
        pb_printf(b, "%d (%.15s) Z %d %d %d 0 -1 0 0 0 0 0 0 0 0 0 20 0 %d 0 %llu 0 0 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 %d 0 0 0 0 0 0 0 0 0 0 0 0 %d\n",
                  pi->pid, pi->comm, pi->ppid, pi->pgid, pi->sid, pi->nthreads,
                  (unsigned long long)(pi->start_mono_ns / 10000000ull), pi->exit_signal, pi->wait_status);
        return 0;
    case PK_STATM: pb_puts(b, "0 0 0 0 0 0 0\n"); return 0;
    case PK_CMDLINE: case PK_ENVIRON: case PK_AUXV: case PK_MAPS: case PK_SMAPS: return 0;   /* no mm: empty read */
    case PK_COMM: pb_printf(b, "%.15s\n", pi->comm); return 0;
    case PK_WCHAN: pb_putc(b, '0'); return 0;
    case PK_EXE: case PK_CWD: case PK_ROOTLNK: case PK_FD: return -LX_ENOENT;
    case PK_FDINFO: return -LX_ENOENT;
    case PK_WIRE_FDLIST: return 0;
    default: return -LX_ESRCH;
    }
}

/* ---- Generation for a viewer ------------------------------------------------------------------- */

static void fill_wire(lkd_task *viewer, const struct pf_node *n, struct lkd_procfs_wire *w)
{
    memset(w, 0, sizeof *w);
    w->kind = (uint32_t)n->kind;
    w->pid = n->pid;
    w->tid = n->tid ? n->tid : n->pid;
    w->per_thread = n->per_thread;
    w->fd = n->fd;
    w->permitted = pf_ptrace_ok(viewer, n->pid, LKD_PTRACE_MODE_READ) == 0;
    w->viewer_pid = viewer && viewer->proc ? viewer->proc->pid : 0;
    if (n->kind == PK_PNET) w->fd = n->sub;
}

/* The one transport: local task, registry-only zombie, or the other host process. */
static int generate_wire(const struct lkd_procfs_wire *w, char **text, size_t *len)
{
    return lkd_procfs_render_wire(w, text, len);
}

int lkd_procfs_render_wire(const struct lkd_procfs_wire *w, char **text, size_t *len)
{
    *text = NULL;
    *len = 0;
    struct pf_target tg;
    int rc = pf_target_get(w->pid, w->tid, w->per_thread, &tg);
    if (rc < 0) return rc;
    struct pbuf b = { 0 };
    /* lkd_host_pid: from the registry, whichever host process carries the target; a zombie has none left */
    if (w->kind == PK_HOSTPID) {
        if (tg.zombie || tg.pi.host_pid <= 0) rc = -LX_ESRCH;
        else pb_printf(&b, "%d\n", tg.pi.host_pid);
    } else if (tg.zombie) rc = pf_render_zombie(w, &tg.pi, &b);
    else if (tg.tk) rc = pf_render_local(w, tg.tk, &b);
    else if (tg.pi.host_pid > 0) {
        pf_target_put(&tg);
        if (w->kind == PK_MAPS) {
            /* the mm module's own cross-process service renders VMAs of another process */
            int err = 0;
            size_t l = 0;
            char *t = lkd_mm_remote_render_maps(w->pid, &l, &err);
            if (!t && err) return err;
            *text = t ? t : strdup("");
            *len = l;
            return *text ? 0 : -LX_ENOMEM;
        }
        return pf_be.remote(tg.pi.host_pid, w, text, len);
    } else rc = -LX_ESRCH;
    pf_target_put(&tg);
    if (rc < 0) { pb_free(&b); return rc; }
    if (!b.p) { b.p = malloc(1); if (b.p) b.p[0] = 0; }
    *text = b.p;
    *len = b.n;
    return b.p ? 0 : -LX_ENOMEM;
}

int pf_generate_pid(lkd_task *viewer, const struct pf_node *n, char **text, size_t *len)
{
    struct lkd_procfs_wire w;
    fill_wire(viewer, n, &w);
    return generate_wire(&w, text, len);
}

/* ---- Descriptors ------------------------------------------------------------------------------- */

int pf_fd_list(lkd_task *viewer, int32_t pid, int32_t tid, int **fds)
{
    *fds = NULL;
    struct lkd_procfs_wire w = { .kind = PK_WIRE_FDLIST, .pid = pid, .tid = tid ? tid : pid };
    (void)viewer;
    char *txt = NULL;
    size_t len = 0;
    int rc = lkd_procfs_render_wire(&w, &txt, &len);
    if (rc < 0) return rc;
    int n = (int)(len / sizeof(int));
    int *a = malloc((size_t)(n ? n : 1) * sizeof *a);
    if (!a) { free(txt); return -LX_ENOMEM; }
    memcpy(a, txt, (size_t)n * sizeof *a);
    free(txt);
    *fds = a;
    return n;
}

/* Status flags of descriptor fd of a LOCAL task (no object lock). -ENOENT when it is not open, -ENOSYS when the
 * process lives in another host process. */
int pf_fd_status_flags(int32_t pid, int32_t tid, int fd, uint32_t *fl)
{
    struct pf_target tg;
    if (pf_target_get(pid, tid, 0, &tg) < 0) return -LX_ENOENT;
    if (!tg.tk) { pf_target_put(&tg); return -LX_ENOSYS; }
    lkd_fdobj *o = tg.tk->fdt ? lkd_fd_get(tg.tk, fd, NULL) : NULL;
    pf_target_put(&tg);
    if (!o) return -LX_ENOENT;
    *fl = atomic_load(&o->status_flags);
    lkd_fdobj_put(o);
    return 0;
}

/* ---- Links ------------------------------------------------------------------------------------- */

/* proc_fd_access_allowed: ptrace_may_access(PTRACE_MODE_READ_FSCREDS). */
int64_t pf_pid_readlink(lkd_task *viewer, const struct pf_node *n, char *buf, size_t cap)
{
    if (pf_ptrace_ok(viewer, n->pid, LKD_PTRACE_MODE_READ) < 0) return -LX_EACCES;
    struct lkd_procfs_wire w;
    fill_wire(viewer, n, &w);
    char *txt = NULL;
    size_t len = 0;
    int rc = generate_wire(&w, &txt, &len);
    if (rc < 0) return rc == -LX_ESRCH ? -LX_ENOENT : rc;
    if (len > cap) len = cap;
    memcpy(buf, txt, len);
    free(txt);
    return (int64_t)len;
}

int pf_pid_open_link(lkd_task *viewer, const struct pf_node *n, uint32_t fl, lkd_fdobj **out)
{
    if (pf_ptrace_ok(viewer, n->pid, LKD_PTRACE_MODE_READ) < 0) return -LX_EACCES;
    if (n->kind == PK_FD) {
        struct pf_target tg;
        int rc = pf_target_get(n->pid, n->tid, n->per_thread, &tg);
        if (rc < 0) return -LX_ENOENT;
        if (tg.tk) {
            lkd_fdobj *o = tg.tk->fdt ? lkd_fd_get(tg.tk, n->fd, NULL) : NULL;
            pf_target_put(&tg);
            if (!o) return -LX_ENOENT;
            if (fl & LX_O_PATH) { *out = o; return 0; }   /* stat of the link: the object itself, own reference */
            rc = lkd_fdobj_reopen(viewer, o, fl, out);
            lkd_fdobj_put(o);
            return rc;
        }
        int32_t host = tg.pi.host_pid;
        pf_target_put(&tg);
        return pf_remote_open_fd(host, n, fl, out);
    }
    char path[4096];
    int64_t l = pf_pid_readlink(viewer, n, path, sizeof path - 1);
    if (l < 0) return (int)l;
    path[l] = 0;
    if (n->kind == PK_EXE) {
        if ((fl & LX_O_ACCMODE) != LX_O_RDONLY && !(fl & LX_O_PATH)) return -LX_ETXTBSY;   /* running image */
        return lkd_fs_open_guest_file(viewer, path, out);
    }
    if (path[0] != '/') return -LX_ENOENT;   /* "(unreachable)/..." */
    return lkd_vfs_open(viewer, LX_AT_FDCWD, path, (fl & LX_O_PATH) ? (LX_O_PATH | LX_O_DIRECTORY) : (LX_O_RDONLY | LX_O_DIRECTORY), 0, out);
}

/* ---- comm -------------------------------------------------------------------------------------- */

/* comm_write (fs/proc/base.c): only a thread of the same group may rename a thread; the new name is
 * truncated to TASK_COMM_LEN - 1 and is NOT stripped of a trailing newline. */
int64_t pf_comm_write(lkd_task *viewer, const struct pf_node *n, const char *data, size_t len)
{
    if (!viewer || !viewer->proc || viewer->proc->pid != n->pid) return -LX_EINVAL;
    lkd_task *tk = lkd_task_lookup(n->tid ? n->tid : n->pid);
    if (!tk) return -LX_ESRCH;
    size_t k = len < LKD_COMM_LEN - 1 ? len : LKD_COMM_LEN - 1;
    char tmp[LKD_COMM_LEN] = { 0 };
    for (size_t i = 0; i < k && data[i]; i++) tmp[i] = data[i];
    memcpy(tk->comm, tmp, LKD_COMM_LEN);
    lkd_registry_publish(tk);
    lkd_task_put(tk);
    return (int64_t)len;
}

/* ---- mem --------------------------------------------------------------------------------------- */

struct mem_priv { int32_t pid; int64_t pos; int writable; };

static int64_t mem_rw(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset, int write)
{
    struct mem_priv *m = o->priv;
    int64_t pos = offset ? *offset : m->pos;
    if (pos < 0) return -LX_EINVAL;
    if (write && !m->writable) return -LX_EBADF;
    char *buf = malloc(65536);
    if (!buf) return -LX_ENOMEM;
    uint64_t done = 0;
    int64_t err = 0;
    while (done < count) {
        uint64_t c = count - done > 65536 ? 65536 : count - done;
        int64_t r;
        if (write) {
            if (lkd_user_read(gva + done, buf, c) < 0) { err = done ? 0 : -LX_EFAULT; break; }
            r = lkd_mm_remote_write(m->pid, (uint64_t)pos + done, buf, c);
        } else {
            r = lkd_mm_remote_read(m->pid, (uint64_t)pos + done, buf, c);
        }
        /* mem_rw: a copy of 0 bytes is -EIO when nothing was copied (task gone: 0) */
        if (r == -LX_ESRCH) break;
        if (r < 0) { if (!done) err = (r == -LX_EFAULT || r == -LX_EPERM) ? -LX_EIO : r; break; }
        if (r == 0) { if (!done) err = -LX_EIO; break; }
        if (!write && lkd_user_write(gva + done, buf, (size_t)r) < 0) { if (!done) err = -LX_EFAULT; break; }
        done += (uint64_t)r;
        if ((uint64_t)r < c) break;
    }
    free(buf);
    if (err && !done) return err;
    if (!offset) m->pos = pos + (int64_t)done;
    return (int64_t)done;
}

static int64_t mem_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{ return mem_rw(t, o, gva, count, offset, 0); }
static int64_t mem_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{ return mem_rw(t, o, gva, count, offset, 1); }

/* mem_lseek: SEEK_SET and SEEK_CUR only */
static int64_t mem_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    struct mem_priv *m = o->priv;
    if (whence == LX_SEEK_SET) m->pos = off;
    else if (whence == LX_SEEK_CUR) m->pos += off;
    else return -LX_EINVAL;
    return m->pos;
}

static int mem_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct mem_priv *m = o->priv;
    struct pf_node n;
    char rel[64];
    snprintf(rel, sizeof rel, "%d/mem", m->pid);
    int rc = pf_resolve(t, PF_PROC, rel, &n);
    if (rc < 0) return rc;
    pf_stat_node(t, &n, out);
    return 0;
}

static void mem_release(lkd_fdobj *o) { free(o->priv); o->priv = NULL; }

static int mem_export(lkd_fdobj *o, int *hfds, int cap)
{
    (void)cap;
    struct mem_priv *m = o->priv;
    return pf_rec_export(o, m->pos, hfds);
}

static const lkd_fd_ops g_mem_ops = {
    .name = "procmem", .read = mem_read, .write = mem_write, .lseek = mem_lseek, .fstat = mem_fstat,
    .release = mem_release, .export_fds = mem_export,
};

int pf_mem_open(lkd_task *viewer, const struct pf_node *n, uint32_t fl, lkd_fdobj **out)
{
    /* proc_mem_open -> mm_access: a refused ptrace_may_access is -EACCES (kernel/fork.c mm_access) */
    if (pf_ptrace_ok(viewer, n->pid, LKD_PTRACE_MODE_ATTACH) < 0) return -LX_EACCES;
    struct mem_priv *m = calloc(1, sizeof *m);
    if (!m) return -LX_ENOMEM;
    m->pid = n->pid;
    m->writable = (fl & LX_O_ACCMODE) != LX_O_RDONLY;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_PROCFD, &g_mem_ops, -1, (fl & (LX_O_ACCMODE | LX_O_NONBLOCK)) | LX_O_LARGEFILE, m);
    if (!o) { free(m); return -LX_ENOMEM; }
    char gp[64];
    snprintf(gp, sizeof gp, "/proc/%d/mem", n->pid);
    o->gpath = strdup(gp);
    o->ino = n->ino;
    *out = o;
    return 0;
}

/* test_procfs_self.c: /proc/self and the per-process files, through the real syscalls (openat, read,
 * readlink, getdents64, newfstatat) on top of the real vfs, mm, task, sig and ptrace modules.
 * Linux behaviors modeled (v6.12): fs/proc/self.c (proc_self_get_link), fs/proc/base.c (tgid_base_stuff
 * modes, proc_pid_get_link, comm_write, mem_rw, task_dump_owner), fs/proc/array.c (proc_pid_status,
 * do_task_stat, proc_pid_statm), fs/proc/task_mmu.c (task_mem), fs/proc/fd.c (fd links, fdinfo). */
#include "test_procfs_util.h"

static uint64_t sget(const char *text, const char *key)
{
    const char *v = field(text, key);
    return v ? strtoull(v, NULL, 10) : (uint64_t)-1;
}

/* proc(5): self is a symlink to the pid of the caller; thread-self to pid/task/tid (fs/proc/self.c,
 * fs/proc/thread_self.c). */
LT_TEST(self_links)
{
    char buf[64], want[64];
    LT_CHECK(rdlink("/proc/self", buf, sizeof buf) > 0);
    snprintf(want, sizeof want, "%d", T->proc->pid);
    LT_STREQ(buf, want);
    LT_CHECK(rdlink("/proc/thread-self", buf, sizeof buf) > 0);
    snprintf(want, sizeof want, "%d/task/%d", T->proc->pid, T->tid);
    LT_STREQ(buf, want);
    struct lx_stat st;
    LT_EQ(glstat("/proc/self", &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    LT_EQ(st.st_mode & 0777, 0777);
    LT_EQ(gstat("/proc/self", &st), 0);        /* followed: the directory of the process */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFDIR);
    LT_EQ(st.st_mode & 0777, 0555);
    snprintf(want, sizeof want, "/proc/%d/status", T->proc->pid);
    int64_t l1, l2;
    char *a = slurp("/proc/self/status", &l1), *b = slurp(want, &l2);
    LT_CHECK(a && b);
    if (a && b) LT_CHECK(l1 > 100 && !strcmp(field(a, "Pid"), field(b, "Pid")));
    free(a); free(b);
    /* /proc/self/net and /proc/mounts: proc_root links (fs/proc/proc_net.c, fs/proc/root.c) */
    LT_CHECK(rdlink("/proc/mounts", buf, sizeof buf) > 0);
    LT_STREQ(buf, "self/mounts");
    LT_CHECK(rdlink("/proc/net", buf, sizeof buf) > 0);
    LT_STREQ(buf, "self/net");
}

/* readlink(/proc/self/exe) returns the exact guest path of the executable (the EAC loader path check,
 * reports/eac-probes-digest.md section 6 item 13); with no executable recorded the link does not exist
 * (proc_exe_link: no mm->exe_file -> -ENOENT). */
LT_TEST(exe_link)
{
    char buf[512];
    free(T->proc->exe_path);
    T->proc->exe_path = NULL;
    LT_ERRNO(rdlink("/proc/self/exe", buf, sizeof buf), LX_ENOENT);
    T->proc->exe_path = strdup("/opt/ge-proton/files/bin/wine-preloader");
    int64_t n = rdlink("/proc/self/exe", buf, sizeof buf);
    LT_EQ(n, 39);
    LT_STREQ(buf, "/opt/ge-proton/files/bin/wine-preloader");
    /* readlinkat with a 10 byte buffer truncates without NUL (readlink(2)) */
    LT_EQ(SC(T, readlink, gstr("/proc/self/exe"), gbulk(), 10), 10);
    LT_MEMEQ((void *)(uintptr_t)gbulk(), "/opt/ge-pr", 10);
    struct lx_stat st;
    LT_EQ(glstat("/proc/self/exe", &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    /* a renamed executable (exec_commit) is seen at once: state, not a cache */
    free(T->proc->exe_path);
    T->proc->exe_path = strdup("/bin/dash");
    LT_EQ(rdlink("/proc/self/exe", buf, sizeof buf), 9);
    LT_STREQ(buf, "/bin/dash");
    /* stat and open follow the link to the executable file; writing to a running image is -ETXTBSY
     * (proc_exe_link + deny_write_access) */
    free(T->proc->exe_path);
    T->proc->exe_path = strdup("/bin/sh");
    LT_EQ(gstat("/proc/self/exe", &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_CHECK(st.st_mode & 0111);
    int64_t xfd = gopen("/proc/self/exe", LX_O_RDONLY);
    LT_CHECK(xfd >= 0);
    LT_EQ(SC(T, read, (uint64_t)xfd, gbulk(), 4), 4);
    LT_CHECK(((uint8_t *)(uintptr_t)gbulk())[0] != 0);
    SC(T, close, (uint64_t)xfd);
    LT_ERRNO(gopen("/proc/self/exe", LX_O_WRONLY), LX_ETXTBSY);
    free(T->proc->exe_path);
    T->proc->exe_path = strdup("/opt/ge-proton/files/bin/wine-preloader");
}

/* proc_pid_cmdline / environ / auxv: NUL separated, partial reads, offsets (fs/proc/base.c). */
LT_TEST(cmdline_environ_auxv)
{
    static const char cmd[] = "wine64\0--debugger\0C:\\game.exe";
    static const char env[] = "HOME=/home/u\0LANG=C\0";
    free(T->proc->cmdline); free(T->proc->environ_snap);
    T->proc->cmdline = malloc(sizeof cmd); memcpy(T->proc->cmdline, cmd, sizeof cmd);
    T->proc->cmdline_len = sizeof cmd;
    T->proc->environ_snap = malloc(sizeof env); memcpy(T->proc->environ_snap, env, sizeof env);
    T->proc->environ_len = sizeof env;
    uint8_t aux[32] = { 33, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct lkd_proc_layout lay = { .start_code = 0x400000, .end_code = 0x401234, .start_data = 0x600000, .end_data = 0x601000,
                                   .start_brk = 0x602000, .start_stack = 0x7ffffffde000, .arg_start = 0x7ffffffdf000,
                                   .arg_end = 0x7ffffffdf01e, .env_start = 0x7ffffffdf01e, .env_end = 0x7ffffffdf032,
                                   .entry = 0x401000, .auxv = aux, .auxv_len = sizeof aux };
    lkd_proc_set_layout(T, &lay);
    int64_t len;
    char *s = slurp("/proc/self/cmdline", &len);
    LT_EQ(len, (int64_t)sizeof cmd);
    if (s) LT_MEMEQ(s, cmd, sizeof cmd);
    free(s);
    s = slurp("/proc/self/environ", &len);
    LT_EQ(len, (int64_t)sizeof env);
    if (s) LT_MEMEQ(s, env, sizeof env);
    free(s);
    s = slurp("/proc/self/auxv", &len);
    LT_EQ(len, 32);
    if (s) LT_MEMEQ(s, aux, 32);
    free(s);
    /* pread of 31 bytes at offset 4 (the EAC probe reads 31 bytes of the parent cmdline) */
    int64_t fd = gopen("/proc/self/cmdline", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, pread64, (uint64_t)fd, gbulk(), 20, 4), 20);
    LT_MEMEQ((void *)(uintptr_t)gbulk(), cmd + 4, 20);
    LT_EQ(SC(T, pread64, (uint64_t)fd, gbulk(), 31, 4), (int64_t)sizeof cmd - 4);   /* short read at the end */
    LT_EQ(SC(T, pread64, (uint64_t)fd, gbulk(), 10, 1000), 0);   /* past the end: EOF */
    SC(T, close, (uint64_t)fd);
    /* the files are not writable (0444 / 0400 without write: -EACCES for an unprivileged writer) */
    LT_ERRNO(gopen("/proc/self/cmdline", LX_O_WRONLY), LX_EACCES);
    LT_ERRNO(gopen("/proc/self/auxv", LX_O_WRONLY), LX_EACCES);
}

/* comm is readable and writable by the thread group (proc_pid_set_comm_operations 0644, comm_write):
 * the name is cut to 15 characters and the registry (Name: of status) follows. */
LT_TEST(comm_read_write)
{
    memset(T->comm, 0, sizeof T->comm);
    memcpy(T->comm, "RocketLeague", 12);
    int64_t len;
    char *s = slurp("/proc/self/comm", &len);
    LT_EQ(len, 13);
    LT_STREQ(s, "RocketLeague\n");
    free(s);
    int64_t fd = gopen("/proc/self/comm", LX_O_WRONLY);
    LT_CHECK(fd >= 0);
    const char *nm = "abcdefghijklmnopqrstuvwxyz";
    memcpy((void *)(uintptr_t)gbulk(), nm, 26);
    LT_EQ(SC(T, write, (uint64_t)fd, gbulk(), 26), 26);
    SC(T, close, (uint64_t)fd);
    LT_STREQ(T->comm, "abcdefghijklmno");
    s = slurp("/proc/self/status", &len);
    LT_STREQ(fline(s, "Name"), "\tabcdefghijklmno");
    free(s);
    /* stat shows the comm in parentheses */
    s = slurp("/proc/self/stat", &len);
    LT_CHECK(strstr(s, "(abcdefghijklmno)") != NULL);
    free(s);
    /* a newline in a thread name is escaped in status (seq_escape_str "\n\\") */
    memset(T->comm, 0, sizeof T->comm);
    memcpy(T->comm, "a\nb\\c", 5);
    s = slurp("/proc/self/status", &len);
    LT_STREQ(fline(s, "Name"), "\ta\\nb\\\\c");
    free(s);
    memset(T->comm, 0, sizeof T->comm);
    memcpy(T->comm, "tester", 6);
}

/* proc_pid_status: every field Linux v6.12 prints for a task with an mm, in the same order. The task
 * module's state is the source: credentials, groups, rlimits, signal sets (sig), threads. */
LT_TEST(status_fields)
{
    int64_t len;
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    T->proc->cred.uid = 1000; T->proc->cred.euid = 1001; T->proc->cred.suid = 1002; T->proc->cred.fsuid = 1003;
    T->proc->cred.gid = 100; T->proc->cred.egid = 101; T->proc->cred.sgid = 102; T->proc->cred.fsgid = 103;
    T->proc->cred.ngroups = 3; T->proc->cred.groups[0] = 4; T->proc->cred.groups[1] = 24; T->proc->cred.groups[2] = 1000;
    T->proc->cred.cap_eff = 0x0000000000000400ull; T->proc->cred.cap_perm = 0x0000000000000500ull;
    T->proc->cred.cap_inh = 0x1; T->proc->cred.cap_bset = 0x000001ffffffffffull; T->proc->cred.cap_amb = 0x2;
    T->proc->no_new_privs = 1;
    T->affinity = 0x5;   /* cpus 0 and 2 */
    T->nvcsw = 42; T->nivcsw = 7;
    atomic_store(&T->sigmask, 1ull << 9);   /* SIGUSR1 blocked: bit 9 */
    char *s = slurp("/proc/self/status", &len);
    LT_CHECK(s != NULL);
    if (!s) return;
    LT_STREQ(fline(s, "Name"), "\ttester");
    LT_STREQ(fline(s, "Umask"), "\t0022");
    LT_STREQ(fline(s, "State"), "\tR (running)");
    char want[64];
    snprintf(want, sizeof want, "\t%d", T->proc->pid);
    LT_STREQ(fline(s, "Tgid"), want);
    LT_STREQ(fline(s, "Pid"), want);
    LT_STREQ(fline(s, "TracerPid"), "\t0");
    LT_STREQ(fline(s, "Uid"), "\t1000\t1001\t1002\t1003");
    LT_STREQ(fline(s, "Gid"), "\t100\t101\t102\t103");
    LT_STREQ(fline(s, "Groups"), "\t4 24 1000 ");               /* trailing space like the kernel */
    LT_STREQ(fline(s, "FDSize"), "\t64");
    LT_STREQ(fline(s, "Threads"), "\t2");
    LT_STREQ(fline(s, "SigBlk"), "\t0000000000000200");
    LT_STREQ(fline(s, "CapInh"), "\t0000000000000001");
    LT_STREQ(fline(s, "CapPrm"), "\t0000000000000500");
    LT_STREQ(fline(s, "CapEff"), "\t0000000000000400");
    LT_STREQ(fline(s, "CapBnd"), "\t000001ffffffffff");
    LT_STREQ(fline(s, "CapAmb"), "\t0000000000000002");
    LT_STREQ(fline(s, "NoNewPrivs"), "\t1");
    LT_STREQ(fline(s, "Cpus_allowed"), "\t00005");   /* %*pb pads the first chunk to 18 bits = 5 digits */
    LT_STREQ(fline(s, "Cpus_allowed_list"), "\t0,2");
    LT_STREQ(fline(s, "voluntary_ctxt_switches"), "\t42");
    LT_STREQ(fline(s, "nonvoluntary_ctxt_switches"), "\t7");
    LT_CHECK(strstr(fline(s, "SigQ"), "/") != NULL);
    LT_CHECK(field(s, "Seccomp") == NULL);                      /* no CONFIG_SECCOMP in the modeled kernel */
    LT_CHECK(field(s, "Seccomp_filters") == NULL);
    LT_STREQ(fline(s, "Kthread"), "\t0");
    free(s);
    /* the thread view: Pid is the tid, Tgid stays the process */
    char path[96];
    snprintf(path, sizeof path, "/proc/self/task/%d/status", th->tid);
    s = slurp(path, &len);
    LT_CHECK(s != NULL);
    if (s) {
        snprintf(want, sizeof want, "\t%d", th->tid);
        LT_STREQ(fline(s, "Pid"), want);
        snprintf(want, sizeof want, "\t%d", T->proc->pid);
        LT_STREQ(fline(s, "Tgid"), want);
    }
    free(s);
    atomic_store(&T->sigmask, 0);
    T->affinity = 0;
    T->proc->cred.uid = T->proc->cred.euid = T->proc->cred.suid = T->proc->cred.fsuid = 1000;
    T->proc->cred.gid = T->proc->cred.egid = T->proc->cred.sgid = T->proc->cred.fsgid = 1000;
    T->proc->cred.ngroups = 1; T->proc->cred.groups[0] = 1000;
    T->proc->cred.cap_eff = T->proc->cred.cap_perm = T->proc->cred.cap_inh = T->proc->cred.cap_amb = 0;
    T->proc->no_new_privs = 0;
}

/* task_mem: the Vm* counters move with the real address space. A 1 MiB mmap adds 1024 kB to VmSize and
 * VmData; locking it adds 1024 kB to VmLck (munlock gives it back). */
LT_TEST(status_vm_counters)
{
    int64_t len;
    char *s = slurp("/proc/self/status", &len);
    uint64_t size0 = sget(s, "VmSize"), data0 = sget(s, "VmData"), lck0 = sget(s, "VmLck");
    LT_CHECK(size0 >= SCRATCH_SZ / 1024);
    free(s);
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
    int64_t a = SC(T, mmap, 0, 1 << 20, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    LT_CHECK(a > 0);
    s = slurp("/proc/self/status", &len);
    LT_EQ(sget(s, "VmSize"), size0 + 1024);
    LT_EQ(sget(s, "VmData"), data0 + 1024);
    LT_EQ(sget(s, "VmPeak") >= sget(s, "VmSize"), 1);
    free(s);
    LT_EQ(SC(T, mlock, (uint64_t)a, 1 << 20), 0);
    s = slurp("/proc/self/status", &len);
    LT_EQ(sget(s, "VmLck"), lck0 + 1024);
    free(s);
    LT_EQ(SC(T, munlock, (uint64_t)a, 1 << 20), 0);
    LT_EQ(SC(T, munmap, (uint64_t)a, 1 << 20), 0);
    s = slurp("/proc/self/status", &len);
    LT_EQ(sget(s, "VmSize"), size0);
    LT_STREQ(fline(s, "THP_enabled"), "\t1");
    LT_STREQ(fline(s, "untag_mask"), "\t0xffffffffffffff");
    free(s);
    /* statm: size in 4 KiB pages */
    s = slurp("/proc/self/statm", &len);
    unsigned long long sz, res, shr, txt, lib, dat, dirty;
    LT_EQ(sscanf(s, "%llu %llu %llu %llu %llu %llu %llu", &sz, &res, &shr, &txt, &lib, &dat, &dirty), 7);
    LT_EQ(sz, size0 / 4);
    LT_EQ(lib, 0);
    LT_EQ(dirty, 0);
    free(s);
}

/* do_task_stat: 52 fields, comm in parentheses, state letter; the numbers come from the task, the mm
 * and the layout the engine reported (start_code ...). */
LT_TEST(stat_fields)
{
    int64_t len;
    char *s = slurp("/proc/self/stat", &len);
    LT_CHECK(s != NULL);
    if (!s) return;
    int ntok = 0;
    for (char *p = strchr(s, ')') + 1; *p;) {   /* tokens after "(comm)" */
        while (*p == ' ') p++;
        if (*p == '\n' || !*p) break;
        ntok++;
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    LT_EQ(ntok + 2, 52);   /* pid and (comm) are fields 1 and 2 */
    unsigned long long f[60];
    char comm[32], st;
    int pos = 0;
    LT_EQ(sscanf(s, "%llu (%31[^)]) %c%n", &f[1], comm, &st, &pos), 3);
    LT_EQ(f[1], T->proc->pid);
    LT_STREQ(comm, "tester");
    LT_EQ(st, 'R');
    const char *p = s + pos;
    int n3 = 0;
    for (int i = 4; i <= 52; i++) { int c = 0; if (sscanf(p, " %llu%n", &f[i], &c) != 1) { if (sscanf(p, " -%llu%n", &f[i], &c) == 1) f[i] = (unsigned long long)-(long long)f[i]; else break; } p += c; n3++; }
    LT_EQ(n3, 49);
    LT_EQ(f[4], 1);                       /* ppid: the registry default (init) */
    LT_EQ(f[5], T->proc->pgid);
    LT_EQ(f[6], T->proc->sid);
    LT_EQ(f[7], 0);                       /* tty_nr: no controlling terminal tracked */
    LT_EQ(f[18], 20);                     /* priority = 20 + nice */
    LT_EQ(f[19], 0);                      /* nice */
    LT_EQ(f[20], 2);                      /* num_threads (status_fields made a thread) */
    LT_EQ(f[21], 0);                      /* itrealvalue is always 0 */
    LT_EQ(f[25], 18446744073709551615ull); /* rsslim: RLIM_INFINITY */
    LT_EQ(f[26], 0x400000);               /* start_code from the layout */
    LT_EQ(f[27], 0x401234);               /* end_code */
    LT_EQ(f[28], 0x7ffffffde000ull);      /* start_stack */
    LT_EQ(f[29], 0);                      /* kstkesp: zeroed like the kernel */
    LT_EQ(f[30], 0);                      /* kstkeip */
    LT_EQ(f[35], 0);                      /* wchan flag: the task runs */
    LT_EQ(f[36], 0);                      /* nswap */
    LT_EQ(f[37], 0);                      /* cnswap */
    LT_EQ(f[38], T->proc->exit_signal);   /* exit_signal */
    LT_CHECK(f[39] < (unsigned)sysconf(_SC_NPROCESSORS_CONF));   /* processor: the caller's CPU */
    LT_EQ(f[45], 0x600000);               /* start_data */
    LT_EQ(f[46], 0x601000);               /* end_data */
    LT_EQ(f[47], 0x602000);               /* start_brk */
    LT_EQ(f[48], 0x7ffffffdf000ull);      /* arg_start */
    LT_EQ(f[49], 0x7ffffffdf01eull);      /* arg_end */
    LT_EQ(f[50], 0x7ffffffdf01eull);      /* env_start */
    LT_EQ(f[51], 0x7ffffffdf032ull);      /* env_end */
    free(s);
    char *st2 = slurp("/proc/self/status", &len);
    LT_EQ(f[23], sget(st2, "VmSize") * 1024);                 /* vsize in bytes */
    LT_EQ(f[24], sget(st2, "VmRSS") * 1024 / 4096);           /* rss in 4 KiB pages */
    free(st2);
    /* starttime is in clock ticks since boot: not in the future, not before the boot */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    LT_CHECK(f[22] <= (unsigned long long)ts.tv_sec * 100 + 100);
}

/* The exact rendering of the format against a stat line recorded on a real Linux kernel
 * (rocket-hyperlinux-20260928/linux-reference/loader-trace/ge-eac-120s-threads.txt:17, kernel 6.x of the
 * reference machine; arm64 host but the file format is architecture independent). Compared: field
 * count, which fields are always zero, shape. Differences explained in docs/status/procfs.md. */
LT_TEST(stat_golden_shape)
{
    static const char golden[] = "17 (wine) S 16 16 1 0 -1 4194560 1186 604 10 4 4 0 0 0 20 0 1 0 34703917 2480574464 1297 "
        "18446744073709551615 140737488515072 140737488983008 281473954612544 0 0 0 268511747 4096 1040 1 0 0 17 5 0 0 "
        "0 0 0 140737489010688 140737490079968 140738377179136 281473954614295 281473954614520 281473954614520 "
        "281473954615231 0\n";
    int64_t len;
    char *s = slurp("/proc/self/stat", &len);
    int gt = 0, ot = 0;
    for (const char *p = golden; *p; p++) if (*p == ' ') gt++;
    for (const char *p = s; *p; p++) if (*p == ' ') ot++;
    LT_EQ(gt, ot);                     /* same number of separators: 52 fields, comm without space */
    LT_EQ(s[len - 1], '\n');
    /* the fields that Linux fixes to 0 are 0 in both: itrealvalue (21), nswap (35), cnswap (36) */
    unsigned long long g[53] = { 0 }, o[53] = { 0 };
    int pg, po;
    char c1[32], c2[32], s1, s2;
    sscanf(golden, "%llu (%31[^)]) %c%n", &g[1], c1, &s1, &pg);
    sscanf(s, "%llu (%31[^)]) %c%n", &o[1], c2, &s2, &po);
    const char *a = golden + pg, *b = s + po;
    for (int i = 4; i <= 52; i++) {
        int ca = 0, cb = 0;
        long long va = 0, vb = 0;
        sscanf(a, " %lld%n", &va, &ca); sscanf(b, " %lld%n", &vb, &cb);
        a += ca; b += cb;
        if (i == 21 || i == 29 || i == 30 || i == 36 || i == 37) { LT_EQ(va, 0); LT_EQ(vb, 0); }
    }
    free(s);
}

/* /proc/self/maps: the VMAs of the real address space in the Linux format (lkd_mm_render_maps); the scratch
 * mapping is in it with its exact range. A read at offset 0 after a change shows the change. */
LT_TEST(maps_and_smaps)
{
    int64_t len;
    char *m = slurp("/proc/self/maps", &len);
    LT_CHECK(m != NULL && len > 0);
    char line[160];
    snprintf(line, sizeof line, "%08llx-%08llx rw-p 00000000 00:00 0", (unsigned long long)(uintptr_t)SB,
             (unsigned long long)(uintptr_t)SB + SCRATCH_SZ);
    LT_CHECK(strstr(m, line) != NULL);
    size_t sz = 0;
    char *want = lkd_mm_render_maps(T->mm, &sz);
    LT_EQ((int64_t)sz, len);
    if (want) LT_MEMEQ(m, want, sz);
    free(want);
    free(m);
    int64_t fd = gopen("/proc/self/maps", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, lseek, (uint64_t)fd, 0, LX_SEEK_END), -LX_EINVAL);      /* seq_lseek: no SEEK_END */
    int64_t a = SC(T, mmap, 0, 1 << 16, LX_PROT_READ, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    LT_CHECK(a > 0);
    LT_CHECK(SC(T, read, (uint64_t)fd, gbulk(), 1 << 20) > 0);          /* generated at the first read */
    LT_EQ(SC(T, lseek, (uint64_t)fd, 0, LX_SEEK_SET), 0);
    int64_t n = SC(T, read, (uint64_t)fd, gbulk(), 1 << 20);            /* offset 0 again: regenerated */
    LT_CHECK(n > 0);
    snprintf(line, sizeof line, "%08llx-%08llx r--p 00000000 00:00 0", (unsigned long long)a, (unsigned long long)a + (1 << 16));
    char *txt = malloc((size_t)n + 1);
    memcpy(txt, (void *)(uintptr_t)gbulk(), (size_t)n);
    txt[n] = 0;
    LT_CHECK(strstr(txt, line) != NULL);
    free(txt);
    SC(T, close, (uint64_t)fd);
    LT_EQ(SC(T, munmap, (uint64_t)a, 1 << 16), 0);
    char *sm = slurp("/proc/self/smaps", &len);
    LT_CHECK(sm && strstr(sm, "Size:") && strstr(sm, "VmFlags:"));
    LT_CHECK(sm && strstr(sm, "KernelPageSize:        4 kB"));
    free(sm);
}

/* The format of maps lines against recorded Linux lines (qemu-neptune-graphics-20260930/.../
 * game-process-maps.txt, a real x86-64 Linux 6.12 guest): same column positions. */
static int maps_line_ok(const char *l, size_t n)
{
    unsigned long long s, e, off, ino;
    unsigned dmaj, dmin;
    char perm[5];
    int pos = 0;
    if (sscanf(l, "%llx-%llx %4s %llx %x:%x %llu%n", &s, &e, perm, &off, &dmaj, &dmin, &ino, &pos) != 7) return 0;
    if (!(strlen(perm) == 4 && (perm[3] == 'p' || perm[3] == 's'))) return 0;
    if (s >= e) return 0;
    if ((size_t)pos >= n || l[pos] != ' ') return 0;     /* a space always follows the inode */
    if ((size_t)pos + 1 == n) return 1;                  /* anonymous: "... 0 " */
    if (n <= 73) return 0;
    for (int i = pos; i < 73; i++) if (l[i] != ' ') return 0;   /* padding: the name starts at offset 73 */
    return l[73] == '/' || l[73] == '[';
}
LT_TEST(maps_golden_columns)
{
    static const char *const golden[] = {
        "00010000-00026000 r--s 00000000 fe:00 1996                               /opt/ge-proton/files/lib/wine/x86_64-windows/apisetschema.dll",
        "00030000-0004b000 rw-p 00000000 00:00 0 ",
        "55558581d000-55558673a000 rw-p 00000000 00:00 0                          [heap]",
        "7febb1d95000-7febb1d97000 r-xp 00000000 00:00 0                          [vdso]",
        "7febb13ee000-7febb140e000 r--p 00000000 00:01 1026                       /memfd:5a9729b5-d83d-5dd7-3b14-3d69f8 (deleted)",
    };
    for (size_t i = 0; i < sizeof golden / sizeof golden[0]; i++) LT_CHECK(maps_line_ok(golden[i], strlen(golden[i])));
    /* a named mapping of ours: the name starts at offset 73 like the recorded lines */
    int64_t a = SC(T, mmap, 0, 1 << 16, LX_PROT_READ, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    LT_CHECK(a > 0);
    lkd_mm_set_vma_name(T->mm, (uint64_t)a, 1 << 16, "[vdso]");
    int64_t len;
    char *m = slurp("/proc/self/maps", &len);
    int named = 0, checked = 0;
    for (char *p = m; p && *p;) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char tmp[512];
        if (n < sizeof tmp) {
            memcpy(tmp, p, n); tmp[n] = 0;
            LT_CHECK(maps_line_ok(tmp, n));
            checked++;
            if (strstr(tmp, "[vdso]")) { named++; LT_EQ(tmp[73], '['); }
        }
        p = nl ? nl + 1 : NULL;
    }
    LT_CHECK(checked >= 2);
    LT_EQ(named, 1);
    free(m);
    SC(T, munmap, (uint64_t)a, 1 << 16);
}

/* Golden comparison of status against a status recorded on real Linux
 * (rocket-hyperlinux-20260928/linux-reference/pid1-control/pid1-before.txt, kernel 7.0 OrbStack x86-64):
 * the same keys in the same order. Our status omits exactly Seccomp and Seccomp_filters (the modeled
 * kernel has no CONFIG_SECCOMP) and the line widths of the Vm* fields are the same. */
LT_TEST(status_golden_keys)
{
    static const char *const gkeys[] = { "Name", "Umask", "State", "Tgid", "Ngid", "Pid", "PPid", "TracerPid", "Uid", "Gid",
        "FDSize", "Groups", "NStgid", "NSpid", "NSpgid", "NSsid", "Kthread", "VmPeak", "VmSize", "VmLck", "VmPin", "VmHWM",
        "VmRSS", "RssAnon", "RssFile", "RssShmem", "VmData", "VmStk", "VmExe", "VmLib", "VmPTE", "VmSwap", "CoreDumping",
        "THP_enabled", "untag_mask", "Threads", "SigQ", "SigPnd", "ShdPnd", "SigBlk", "SigIgn", "SigCgt", "CapInh", "CapPrm",
        "CapEff", "CapBnd", "CapAmb", "NoNewPrivs", "Seccomp", "Seccomp_filters", "Speculation_Store_Bypass",
        "SpeculationIndirectBranch", "Cpus_allowed", "Cpus_allowed_list", "Mems_allowed", "Mems_allowed_list",
        "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches" };
    int64_t len;
    char *s = slurp("/proc/self/status", &len);
    size_t gi = 0;
    int missing = 0, order_ok = 1;
    int seen_missing[2] = { 0, 0 };
    for (size_t k = 0; k < sizeof gkeys / sizeof gkeys[0]; k++) {
        char key[64];
        snprintf(key, sizeof key, "%s:\t", gkeys[k]);
        char *at = strstr(s, key);
        int is_start = at && (at == s || at[-1] == '\n');
        if (!is_start) {
            missing++;
            if (!strcmp(gkeys[k], "Seccomp")) seen_missing[0] = 1;
            if (!strcmp(gkeys[k], "Seccomp_filters")) seen_missing[1] = 1;
            continue;
        }
        size_t pos = (size_t)(at - s);
        if (pos < gi) order_ok = 0;
        gi = pos;
    }
    LT_EQ(missing, 2);
    LT_CHECK(seen_missing[0] && seen_missing[1]);
    LT_CHECK(order_ok);
    /* widths: "VmRSS:\t     3708 kB" is the recorded shape; ours has the same length for a value < 1e8 kB */
    static const char *const golden_vm = "VmRSS:\t    3708 kB";
    const char *l = strstr(s, "VmRSS:\t");
    char ours[64];
    size_t n = (size_t)(strchr(l, '\n') - l);
    memcpy(ours, l, n); ours[n] = 0;
    LT_EQ((int64_t)n, (int64_t)strlen(golden_vm) + 0);
    /* hex sets are 16 digits like "SigIgn:\t0000000200000000" */
    LT_EQ(strlen(fline(s, "SigIgn")), 17);
    LT_EQ(strlen(fline(s, "CapEff")), 17);
    free(s);
}

static int64_t getdents_names(const char *path, char names[][32], int max)
{
    int64_t fd = gopen(path, LX_O_RDONLY | LX_O_DIRECTORY);
    if (fd < 0) return fd;
    int n = 0;
    for (;;) {
        int64_t r = SC(T, getdents64, (uint64_t)fd, gbulk(), 32768);
        if (r <= 0) { SC(T, close, (uint64_t)fd); return r < 0 ? r : n; }
        uint8_t *p = (uint8_t *)(uintptr_t)gbulk();
        for (int64_t off = 0; off < r;) {
            uint16_t reclen;
            memcpy(&reclen, p + off + 16, 2);
            const char *nm = (const char *)(p + off + 19);
            if (n < max) snprintf(names[n], 32, "%s", nm);
            n++;
            off += reclen;
        }
    }
}

/* fd links: readlink gives the object (fs/proc/fd.c, d_path / anon inode names); open() of the link is a
 * NEW open file description with its own offset (the memfd dlopen path of the EAC module). */
LT_TEST(fd_links_and_reopen)
{
    int64_t mfd = SC(T, memfd_create, gstr("wine-mapping"), LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING);
    LT_CHECK(mfd >= 0);
    memcpy((void *)(uintptr_t)gbulk(), "\177ELFpayload", 11);
    LT_EQ(SC(T, write, (uint64_t)mfd, gbulk(), 11), 11);
    char link[128], path[64], buf[256];
    snprintf(path, sizeof path, "/proc/self/fd/%d", (int)mfd);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_STREQ(buf, "/memfd:wine-mapping (deleted)");
    struct lx_stat st;
    LT_EQ(glstat(path, &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    LT_EQ(st.st_mode & 0777, 0700);                 /* read-write descriptor: fs/proc/fd.c proc_fd_instantiate */
    LT_EQ(gstat(path, &st), 0);                     /* followed: the memfd itself */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_EQ(st.st_size, 11);
    /* open("/proc/self/fd/N"): independent offset 0, same content */
    int64_t r = gopen(path, LX_O_RDONLY);
    LT_CHECK(r >= 0);
    LT_EQ(SC(T, read, (uint64_t)r, gbulk(), 64), 11);
    LT_MEMEQ((void *)(uintptr_t)gbulk(), "\177ELFpayload", 11);
    LT_EQ(SC(T, lseek, (uint64_t)mfd, 0, LX_SEEK_CUR), 11);   /* the original position is untouched */
    SC(T, close, (uint64_t)r);
    /* fdinfo: pos, flags in octal with O_CLOEXEC, mnt_id, ino (fs/proc/fd.c seq_show) */
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", (int)mfd);
    int64_t len;
    char *fi = slurp(path, &len);
    LT_CHECK(fi != NULL);
    if (fi) {
        LT_STREQ(fline(fi, "pos"), "\t11");
        LT_STREQ(fline(fi, "flags"), "\t02100002");   /* O_CLOEXEC | O_LARGEFILE | O_RDWR, octal */
        LT_CHECK(field(fi, "mnt_id") != NULL);
        LT_CHECK(field(fi, "ino") != NULL);
        LT_EQ(gstat("/proc/self/fdinfo", &st), 0);
        LT_EQ(st.st_mode & 0777, 0555);
    }
    free(fi);
    /* pipes and sockets: pipe:[ino], the inode number is the one fstat reports */
    int64_t pr = SC(T, pipe2, gbulk() + 4096, 0);
    LT_EQ(pr, 0);
    int pfds[2];
    memcpy(pfds, (void *)(uintptr_t)(gbulk() + 4096), sizeof pfds);
    snprintf(path, sizeof path, "/proc/self/fd/%d", pfds[0]);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_EQ(SC(T, fstat, (uint64_t)pfds[0], gbulk() + 8192), 0);
    struct lx_stat pst;
    memcpy(&pst, (void *)(uintptr_t)(gbulk() + 8192), sizeof pst);
    snprintf(link, sizeof link, "pipe:[%llu]", (unsigned long long)pst.st_ino);
    LT_STREQ(buf, link);
    snprintf(path, sizeof path, "/proc/self/fd/%d", pfds[1]);
    LT_EQ(glstat(path, &st), 0);
    LT_EQ(st.st_mode & 0777, 0300);                 /* write-only end */
    /* directory listing: ascending numbers, the same set as the descriptor table */
    static char names[256][32];
    int64_t cnt = getdents_names("/proc/self/fd", names, 256);
    int fds[256];
    int expect = lkd_fd_list(T, fds, 256);
    LT_CHECK(cnt >= 0);
    int ndots = 0;
    for (int i = 0; i < cnt && i < 256; i++) if (!strcmp(names[i], ".") || !strcmp(names[i], "..")) ndots++;
    LT_EQ(cnt - ndots, expect + 1);       /* + the directory descriptor that was open while listing */
    int ok = 1, prev = -1, found = 0;
    for (int i = 0; i < cnt && i < 256; i++) {
        if (!strcmp(names[i], ".") || !strcmp(names[i], "..")) continue;
        int v = atoi(names[i]);
        if (v <= prev) ok = 0;            /* ascending */
        prev = v;
        for (int k = 0; k < expect; k++) if (fds[k] == v) found++;
    }
    LT_CHECK(ok);
    LT_EQ(found, expect);
    LT_ERRNO(rdlink("/proc/self/fd/9999", buf, sizeof buf), LX_ENOENT);
    SC(T, close, (uint64_t)mfd); SC(T, close, (uint64_t)pfds[0]); SC(T, close, (uint64_t)pfds[1]);
}

/* cwd and root links (proc_cwd_link, proc_root_link): the guest path of the task. */
LT_TEST(cwd_root_links)
{
    char buf[256];
    LT_EQ(rdlink("/proc/self/root", buf, sizeof buf), 1);
    LT_STREQ(buf, "/");
    LT_EQ(SC(T, chdir, gstr("/proc/self")), 0);
    char want[64];
    snprintf(want, sizeof want, "/proc/%d", T->proc->pid);
    LT_CHECK(rdlink("/proc/self/cwd", buf, sizeof buf) > 0);
    LT_STREQ(buf, want);
    /* open() of the link opens the directory itself */
    int64_t d = gopen("/proc/self/cwd", LX_O_RDONLY | LX_O_DIRECTORY);
    LT_CHECK(d >= 0);
    SC(T, close, (uint64_t)d);
    LT_EQ(SC(T, chdir, gstr("/")), 0);
    LT_EQ(rdlink("/proc/self/cwd", buf, sizeof buf), 1);
    LT_STREQ(buf, "/");
}

/* /proc/self/mem: reads and writes the own address space (mem_rw, FOLL_FORCE); an unmapped address is
 * -EIO; SEEK_END is -EINVAL (mem_lseek). */
LT_TEST(mem_file)
{
    int64_t fd = gopen("/proc/self/mem", LX_O_RDWR);
    LT_CHECK(fd >= 0);
    uint8_t *spot = SB + 70000;
    memcpy(spot, "hello-mem", 9);
    LT_EQ(SC(T, pread64, (uint64_t)fd, gbulk() + 1000, 9, (uint64_t)(uintptr_t)spot), 9);
    LT_MEMEQ((void *)(uintptr_t)(gbulk() + 1000), "hello-mem", 9);
    memcpy((void *)(uintptr_t)(gbulk() + 2000), "WRITTEN!!", 9);
    LT_EQ(SC(T, pwrite64, (uint64_t)fd, gbulk() + 2000, 9, (uint64_t)(uintptr_t)spot), 9);
    LT_MEMEQ(spot, "WRITTEN!!", 9);
    LT_EQ(SC(T, lseek, (uint64_t)fd, (uint64_t)(uintptr_t)spot, LX_SEEK_SET), (int64_t)(uintptr_t)spot);
    LT_EQ(SC(T, read, (uint64_t)fd, gbulk() + 3000, 4), 4);
    LT_MEMEQ((void *)(uintptr_t)(gbulk() + 3000), "WRIT", 4);
    LT_ERRNO(SC(T, lseek, (uint64_t)fd, 0, LX_SEEK_END), LX_EINVAL);
    LT_ERRNO(SC(T, pread64, (uint64_t)fd, gbulk() + 1000, 8, 0x10), LX_EIO);   /* unmapped */
    SC(T, close, (uint64_t)fd);
    int64_t ro = gopen("/proc/self/mem", LX_O_RDONLY);
    LT_CHECK(ro >= 0);
    LT_ERRNO(SC(T, pwrite64, (uint64_t)ro, gbulk() + 2000, 4, (uint64_t)(uintptr_t)spot), LX_EBADF);
    SC(T, close, (uint64_t)ro);
}

/* limits: header and rows exactly as proc_pid_limits prints them. */
LT_TEST(limits_format)
{
    int64_t len;
    char *s = slurp("/proc/self/limits", &len);
    LT_CHECK(s != NULL);
    if (!s) return;
    static const char hdr[] = "Limit                     Soft Limit           Hard Limit           Units     \n";
    LT_CHECK(!strncmp(s, hdr, sizeof hdr - 1));
    LT_CHECK(strstr(s, "Max cpu time              unlimited            unlimited            seconds   \n") != NULL);
    LT_CHECK(strstr(s, "Max nice priority         0                    0                    \n") != NULL);
    int lines = 0;
    for (char *p = s; *p; p++) if (*p == '\n') lines++;
    LT_EQ(lines, 17);
    T->proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur = 1234;
    T->proc->rlimits[LX_RLIMIT_NOFILE].rlim_max = 4321;
    free(s);
    s = slurp("/proc/self/limits", &len);
    LT_CHECK(strstr(s, "Max open files            1234                 4321                 files     \n") != NULL);
    free(s);
}

/* syscall, wchan, cgroup, personality, mountinfo: honest minimal answers. */
LT_TEST(small_files)
{
    int64_t len;
    char *s = slurp("/proc/self/syscall", &len);
    /* the read in flight: nr 0 and six hex arguments, then sp and pc (proc_pid_syscall) */
    LT_CHECK(s && !strncmp(s, "0 0x", 4));
    int words = 0;
    for (char *p = s; p && *p && *p != '\n'; p++) if (*p == ' ') words++;
    LT_EQ(words, 8);
    free(s);
    s = slurp("/proc/self/wchan", &len);
    LT_STREQ(s, "0");
    free(s);
    s = slurp("/proc/self/cgroup", &len);
    LT_STREQ(s, "0::/\n");
    free(s);
    s = slurp("/proc/self/personality", &len);
    LT_STREQ(s, "00000000\n");
    free(s);
    s = slurp("/proc/self/mounts", &len);
    LT_CHECK(s && strstr(s, " /proc proc ") != NULL && strstr(s, " /dev devtmpfs ") != NULL && strstr(s, " /sys sysfs ") != NULL);
    free(s);
    s = slurp("/proc/self/mountinfo", &len);
    LT_CHECK(s && strstr(s, " /proc ") != NULL && strstr(s, " - proc proc ") != NULL);
    free(s);
    s = slurp("/proc/self/net/dev", &len);
    LT_CHECK(s && !strncmp(s, "Inter-|   Receive", 17));
    free(s);
    s = slurp("/proc/self/net/route", &len);   /* fib_route_seq_show: lines of 127 columns, the header first */
    LT_CHECK(s && len >= 128 && len % 128 == 0 && !strncmp(s, "Iface\tDestination\tGateway \tFlags\t", 33) && s[127] == '\n');
    free(s);
    s = slurp("/proc/self/net/ipv6_route", &len);   /* rt6_info_route: lo carries ::1/128 */
    LT_CHECK(s && strstr(s, "00000000000000000000000000000001 80 00000000000000000000000000000000 00 ") != NULL &&
             strstr(s, " 00000100 00000001 00000000 00000001       lo\n") != NULL);
    free(s);
    LT_ERRNO(gopen("/proc/self/nonexistent", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/proc/self/task/999999/status", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/proc/01/status", LX_O_RDONLY), LX_ENOENT);              /* leading zero: not a number */
    LT_ERRNO(gopen("/proc/self", LX_O_RDWR), LX_EISDIR);
}

/* Owner and mode of the nodes follow task_dump_owner: a non-dumpable process (prctl PR_SET_DUMPABLE 0)
 * owns its files as root, its directory stays with euid; the EAC child sets this before PTRACE_TRACEME. */
LT_TEST(dumpable_owner_and_viewer_access)
{
    struct lx_stat st;
    char path[96];
    snprintf(path, sizeof path, "/proc/%d", T->proc->pid);
    LT_EQ(gstat("/proc/self/status", &st), 0);
    LT_EQ(st.st_uid, 1000);
    LT_EQ(st.st_mode & 0777, 0444);
    LT_EQ(SC(T, prctl, LX_PR_SET_DUMPABLE, 0), 0);
    lkd_registry_publish(T);
    LT_EQ(gstat("/proc/self/status", &st), 0);
    LT_EQ(st.st_uid, 0);
    LT_EQ(st.st_gid, 0);
    LT_EQ(gstat("/proc/self", &st), 0);
    LT_EQ(st.st_uid, 1000);                              /* the 0555 directory keeps euid */
    LT_EQ(gstat("/proc/self/maps", &st), 0);
    LT_EQ(st.st_uid, 0);
    /* another process of the same user (a viewer): status is world readable, maps and mem need
     * ptrace_may_access, which refuses a non-dumpable target (kernel/ptrace.c __ptrace_may_access) */
    lkd_task *V = fake_make_task();
    V->fdt = lkd_fdtable_new();
    V->fsctx = lkd_fsctx_new();
    lkd_set_current(V);
    int64_t len;
    char spath[128];
    snprintf(spath, sizeof spath, "%s/status", path);
    char *s = slurp_as(V, spath, &len);
    LT_CHECK(s != NULL);
    free(s);
    snprintf(spath, sizeof spath, "%s/maps", path);
    s = slurp_as(V, spath, &len);
    LT_ERRNO(len, LX_EACCES);
    snprintf(spath, sizeof spath, "%s/environ", path);
    s = slurp_as(V, spath, &len);
    LT_ERRNO(len, LX_EACCES);
    snprintf(spath, sizeof spath, "%s/mem", path);
    lkd_set_current(V);
    LT_ERRNO(SC(V, openat, AT_CWD, gstr(spath), LX_O_RDONLY, 0), LX_EACCES);
    char buf[128];
    snprintf(spath, sizeof spath, "%s/exe", path);
    LT_ERRNO(rdlink_as(V, spath, buf, sizeof buf), LX_EACCES);           /* proc_fd_access_allowed */
    /* dumpable again (exec resets it): maps readable by the same user */
    lkd_set_current(T);
    LT_EQ(SC(T, prctl, LX_PR_SET_DUMPABLE, 1), 0);
    lkd_registry_publish(T);
    snprintf(spath, sizeof spath, "%s/maps", path);
    s = slurp_as(V, spath, &len);
    LT_CHECK(s != NULL && len > 0);
    free(s);
    snprintf(spath, sizeof spath, "%s/mem", path);
    lkd_set_current(V);
    int64_t mfd = SC(V, openat, AT_CWD, gstr(spath), LX_O_RDONLY, 0);
    LT_CHECK(mfd >= 0);
    memcpy(SB + 80000, "peer-visible", 12);
    LT_EQ(SC(V, pread64, (uint64_t)mfd, gbulk() + 5000, 12, (uint64_t)(uintptr_t)(SB + 80000)), 12);
    LT_MEMEQ((void *)(uintptr_t)(gbulk() + 5000), "peer-visible", 12);
    SC(V, close, (uint64_t)mfd);
    /* a viewer with another uid cannot read it either, whatever dumpable says */
    V->proc->cred.uid = V->proc->cred.euid = V->proc->cred.suid = V->proc->cred.fsuid = 4242;
    snprintf(spath, sizeof spath, "%s/maps", path);
    s = slurp_as(V, spath, &len);
    LT_ERRNO(len, LX_EACCES);
    lkd_set_current(T);
}

int main(void)
{
    procfs_fixture();
    LT_RUN(self_links);
    LT_RUN(exe_link);
    LT_RUN(cmdline_environ_auxv);
    LT_RUN(comm_read_write);
    LT_RUN(status_fields);
    LT_RUN(status_vm_counters);
    LT_RUN(stat_fields);
    LT_RUN(stat_golden_shape);
    LT_RUN(maps_and_smaps);
    LT_RUN(maps_golden_columns);
    LT_RUN(status_golden_keys);
    LT_RUN(fd_links_and_reopen);
    LT_RUN(cwd_root_links);
    LT_RUN(mem_file);
    LT_RUN(limits_format);
    LT_RUN(small_files);
    LT_RUN(dumpable_owner_and_viewer_access);
    return lt_finish();
}

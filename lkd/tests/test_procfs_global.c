/* test_procfs_global.c: /proc/cpuinfo, meminfo, uptime, loadavg, version, stat, filesystems, /proc/sys,
 * /sys/devices/system/cpu and /dev. Expected values are literals or are measured on the host through
 * sysctl, never recomputed with the code under test.
 * Linux models (v6.12): arch/x86/kernel/cpu/proc.c show_cpuinfo, fs/proc/meminfo.c, fs/proc/uptime.c,
 * fs/proc/loadavg.c, fs/proc/stat.c, fs/proc/version.c, kernel/sysctl.c, drivers/base/cpu.c,
 * drivers/base/topology.c, drivers/base/cacheinfo.c, drivers/char/mem.c, drivers/char/random.c. */
#include <sys/sysctl.h>

#include "test_procfs_util.h"
#include "../src/procfs/procfs_priv.h"

static int host_ncpu(void)
{
    int n = 0;
    size_t sz = sizeof n;
    sysctlbyname("hw.logicalcpu", &n, &sz, NULL, 0);
    return n;
}

/* A CPUID provider with a model that exercises every translated field (literal values below). */
static int fake_cpuid(uint32_t leaf, uint32_t sub, int cpu, struct lkd_cpuid_regs *r)
{
    memset(r, 0, sizeof *r);
    switch (leaf) {
    case 0: r->eax = 0x16; memcpy(&r->ebx, "Genu", 4); memcpy(&r->edx, "ineI", 4); memcpy(&r->ecx, "ntel", 4); return 0;
    case 1:
        r->eax = 0x000A0661;                      /* stepping 1, model 6, family 6, extended model 0xA */
        r->ebx = (8u << 8) | ((uint32_t)host_ncpu() << 16) | ((uint32_t)cpu << 24);
        r->ecx = (1u << 0) | (1u << 19) | (1u << 31);   /* pni sse4_1 hypervisor */
        r->edx = (1u << 0) | (1u << 4) | (1u << 25) | (1u << 26);   /* fpu tsc sse sse2 */
        return 0;
    case 4:
        if (sub == 0) { r->eax = 1 | (1 << 5); r->ebx = 63 | (7u << 22); r->ecx = 63; return 0; }                 /* L1 data 32K, 8 ways */
        if (sub == 1) { r->eax = 3 | (2 << 5) | (3u << 14); r->ebx = 63 | (15u << 22); r->ecx = 2047; return 0; }  /* L2 unified 2M, 4 cpus */
        return 0;
    case 7: r->ebx = 1u << 5; return 0;           /* avx2 */
    case 0x15: r->eax = 2; r->ebx = 125; r->ecx = 24000000; return 0;       /* 24e6 * 125 / 2 = 1.5 GHz */
    case 0x80000001u: r->edx = (1u << 11) | (1u << 20) | (1u << 29); return 0;   /* syscall nx lm */
    case 0x80000002u: snprintf((char *)r, 16, "Test CPU %d", cpu); return 0;
    case 0x80000007u: r->edx = 1u << 8; return 0;   /* invariant TSC */
    case 0x80000008u: r->eax = 40 | (48u << 8); return 0;
    }
    return 0;
}

LT_TEST(cpuinfo_from_cpuid)
{
    lkd_procfs_set_cpuid(NULL);
    LT_ERRNO(gopen("/proc/cpuinfo", LX_O_RDONLY), LX_ENOSYS);        /* no engine: visible error, not an invented CPU */
    lkd_procfs_set_cpuid(fake_cpuid);
    int64_t len;
    char *s = slurp("/proc/cpuinfo", &len);
    LT_CHECK(s != NULL);
    if (!s) return;
    int n = host_ncpu(), blocks = 0;
    for (char *p = s; (p = strstr(p, "processor\t: ")); p++) blocks++;
    LT_EQ(blocks, n);                                                /* the true CPU count of the Mac */
    static const char want0[] =
        "processor\t: 0\n"
        "vendor_id\t: GenuineIntel\n"
        "cpu family\t: 6\n"
        "model\t\t: 166\n"                                           /* 6 + (0xA << 4) */
        "model name\t: Test CPU 0\n"
        "stepping\t: 1\n"
        "cpu MHz\t\t: 1500.000\n"
        "cache size\t: 2048 KB\n";
    LT_CHECK(!strncmp(s, want0, sizeof want0 - 1));
    LT_CHECK(strstr(s, "flags\t\t: fpu tsc sse sse2 syscall nx lm constant_tsc art rep_good nopl xtopology nonstop_tsc cpuid "
                       "tsc_known_freq pni sse4_1 hypervisor avx2\nbugs\t\t:\nbogomips\t: 3000.00\n") != NULL);
    LT_CHECK(strstr(s, "clflush size\t: 64\ncache_alignment\t: 64\naddress sizes\t: 40 bits physical, 48 bits virtual\n"
                       "power management:\n\n") != NULL);
    char tail[64];
    snprintf(tail, sizeof tail, "siblings\t: %d\ncore id\t\t: 0\ncpu cores\t: %d\n", n, n);
    LT_CHECK(strstr(s, tail) != NULL);
    LT_CHECK(strstr(s, "fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: 22\nwp\t\t: yes\n") != NULL);
    char last[48];
    snprintf(last, sizeof last, "processor\t: %d\n", n - 1);
    LT_CHECK(strstr(s, last) != NULL);
    snprintf(last, sizeof last, "model name\t: Test CPU %d\n", n - 1);   /* the brand string is per CPU */
    LT_CHECK(strstr(s, last) != NULL);
    LT_CHECK(strstr(s, "microcode") == NULL);                        /* unknown on this machine: not printed */
    /* each line the guest can compute from CPUID agrees with CPUID itself */
    struct lkd_cpuid_regs r;
    fake_cpuid(1, 0, 0, &r);
    LT_CHECK(((r.ecx >> 31) & 1) && strstr(s, " hypervisor") != NULL);   /* the CPUID hypervisor bit is shown, not hidden */
    free(s);
}

LT_TEST(meminfo_uptime_loadavg)
{
    int64_t len;
    char *s = slurp("/proc/meminfo", &len);
    uint64_t total = 0;
    size_t sz = sizeof total;
    sysctlbyname("hw.memsize", &total, &sz, NULL, 0);
    LT_CHECK(s != NULL);
    unsigned long long mt = 0, mf = 0, ma = 0, st = 0, sf = 0;
    LT_EQ(sscanf(field(s, "MemTotal"), " %llu", &mt), 1);
    LT_EQ(mt, total / 1024);
    LT_EQ(sscanf(field(s, "MemFree"), " %llu", &mf), 1);
    LT_EQ(sscanf(field(s, "MemAvailable"), " %llu", &ma), 1);
    LT_CHECK(mf > 0 && mf <= mt && ma >= mf && ma <= mt);
    LT_EQ(sscanf(field(s, "SwapTotal"), " %llu", &st), 1);
    LT_EQ(sscanf(field(s, "SwapFree"), " %llu", &sf), 1);
    LT_CHECK(sf <= st);
    LT_CHECK(!strncmp(s, "MemTotal:       ", 16));
    LT_CHECK(strstr(s, " kB\n") != NULL);
    free(s);
    /* uptime: "%lu.%02lu %lu.%02lu", the same clock as clock_gettime(CLOCK_BOOTTIME) */
    s = slurp("/proc/uptime", &len);
    double up, idle;
    LT_EQ(sscanf(s, "%lf %lf", &up, &idle), 2);
    LT_EQ(SC(T, clock_gettime, LX_CLOCK_BOOTTIME, gbulk() + 4096), 0);
    int64_t ts[2];
    memcpy(ts, (void *)(uintptr_t)(gbulk() + 4096), sizeof ts);
    double bt_clock = (double)ts[0] + (double)ts[1] / 1e9;
    LT_CHECK(up > 0 && up <= bt_clock + 0.01 && up >= bt_clock - 2.0);
    struct timeval bt;
    sz = sizeof bt;
    sysctlbyname("kern.boottime", &bt, &sz, NULL, 0);
    struct timeval now;
    gettimeofday(&now, NULL);
    LT_CHECK(up >= (double)(now.tv_sec - bt.tv_sec) - 120 && up <= (double)(now.tv_sec - bt.tv_sec) + 120);   /* sanity vs the host */
    LT_CHECK(idle >= 0);
    LT_EQ(s[len - 1], '\n');
    free(s);
    s = slurp("/proc/loadavg", &len);
    double l1, l5, l15;
    int run, tot, last;
    LT_EQ(sscanf(s, "%lf %lf %lf %d/%d %d", &l1, &l5, &l15, &run, &tot, &last), 6);
    double la[3];
    getloadavg(la, 3);
    LT_CHECK(l1 >= 0 && l1 < la[0] * 4 + 2);
    LT_CHECK(tot >= 1 && run >= 0 && last >= T->proc->pid);
    free(s);
}

LT_TEST(version_stat_filesystems)
{
    int64_t len;
    char *s = slurp("/proc/version", &len);
    LT_CHECK(s && !strncmp(s, "Linux version 6.12.0-lkd (", 26));
    /* consistent with uname(2) */
    int64_t r = SC(T, uname, gbulk() + 4096);
    LT_EQ(r, 0);
    struct lx_utsname u;
    memcpy(&u, (void *)(uintptr_t)(gbulk() + 4096), sizeof u);
    LT_CHECK(strstr(s, u.release) != NULL && strstr(s, u.version) != NULL);
    free(s);
    s = slurp("/proc/sys/kernel/osrelease", &len);
    char want[128];
    snprintf(want, sizeof want, "%s\n", u.release);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/proc/sys/kernel/ostype", &len);
    LT_STREQ(s, "Linux\n");
    free(s);
    s = slurp("/proc/sys/kernel/hostname", &len);
    snprintf(want, sizeof want, "%s\n", u.nodename);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/proc/sys/kernel/version", &len);
    snprintf(want, sizeof want, "%s\n", u.version);
    LT_STREQ(s, want);
    free(s);
    /* /proc/stat */
    s = slurp("/proc/stat", &len);
    int n = host_ncpu(), lines = 0;
    for (char *p = s; (p = strstr(p, "\ncpu")) != NULL; p++) lines++;
    LT_CHECK(!strncmp(s, "cpu  ", 5));
    LT_EQ(lines, n);                                                  /* "cpuN" lines after the first */
    const char *bts = strstr(s, "\nbtime ");
    LT_CHECK(bts != NULL);
    long long btime = bts ? atoll(bts + 7) : 0;
    long long nowsec = (long long)time(NULL);
    char *up2 = slurp("/proc/uptime", &len);
    double upn = atof(up2);
    free(up2);
    LT_CHECK(llabs(btime - (nowsec - (long long)upn)) <= 2);   /* btime + uptime = now */
    LT_CHECK(strstr(s, "procs_running ") != NULL);
    /* processes: tasks created since boot, between btime and procs_running like fs/proc/stat.c (request
     * procfs-4 point 7); at least the test's own task */
    const char *pr = strstr(s, "\nprocesses ");
    LT_CHECK(pr != NULL && pr > bts && pr < strstr(s, "procs_running "));
    LT_CHECK(pr && atoll(pr + 11) >= 1);
    LT_CHECK(strstr(s, "\nintr ") == NULL && strstr(s, "\nctxt ") == NULL);   /* not measurable on Darwin: absent */
    free(s);
    s = slurp("/proc/filesystems", &len);
    LT_CHECK(s && strstr(s, "nodev\tproc\n") && strstr(s, "nodev\tsysfs\n") && strstr(s, "nodev\tdevtmpfs\n"));
    free(s);
}

LT_TEST(proc_sys_values)
{
    int64_t len;
    char *s = slurp("/proc/sys/vm/mmap_min_addr", &len);
    LT_STREQ(s, "65536\n");
    free(s);
    /* consistency with the guest window: mmap below mmap_min_addr is refused (security_mmap_addr) */
    LT_ERRNO(SC(T, mmap, 0x8000, 0x1000, LX_PROT_READ, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, (uint64_t)-1, 0), LX_EPERM);
    s = slurp("/proc/sys/vm/max_map_count", &len);
    LT_STREQ(s, "1048576\n");
    free(s);
    s = slurp("/proc/sys/vm/overcommit_memory", &len);
    LT_STREQ(s, "1\n");
    free(s);
    s = slurp("/proc/sys/kernel/randomize_va_space", &len);
    LT_STREQ(s, "0\n");                                               /* the mm module has no ASLR */
    free(s);
    s = slurp("/proc/sys/kernel/pid_max", &len);
    LT_EQ(atoll(s), (long long)LKD_PID_MAX + 1);
    free(s);
    char *u1 = slurp("/proc/sys/kernel/random/uuid", &len);
    char *u2 = slurp("/proc/sys/kernel/random/uuid", &len);
    LT_EQ(len, 37);
    LT_CHECK(u1[8] == '-' && u1[13] == '-' && u1[18] == '-' && u1[23] == '-' && u1[14] == '4');   /* version 4 */
    LT_CHECK(strchr("89ab", u1[19]) != NULL);
    LT_CHECK(strcmp(u1, u2) != 0);                                    /* a new one at every read */
    free(u1); free(u2);
    char *b1 = slurp("/proc/sys/kernel/random/boot_id", &len);
    char *b2 = slurp("/proc/sys/kernel/random/boot_id", &len);
    LT_EQ(len, 37);
    LT_STREQ(b1, b2);                                                 /* fixed for the boot */
    free(b1); free(b2);
    /* inotify limits exist exactly when the net module serves inotify */
    int present = lkd_syscall_owner(LKD_NR_inotify_init1) != NULL;
    int64_t fd = gopen("/proc/sys/fs/inotify/max_user_instances", LX_O_RDONLY);
    LT_EQ(fd >= 0, present);
    if (fd >= 0) SC(T, close, (uint64_t)fd);
    if (present) {
        /* the values are the limits net enforces (inotify.c: 128 instances, 8192 watches, 16384 events),
         * not the Linux defaults (65536 watches): request procfs-4 point 3 */
        s = slurp("/proc/sys/fs/inotify/max_user_watches", &len);
        LT_STREQ(s, "8192\n");
        free(s);
        s = slurp("/proc/sys/fs/inotify/max_user_instances", &len);
        LT_STREQ(s, "128\n");
        free(s);
        s = slurp("/proc/sys/fs/inotify/max_queued_events", &len);
        LT_STREQ(s, "16384\n");
        free(s);
    }
    s = slurp("/proc/sys/fs/pipe-max-size", &len);
    LT_STREQ(s, "65536\n");
    free(s);
    /* the files are root-owned 0644: an unprivileged writer is refused at open, a root writer is refused by LKD */
    struct lx_stat st;
    LT_EQ(gstat("/proc/sys/kernel/hostname", &st), 0);
    LT_EQ(st.st_mode, LX_S_IFREG | 0644);
    LT_EQ(st.st_uid, 0);
    LT_ERRNO(gopen("/proc/sys/kernel/hostname", LX_O_WRONLY), LX_EACCES);
    LT_ERRNO(gopen("/proc/sys/kernel/nonexistent", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/proc/sys/kernel/yama/ptrace_scope", LX_O_RDONLY), LX_ENOENT);   /* no Yama in the modeled kernel */
    LT_EQ(gstat("/proc/sys/kernel", &st), 0);
    LT_EQ(st.st_mode, LX_S_IFDIR | 0555);
}

static int64_t listing(const char *path, char names[][32], int max)
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
            if (n < max) snprintf(names[n], 32, "%s", (const char *)(p + off + 19));
            n++;
            off += reclen;
        }
    }
}
static int has(char names[][32], int n, const char *w)
{
    for (int i = 0; i < n && i < 128; i++) if (!strcmp(names[i], w)) return 1;
    return 0;
}

LT_TEST(sys_cpu_tree)
{
    lkd_procfs_set_cpuid(fake_cpuid);
    int n = host_ncpu();
    int64_t len;
    char want[64];
    char *s = slurp("/sys/devices/system/cpu/online", &len);
    snprintf(want, sizeof want, "0-%d\n", n - 1);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/sys/devices/system/cpu/possible", &len);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/sys/devices/system/cpu/present", &len);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/sys/devices/system/cpu/offline", &len);
    LT_STREQ(s, "\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/kernel_max", &len);
    snprintf(want, sizeof want, "%d\n", n - 1);
    LT_STREQ(s, want);
    free(s);
    static char names[160][32];
    int c = (int)listing("/sys/devices/system/cpu", names, 160);
    LT_CHECK(c >= n + 5);
    LT_CHECK(has(names, c, "cpu0") && has(names, c, "online"));
    snprintf(want, sizeof want, "cpu%d", n - 1);
    LT_CHECK(has(names, c, want));
    snprintf(want, sizeof want, "cpu%d", n);
    LT_CHECK(!has(names, c, want));
    struct lx_stat st;
    LT_EQ(gstat("/sys/devices/system/cpu/cpu0", &st), 0);
    LT_EQ(st.st_mode, LX_S_IFDIR | 0755);
    LT_ERRNO(gstat(want, &st), LX_ENOENT);
    /* topology: flat, one thread per core, one package (the same view as /proc/cpuinfo) */
    s = slurp("/sys/devices/system/cpu/cpu3/topology/core_id", &len);
    LT_STREQ(s, "3\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu3/topology/physical_package_id", &len);
    LT_STREQ(s, "0\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu3/topology/thread_siblings_list", &len);
    LT_STREQ(s, "3\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu3/topology/core_siblings_list", &len);
    snprintf(want, sizeof want, "0-%d\n", n - 1);
    LT_STREQ(s, want);
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu3/topology/thread_siblings", &len);
    LT_STREQ(s, n <= 8 ? "08\n" : (n <= 12 ? "008\n" : (n <= 16 ? "0008\n" : (n <= 20 ? "00008\n" : "000008\n"))));
    free(s);
    LT_ERRNO(gopen("/sys/devices/system/cpu/cpu0/online", LX_O_RDONLY), LX_ENOENT);   /* cpu0 cannot be hot-unplugged */
    s = slurp("/sys/devices/system/cpu/cpu1/online", &len);
    LT_STREQ(s, "1\n");
    free(s);
    /* cache: from CPUID leaf 4 */
    int cc = (int)listing("/sys/devices/system/cpu/cpu5/cache", names, 160);
    LT_EQ(cc, 4);                                                     /* ., .., index0, index1 */
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/level", &len);
    LT_STREQ(s, "1\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/type", &len);
    LT_STREQ(s, "Data\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/size", &len);
    LT_STREQ(s, "32K\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/ways_of_associativity", &len);
    LT_STREQ(s, "8\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/number_of_sets", &len);
    LT_STREQ(s, "64\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index0/coherency_line_size", &len);
    LT_STREQ(s, "64\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index1/type", &len);
    LT_STREQ(s, "Unified\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index1/size", &len);
    LT_STREQ(s, "2M\n");
    free(s);
    s = slurp("/sys/devices/system/cpu/cpu5/cache/index1/shared_cpu_list", &len);   /* 4 CPUs share L2: 4-7 */
    LT_STREQ(s, "4-7\n");
    free(s);
    LT_ERRNO(gopen("/sys/devices/system/cpu/cpu5/cache/index2/level", LX_O_RDONLY), LX_ENOENT);
    /* clock source: the TSC is invariant in the guest's CPUID */
    s = slurp("/sys/devices/system/clocksource/clocksource0/current_clocksource", &len);
    LT_STREQ(s, "tsc\n");
    free(s);
    /* absent on purpose: DMI, class, bus/pci (the EAC probes accept ENOENT) */
    LT_ERRNO(gopen("/sys/devices/virtual/dmi/id/board_name", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/sys/devices/virtual/dmi/id/product_name", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/sys/devices/virtual/dmi/id/sys_vendor", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gstat("/sys/class", &st), LX_ENOENT);
    LT_ERRNO(gstat("/sys/bus/pci", &st), LX_ENOENT);
    LT_ERRNO(gopen("/proc/bus/pci/devices", LX_O_RDONLY), LX_ENOENT);
    /* without a CPUID provider there is no cache description and no clock source claim */
    lkd_procfs_set_cpuid(NULL);
    LT_ERRNO(gstat("/sys/devices/system/cpu/cpu5/cache", &st), LX_ENOENT);
    LT_ERRNO(gopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", LX_O_RDONLY), LX_ENOENT);
    lkd_procfs_set_cpuid(fake_cpuid);
}

LT_TEST(dev_nodes)
{
    struct lx_stat st;
    static const struct { const char *p; uint32_t mode; uint64_t rdev; } d[] = {
        { "/dev/null", LX_S_IFCHR | 0666, (1 << 8) | 3 }, { "/dev/zero", LX_S_IFCHR | 0666, (1 << 8) | 5 },
        { "/dev/full", LX_S_IFCHR | 0666, (1 << 8) | 7 }, { "/dev/random", LX_S_IFCHR | 0666, (1 << 8) | 8 },
        { "/dev/urandom", LX_S_IFCHR | 0666, (1 << 8) | 9 }, { "/dev/tty", LX_S_IFCHR | 0666, (5 << 8) | 0 },
        { "/dev/ptmx", LX_S_IFCHR | 0666, (5 << 8) | 2 },
    };
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        LT_EQ(gstat(d[i].p, &st), 0);
        LT_EQ(st.st_mode, d[i].mode);
        LT_EQ(st.st_rdev, d[i].rdev);
        LT_EQ(st.st_uid, 0);
    }
    /* /dev/null (read_null, write_null) */
    int64_t fd = gopen("/dev/null", LX_O_RDWR);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, read, (uint64_t)fd, gbulk(), 100), 0);
    LT_EQ(SC(T, write, (uint64_t)fd, gbulk(), 100), 100);
    LT_EQ(SC(T, lseek, (uint64_t)fd, 1234, LX_SEEK_SET), 0);          /* null_lseek: position stays 0 */
    LT_EQ(SC(T, fstat, (uint64_t)fd, gbulk() + 4096), 0);
    memcpy(&st, (void *)(uintptr_t)(gbulk() + 4096), sizeof st);
    LT_EQ(st.st_rdev, (1 << 8) | 3);
    char buf[64];
    snprintf(buf, sizeof buf, "/proc/self/fd/%d", (int)fd);
    char target[64];
    LT_CHECK(rdlink(buf, target, sizeof target) > 0);
    LT_STREQ(target, "/dev/null");
    SC(T, close, (uint64_t)fd);
    /* /dev/zero (read_zero) */
    fd = gopen("/dev/zero", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    memset((void *)(uintptr_t)gbulk(), 0x5a, 8192);
    LT_EQ(SC(T, read, (uint64_t)fd, gbulk(), 8192), 8192);
    int allzero = 1;
    for (int i = 0; i < 8192; i++) if (((uint8_t *)(uintptr_t)gbulk())[i]) allzero = 0;
    LT_CHECK(allzero);
    LT_CHECK(lkd_procfs_dev_is_zero(lkd_fd_get(T, (int)fd, NULL)));    /* the mm module maps it as anonymous memory */
    /* mmap of /dev/zero (mmap_zero, request procfs-2): private is anonymous copy-on-write memory, shared a
     * shared anonymous object, "/dev/zero (deleted)" in maps; a read-only fd cannot give a shared writable
     * mapping (EACCES, like any file) */
    int64_t zp = SC(T, mmap, 0, 16384, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE, (uint64_t)fd, 0);
    LT_CHECK(zp > 0);
    if (zp > 0) {
        LT_EQ(((uint8_t *)lkd_g2h((uint64_t)zp))[100], 0);
        ((uint8_t *)lkd_g2h((uint64_t)zp))[100] = 7;
        LT_EQ(((uint8_t *)lkd_g2h((uint64_t)zp))[100], 7);
    }
    LT_ERRNO(SC(T, mmap, 0, 16384, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, (uint64_t)fd, 0), LX_EACCES);
    SC(T, close, (uint64_t)fd);
    fd = gopen("/dev/zero", LX_O_RDWR);
    int64_t zs = SC(T, mmap, 0, 16384, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, (uint64_t)fd, 0);
    LT_CHECK(zs > 0);
    int64_t mlen = 0;
    char *maps = slurp_as(T, "/proc/self/maps", &mlen);
    LT_CHECK(maps && strstr(maps, "/dev/zero (deleted)") != NULL);
    LT_CHECK(maps && strstr(maps, " /dev/zero\n") != NULL);
    free(maps);
    if (zp > 0) SC(T, munmap, (uint64_t)zp, 16384);
    if (zs > 0) SC(T, munmap, (uint64_t)zs, 16384);
    SC(T, close, (uint64_t)fd);
    /* /dev/full (write_full) */
    fd = gopen("/dev/full", LX_O_RDWR);
    LT_CHECK(fd >= 0);
    LT_ERRNO(SC(T, write, (uint64_t)fd, gbulk(), 10), LX_ENOSPC);
    LT_EQ(SC(T, read, (uint64_t)fd, gbulk(), 10), 10);
    SC(T, close, (uint64_t)fd);
    /* /dev/urandom and /dev/random: host entropy, different at every read, big reads complete */
    for (int k = 0; k < 2; k++) {
        const char *p = k ? "/dev/random" : "/dev/urandom";
        fd = gopen(p, LX_O_RDONLY);
        LT_CHECK(fd >= 0);
        LT_EQ(SC(T, read, (uint64_t)fd, gbulk(), 1000), 1000);
        uint8_t a[1000], b[1000];
        memcpy(a, (void *)(uintptr_t)gbulk(), 1000);
        LT_EQ(SC(T, read, (uint64_t)fd, gbulk(), 1000), 1000);
        memcpy(b, (void *)(uintptr_t)gbulk(), 1000);
        LT_CHECK(memcmp(a, b, 1000) != 0);
        LT_CHECK(memcmp(a, (uint8_t[1000]){ 0 }, 1000) != 0);
        SC(T, close, (uint64_t)fd);
        fd = gopen(p, LX_O_RDWR);
        LT_EQ(SC(T, write, (uint64_t)fd, gbulk(), 64), 64);         /* writing mixes data into the pool: accepted */
        LT_EQ(SC(T, read, (uint64_t)fd, 8, 16), -LX_EFAULT);        /* a bad buffer is -EFAULT, no crash */
        SC(T, close, (uint64_t)fd);
    }
    /* open permission: a user without write permission cannot open /dev/null for writing? it is 0666 */
    LT_ERRNO(gopen("/dev/null", LX_O_RDONLY | LX_O_DIRECTORY), LX_ENOTDIR);
    /* pseudo terminals are NOT supported: /dev/ptmx opens with -ENODEV, /dev/pts is an empty directory */
    LT_ERRNO(gopen("/dev/ptmx", LX_O_RDWR), LX_ENODEV);
    static char names[64][32];
    LT_EQ(listing("/dev/pts", names, 64), 2);
    /* /dev/tty: the host controlling terminal, or ENXIO when the host process has none (tty_open) */
    int64_t tfd = gopen("/dev/tty", LX_O_RDWR);
    LT_CHECK(tfd >= 0 || tfd == -LX_ENXIO);
    if (tfd >= 0) SC(T, close, (uint64_t)tfd);
    /* links: fd, stdin, stdout, stderr point into /proc/self/fd (Documentation/admin-guide/devices.txt) */
    LT_EQ(rdlink("/dev/fd", buf, sizeof buf), 13);
    LT_STREQ(buf, "/proc/self/fd");
    LT_CHECK(rdlink("/dev/stdin", buf, sizeof buf) > 0);
    LT_STREQ(buf, "/proc/self/fd/0");
    LT_CHECK(rdlink("/dev/stderr", buf, sizeof buf) > 0);
    LT_STREQ(buf, "/proc/self/fd/2");
    /* listing */
    int c = (int)listing("/dev", names, 64);
    LT_CHECK(has(names, c, "null") && has(names, c, "zero") && has(names, c, "urandom") && has(names, c, "random") &&
             has(names, c, "tty") && has(names, c, "full") && has(names, c, "ptmx") && has(names, c, "pts") &&
             has(names, c, "fd") && has(names, c, "stdin"));
    LT_ERRNO(gopen("/dev/mem", LX_O_RDONLY), LX_ENOENT);
    LT_ERRNO(gopen("/dev/kmsg", LX_O_RDONLY), LX_ENOENT);
}

int main(void)
{
    procfs_fixture();
    LT_RUN(cpuinfo_from_cpuid);
    LT_RUN(meminfo_uptime_loadavg);
    LT_RUN(version_stat_filesystems);
    LT_RUN(proc_sys_values);
    LT_RUN(sys_cpu_tree);
    LT_RUN(dev_nodes);
    return lt_finish();
}

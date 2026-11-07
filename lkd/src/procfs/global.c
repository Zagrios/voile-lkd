/* global.c: /proc/cpuinfo, meminfo, uptime, loadavg, version, stat, filesystems and /proc/sys.
 *
 * Every value is measured on the host at read time (sysctl, host_statistics64, host_processor_info,
 * getloadavg, the family registry) or comes from the guest's own CPUID. A quantity the host cannot
 * supply is not printed with a made-up number: the line is omitted, or printed as 0 only where the
 * quantity has no Darwin equivalent at all (listed in docs/status/procfs.md).
 *
 * /proc/cpuinfo model: FEX Source/Tools/LinuxEmulation/LinuxSyscalls/EmulatedFiles/EmulatedFiles.cpp
 * GenerateCPUInfo (MIT), rewritten for LKD. The field order is the one of arch/x86/kernel/cpu/proc.c
 * (v6.12) show_cpuinfo. The flag names are Linux's (arch/x86/include/asm/cpufeatures.h); FEX spells
 * "xtopology", "osxsave" and "sgx_lc" wrongly, LKD uses the Linux spelling.
 */
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "procfs_priv.h"

/* ---- Host facts -------------------------------------------------------------------------------- */

int pf_host_ncpu(void)
{
    static _Atomic int n;
    int v = atomic_load(&n);
    if (v) return v;
    int c = 0;
    size_t sz = sizeof c;
    if (sysctlbyname("hw.logicalcpu", &c, &sz, NULL, 0) != 0 || c < 1) c = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (c < 1) c = 1;
    atomic_store(&n, c);
    return c;
}
int lkd_procfs_ncpu(void) { return pf_host_ncpu(); }

/* Linux CLOCK_BOOTTIME: Darwin CLOCK_MONOTONIC_RAW counts time asleep (task module, time.c). */
int64_t pf_boot_ns(void) { return (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); }

int64_t pf_now_realtime_sec(void) { return (int64_t)time(NULL); }

/* btime: the instant the uptime clock started, now - uptime, so that btime + /proc/uptime is the wall clock
 * and agrees with clock_gettime(CLOCK_BOOTTIME) (kern.boottime is set a few seconds off the monotonic
 * origin, MESURE 8 s on this Mac, so it is not used). */
int64_t pf_boot_realtime_sec(void) { return (int64_t)time(NULL) - pf_boot_ns() / 1000000000; }

static _Atomic(lkd_cpuid_fn) g_cpuid;
void lkd_procfs_set_cpuid(lkd_cpuid_fn fn) { atomic_store(&g_cpuid, fn); }
int pf_cpuid_available(void) { return atomic_load(&g_cpuid) != NULL; }
int pf_cpuid(uint32_t leaf, uint32_t sub, int cpu, struct lkd_cpuid_regs *r)
{
    lkd_cpuid_fn fn = atomic_load(&g_cpuid);
    memset(r, 0, sizeof *r);
    if (!fn) return -1;
    return fn(leaf, sub, cpu, r);
}

int pf_inotify_present(void) { return lkd_syscall_owner(LKD_NR_inotify_init1) != NULL; }

/* ---- /proc/cpuinfo ----------------------------------------------------------------------------- */

enum { L0, L1, L6, L7, L7_1, LD_1, L10, E1, E7, E8, EA, E1F, NL };
enum { REAX, REBX, RECX, REDX };
#define SRC(l, r) ((uint8_t)((l) * 4 + (r)))
#define ALWAYS 0xff
#define FAM16 0xfe
struct flag { uint8_t src, bit; const char *name; };

#define F(l, r, b, n) { SRC(l, r), b, n }
static const struct flag g_flags[] = {
    F(L1,REDX,0,"fpu"), F(L1,REDX,1,"vme"), F(L1,REDX,2,"de"), F(L1,REDX,3,"pse"), F(L1,REDX,4,"tsc"),
    F(L1,REDX,5,"msr"), F(L1,REDX,6,"pae"), F(L1,REDX,7,"mce"), F(L1,REDX,8,"cx8"), F(L1,REDX,9,"apic"),
    F(L1,REDX,11,"sep"), F(L1,REDX,12,"mtrr"), F(L1,REDX,13,"pge"), F(L1,REDX,14,"mca"), F(L1,REDX,15,"cmov"),
    F(L1,REDX,16,"pat"), F(L1,REDX,17,"pse36"), F(L1,REDX,18,"pn"), F(L1,REDX,19,"clflush"), F(L1,REDX,21,"ds"),
    F(L1,REDX,22,"acpi"), F(L1,REDX,23,"mmx"), F(L1,REDX,24,"fxsr"), F(L1,REDX,25,"sse"), F(L1,REDX,26,"sse2"),
    F(L1,REDX,27,"ss"), F(L1,REDX,28,"ht"), F(L1,REDX,29,"tm"), F(L1,REDX,30,"ia64"), F(L1,REDX,31,"pbe"),
    F(E1,REDX,11,"syscall"), F(E1,REDX,19,"mp"), F(E1,REDX,20,"nx"), F(E1,REDX,22,"mmxext"),
    F(E1,REDX,25,"fxsr_opt"), F(E1,REDX,26,"pdpe1gb"), F(E1,REDX,27,"rdtscp"), F(E1,REDX,29,"lm"),
    F(E1,REDX,31,"3dnow"), F(E1,REDX,30,"3dnowext"),
    F(E7,REDX,8,"constant_tsc"),
    { ALWAYS, 0, "art" }, { ALWAYS, 0, "rep_good" }, F(E7,REDX,12,"tm"), { ALWAYS, 0, "nopl" },
    { ALWAYS, 0, "xtopology" }, F(E7,REDX,8,"nonstop_tsc"), { ALWAYS, 0, "cpuid" }, { FAM16, 0, "extd_apicid" },
    F(E7,REDX,11,"aperfmperf"), { ALWAYS, 0, "tsc_known_freq" },
    F(L1,RECX,0,"pni"), F(L1,RECX,1,"pclmulqdq"), F(L1,RECX,2,"dtes64"), F(L1,RECX,3,"monitor"),
    F(L1,RECX,4,"ds_cpl"), F(L1,RECX,5,"vmx"), F(L1,RECX,6,"smx"), F(L1,RECX,7,"est"), F(L1,RECX,8,"tm2"),
    F(L1,RECX,9,"ssse3"), F(L1,RECX,10,"cid"), F(L1,RECX,11,"sdbg"), F(L1,RECX,12,"fma"), F(L1,RECX,13,"cx16"),
    F(L1,RECX,14,"xtpr"), F(L1,RECX,15,"pdcm"), F(L1,RECX,17,"pcid"), F(L1,RECX,18,"dca"), F(L1,RECX,19,"sse4_1"),
    F(L1,RECX,20,"sse4_2"), F(L1,RECX,21,"x2apic"), F(L1,RECX,22,"movbe"), F(L1,RECX,23,"popcnt"),
    F(L1,RECX,24,"tsc_deadline_timer"), F(L1,RECX,25,"aes"), F(L1,RECX,26,"xsave"), F(L1,RECX,27,"osxsave"),
    F(L1,RECX,28,"avx"), F(L1,RECX,29,"f16c"), F(L1,RECX,30,"rdrand"), F(L1,RECX,31,"hypervisor"),
    F(E1,RECX,0,"lahf_lm"), F(E1,RECX,1,"cmp_legacy"), F(E1,RECX,2,"svm"), F(E1,RECX,3,"extapic"),
    F(E1,RECX,4,"cr8_legacy"), F(E1,RECX,5,"abm"), F(E1,RECX,6,"sse4a"), F(E1,RECX,7,"misalignsse"),
    F(E1,RECX,8,"3dnowprefetch"), F(E1,RECX,9,"osvw"), F(E1,RECX,10,"ibs"), F(E1,RECX,11,"xop"),
    F(E1,RECX,12,"skinit"), F(E1,RECX,13,"wdt"), F(E1,RECX,15,"lwp"), F(E1,RECX,16,"fma4"), F(E1,RECX,17,"tce"),
    F(E1,RECX,19,"nodeid_msr"), F(E1,RECX,21,"tbm"), F(E1,RECX,22,"topoext"), F(E1,RECX,23,"perfctr_core"),
    F(E1,RECX,24,"perfctr_nb"), F(E1,RECX,26,"bpext"), F(E1,RECX,27,"ptsc"), F(E1,RECX,28,"perfctr_llc"),
    F(E1,RECX,29,"mwaitx"),
    F(E7,REDX,9,"cpb"), F(L6,RECX,3,"epb"), F(L10,REBX,1,"cat_l3"), F(L10,REBX,2,"cat_l2"),
    F(E7,REDX,7,"hw_pstate"), F(E1F,REAX,0,"sme"), F(E8,REBX,6,"mba"), F(E1F,REAX,1,"sev"),
    F(L7,REDX,31,"ssbd"), F(L7,REDX,27,"stibp"),
    F(L7,REBX,0,"fsgsbase"), F(L7,REBX,1,"tsc_adjust"), F(L7,REBX,2,"sgx"), F(L7,REBX,3,"bmi1"),
    F(L7,REBX,4,"hle"), F(L7,REBX,5,"avx2"), F(L7,REBX,6,"fdp_excptn_only"), F(L7,REBX,7,"smep"),
    F(L7,REBX,8,"bmi2"), F(L7,REBX,9,"erms"), F(L7,REBX,10,"invpcid"), F(L7,REBX,11,"rtm"),
    F(L7,REBX,12,"rdt_m"), F(L7,REBX,13,"depc_fpu_cs_ds"), F(L7,REBX,14,"mpx"), F(L7,REBX,15,"rdt_a"),
    F(L7,REBX,16,"avx512f"), F(L7,REBX,17,"avx512dq"), F(L7,REBX,18,"rdseed"), F(L7,REBX,19,"adx"),
    F(L7,REBX,20,"smap"), F(L7,REBX,21,"avx512ifma"), F(L7,REBX,23,"clflushopt"), F(L7,REBX,24,"clwb"),
    F(L7,REBX,25,"intel_pt"), F(L7,REBX,26,"avx512pf"), F(L7,REBX,27,"avx512er"), F(L7,REBX,28,"avx512cd"),
    F(L7,REBX,29,"sha_ni"), F(L7,REBX,30,"avx512bw"), F(L7,REBX,31,"avx512vl"),
    F(LD_1,REAX,0,"xsaveopt"), F(LD_1,REAX,1,"xsavec"), F(LD_1,REAX,2,"xgetbv1"), F(LD_1,REAX,3,"xsaves"),
    F(LD_1,REAX,4,"xfd"),
    F(L7_1,REAX,4,"avx_vnni"), F(L7_1,REAX,5,"avx512_bf16"), F(L7_1,REAX,23,"avx_ifma"),
    F(L7_1,REDX,4,"avx_vnni_int8"), F(L7_1,REDX,5,"avx_ne_convert"), F(L7_1,REDX,10,"avx_vnni_int16"),
    F(E8,REBX,0,"clzero"), F(E8,REBX,1,"irperf"), F(E8,REBX,2,"xsaveerptr"),
    F(L6,REAX,2,"arat"),
    F(EA,REBX,0,"npt"), F(EA,REBX,1,"lbrv"), F(EA,REBX,2,"svm_lock"), F(EA,REBX,3,"nrip_save"),
    F(EA,REBX,4,"tsc_scale"), F(EA,REBX,5,"vmcb_clean"), F(EA,REBX,6,"flushbyasid"), F(EA,REBX,7,"decodeassists"),
    F(EA,REBX,10,"pausefilter"), F(EA,REBX,12,"pfthreshold"), F(EA,REBX,13,"avic"), F(EA,REBX,15,"v_vmsave_vmload"),
    F(EA,REBX,16,"vgif"),
    F(L7,RECX,1,"avx512vbmi"), F(L7,RECX,2,"umip"), F(L7,RECX,3,"pku"), F(L7,RECX,4,"ospke"),
    F(L7,RECX,5,"waitpkg"), F(L7,RECX,6,"avx512_vbmi2"), F(L7,RECX,8,"gfni"), F(L7,RECX,9,"vaes"),
    F(L7,RECX,10,"vpclmulqdq"), F(L7,RECX,11,"avx512_vnni"), F(L7,RECX,12,"avx512_bitalg"), F(L7,RECX,13,"tme"),
    F(L7,RECX,14,"avx512_vpopcntdq"), F(L7,RECX,16,"la57"), F(L7,RECX,22,"rdpid"), F(L7,RECX,24,"bus_lock_detect"),
    F(L7,RECX,25,"cldemote"), F(L7,RECX,27,"movdiri"), F(L7,RECX,28,"movdir64b"), F(L7,RECX,29,"enqcmd"),
    F(L7,RECX,30,"sgx_lc"),
    F(E7,REBX,0,"overflow_recov"), F(E7,REBX,1,"succor"), F(E7,REBX,3,"smca"),
    F(L7,REDX,2,"avx512_4vnniw"), F(L7,REDX,3,"avx512_4fmaps"), F(L7,REDX,4,"fsrm"),
    F(L7,REDX,8,"avx512_vp2intersect"), F(L7,REDX,10,"md_clear"), F(L7,REDX,14,"serialize"),
    F(L7,REDX,18,"pconfig"), F(L7,REDX,19,"arch_lbr"), F(L7,REDX,20,"ibt"), F(L7,REDX,22,"amx_bf16"),
    F(L7,REDX,23,"avx512_fp16"), F(L7,REDX,24,"amx_tile"), F(L7,REDX,25,"amx_int8"), F(L7,REDX,28,"flush_l1d"),
    F(L7,REDX,29,"arch_capabilities"),
};

static uint32_t reg_of(const struct lkd_cpuid_regs *r, int which)
{
    switch (which) { case REAX: return r->eax; case REBX: return r->ebx; case RECX: return r->ecx; default: return r->edx; }
}

static int cpuid_all(struct lkd_cpuid_regs R[NL])
{
    static const struct { uint32_t leaf, sub; } q[NL] = {
        [L0] = { 0, 0 }, [L1] = { 1, 0 }, [L6] = { 6, 0 }, [L7] = { 7, 0 }, [L7_1] = { 7, 1 }, [LD_1] = { 0xd, 1 },
        [L10] = { 0x10, 0 }, [E1] = { 0x80000001u, 0 }, [E7] = { 0x80000007u, 0 }, [E8] = { 0x80000008u, 0 },
        [EA] = { 0x8000000au, 0 }, [E1F] = { 0x8000001fu, 0 },
    };
    for (int i = 0; i < NL; i++) pf_cpuid(q[i].leaf, q[i].sub, 0, &R[i]);
    return 0;
}

/* Cache levels from CPUID leaf 4 (deterministic cache parameters), the same numbers the guest reads. */
int pf_cache_count(void)
{
    struct lkd_cpuid_regs r;
    int n = 0;
    if (!pf_cpuid_available()) return 0;
    for (uint32_t i = 0; i < 16; i++) {
        if (pf_cpuid(4, i, 0, &r) != 0 || (r.eax & 31) == 0) break;
        n++;
    }
    return n;
}

static void render_cpuinfo(struct pbuf *b)
{
    struct lkd_cpuid_regs R[NL];
    cpuid_all(R);
    int ncpu = pf_host_ncpu();
    uint32_t eax1 = R[L1].eax;
    uint32_t stepping = eax1 & 15, model = (eax1 >> 4) & 15, family = (eax1 >> 8) & 15;
    uint32_t exmodel = (eax1 >> 16) & 15, exfam = (eax1 >> 20) & 0xff;
    uint32_t fam = family + (family == 0xf ? exfam : 0);
    uint32_t mod = model + (family >= 6 ? exmodel << 4 : 0);
    char vendor[13];
    memcpy(vendor, &R[L0].ebx, 4); memcpy(vendor + 4, &R[L0].edx, 4); memcpy(vendor + 8, &R[L0].ecx, 4);
    vendor[12] = 0;

    struct pbuf flags = { 0 };
    for (size_t i = 0; i < sizeof g_flags / sizeof g_flags[0]; i++) {
        const struct flag *f = &g_flags[i];
        int on;
        if (f->src == ALWAYS) on = 1;
        else if (f->src == FAM16) on = fam > 0x16;
        else on = (reg_of(&R[f->src / 4], f->src % 4) >> f->bit) & 1;
        if (on) { pb_puts(&flags, f->name); pb_putc(&flags, ' '); }
    }

    /* TSC frequency, CPUID 0x15: ecx * ebx / eax Hz. Without it the lines are omitted. */
    struct lkd_cpuid_regs r15;
    pf_cpuid(0x15, 0, 0, &r15);
    int have_freq = r15.eax != 0 && r15.ebx != 0 && r15.ecx != 0;
    double hz = have_freq ? (double)r15.ecx * (double)r15.ebx / (double)r15.eax : 0;

    /* cache size: the largest cache reported by leaf 4, in KB. */
    uint64_t cache_kb = 0;
    for (uint32_t i = 0; i < 16; i++) {
        struct lkd_cpuid_regs c;
        if (pf_cpuid(4, i, 0, &c) != 0 || (c.eax & 31) == 0) break;
        uint64_t sz = (uint64_t)((c.ebx >> 22) + 1) * (((c.ebx >> 12) & 0x3ff) + 1) * ((c.ebx & 0xfff) + 1) * ((uint64_t)c.ecx + 1);
        if (sz / 1024 > cache_kb) cache_kb = sz / 1024;
    }
    int clflush = (int)((R[L1].ebx >> 8) & 0xff) * 8;
    struct lkd_cpuid_regs a8;
    pf_cpuid(0x80000008u, 0, 0, &a8);
    int phys = (int)(a8.eax & 0xff), virt = (int)((a8.eax >> 8) & 0xff);

    for (int i = 0; i < ncpu; i++) {
        struct lkd_cpuid_regs n2, n3, n4;
        pf_cpuid(0x80000002u, 0, i, &n2); pf_cpuid(0x80000003u, 0, i, &n3); pf_cpuid(0x80000004u, 0, i, &n4);
        char name[49];
        memcpy(name, &n2, 16); memcpy(name + 16, &n3, 16); memcpy(name + 32, &n4, 16);
        name[48] = 0;
        pb_printf(b, "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\n", i,
                  vendor[0] ? vendor : "unknown", fam, mod, name[0] ? name : "unknown");
        pb_printf(b, "stepping\t: %u\n", stepping);
        if (have_freq) pb_printf(b, "cpu MHz\t\t: %.3f\n", hz / 1e6);
        if (cache_kb) pb_printf(b, "cache size\t: %llu KB\n", (unsigned long long)cache_kb);
        /* flat topology: ncpu cores, one thread each (same view as /sys/devices/system/cpu/cpuN/topology) */
        pb_printf(b, "physical id\t: 0\nsiblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\napicid\t\t: %d\ninitial apicid\t: %d\n",
                  ncpu, i, ncpu, i, i);
        pb_printf(b, "fpu\t\t: %s\nfpu_exception\t: %s\ncpuid level\t: %u\nwp\t\t: yes\n",
                  (R[L1].edx & 1) ? "yes" : "no", (R[L1].edx & 1) ? "yes" : "no", R[L0].eax);
        pb_printf(b, "flags\t\t: ");
        if (flags.n) pb_write(b, flags.p, flags.n - 1);   /* "flags\t\t:" then " name" each: drop the trailing space */
        pb_puts(b, "\nbugs\t\t:\n");
        if (have_freq) pb_printf(b, "bogomips\t: %.2f\n", hz / 1e3 / 500.0);
        pb_printf(b, "clflush size\t: %d\ncache_alignment\t: %d\n", clflush, clflush);
        if (phys) pb_printf(b, "address sizes\t: %d bits physical, %d bits virtual\n", phys, virt);
        pb_puts(b, "power management:\n\n");
    }
    pb_free(&flags);
}

/* ---- meminfo, uptime, loadavg, stat ------------------------------------------------------------ */

static void render_meminfo(struct pbuf *b)
{
    uint64_t total = 0;
    size_t sz = sizeof total;
    sysctlbyname("hw.memsize", &total, &sz, NULL, 0);
    vm_size_t ps = 0;
    host_page_size(mach_host_self(), &ps);
    vm_statistics64_data_t vs;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    memset(&vs, 0, sizeof vs);
    host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt);
    struct xsw_usage sw = { 0 };
    sz = sizeof sw;
    sysctlbyname("vm.swapusage", &sw, &sz, NULL, 0);
    uint64_t kb = 1024;
    uint64_t free_b = (uint64_t)(vs.free_count - vs.speculative_count) * ps;
    uint64_t avail = free_b + (uint64_t)(vs.inactive_count + vs.speculative_count + vs.purgeable_count) * ps;
    if (avail > total) avail = total;
    uint64_t anon = (uint64_t)vs.internal_page_count * ps, file = (uint64_t)vs.external_page_count * ps;
    uint64_t swap_total = sw.xsu_total, swap_free = sw.xsu_avail;
    /* Lines with no Darwin counterpart (Buffers, Dirty, Writeback, Slab, KernelStack, ...) read 0:
     * LKD has no page cache accounting or kernel allocator (docs/status/procfs.md). */
    pb_printf(b,
        "MemTotal:       %8llu kB\nMemFree:        %8llu kB\nMemAvailable:   %8llu kB\nBuffers:        %8llu kB\n"
        "Cached:         %8llu kB\nSwapCached:     %8llu kB\nActive:         %8llu kB\nInactive:       %8llu kB\n"
        "Unevictable:    %8llu kB\nMlocked:        %8llu kB\nSwapTotal:      %8llu kB\nSwapFree:       %8llu kB\n"
        "Dirty:          %8llu kB\nWriteback:      %8llu kB\nAnonPages:      %8llu kB\nMapped:         %8llu kB\n"
        "Shmem:          %8llu kB\nSlab:           %8llu kB\nSReclaimable:   %8llu kB\nSUnreclaim:     %8llu kB\n"
        "KernelStack:    %8llu kB\nPageTables:     %8llu kB\nCommitLimit:    %8llu kB\nCommitted_AS:   %8llu kB\n",
        (unsigned long long)(total / kb), (unsigned long long)(free_b / kb), (unsigned long long)(avail / kb), 0ull,
        (unsigned long long)(file / kb), 0ull, (unsigned long long)((uint64_t)vs.active_count * ps / kb),
        (unsigned long long)((uint64_t)vs.inactive_count * ps / kb), (unsigned long long)((uint64_t)vs.wire_count * ps / kb),
        0ull, (unsigned long long)(swap_total / kb), (unsigned long long)(swap_free / kb), 0ull, 0ull,
        (unsigned long long)(anon / kb), (unsigned long long)(file / kb), 0ull, 0ull, 0ull, 0ull, 0ull, 0ull,
        (unsigned long long)((total / 2 + swap_total) / kb), 0ull);
}

static int cpu_ticks(natural_t *n, processor_cpu_load_info_t *info, mach_msg_type_number_t *cnt)
{
    return host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, n, (processor_info_array_t *)info, cnt) == KERN_SUCCESS ? 0 : -1;
}

static void render_uptime(struct pbuf *b)
{
    int64_t ns = pf_boot_ns();
    uint64_t idle = 0;
    natural_t n;
    processor_cpu_load_info_t info;
    mach_msg_type_number_t cnt;
    if (cpu_ticks(&n, &info, &cnt) == 0) {
        for (natural_t i = 0; i < n; i++) idle += info[i].cpu_ticks[CPU_STATE_IDLE];
        vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(int));
    }
    /* proc/uptime.c: uptime and the idle time of all CPUs, two decimals; ticks are 100 Hz */
    pb_printf(b, "%llu.%02llu %llu.%02llu\n", (unsigned long long)(ns / 1000000000), (unsigned long long)(ns / 10000000 % 100),
              (unsigned long long)(idle / 100), (unsigned long long)(idle % 100));
}

static void render_loadavg(struct pbuf *b)
{
    double la[3] = { 0, 0, 0 };
    if (getloadavg(la, 3) < 0) la[0] = la[1] = la[2] = 0;
    lkd_proc_info *list = malloc(1024 * sizeof *list);
    int running = 0, threads = 0, last = 0, c = list ? pf_be.proc_list(list, 1024) : 0;
    for (int i = 0; i < c; i++) {
        if (list[i].state == 'R') running++;
        threads += list[i].nthreads;
        if (list[i].pid > last) last = list[i].pid;
    }
    free(list);
    /* fs/proc/loadavg.c: "%lu.%02lu x3 nr_running/nr_threads last_pid" */
    pb_printf(b, "%.2f %.2f %.2f %d/%d %d\n", la[0], la[1], la[2], running, threads, last);
}

static void render_gstat(struct pbuf *b)
{
    natural_t n;
    processor_cpu_load_info_t info;
    mach_msg_type_number_t cnt;
    if (cpu_ticks(&n, &info, &cnt) == 0) {
        uint64_t t[4] = { 0, 0, 0, 0 };
        for (natural_t i = 0; i < n; i++)
            for (int k = 0; k < 4; k++) t[k] += info[i].cpu_ticks[k];
        /* user nice system idle iowait irq softirq steal guest guest_nice; Darwin has no iowait/irq/
         * softirq/steal/guest ticks: they read 0 because the concept does not exist. */
        pb_printf(b, "cpu  %llu %llu %llu %llu 0 0 0 0 0 0\n", (unsigned long long)t[CPU_STATE_USER],
                  (unsigned long long)t[CPU_STATE_NICE], (unsigned long long)t[CPU_STATE_SYSTEM],
                  (unsigned long long)t[CPU_STATE_IDLE]);
        for (natural_t i = 0; i < n; i++)
            pb_printf(b, "cpu%u %u %u %u %u 0 0 0 0 0 0\n", i, info[i].cpu_ticks[CPU_STATE_USER],
                      info[i].cpu_ticks[CPU_STATE_NICE], info[i].cpu_ticks[CPU_STATE_SYSTEM], info[i].cpu_ticks[CPU_STATE_IDLE]);
        vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(int));
    }
    /* intr, ctxt, softirq are not measurable on Darwin: not printed. processes (tasks created since boot,
     * threads included like total_forks) counts the ids the family handed out since its first process. */
    lkd_proc_info *list = malloc(1024 * sizeof *list);
    int running = 0, c = list ? pf_be.proc_list(list, 1024) : 0;
    for (int i = 0; i < c; i++) if (list[i].state == 'R') running++;
    free(list);
    uint32_t next = lkd_pid_peek(), first = lkd_config_get()->first_pid ? lkd_config_get()->first_pid : 2;
    pb_printf(b, "btime %lld\n", (long long)pf_boot_realtime_sec());
    if (next > first) pb_printf(b, "processes %u\n", next - first);
    pb_printf(b, "procs_running %d\nprocs_blocked 0\n", running);
}

static void render_version(struct pbuf *b)
{
    struct lx_utsname u;
    lkd_task_uname(&u);
    /* init/version-timestamp.c linux_proc_banner: "Linux version %s (%s) (%s) %s\n" */
    pb_printf(b, "Linux version %s (lkd@darwin) (Apple clang) %s\n", u.release, u.version);
}

static void render_filesystems(lkd_task *t, struct pbuf *b)
{
    pb_puts(b, "nodev\tsysfs\nnodev\tproc\nnodev\tdevtmpfs\n");
    size_t len = 0;
    char *m = lkd_vfs_render_mounts(t, 0, &len);
    if (!m) return;
    /* host-backed mounts: the file system type the mount table shows (third column), once each */
    char seen[8][32];
    int ns = 0;
    for (char *line = m; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char src[256], mp[512], type[64];
        if (sscanf(line, "%255s %511s %63s", src, mp, type) == 3 && strcmp(type, "proc") && strcmp(type, "sysfs") &&
            strcmp(type, "devtmpfs")) {
            int dup = 0;
            for (int i = 0; i < ns; i++) if (!strcmp(seen[i], type)) dup = 1;
            if (!dup && ns < 8) { snprintf(seen[ns++], 32, "%s", type); pb_printf(b, "\t%s\n", type); }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(m);
}

/* ---- /proc/sys --------------------------------------------------------------------------------- */

static const struct pf_tent t_random[] = {
    { "boot_id", 'f', 0444, SF_BOOT_ID }, { "uuid", 'f', 0444, SF_UUID }, { NULL },
};
static const struct pf_tent t_kernel[] = {
    { "domainname", 'f', 0644, SF_DOMAINNAME }, { "hostname", 'f', 0644, SF_HOSTNAME },
    { "osrelease", 'f', 0444, SF_OSRELEASE }, { "ostype", 'f', 0444, SF_OSTYPE },
    { "pid_max", 'f', 0644, SF_PID_MAX }, { "random", 'd', 0555, 0, t_random },
    { "randomize_va_space", 'f', 0644, SF_RANDOMIZE }, { "version", 'f', 0444, SF_VERSION_STR }, { NULL },
};
static const struct pf_tent t_vm[] = {
    { "max_map_count", 'f', 0644, SF_MAX_MAP_COUNT }, { "mmap_min_addr", 'f', 0644, SF_MMAP_MIN_ADDR },
    { "overcommit_memory", 'f', 0644, SF_OVERCOMMIT }, { NULL },
};
static const struct pf_tent t_inotify[] = {
    { "max_queued_events", 'f', 0644, SF_INOTIFY_QUEUED }, { "max_user_instances", 'f', 0644, SF_INOTIFY_INSTANCES },
    { "max_user_watches", 'f', 0644, SF_INOTIFY_WATCHES }, { NULL },
};
static const struct pf_tent t_fs[] = {
    { "inotify", 'd', 0555, 0, t_inotify, pf_inotify_present }, { "pipe-max-size", 'f', 0644, SF_PIPE_MAX }, { NULL },
};
const struct pf_tent pf_proc_sys[] = {
    { "fs", 'd', 0555, 0, t_fs }, { "kernel", 'd', 0555, 0, t_kernel }, { "vm", 'd', 0555, 0, t_vm }, { NULL },
};

static void render_sfile(int sub, struct pbuf *b)
{
    struct lx_utsname u;
    lkd_task_uname(&u);
    switch (sub) {
    case SF_OSRELEASE: pb_printf(b, "%s\n", u.release); break;
    case SF_OSTYPE: pb_printf(b, "%s\n", u.sysname); break;
    case SF_HOSTNAME: pb_printf(b, "%s\n", u.nodename); break;
    case SF_DOMAINNAME: pb_printf(b, "%s\n", u.domainname); break;
    case SF_VERSION_STR: pb_printf(b, "%s\n", u.version); break;
    case SF_PID_MAX: pb_printf(b, "%d\n", LKD_PID_MAX + 1); break;   /* pid counter limit of the family (task.h) */
    case SF_RANDOMIZE: pb_puts(b, "0\n"); break;                      /* mm has no ASLR (docs/status/mm.md) */
    case SF_UUID: case SF_BOOT_ID: {
        uint8_t r[16];
        char uuid[64];
        if (sub == SF_BOOT_ID) {
            size_t sz = sizeof uuid;
            if (sysctlbyname("kern.bootsessionuuid", uuid, &sz, NULL, 0) != 0) { pb_puts(b, "\n"); break; }
            for (char *p = uuid; *p; p++) if (*p >= 'A' && *p <= 'F') *p += 32;
            pb_printf(b, "%s\n", uuid);
            break;
        }
        getentropy(r, sizeof r);
        r[6] = (uint8_t)((r[6] & 0x0f) | 0x40);   /* version 4, drivers/char/random.c proc_do_uuid */
        r[8] = (uint8_t)((r[8] & 0x3f) | 0x80);
        pb_printf(b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", r[0], r[1], r[2], r[3], r[4],
                  r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
        break;
    }
    case SF_OVERCOMMIT: pb_puts(b, "1\n"); break;                     /* mm does no commit accounting: always */
    case SF_MAX_MAP_COUNT: pb_puts(b, LKD_MAX_MAP_COUNT_STR "\n"); break; /* mm.h */
    case SF_MMAP_MIN_ADDR: pb_puts(b, "65536\n"); break;              /* mm window.c min_addr */
    case SF_INOTIFY_INSTANCES: case SF_INOTIFY_WATCHES: case SF_INOTIFY_QUEUED: {
        struct lkd_inotify_limits il;   /* the limits net enforces (request procfs-4 point 3) */
        lkd_net_inotify_limits(&il);
        pb_printf(b, "%u\n", sub == SF_INOTIFY_INSTANCES ? il.max_user_instances
                             : sub == SF_INOTIFY_WATCHES ? il.max_user_watches : il.max_queued_events);
        break;
    }
    case SF_PIPE_MAX: pb_puts(b, "65536\n"); break;                   /* fs.pipe-max-size, docs/status/fs.md */
    default: break;
    }
}

int pf_generate_global(lkd_task *viewer, const struct pf_node *n, struct pbuf *out)
{
    switch (n->kind) {
    case PK_CPUINFO: render_cpuinfo(out); return 0;
    case PK_MEMINFO: render_meminfo(out); return 0;
    case PK_UPTIME: render_uptime(out); return 0;
    case PK_LOADAVG: render_loadavg(out); return 0;
    case PK_VERSION: render_version(out); return 0;
    case PK_GSTAT: render_gstat(out); return 0;
    case PK_FILESYSTEMS: render_filesystems(viewer, out); return 0;
    case PK_SFILE: render_sfile(n->sub, out); return 0;
    default: return -LX_EINVAL;
    }
}

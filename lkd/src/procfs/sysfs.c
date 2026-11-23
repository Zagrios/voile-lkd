/* sysfs.c: the part of /sys LKD can answer truthfully: /sys/devices/system/cpu and the clock source.
 *
 * The CPU count is the host's (hw.logicalcpu). Cache geometry comes from the guest's CPUID leaf 4, so
 * /sys and the CPUID instruction agree; without an engine CPUID provider there is no cache/ directory.
 * cpuN/cpufreq gives the host's highest clock when the engine knows it (lkd_procfs_set_cpu_max_khz).
 * /sys/bus holds only the clock source's link, where Wine's wineboot looks before it trusts the TSC.
 * Everything else under /sys is absent on purpose: /sys/devices/virtual (dmi/id), /sys/class, /sys/bus/pci
 * answer ENOENT, which is what a kernel without those devices answers (the EAC probes accept it,
 * reports/eac-probes-digest.md item 17).
 *
 * Linux references: drivers/base/cpu.c (online, possible, present, kernel_max), drivers/base/topology.c,
 * drivers/base/cacheinfo.c, arch/x86/kernel/cpu/cacheinfo.c, lib/vsprintf.c (cpumask format),
 * drivers/cpufreq/cpufreq.c (cpuinfo_max_freq, scaling_max_freq), kernel/time/clocksource.c.
 */
#include "procfs_priv.h"

static int clocksource_present_fn(void) { return pf_clocksource_present(); }

int pf_clocksource_present(void)
{
    struct lkd_cpuid_regs r;
    /* FEX publishes "tsc" under the same condition: invariant TSC in CPUID 0x80000007 edx bit 8 */
    return pf_cpuid_available() && pf_cpuid(0x80000007u, 0, 0, &r) == 0 && (r.edx & (1u << 8));
}

static const struct pf_tent t_cs0[] = {
    { "available_clocksource", 'f', 0444, SF_CLOCKSOURCE_AVAIL }, { "current_clocksource", 'f', 0644, SF_CLOCKSOURCE_CUR },
    { NULL },
};
static const struct pf_tent t_cs[] = { { "clocksource0", 'd', 0755, 0, t_cs0 }, { NULL } };
static const struct pf_tent t_cpu[] = {
    { "kernel_max", 'f', 0444, SF_CPU_KERNEL_MAX }, { "offline", 'f', 0444, SF_CPU_OFFLINE },
    { "online", 'f', 0444, SF_CPU_ONLINE }, { "possible", 'f', 0444, SF_CPU_POSSIBLE },
    { "present", 'f', 0444, SF_CPU_PRESENT }, { NULL },
};
static const struct pf_tent t_system[] = {
    { "clocksource", 'd', 0755, 0, t_cs, clocksource_present_fn }, { "cpu", 'd', 0755, 1, t_cpu }, { NULL },
};
static const struct pf_tent t_devices[] = { { "system", 'd', 0755, 0, t_system }, { NULL } };
static const struct pf_tent t_bus_cs_devices[] = {
    { "clocksource0", 'l', 0777, 0, NULL, clocksource_present_fn, "../../../devices/system/clocksource/clocksource0" },
    { NULL },
};
static const struct pf_tent t_bus_cs[] = { { "devices", 'd', 0755, 0, t_bus_cs_devices }, { NULL } };
static const struct pf_tent t_bus[] = { { "clocksource", 'd', 0755, 0, t_bus_cs, clocksource_present_fn }, { NULL } };
const struct pf_tent pf_sys_root[] = {
    { "bus", 'd', 0755, 0, t_bus, clocksource_present_fn }, { "devices", 'd', 0755, 0, t_devices }, { NULL },
};

/* Topology and cache file names; node.sub is the index. */
enum { TF_CORE_ID, TF_PKG_ID, TF_DIE_ID, TF_CORE_SIB, TF_CORE_SIB_L, TF_THR_SIB, TF_THR_SIB_L, TF_PKG_CPUS,
       TF_PKG_CPUS_L, TF_CORE_CPUS, TF_CORE_CPUS_L, TF_N, TF_ONLINE = 99 };
static const char *const g_topo[TF_N] = {
    "core_id", "physical_package_id", "die_id", "core_siblings", "core_siblings_list", "thread_siblings",
    "thread_siblings_list", "package_cpus", "package_cpus_list", "core_cpus", "core_cpus_list",
};
enum { CF_ID, CF_LEVEL, CF_TYPE, CF_SIZE, CF_LINE, CF_SETS, CF_WAYS, CF_PART, CF_MAP, CF_LIST, CF_N };
static const char *const g_cache[CF_N] = {
    "id", "level", "type", "size", "coherency_line_size", "number_of_sets", "ways_of_associativity",
    "physical_line_partition", "shared_cpu_map", "shared_cpu_list",
};
static const char *const g_freq[] = { "cpuinfo_max_freq", "scaling_max_freq" };
#define FREQ_N ((int)(sizeof g_freq / sizeof g_freq[0]))

static void leaf(struct pf_node *n, int kind, int sub)
{
    n->kind = kind;
    n->sub = sub;
    n->mode = LX_S_IFREG | 0444;
    n->nlink = 1;
    n->size = 4096;   /* kernfs attributes report PAGE_SIZE */
    n->tent = NULL;
}
static void dirn(struct pf_node *n, int kind)
{
    n->kind = kind;
    n->mode = LX_S_IFDIR | 0755;
    n->nlink = 2;
    n->size = 0;
    n->tent = NULL;
}

int pf_sys_cpu_lookup(int dirkind, const char *name, struct pf_node *n)
{
    int ncpu = pf_host_ncpu();
    if (dirkind == -1) {
        if (strncmp(name, "cpu", 3) || !name[3] || (name[3] == '0' && name[4])) return -LX_ENOENT;
        int v = 0;
        for (const char *p = name + 3; *p; p++) {
            if (*p < '0' || *p > '9') return -LX_ENOENT;
            v = v * 10 + (*p - '0');
            if (v > 100000) return -LX_ENOENT;
        }
        if (v >= ncpu) return -LX_ENOENT;
        n->cpu = v;
        dirn(n, PK_CPUN);
        return 0;
    }
    switch (dirkind) {
    case PK_CPUN:
        if (!strcmp(name, "online") && n->cpu > 0) { leaf(n, PK_TOPO, TF_ONLINE); return 0; }   /* cpu0 is not hotpluggable */
        if (!strcmp(name, "topology")) { dirn(n, PK_TOPODIR); return 0; }
        if (!strcmp(name, "cache") && pf_cache_count() > 0) { dirn(n, PK_CACHEDIR); return 0; }
        if (!strcmp(name, "cpufreq") && pf_cpu_max_khz()) { dirn(n, PK_FREQDIR); return 0; }
        return -LX_ENOENT;
    case PK_FREQDIR:
        for (int i = 0; i < FREQ_N; i++) if (!strcmp(name, g_freq[i])) { leaf(n, PK_FREQ, i); return 0; }
        return -LX_ENOENT;
    case PK_TOPODIR:
        for (int i = 0; i < TF_N; i++) if (!strcmp(name, g_topo[i])) { leaf(n, PK_TOPO, i); return 0; }
        return -LX_ENOENT;
    case PK_CACHEDIR: {
        if (strncmp(name, "index", 5) || !name[5]) return -LX_ENOENT;
        int v = 0;
        for (const char *p = name + 5; *p; p++) { if (*p < '0' || *p > '9') return -LX_ENOENT; v = v * 10 + (*p - '0'); if (v > 64) return -LX_ENOENT; }
        if (v >= pf_cache_count() || (name[5] == '0' && name[6])) return -LX_ENOENT;
        n->cidx = v;
        dirn(n, PK_CACHEIDX);
        return 0;
    }
    case PK_CACHEIDX:
        for (int i = 0; i < CF_N; i++) if (!strcmp(name, g_cache[i])) { leaf(n, PK_CACHEF, i); return 0; }
        return -LX_ENOENT;
    }
    return -LX_ENOENT;
}

int pf_sys_cpu_readdir(const struct pf_node *dir, uint64_t i, char name[256], uint8_t *dt)
{
    const char *nm = NULL;
    uint8_t t = LX_DT_REG;
    switch (dir->kind) {
    case PK_CPUN: {
        const char *l[4]; int k = 0;
        uint8_t ty[4];
        if (dir->cpu > 0) { l[k] = "online"; ty[k++] = LX_DT_REG; }
        l[k] = "topology"; ty[k++] = LX_DT_DIR;
        if (pf_cache_count() > 0) { l[k] = "cache"; ty[k++] = LX_DT_DIR; }
        if (pf_cpu_max_khz()) { l[k] = "cpufreq"; ty[k++] = LX_DT_DIR; }
        if (i < (uint64_t)k) { nm = l[i]; t = ty[i]; }
        break;
    }
    case PK_TOPODIR: if (i < TF_N) nm = g_topo[i]; break;
    case PK_CACHEDIR:
        if (i < (uint64_t)pf_cache_count()) { snprintf(name, 256, "index%d", (int)i); *dt = LX_DT_DIR; return 1; }
        break;
    case PK_CACHEIDX: if (i < CF_N) nm = g_cache[i]; break;
    case PK_FREQDIR: if (i < (uint64_t)FREQ_N) nm = g_freq[i]; break;
    }
    if (!nm) return 0;
    snprintf(name, 256, "%s", nm);
    *dt = t;
    return 1;
}

static uint64_t all_mask(int ncpu) { return ncpu >= 64 ? ~0ull : ((1ull << ncpu) - 1); }

static void put_map(struct pbuf *b, uint64_t mask, int ncpu, int list)
{
    if (ncpu > 64) ncpu = 64;
    if (list) pb_mask_list(b, mask, ncpu); else pb_mask(b, mask, ncpu);
    pb_putc(b, '\n');
}

int pf_sys_generate(const struct pf_node *n, struct pbuf *b)
{
    int ncpu = pf_host_ncpu();
    uint64_t all = all_mask(ncpu), self = n->cpu < 64 ? 1ull << n->cpu : 0;
    switch (n->kind) {
    case PK_SFILE:
        switch (n->sub) {
        case SF_CPU_ONLINE: case SF_CPU_POSSIBLE: case SF_CPU_PRESENT:
            if (ncpu > 1) pb_printf(b, "0-%d\n", ncpu - 1); else pb_puts(b, "0\n");
            return 0;
        case SF_CPU_OFFLINE: pb_putc(b, '\n'); return 0;
        case SF_CPU_KERNEL_MAX: pb_printf(b, "%d\n", ncpu - 1); return 0;
        case SF_CLOCKSOURCE_CUR: case SF_CLOCKSOURCE_AVAIL: pb_puts(b, "tsc\n"); return 0;
        }
        return -LX_EINVAL;
    case PK_TOPO:
        switch (n->sub) {
        case TF_ONLINE: pb_puts(b, "1\n"); return 0;
        case TF_CORE_ID: pb_printf(b, "%d\n", n->cpu); return 0;
        case TF_PKG_ID: case TF_DIE_ID: pb_puts(b, "0\n"); return 0;
        case TF_CORE_SIB: case TF_PKG_CPUS: put_map(b, all, ncpu, 0); return 0;
        case TF_CORE_SIB_L: case TF_PKG_CPUS_L: put_map(b, all, ncpu, 1); return 0;
        case TF_THR_SIB: case TF_CORE_CPUS: put_map(b, self, ncpu, 0); return 0;
        case TF_THR_SIB_L: case TF_CORE_CPUS_L: put_map(b, self, ncpu, 1); return 0;
        }
        return -LX_EINVAL;
    case PK_FREQ: {
        uint32_t khz = pf_cpu_max_khz();
        if (!khz) return -LX_ENOENT;
        pb_printf(b, "%u\n", khz);
        return 0;
    }
    case PK_CACHEF: {
        struct lkd_cpuid_regs c;
        if (pf_cpuid(4, (uint32_t)n->cidx, n->cpu, &c) != 0 || (c.eax & 31) == 0) return -LX_ENOENT;
        uint32_t type = c.eax & 31, level = (c.eax >> 5) & 7, share = ((c.eax >> 14) & 0xfff) + 1;
        uint32_t line = (c.ebx & 0xfff) + 1, part = ((c.ebx >> 12) & 0x3ff) + 1, ways = (c.ebx >> 22) + 1, sets = c.ecx + 1;
        uint64_t size = (uint64_t)line * part * ways * sets;
        switch (n->sub) {
        case CF_ID: pb_printf(b, "%d\n", (int)(n->cpu / share)); break;
        case CF_LEVEL: pb_printf(b, "%u\n", level); break;
        case CF_TYPE: pb_puts(b, type == 1 ? "Data\n" : type == 2 ? "Instruction\n" : "Unified\n"); break;
        case CF_SIZE:
            if (size >= (1u << 20) && !(size & ((1u << 20) - 1))) pb_printf(b, "%lluM\n", (unsigned long long)(size >> 20));
            else pb_printf(b, "%lluK\n", (unsigned long long)(size >> 10));
            break;
        case CF_LINE: pb_printf(b, "%u\n", line); break;
        case CF_SETS: pb_printf(b, "%u\n", sets); break;
        case CF_WAYS: pb_printf(b, "%u\n", ways); break;
        case CF_PART: pb_printf(b, "%u\n", part); break;
        case CF_MAP: case CF_LIST: {
            int first = (int)(n->cpu / share) * (int)share;
            uint64_t m = 0;
            for (int k = first; k < first + (int)share && k < ncpu && k < 64; k++) m |= 1ull << k;
            put_map(b, m, ncpu, n->sub == CF_LIST);
            break;
        }
        }
        return 0;
    }
    }
    return -LX_EINVAL;
}

/* lkd_procfs.h: services of the procfs module for the engine and for tests. Owner: procfs.
 * Include as "lkd_procfs.h" (src/include/lkd_procfs.h forwards here, request procfs-1).
 *
 * procfs registers no syscall: it serves /proc, /sys and /dev through the virtual mount interface of
 * vfs.h (mounted by lkd_procfs_init). Every file is generated from runtime state at read time.
 */
#ifndef LKD_PROCFS_H
#define LKD_PROCFS_H

#include "task.h"

struct lkd_fdobj;

/* ---- CPUID provider (engine) ------------------------------------------------------------------ */

/* /proc/cpuinfo is built from the CPUID results the guest's CPUID instruction returns, so that both
 * views agree. Only the engine can answer them (FEXCore RunCPUIDFunction / RunCPUIDFunctionName).
 * cpu is the logical CPU index for per-CPU leaves (brand string leaves 0x80000002..4 of a hybrid
 * model); other leaves ignore it. Returns 0, or -1 when the leaf is not supported. Called from any
 * thread, without LKD locks. Without a provider, opening /proc/cpuinfo fails with -ENOSYS (logged). */
struct lkd_cpuid_regs { uint32_t eax, ebx, ecx, edx; };
typedef int (*lkd_cpuid_fn)(uint32_t leaf, uint32_t subleaf, int cpu, struct lkd_cpuid_regs *out);
void lkd_procfs_set_cpuid(lkd_cpuid_fn fn);

/* The host's highest CPU clock in kHz (the top bin of its fastest cores), 0 when unknown. Asked once per process, at
 * the first read of a file that shows it: /proc/cpuinfo "cpu MHz" and /sys/devices/system/cpu/cpuN/cpufreq. Without a
 * provider, "cpu MHz" comes from CPUID 0x15 and there is no cpufreq directory. */
typedef uint32_t (*lkd_cpu_max_khz_fn)(void);
void lkd_procfs_set_cpu_max_khz(lkd_cpu_max_khz_fn fn);

/* ---- Backend (tests replace it) --------------------------------------------------------------- */

/* Request of a file of another host process of the family, answered by its procfs (famsvc op
 * LKD_FAMSVC_OP_TASK_STATUS). The receiver renders with its own local state. */
struct lkd_procfs_wire {
    uint32_t kind;        /* procfs node kind (procfs_priv.h PK_*) */
    int32_t pid, tid;     /* thread group and thread */
    int32_t per_thread;   /* task/<tid> view */
    int32_t fd;           /* fd/N, fdinfo/N */
    int32_t permitted;    /* the viewer passed PTRACE_MODE_READ for this target (stat fields) */
    int32_t viewer_pid;
    int32_t open_fd;      /* PK_FD: reopen the fd in the target and send a host descriptor back */
    uint32_t lx_flags;    /* open flags for open_fd */
};

/* Where procfs gets process facts. Defaults: the task module registry (lkd_proc_lookup ...), the
 * ptrace module access check (lkd_ptrace_check_access) and famsvc for other host processes.
 * remote returns 0 with a malloc'ed text in *text (caller frees), or -errno. */
struct lkd_procfs_backend {
    int (*proc_lookup)(int32_t pid, lkd_proc_info *out);
    int (*tid_lookup)(int32_t tid, lkd_tid_info *out);
    int (*proc_list)(lkd_proc_info *out, int max);
    int (*tid_list)(int32_t tgid, int32_t *tids, int max);
    int (*access)(lkd_task *caller, int32_t pid, unsigned mode);
    int (*remote)(int32_t host_pid, const struct lkd_procfs_wire *req, char **text, size_t *len);
};
/* Replace the backend (copied); NULL restores the defaults. Tests only. */
void lkd_procfs_set_backend(const struct lkd_procfs_backend *b);
/* Render a request as the receiving process would (tests drive the remote path with it). */
int lkd_procfs_render_wire(const struct lkd_procfs_wire *req, char **text, size_t *len);

/* ---- Devices ---------------------------------------------------------------------------------- */

/* 1 if o is an open /dev/zero (mmap of it is anonymous memory in Linux, drivers/char/mem.c
 * mmap_zero); 0 otherwise. For the mm module (docs/requests/procfs-2.md). */
int lkd_procfs_dev_is_zero(struct lkd_fdobj *o);

/* A host descriptor that arrives without a guest path (SCM_RIGHTS from another host process, the runtime's
 * standard descriptors) and is a host twin of an emulated device (/dev/null, /dev/zero, /dev/random,
 * /dev/urandom): 1 and *out = that device, which takes host_fd; 0 when it is not one (host_fd untouched);
 * -errno. st is fstat(host_fd). For the fs module. */
struct stat;
int lkd_procfs_dev_adopt(int host_fd, const struct stat *st, uint32_t lx_flags, struct lkd_fdobj **out);

/* Number of logical CPUs presented to the guest (/sys/devices/system/cpu, /proc/cpuinfo, /proc/stat):
 * the host's hw.logicalcpu. */
int lkd_procfs_ncpu(void);

#endif

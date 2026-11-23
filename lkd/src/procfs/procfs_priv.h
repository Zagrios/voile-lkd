/* procfs_priv.h: private interfaces of the procfs module (src/procfs). Not for other modules.
 *
 * Layout:
 *   buf.c      growable text buffer
 *   node.c     path resolution of the three trees (/proc, /sys, /dev), getattr, readdir, readlink
 *   file.c     the open file object of generated files (read, write, lseek, fstat) and open()
 *   pid.c      per-process files (/proc/<pid>/...), local rendering, permission rules
 *   global.c   /proc/cpuinfo, meminfo, uptime, loadavg, version, stat, filesystems, sys/, net/
 *   sysfs.c    /sys/devices/system/cpu and clocksource
 *   dev.c      /dev nodes and their objects
 *   remote.c   famsvc handlers and the default backend
 *   init.c     mounts, module hooks
 */
#ifndef LKD_PROCFS_PRIV_H
#define LKD_PROCFS_PRIV_H

/* tables of struct pf_tent leave the trailing fields zero on purpose */
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include <sys/random.h>
#include "lkd_internal.h"
#include "fd.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task.h"
#include "vfs.h"
#include "lkd_procfs.h"
#include "net.h"
#include "../fs/lkd_fs.h"

/* ---- Text buffer (buf.c) ---------------------------------------------------------------------- */

struct pbuf { char *p; size_t n, cap; int oom; };
void pb_need(struct pbuf *b, size_t more);
void pb_putc(struct pbuf *b, char c);
void pb_write(struct pbuf *b, const void *s, size_t n);
void pb_puts(struct pbuf *b, const char *s);
void pb_printf(struct pbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* seq_put_decimal_ull_width: value right-aligned in width columns. */
void pb_udec_w(struct pbuf *b, const char *prefix, uint64_t v, int width);
/* %*pb and %*pbl of a CPU mask with nbits bits (lib/bitmap-str.c bitmap_print_to_pagebuf). */
void pb_mask(struct pbuf *b, uint64_t mask, int nbits);
void pb_mask_list(struct pbuf *b, uint64_t mask, int nbits);
void pb_free(struct pbuf *b);

/* ---- Mounts and nodes ------------------------------------------------------------------------- */

enum { PF_PROC = 0, PF_SYS = 1, PF_DEV = 2 };

enum pf_kind {
    PK_NONE = 0,
    /* /proc, global */
    PK_ROOT, PK_SELF, PK_THREAD_SELF, PK_MOUNTS_LNK, PK_NET_LNK,
    PK_CPUINFO, PK_MEMINFO, PK_UPTIME, PK_LOADAVG, PK_VERSION, PK_GSTAT, PK_FILESYSTEMS,
    PK_SDIR,                /* directory of a static table (/proc/sys..., /sys...) */
    PK_SFILE,               /* file of a static table, generator id in node.sub */
    /* /proc/<pid> */
    PK_PID, PK_TASKDIR, PK_TID, PK_FDDIR, PK_FDINFODIR, PK_FD, PK_FDINFO, PK_PNETDIR, PK_PNET,
    PK_ENVIRON, PK_AUXV, PK_STATUS, PK_PERSONALITY, PK_LIMITS, PK_COMM, PK_SYSCALL, PK_CMDLINE,
    PK_STAT, PK_STATM, PK_MAPS, PK_MEM, PK_CWD, PK_ROOTLNK, PK_EXE, PK_MOUNTS, PK_MOUNTINFO, PK_SMAPS,
    PK_WCHAN, PK_CGROUP,
    PK_HOSTPID,             /* LKD's own lkd_host_pid, not a Linux file */
    /* /sys dynamic */
    PK_CPUN, PK_TOPODIR, PK_TOPO, PK_CACHEDIR, PK_CACHEIDX, PK_CACHEF, PK_FREQDIR, PK_FREQ,
    /* /dev */
    PK_DEVROOT, PK_DEVNODE, PK_DEVLNK,
    /* remote-only requests (famsvc) */
    PK_WIRE_FDLIST,
    PK_COUNT
};

/* Generator ids of static files (node.sub for PK_SFILE). */
enum pf_sfile {
    SF_OSRELEASE = 1, SF_OSTYPE, SF_HOSTNAME, SF_DOMAINNAME, SF_PID_MAX, SF_RANDOMIZE, SF_UUID, SF_BOOT_ID,
    SF_OVERCOMMIT, SF_MAX_MAP_COUNT, SF_MMAP_MIN_ADDR,
    SF_INOTIFY_INSTANCES, SF_INOTIFY_WATCHES, SF_INOTIFY_QUEUED,
    SF_CPU_ONLINE, SF_CPU_POSSIBLE, SF_CPU_PRESENT, SF_CPU_OFFLINE,
    SF_CLOCKSOURCE_CUR, SF_CLOCKSOURCE_AVAIL, SF_VERSION_STR, SF_PIPE_MAX, SF_CPU_KERNEL_MAX,
};

struct pf_node {
    int mnt;               /* PF_PROC, PF_SYS, PF_DEV */
    int kind;              /* PK_* */
    uint32_t mode;         /* LX_S_IF* | permissions */
    uint32_t uid, gid;
    uint32_t nlink;
    int32_t pid;           /* thread group of a /proc/<pid> node, 0 otherwise */
    int32_t tid;           /* thread: the leader for a process view, the thread for task/<tid> */
    int per_thread;        /* task/<tid>/ view (or /proc/<tid> of a non-leader thread) */
    int fd;                /* fd/N, fdinfo/N */
    int cpu, cidx;         /* /sys cpuN, cache indexN */
    int sub;               /* generator id (PK_SFILE, PK_PNET, PK_TOPO, PK_CACHEF, PK_DEVNODE) */
    const void *tent;      /* static table entry of a PK_SDIR / PK_SFILE node */
    uint64_t ino;
    uint64_t rdev;
    uint64_t size;
    uint8_t magic;
    int64_t mtime;
    uint32_t euid, egid;   /* owner of the process a /proc/<pid> node belongs to */
    uint8_t dumpable, has_mm;
};

/* Resolve rel (canonical, relative to the mount) into node. 0, -ENOENT, -EACCES, -ENOTDIR. */
int pf_resolve(lkd_task *t, int mnt, const char *rel, struct pf_node *n);
int pf_readdir(lkd_task *t, int mnt, const char *rel, uint64_t i, char name[256], uint8_t *dt, uint64_t *ino);
int64_t pf_readlink(lkd_task *t, int mnt, const char *rel, char *buf, size_t cap);
int pf_open(lkd_task *t, int mnt, const char *rel, uint32_t fl, uint32_t mode, lkd_fdobj **out);
/* Importers of the records this module sends (fd.h): LKD_FDREC_DEV (dev.c), LKD_FDREC_PROCFS (file.c). */
void pf_dev_register(void);
void pf_file_register(void);
/* The LKD_FDREC_PROCFS record of an object with a canonical gpath at position pos (file.c). */
int pf_rec_export(lkd_fdobj *o, int64_t pos, int *hfd);
void pf_vattr(const struct pf_node *n, struct lkd_vattr *va);
/* Directory entries of a static directory node (/proc/sys..., /sys...). */
struct pf_tent {
    const char *name;
    int type;                       /* 'd', 'f', 'l' */
    uint16_t perm;
    int sub;                        /* SF_* of a file */
    const struct pf_tent *kids;     /* directory content, NULL-name terminated */
    int (*present)(void);           /* optional: entry exists only when this returns 1 */
    const char *link;               /* 'l' target */
};

/* ---- Processes (pid.c) ------------------------------------------------------------------------ */

/* The process (and thread) a /proc/<pid> node designates. tk is a counted reference to the local task
 * when the process lives in this host process, NULL when it is in another host process (remote) or a
 * zombie (registry record only). */
struct pf_target {
    int32_t pid, tid;
    int per_thread;
    lkd_proc_info pi;
    lkd_task *tk;
    int zombie;
};
int pf_target_get(int32_t pid, int32_t tid, int per_thread, struct pf_target *out);
void pf_target_put(struct pf_target *tg);

/* generic_permission (fs/namei.c) for a node with mode/uid/gid and the viewer's fsuid/fsgid/groups.
 * want: MAY_READ 4, MAY_WRITE 2, MAY_EXEC 1. Returns 0 or -EACCES. */
int pf_permission(lkd_task *viewer, uint32_t mode, uint32_t uid, uint32_t gid, int want);
/* ptrace_may_access(): the same thread group always passes (kernel/ptrace.c __ptrace_may_access). */
int pf_ptrace_ok(lkd_task *viewer, int32_t pid, unsigned mode);
/* Render a per-process file of a LOCAL task. req->permitted is the viewer's PTRACE_MODE_READ result. */
int pf_render_local(const struct lkd_procfs_wire *req, lkd_task *tk, struct pbuf *out);
/* Render from the registry only (zombie). */
int pf_render_zombie(const struct lkd_procfs_wire *req, const lkd_proc_info *pi, struct pbuf *out);
/* Generate the content of a /proc/<pid> node for a viewer: local, zombie or remote. */
int pf_generate_pid(lkd_task *viewer, const struct pf_node *n, char **text, size_t *len);
/* Readlink of exe, cwd, root, fd/N (text into buf). */
int64_t pf_pid_readlink(lkd_task *viewer, const struct pf_node *n, char *buf, size_t cap);
/* Open of a magic link (exe, cwd, root, fd/N). */
int pf_pid_open_link(lkd_task *viewer, const struct pf_node *n, uint32_t fl, lkd_fdobj **out);
int pf_fd_status_flags(int32_t pid, int32_t tid, int fd, uint32_t *fl);
/* Open fds of the target (ascending); count or -errno. *fds is malloc'ed. */
int pf_fd_list(lkd_task *viewer, int32_t pid, int32_t tid, int **fds);
/* /proc/<pid>/mem object. */
int pf_mem_open(lkd_task *viewer, const struct pf_node *n, uint32_t fl, lkd_fdobj **out);
/* comm write */
int64_t pf_comm_write(lkd_task *viewer, const struct pf_node *n, const char *data, size_t len);

/* ---- Global files (global.c) ------------------------------------------------------------------ */

int pf_generate_global(lkd_task *viewer, const struct pf_node *n, struct pbuf *out);
extern const struct pf_tent pf_proc_sys[];
extern const struct pf_tent pf_sys_root[];
/* Host facts. */
int pf_host_ncpu(void);
int64_t pf_boot_realtime_sec(void);          /* kern.boottime */
int64_t pf_boot_ns(void);                    /* ns since boot (CLOCK_MONOTONIC_RAW, the guest BOOTTIME) */
int pf_cpuid(uint32_t leaf, uint32_t sub, int cpu, struct lkd_cpuid_regs *r);
int pf_cpuid_available(void);
uint32_t pf_cpu_max_khz(void);               /* lkd_procfs_set_cpu_max_khz, 0 when unknown */
int pf_inotify_present(void);
/* fs_get_root, fs_host_io and fs_stat_from_host: declared in lkd_fs.h (request procfs-4 point 1). */

/* ---- sysfs (sysfs.c) -------------------------------------------------------------------------- */

int pf_sys_generate(const struct pf_node *n, struct pbuf *out);
int pf_sys_cpu_lookup(int cpu, const char *name, struct pf_node *n);
int pf_sys_cpu_readdir(const struct pf_node *dir, uint64_t i, char name[256], uint8_t *dt);
int pf_cache_count(void);
int pf_clocksource_present(void);

/* ---- /dev (dev.c) ----------------------------------------------------------------------------- */

int pf_dev_lookup(const char *name, struct pf_node *n);
int pf_dev_readdir(uint64_t i, char name[256], uint8_t *dt, uint64_t *ino);
int pf_dev_open(lkd_task *t, const struct pf_node *n, uint32_t fl, lkd_fdobj **out);
int64_t pf_dev_readlink(const struct pf_node *n, char *buf, size_t cap);

/* ---- Generated file objects (file.c) ---------------------------------------------------------- */

/* st_dev of a mount (learned from the vfs on first use). */
uint64_t pf_mount_dev(lkd_task *t, int mnt);
const char *pf_mount_point(int mnt);
/* fstat fields of a node. */
void pf_stat_node(lkd_task *t, const struct pf_node *n, struct lx_stat *st);

/* ---- Backend and remote (remote.c) ------------------------------------------------------------ */

extern struct lkd_procfs_backend pf_be;
/* Reopen fd n->fd of a process of another host process and receive a host descriptor for it. */
int pf_remote_open_fd(int32_t host_pid, const struct pf_node *n, uint32_t fl, lkd_fdobj **out);
void pf_remote_init(void);

/* A new open file description on a received host descriptor (owned by the object), with a private offset. */
int pf_dupfile_new(int host_fd, uint32_t status_flags, const char *gpath, lkd_fdobj **out);

/* ---- Misc ------------------------------------------------------------------------------------- */

int64_t pf_now_realtime_sec(void);
/* Same for the uid used by fs to present the host owner (src/core/api.c, docs/requests/fs-1.md). */
const lkd_config *lkd_config_get(void);

#define PF_TEXT_MAX (64u << 20)   /* generated files are capped (maps of a huge process is ~ MiBs) */

#endif

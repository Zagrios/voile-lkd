/* node.c: path resolution, attributes, listings and symlinks of the /proc, /sys and /dev trees.
 *
 * Linux references (v6.12): fs/proc/root.c, fs/proc/base.c (tgid_base_stuff, tid_base_stuff, modes,
 * proc_pid_lookup, proc_fd_permission), fs/proc/fd.c (fd link modes), fs/proc/self.c,
 * fs/proc/thread_self.c. The vfs (fs module) walks one component at a time and calls getattr on each
 * prefix, so every lookup below re-resolves the whole relative path.
 */
#include "procfs_priv.h"

static const char *const g_mount_points[3] = { "/proc", "/sys", "/dev" };
const char *pf_mount_point(int mnt) { return (mnt >= 0 && mnt < 3) ? g_mount_points[mnt] : "/"; }

/* Inode numbers: stable hash of what designates the node (Linux allocates proc inodes dynamically;
 * only stability between two stats of the same path matters to programs). The /proc root is 1
 * (PROC_ROOT_INO). */
static uint64_t node_ino(const struct pf_node *n)
{
    if (n->mnt == PF_PROC && n->kind == PK_ROOT) return 1;
    uint64_t h = 1469598103934665603ull;
    uint64_t parts[10] = { (uint64_t)n->mnt, (uint64_t)n->kind, (uint64_t)(uint32_t)n->pid, (uint64_t)(uint32_t)n->tid,
                           (uint64_t)n->per_thread, (uint64_t)(uint32_t)n->fd, (uint64_t)(uint32_t)n->cpu,
                           (uint64_t)(uint32_t)n->cidx, (uint64_t)(uint32_t)n->sub, (uint64_t)(uintptr_t)n->tent };
    for (int i = 0; i < 10; i++) { h ^= parts[i]; h *= 1099511628211ull; }
    h = (h ^ (h >> 32)) & 0xffffffffu;
    return h < 16 ? h + 16 : h;
}

void pf_vattr(const struct pf_node *n, struct lkd_vattr *va)
{
    memset(va, 0, sizeof *va);
    va->mode = n->mode;
    va->uid = n->uid;
    va->gid = n->gid;
    va->nlink = n->nlink ? n->nlink : 1;
    va->ino = n->ino;
    va->size = n->size;
    va->rdev = n->rdev;
    va->mtime_sec = n->mtime;
    va->magic_link = n->magic;
}

static void set_dir(struct pf_node *n, uint32_t perm) { n->mode = LX_S_IFDIR | perm; n->nlink = 2; }
static void set_reg(struct pf_node *n, uint32_t perm) { n->mode = LX_S_IFREG | perm; n->nlink = 1; }
static void set_lnk(struct pf_node *n) { n->mode = LX_S_IFLNK | 0777; n->nlink = 1; }

static int parse_num(const char *s, int32_t *out)
{
    if (!s[0] || (s[0] == '0' && s[1])) return 0;   /* no leading zeros: "01" is not a pid (name_to_int) */
    int64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (*p - '0');
        if (v > 0x7fffffff) return 0;
    }
    *out = (int32_t)v;
    return 1;
}

/* ---- /proc tables ------------------------------------------------------------------------------ */

struct pid_ent { const char *name; int kind; char type; uint16_t perm; };

/* fs/proc/base.c tgid_base_stuff order, limited to the entries LKD serves. Entries absent here are
 * absent in the modeled kernel: io (no CONFIG_TASK_IO_ACCOUNTING data, docs/status/procfs.md),
 * map_files, ns, sched, stack, pagemap, oom_*, attr, loginuid, uid_map. */
static const struct pid_ent g_pid_ents[] = {
    { "task", PK_TASKDIR, 'd', 0555 }, { "fd", PK_FDDIR, 'd', 0500 }, { "fdinfo", PK_FDINFODIR, 'd', 0555 },
    { "net", PK_PNETDIR, 'd', 0555 }, { "environ", PK_ENVIRON, 'f', 0400 }, { "auxv", PK_AUXV, 'f', 0400 },
    { "status", PK_STATUS, 'f', 0444 }, { "personality", PK_PERSONALITY, 'f', 0400 },
    { "limits", PK_LIMITS, 'f', 0444 }, { "comm", PK_COMM, 'f', 0644 }, { "syscall", PK_SYSCALL, 'f', 0400 },
    { "cmdline", PK_CMDLINE, 'f', 0444 }, { "stat", PK_STAT, 'f', 0444 }, { "statm", PK_STATM, 'f', 0444 },
    { "maps", PK_MAPS, 'f', 0444 }, { "mem", PK_MEM, 'f', 0600 }, { "cwd", PK_CWD, 'l', 0777 },
    { "root", PK_ROOTLNK, 'l', 0777 }, { "exe", PK_EXE, 'l', 0777 }, { "mounts", PK_MOUNTS, 'f', 0444 },
    { "mountinfo", PK_MOUNTINFO, 'f', 0444 }, { "smaps", PK_SMAPS, 'f', 0444 }, { "wchan", PK_WCHAN, 'f', 0444 },
    { "cgroup", PK_CGROUP, 'f', 0444 },
    /* LKD's own: the macOS pid of the host process that carries the process (or the thread), for guest code that
     * hands its pid to a macOS program (Steam's IPC). Reachable by name, never listed: a listing of /proc/<pid>
     * stays the one of the modeled kernel. */
    { "lkd_host_pid", PK_HOSTPID, 'f', 0444 },
    { NULL, 0, 0, 0 }
};

/* The index is the generator id pid.c renders (n->sub). */
static const char *const g_pnet_files[] = { "dev", "if_inet6", "route", "ipv6_route", NULL };

struct root_ent { const char *name; int kind; char type; uint16_t perm; const char *link; };
/* Alphabetical, like the proc_root rbtree. */
static const struct root_ent g_root_ents[] = {
    { "cpuinfo", PK_CPUINFO, 'f', 0444, NULL }, { "filesystems", PK_FILESYSTEMS, 'f', 0444, NULL },
    { "loadavg", PK_LOADAVG, 'f', 0444, NULL }, { "meminfo", PK_MEMINFO, 'f', 0444, NULL },
    { "mounts", PK_MOUNTS_LNK, 'l', 0777, "self/mounts" }, { "net", PK_NET_LNK, 'l', 0777, "self/net" },
    { "self", PK_SELF, 'l', 0777, NULL }, { "stat", PK_GSTAT, 'f', 0444, NULL },
    { "sys", PK_SDIR, 'd', 0555, NULL }, { "thread-self", PK_THREAD_SELF, 'l', 0777, NULL },
    { "uptime", PK_UPTIME, 'f', 0444, NULL }, { "version", PK_VERSION, 'f', 0444, NULL },
    { NULL, 0, 0, 0, NULL }
};

/* ---- Static trees ------------------------------------------------------------------------------ */

static int tent_present(const struct pf_tent *e) { return !e->present || e->present(); }

static int tent_step(const struct pf_tent *dir, const char *name, struct pf_node *n)
{
    for (const struct pf_tent *e = dir->kids; e && e->name; e++) {
        if (strcmp(e->name, name) || !tent_present(e)) continue;
        n->tent = e;
        n->sub = e->sub;
        if (e->type == 'd') { n->kind = PK_SDIR; set_dir(n, e->perm); }
        else if (e->type == 'l') { n->kind = PK_SFILE; set_lnk(n); }
        else { n->kind = PK_SFILE; set_reg(n, e->perm); }
        if (n->mnt == PF_SYS && e->type == 'f') n->size = 4096; /* kernfs attributes: i_size = PAGE_SIZE */
        return 0;
    }
    return -LX_ENOENT;
}

static const struct pf_tent g_proc_sys_dir = { "sys", 'd', 0555, 0, pf_proc_sys, NULL, NULL };
static const struct pf_tent g_sys_root_dir = { "", 'd', 0755, 0, pf_sys_root, NULL, NULL };

/* ---- Walk -------------------------------------------------------------------------------------- */

static void root_node(int mnt, struct pf_node *n)
{
    memset(n, 0, sizeof *n);
    n->mnt = mnt;
    n->fd = -1;
    if (mnt == PF_PROC) { n->kind = PK_ROOT; set_dir(n, 0555); }
    else if (mnt == PF_SYS) { n->kind = PK_SDIR; n->tent = &g_sys_root_dir; set_dir(n, 0755); }
    else { n->kind = PK_DEVROOT; set_dir(n, 0755); }
}

/* task_dump_owner (fs/proc/base.c): euid/egid, but root for everything except the 0555 directories when
 * the process has an mm that is not dumpable. */
static void own(struct pf_node *n)
{
    int plain_dir = n->mode == (LX_S_IFDIR | 0555);
    if (!plain_dir && n->has_mm && n->dumpable != 1) { n->uid = n->gid = 0; return; }
    n->uid = n->euid;
    n->gid = n->egid;
}

/* Attributes common to every node under /proc/<pid>. */
static int pid_common(const struct pf_node *dir, struct pf_node *n)
{
    *n = *dir;
    n->tent = NULL;
    n->sub = 0;
    n->size = 0;
    n->magic = 0;
    return 0;
}

static int step_pid_dir(lkd_task *t, const struct pf_node *dir, const char *name, struct pf_node *n)
{
    pid_common(dir, n);
    for (const struct pid_ent *e = g_pid_ents; e->name; e++) {
        if (strcmp(e->name, name)) continue;
        if (e->kind == PK_TASKDIR && dir->kind == PK_TID) return -LX_ENOENT;  /* tid_base_stuff has no task */
        n->kind = e->kind;
        if (e->type == 'd') set_dir(n, e->perm);
        else if (e->type == 'l') { set_lnk(n); n->magic = 1; }
        else set_reg(n, e->perm);
        own(n);
        return 0;
    }
    return -LX_ENOENT;
}

/* fs/proc/fd.c: the link mode follows the access mode of the open file. A local task is read directly
 * (no fdinfo, so no object lock: readdir of fd/ runs with the lock of the directory object held). */
static uint32_t fd_link_mode(lkd_task *viewer, int32_t pid, int32_t tid, int fd)
{
    uint32_t fl = 0;
    int have = pf_fd_status_flags(pid, tid, fd, &fl) == 0;
    if (!have) {
        struct pf_node q = { .mnt = PF_PROC, .kind = PK_FDINFO, .pid = pid, .tid = tid, .fd = fd };
        char *txt = NULL;
        size_t len = 0;
        if (pf_generate_pid(viewer, &q, &txt, &len) == 0 && txt) {
            const char *f = strstr(txt, "flags:\t");
            if (f) { fl = (uint32_t)strtoul(f + 7, NULL, 8); have = 1; }
        }
        free(txt);
    }
    if (!have) return LX_S_IFLNK | 0700;
    uint32_t acc = fl & LX_O_ACCMODE;
    if (fl & LX_O_PATH) return LX_S_IFLNK;      /* FMODE_PATH: neither read nor write */
    if (acc == LX_O_RDONLY) return LX_S_IFLNK | 0500;
    if (acc == LX_O_WRONLY) return LX_S_IFLNK | 0300;
    return LX_S_IFLNK | 0700;
}

static int fd_exists(lkd_task *viewer, int32_t pid, int32_t tid, int fd)
{
    int *fds = NULL;
    int n = pf_fd_list(viewer, pid, tid, &fds);
    int found = 0;
    for (int i = 0; i < n; i++) if (fds[i] == fd) { found = 1; break; }
    free(fds);
    return n < 0 ? n : found;
}

/* proc_fd_permission: generic_permission on the 0500 directory, or the caller's own thread group. */
static int fd_dir_access(lkd_task *t, const struct pf_node *dir)
{
    if (t && t->proc && t->proc->pid == dir->pid) return 0;
    return pf_permission(t, dir->mode, dir->uid, dir->gid, 1);
}

static int step(lkd_task *t, const struct pf_node *dir, const char *name, struct pf_node *n)
{
    int32_t num;
    switch (dir->kind) {
    case PK_ROOT: {
        if (parse_num(name, &num) && num > 0) {
            memset(n, 0, sizeof *n);
            n->mnt = PF_PROC;
            n->fd = -1;
            lkd_proc_info *pi = malloc(sizeof *pi);
            if (!pi) return -LX_ENOMEM;
            int found = pf_be.proc_lookup(num, pi) == 0;
            if (found) { n->pid = num; n->tid = num; }
            else {
                /* proc_pid_lookup finds any task by id: /proc/<tid> of a thread is reachable, unlisted. */
                lkd_tid_info ti;
                if (pf_be.tid_lookup(num, &ti) == 0 && pf_be.proc_lookup(ti.tgid, pi) == 0) {
                    n->pid = ti.tgid;
                    n->tid = num;
                    found = 1;
                }
            }
            if (found) {
                n->kind = PK_PID;
                set_dir(n, 0555);
                n->euid = pi->euid; n->egid = pi->egid; n->dumpable = pi->dumpable;
                n->has_mm = pi->state != LKD_PS_ZOMBIE;
                own(n);
                n->mtime = pf_boot_realtime_sec() + (int64_t)(pi->start_mono_ns / 1000000000ull);
            }
            free(pi);
            return found ? 0 : -LX_ENOENT;
        }
        for (const struct root_ent *e = g_root_ents; e->name; e++) {
            if (strcmp(e->name, name)) continue;
            memset(n, 0, sizeof *n);
            n->mnt = PF_PROC;
            n->fd = -1;
            n->kind = e->kind;
            if (e->kind == PK_SDIR) { n->tent = &g_proc_sys_dir; set_dir(n, 0555); }
            else if (e->type == 'l') set_lnk(n);
            else set_reg(n, e->perm);
            n->mtime = pf_boot_realtime_sec();
            return 0;
        }
        return -LX_ENOENT;
    }
    case PK_PID:
    case PK_TID:
        return step_pid_dir(t, dir, name, n);
    case PK_TASKDIR: {
        if (!parse_num(name, &num)) return -LX_ENOENT;
        int32_t tids[512];
        int c = pf_be.tid_list(dir->pid, tids, 512);
        for (int i = 0; i < c; i++) {
            if (tids[i] != num) continue;
            pid_common(dir, n);
            n->kind = PK_TID;
            n->tid = num;
            n->per_thread = 1;
            set_dir(n, 0555);
            return 0;
        }
        return -LX_ENOENT;
    }
    case PK_FDDIR:
    case PK_FDINFODIR: {
        if (dir->kind == PK_FDDIR) {
            int a = fd_dir_access(t, dir);
            if (a < 0) return a;
        }
        if (!parse_num(name, &num)) return -LX_ENOENT;
        int ex = fd_exists(t, dir->pid, dir->tid, num);
        if (ex < 0) return ex == -LX_EACCES ? -LX_EACCES : -LX_ENOENT;
        if (!ex) return -LX_ENOENT;
        pid_common(dir, n);
        n->fd = num;
        if (dir->kind == PK_FDDIR) {
            n->kind = PK_FD;
            n->mode = fd_link_mode(t, dir->pid, dir->tid, num);
            n->nlink = 1;
            n->magic = 1;
        } else {
            n->kind = PK_FDINFO;
            set_reg(n, 0444);   /* proc_fdinfo_instantiate: S_IFREG | S_IRUGO */
        }
        own(n);
        return 0;
    }
    case PK_PNETDIR:
        for (int i = 0; g_pnet_files[i]; i++) {
            if (strcmp(g_pnet_files[i], name)) continue;
            pid_common(dir, n);
            n->kind = PK_PNET;
            n->sub = i;
            set_reg(n, 0444);
            n->uid = n->gid = 0;   /* proc_net entries belong to the namespace owner (root) */
            return 0;
        }
        return -LX_ENOENT;
    case PK_SDIR: {
        const struct pf_tent *td = dir->tent;
        memset(n, 0, sizeof *n);
        n->mnt = dir->mnt;
        n->fd = -1;
        n->mtime = dir->mtime;
        if (td && tent_step(td, name, n) == 0) return 0;
        if (td && td->sub == 1 && dir->mnt == PF_SYS)   /* /sys/devices/system/cpu: cpuN */
            return pf_sys_cpu_lookup(-1, name, n);
        return -LX_ENOENT;
    }
    case PK_CPUN: case PK_TOPODIR: case PK_CACHEDIR: case PK_CACHEIDX: {
        *n = *dir;
        return pf_sys_cpu_lookup(dir->kind, name, n);
    }
    case PK_DEVROOT:
        return pf_dev_lookup(name, n);
    default:
        return -LX_ENOTDIR;
    }
}

int pf_resolve(lkd_task *t, int mnt, const char *rel, struct pf_node *n)
{
    struct pf_node cur;
    root_node(mnt, &cur);
    cur.mtime = pf_boot_realtime_sec();
    char comp[256];
    const char *p = rel ? rel : "";
    while (*p) {
        const char *s = strchr(p, '/');
        size_t len = s ? (size_t)(s - p) : strlen(p);
        if (len == 0) { p++; continue; }
        if (len >= sizeof comp) return -LX_ENAMETOOLONG;
        memcpy(comp, p, len);
        comp[len] = 0;
        if (!LX_S_ISDIR(cur.mode)) return -LX_ENOTDIR;
        struct pf_node nx;
        memset(&nx, 0, sizeof nx);
        nx.mnt = mnt;
        nx.fd = -1;
        int rc = step(t, &cur, comp, &nx);
        if (rc < 0) return rc;
        if (!nx.mtime) nx.mtime = cur.mtime;
        cur = nx;
        p += len;
    }
    cur.ino = node_ino(&cur);
    *n = cur;
    return 0;
}

/* ---- readdir ----------------------------------------------------------------------------------- */

static uint8_t dt_of(char type) { return type == 'd' ? LX_DT_DIR : type == 'l' ? LX_DT_LNK : LX_DT_REG; }

static int emit(const char *nm, uint8_t type, char name[256], uint8_t *dt)
{
    snprintf(name, 256, "%s", nm);
    *dt = type;
    return 1;
}

/* The listing of dir at index i; *ino is filled by resolving the child (same number as stat). */
static int list_entry(lkd_task *t, const struct pf_node *dir, uint64_t i, char name[256], uint8_t *dt)
{
    switch (dir->kind) {
    case PK_ROOT: {
        uint64_t nfix = 0;
        while (g_root_ents[nfix].name) nfix++;
        if (i < nfix) return emit(g_root_ents[i].name, dt_of(g_root_ents[i].type), name, dt);
        i -= nfix;
        lkd_proc_info *list = malloc(1024 * sizeof *list);
        if (!list) return -LX_ENOMEM;
        int c = pf_be.proc_list(list, 1024);
        int r = 0;
        if (i < (uint64_t)c) { snprintf(name, 256, "%d", list[i].pid); *dt = LX_DT_DIR; r = 1; }
        free(list);
        return r;
    }
    case PK_PID:
    case PK_TID: {
        uint64_t k = 0;
        for (const struct pid_ent *e = g_pid_ents; e->name; e++) {
            if (e->kind == PK_TASKDIR && dir->kind == PK_TID) continue;
            if (e->kind == PK_HOSTPID) continue;
            if (k++ == i) return emit(e->name, dt_of(e->type), name, dt);
        }
        return 0;
    }
    case PK_TASKDIR: {
        int32_t tids[512];
        int c = pf_be.tid_list(dir->pid, tids, 512);
        if (c < 0) return c;
        if (i >= (uint64_t)c) return 0;
        snprintf(name, 256, "%d", tids[i]);
        *dt = LX_DT_DIR;
        return 1;
    }
    case PK_FDDIR:
    case PK_FDINFODIR: {
        int *fds = NULL;
        int c = pf_fd_list(t, dir->pid, dir->tid, &fds);
        if (c < 0) return c;
        int r = 0;
        if (i < (uint64_t)c) { snprintf(name, 256, "%d", fds[i]); *dt = dir->kind == PK_FDDIR ? LX_DT_LNK : LX_DT_REG; r = 1; }
        free(fds);
        return r;
    }
    case PK_PNETDIR:
        for (uint64_t k = 0; g_pnet_files[k]; k++) if (k == i) return emit(g_pnet_files[k], LX_DT_REG, name, dt);
        return 0;
    case PK_SDIR: {
        uint64_t k = 0;
        const struct pf_tent *td = dir->tent;
        for (const struct pf_tent *e = td ? td->kids : NULL; e && e->name; e++) {
            if (!tent_present(e)) continue;
            if (k++ == i) return emit(e->name, dt_of(e->type), name, dt);
        }
        if (td && td->sub == 1 && dir->mnt == PF_SYS) {
            int ncpu = pf_host_ncpu();
            if (i - k < (uint64_t)ncpu) { snprintf(name, 256, "cpu%d", (int)(i - k)); *dt = LX_DT_DIR; return 1; }
        }
        return 0;
    }
    case PK_CPUN: case PK_TOPODIR: case PK_CACHEDIR: case PK_CACHEIDX:
        return pf_sys_cpu_readdir(dir, i, name, dt);
    case PK_DEVROOT: {
        uint64_t ino;
        return pf_dev_readdir(i, name, dt, &ino);
    }
    default:
        return -LX_ENOTDIR;
    }
}

int pf_readdir(lkd_task *t, int mnt, const char *rel, uint64_t i, char name[256], uint8_t *dt, uint64_t *ino)
{
    struct pf_node dir;
    int rc = pf_resolve(t, mnt, rel, &dir);
    if (rc < 0) return rc;
    if (!LX_S_ISDIR(dir.mode)) return -LX_ENOTDIR;
    if (dir.kind == PK_FDDIR) {
        rc = fd_dir_access(t, &dir);
        if (rc < 0) return rc;
    }
    rc = list_entry(t, &dir, i, name, dt);
    if (rc <= 0) return rc;
    if (dir.kind == PK_FDDIR || dir.kind == PK_FDINFODIR) {
        struct pf_node cn = dir;   /* same hash as pf_resolve, without inspecting the descriptor */
        cn.kind = dir.kind == PK_FDDIR ? PK_FD : PK_FDINFO;
        cn.fd = atoi(name);
        *ino = node_ino(&cn);
        return 1;
    }
    char child[1024];
    snprintf(child, sizeof child, "%s%s%s", rel && rel[0] ? rel : "", rel && rel[0] ? "/" : "", name);
    struct pf_node cn;
    *ino = pf_resolve(t, mnt, child, &cn) == 0 ? cn.ino : 0;
    return 1;
}

/* ---- readlink ---------------------------------------------------------------------------------- */

static int64_t copy_link(const char *s, char *buf, size_t cap)
{
    size_t n = strlen(s);
    if (n > cap) n = cap;
    memcpy(buf, s, n);
    return (int64_t)n;
}

int64_t pf_readlink(lkd_task *t, int mnt, const char *rel, char *buf, size_t cap)
{
    struct pf_node n;
    int rc = pf_resolve(t, mnt, rel, &n);
    if (rc < 0) return rc;
    if (!LX_S_ISLNK(n.mode)) return -LX_EINVAL;
    char tmp[64];
    switch (n.kind) {
    case PK_SELF:   /* fs/proc/self.c: the tgid of the caller; no task (service thread): -ENOENT */
        if (!t || !t->proc) return -LX_ENOENT;
        snprintf(tmp, sizeof tmp, "%d", t->proc->pid);
        return copy_link(tmp, buf, cap);
    case PK_THREAD_SELF:
        if (!t || !t->proc) return -LX_ENOENT;
        snprintf(tmp, sizeof tmp, "%d/task/%d", t->proc->pid, t->tid);
        return copy_link(tmp, buf, cap);
    case PK_MOUNTS_LNK: return copy_link("self/mounts", buf, cap);
    case PK_NET_LNK: return copy_link("self/net", buf, cap);
    case PK_SFILE: {
        const struct pf_tent *e = n.tent;
        return e && e->link ? copy_link(e->link, buf, cap) : -LX_EINVAL;
    }
    case PK_EXE: case PK_CWD: case PK_ROOTLNK: case PK_FD:
        return pf_pid_readlink(t, &n, buf, cap);
    case PK_DEVLNK:
        return pf_dev_readlink(&n, buf, cap);
    default:
        return -LX_EINVAL;
    }
}

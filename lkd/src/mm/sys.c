/* sys.c: memory syscalls (mmap, munmap, mprotect, brk, madvise, mincore, msync, mlock family, pkeys)
 * and the public mapping entry points of mm.h. Owner: mm. Error orders follow Linux v6.12:
 * mm/mmap.c do_mmap, mm/mprotect.c do_mprotect_pkey, mm/madvise.c do_madvise, mm/mincore.c,
 * mm/msync.c, mm/mlock.c, arch/x86/kernel/sys_x86_64.c. */
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mm_priv.h"
#include "lkd_procfs.h"
#include "lkd_fs.h"

#define LEGACY_MAP_MASK (LX_MAP_SHARED | LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_ANONYMOUS | LX_MAP_DENYWRITE | \
    LX_MAP_EXECUTABLE | LX_MAP_UNINITIALIZED | LX_MAP_GROWSDOWN | LX_MAP_LOCKED | LX_MAP_NORESERVE | \
    LX_MAP_POPULATE | LX_MAP_NONBLOCK | LX_MAP_STACK | LX_MAP_HUGETLB | LX_MAP_32BIT | 0x80u /* MAP_ABOVE4G */ | \
    (21u << 26) /* MAP_HUGE_2MB */ | (30u << 26) /* MAP_HUGE_1GB */)
#define LX_MAP_DROPPABLE 0x08u
#define LX_MADV_SOFT_OFFLINE 101
#define LX_MADV_HWPOISON 100

static uint64_t rlim_cur(lkd_task *t, int r)
{
    return t && t->proc ? t->proc->rlimits[r].rlim_cur : LX_RLIM_INFINITY;
}

/* can_do_mlock (mm/mlock.c) */
static int can_do_mlock(lkd_task *t)
{
    return rlim_cur(t, LX_RLIMIT_MEMLOCK) != 0 || mm_capable(t, LKD_CAP_IPC_LOCK);
}

/* mlock_future_ok (mm/mmap.c) */
static int mlock_future_ok(lkd_task *t, lkd_mm *mm, uint32_t vflags, uint64_t bytes)
{
    if (!(vflags & VF_LOCKED) || mm_capable(t, LKD_CAP_IPC_LOCK)) return 1;
    uint64_t lim = rlim_cur(t, LX_RLIMIT_MEMLOCK);
    return lim == LX_RLIM_INFINITY || mm->locked_bytes + bytes <= lim;
}

static uint32_t parse_maps_dev(uint64_t d, uint32_t *minor)
{
    *minor = (uint32_t)((d & 0xff) | ((d >> 12) & 0xfff00));
    return (uint32_t)((d & 0xfff00) >> 8);
}

/* Backing object for a file mapping: identity (device, inode, path) as the guest sees it. */
static int file_backing(lkd_task *t, struct lkd_fdobj *f, int host_fd, const char *name, struct mm_backing **out)
{
    struct mm_backing *b = mb_new(MB_FILE);
    if (!b) return -LX_ENOMEM;
    b->file = lkd_fdobj_ref(f);
    b->host_fd = host_fd;
    struct stat hs;
    if (fstat(host_fd, &hs) < 0) { int64_t e = LKD_HOST_ERR(); mb_put(b); return (int)e; }
    b->file_size = hs.st_size > 0 ? (uint64_t)hs.st_size : 0;
    struct lx_stat ls;
    if (!(f->ops && f->ops->fstat && f->ops->fstat(t, f, &ls) == 0)) lkd_stat_from_host(&hs, &ls);
    b->ino = ls.st_ino;
    b->dev_major = parse_maps_dev(ls.st_dev, &b->dev_minor);
    char buf[512];
    const char *nm = name;
    if (!nm) {
        int n = f->ops && f->ops->describe ? f->ops->describe(f, buf, sizeof buf) : -1;
        if (n >= 0) { buf[n < (int)sizeof buf ? n : (int)sizeof buf - 1] = 0; nm = buf; }
        else nm = f->gpath ? f->gpath : "";
    }
    b->name = strdup(nm);
    if (!b->name) { mb_put(b); return -LX_ENOMEM; }
    size_t l = strlen(b->name);
    if (l >= 10 && !strcmp(b->name + l - 10, " (deleted)")) { b->name[l - 10] = 0; b->deleted = 1; }
    if (S_ISREG(hs.st_mode) && hs.st_nlink == 0) b->deleted = 1;
    *out = b;
    return 0;
}

static _Atomic uint64_t shanon_counter;

static struct mm_backing *shared_anon_backing(uint64_t len)
{
    struct mm_backing *b = mb_new(MB_SHANON);
    if (!b) return NULL;
    uint64_t c = atomic_fetch_add(&shanon_counter, 1) + 1;
    b->obj_id = ((uint64_t)getpid() << 24) | c;
    b->obj_size = HP_UP(len);
    b->name = strdup("/dev/zero");
    if (!b->name) { mb_put(b); return NULL; }
    b->deleted = 1;      /* shmem_zero_setup: "/dev/zero (deleted)" */
    b->dev_major = 0;
    b->dev_minor = 1;    /* the internal shmem mount */
    b->ino = 1000 + c;   /* LKD-assigned inode number of the anonymous shared object */
    return b;
}

static uint8_t label_kind(const char *name)
{
    if (!name || !*name) return LBL_NONE;
    if (!strcmp(name, "[stack]")) return LBL_STACK;
    if (!strncmp(name, "[anon:", 6)) return LBL_ANON;
    return LBL_SPECIAL;
}

/* do_mmap. `t` may be NULL (engine loader): no personality, rlimit or capability then. */
static int64_t do_mmap(lkd_task *t, lkd_mm *mm, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
                       struct lkd_fdobj *file, uint64_t off, const char *name, struct mm_inval *iv)
{
    if (!len) return -LX_EINVAL;
    if (t && t->proc && (t->proc->personality & LKD_READ_IMPLIES_EXEC) && (prot & LX_PROT_READ)) prot |= LX_PROT_EXEC;
    if (flags & LX_MAP_FIXED_NOREPLACE) flags |= LX_MAP_FIXED;
    int fixed = (flags & LX_MAP_FIXED) != 0;
    if (!fixed) {
        addr &= ~(GP - 1);
        if (addr && addr < mm_win.min_addr) addr = GP_ALIGN(mm_win.min_addr); /* round_hint_to_min */
    }
    uint64_t alen = GP_ALIGN(len);
    if (!alen) return -LX_ENOMEM;
    if ((off >> 12) + (alen >> 12) < (off >> 12)) return -LX_EOVERFLOW;
    if (mm_map_count_over(mm, 0)) return -LX_ENOMEM;   /* map_count > sysctl_max_map_count */
    uint32_t type = flags & LX_MAP_TYPE;
    int hostshared = type == LX_MAP_SHARED || type == LX_MAP_SHARED_VALIDATE;
    uint64_t span = mm_win.g_hi - mm_win.g_lo;

    /* Host phase of the file (request wine-7, src/fs/memfd.c): host offset = file offset + phase. A memfd
     * that is not mapped yet takes the phase this mapping needs; it is decided below, once the request
     * passed every check. */
    int64_t phase = 0, want_phase = LKD_PHASE_ANY;
    int zero_dev = file && lkd_procfs_dev_is_zero(file);
    if (file && !zero_dev) {
        phase = lkd_fs_map_phase(file, LKD_PHASE_PEEK);
        if (phase < 0) phase = 0;
        if (fixed && hostshared) want_phase = (int64_t)((addr - off) & (mm_hps - 1));
    }

    /* __get_unmapped_area */
    if (alen > span) return -LX_ENOMEM;
    if (fixed) {
        if (addr < mm_win.g_lo || addr > mm_win.g_hi - alen) return addr < mm_win.g_lo ? -LX_EPERM : -LX_ENOMEM;
        if (addr & (GP - 1)) return -LX_EINVAL;
    } else {
        int64_t err;
        addr = mm_get_unmapped(mm, addr, alen, off + (uint64_t)phase, flags, hostshared, file != NULL && !hostshared, &err);
        if (err) return err;
    }
    if (addr < mm_win.min_addr) return -LX_EPERM; /* security_mmap_addr: below vm.mmap_min_addr */

    if (flags & LX_MAP_FIXED_NOREPLACE) {
        mm_vma *v = vt_first_end_after(&mm->vt, addr);
        if (v && v->start < addr + alen) return -LX_EEXIST;
    }
    uint32_t vflags = VF_MAYWRITE;
    if (flags & LX_MAP_LOCKED) {
        if (!can_do_mlock(t)) return -LX_EPERM;
        vflags |= VF_LOCKED;
    }
    if (!mlock_future_ok(t, mm, vflags | mm->def_flags, alen)) return -LX_EAGAIN;
    if (flags & LX_MAP_NORESERVE) vflags |= VF_NORESERVE;

    struct mm_backing *bk = NULL;
    int r;
    if (zero_dev) {
        /* mmap_zero (drivers/char/mem.c): anonymous memory. Shared: an anonymous shared object, shown as
         * "/dev/zero (deleted)" like shmem_zero_setup; private: anonymous pages, the file name kept for maps
         * (request procfs-2). The access mode of the open file is checked as for any file. */
        uint32_t acc = atomic_load(&file->status_flags) & LX_O_ACCMODE;
        if (acc == LX_O_WRONLY) return -LX_EACCES;
        if ((type == LX_MAP_SHARED || type == LX_MAP_SHARED_VALIDATE) && (prot & LX_PROT_WRITE) && acc != LX_O_RDWR)
            return -LX_EACCES;
        if (type == LX_MAP_SHARED_VALIDATE) type = LX_MAP_SHARED;
        if (type == LX_MAP_PRIVATE && !name) name = "/dev/zero";
        file = NULL;
    }
    if (file) {
        uint32_t acc = atomic_load(&file->status_flags) & LX_O_ACCMODE;
        int fread_ok = acc == LX_O_RDONLY || acc == LX_O_RDWR, fwrite_ok = acc == LX_O_WRONLY || acc == LX_O_RDWR;
        switch (type) {
        case LX_MAP_SHARED:
            flags &= LEGACY_MAP_MASK;
            /* fallthrough */
        case LX_MAP_SHARED_VALIDATE:
            if (flags & ~LEGACY_MAP_MASK) return -LX_EOPNOTSUPP;
            if ((prot & LX_PROT_WRITE) && !fwrite_ok) return -LX_EACCES;
            vflags |= VF_SHARED | VF_HOSTSHARED;
            if (!fwrite_ok) vflags &= ~VF_MAYWRITE;
            /* fallthrough */
        case LX_MAP_PRIVATE:
            if (!fread_ok) return -LX_EACCES;
            break;
        default:
            return -LX_EINVAL;
        }
        int hfd = file->ops && file->ops->mmap_fd ? file->ops->mmap_fd(file) : -1;
        if (hfd < 0) return -LX_ENODEV;
        if (flags & LX_MAP_GROWSDOWN) return -LX_EINVAL;
        phase = lkd_fs_map_phase(file, want_phase);
        if (phase < 0) {
            lkd_log_once("mm: MAP_SHARED at 0x%llx offset 0x%llx: the object is already mapped with another position in "
                         "the 16 KiB host page (memfd phase) or cannot be shifted (host file): EINVAL",
                         (unsigned long long)addr, (unsigned long long)off);
            return -LX_EINVAL;
        }
        if ((r = file_backing(t, file, hfd, name, &bk))) return r;
        bk->writable = (uint8_t)fwrite_ok;
        bk->hbias = (uint64_t)phase;
        bk->file_size = bk->file_size > bk->hbias ? bk->file_size - bk->hbias : 0;
    } else {
        switch (type) {
        case LX_MAP_SHARED:
            if (flags & LX_MAP_GROWSDOWN) return -LX_EINVAL;
            vflags |= VF_SHARED | VF_HOSTSHARED;
            off = 0;
            break;
        case LX_MAP_DROPPABLE:
            if (flags & (LX_MAP_LOCKED | LX_MAP_HUGETLB)) return -LX_EINVAL;
            if (flags & LX_MAP_GROWSDOWN) return -LX_EINVAL;
            vflags |= VF_WIPEONFORK | VF_DONTDUMP | VF_NORESERVE;
            off = 0;
            break;
        case LX_MAP_PRIVATE:
            if (flags & LX_MAP_GROWSDOWN) {
                /* Linux supports grows-down stacks; LKD does not emulate stack expansion. */
                lkd_log_once("mm: MAP_GROWSDOWN is not supported: EINVAL");
                return -LX_EINVAL;
            }
            off = 0;
            break;
        default:
            return -LX_EINVAL;
        }
        if (hostshared && !(bk = shared_anon_backing(alen))) return -LX_ENOMEM;
    }
    struct mm_mapreq rq = {
        .addr = addr, .len = alen, .prot = prot, .lxflags = flags, .vflags = vflags, .bk = bk, .pgoff = off,
        .label = bk ? NULL : name, .lkind = bk ? LBL_NONE : label_kind(name), .fixed = fixed,
        .noreplace = (flags & LX_MAP_FIXED_NOREPLACE) != 0,
    };
    int64_t res = mm_map_locked(mm, &rq, iv);
    mb_put(bk);
    return res;
}

int64_t lkd_mm_map(lkd_mm *mm, uint64_t addr, uint64_t len, int prot, uint32_t flags,
                   struct lkd_fdobj *file, uint64_t offset, const char *name)
{
    if (!mm) return -LX_EINVAL;
    if (offset & (GP - 1)) return -LX_EINVAL;
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    int64_t r = do_mmap(lkd_current(), mm, addr, len, (uint32_t)prot, flags, file, offset, name, &iv);
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return r;
}

LKD_SYSCALL(mmap)
{
    uint64_t addr = LKD_ARGU(0), len = LKD_ARGU(1), off = LKD_ARGU(5);
    uint32_t prot = (uint32_t)LKD_ARGU(2), flags = (uint32_t)LKD_ARGU(3);
    int fd = LKD_ARGI(4);
    if (off & (GP - 1)) return -LX_EINVAL; /* arch/x86/kernel/sys_x86_64.c */
    lkd_mm *mm = mm_of(t);
    if (!mm) return -LX_ENOMEM;
    struct lkd_fdobj *f = NULL;
    if (!(flags & LX_MAP_ANONYMOUS)) {
        f = lkd_fd_get(t, fd, NULL);
        if (!f) return -LX_EBADF;
        if (flags & LX_MAP_HUGETLB) { lkd_fdobj_put(f); return -LX_EINVAL; }
    } else if (flags & LX_MAP_HUGETLB) {
        /* hugetlb_file_setup: no huge page pool exists (vm.nr_hugepages = 0), sizes other than 2 MiB
         * and 1 GiB are invalid */
        unsigned lg = (flags >> 26) & 0x3f;
        return lg && lg != 21 && lg != 30 ? -LX_EINVAL : -LX_ENOMEM;
    }
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    int64_t r = do_mmap(t, mm, addr, len, prot, flags, f, off, NULL, &iv);
    mm_unlock(mm);
    mm_inval_flush(&iv);
    if (f) lkd_fdobj_put(f);
    return r;
}

/* ---- munmap ------------------------------------------------------------------------------------ */

int lkd_mm_unmap(lkd_mm *mm, uint64_t addr, uint64_t len)
{
    if (!mm) return -LX_EINVAL;
    uint64_t span = mm_win.g_hi;
    if ((addr & (GP - 1)) || addr > span || len > span - addr || addr < mm_win.g_lo) return -LX_EINVAL;
    uint64_t end = addr + GP_ALIGN(len);
    if (end == addr) return -LX_EINVAL;
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    int r = mm_unmap_locked(mm, addr, end, &iv);
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return r;
}

LKD_SYSCALL(munmap) { return lkd_mm_unmap(mm_of(t), LKD_ARGU(0), LKD_ARGU(1)); }

/* ---- mprotect ---------------------------------------------------------------------------------- */

static int do_mprotect(lkd_task *t, lkd_mm *mm, uint64_t start, uint64_t len, uint64_t prot, int pkey)
{
    uint64_t grows = prot & (LX_PROT_GROWSDOWN | LX_PROT_GROWSUP);
    prot &= ~(uint64_t)(LX_PROT_GROWSDOWN | LX_PROT_GROWSUP);
    if (grows == (LX_PROT_GROWSDOWN | LX_PROT_GROWSUP)) return -LX_EINVAL;
    if (start & (GP - 1)) return -LX_EINVAL;
    if (!len) return 0;
    len = GP_ALIGN(len);
    uint64_t end = start + len;
    if (end <= start) return -LX_ENOMEM;
    if (prot & ~(uint64_t)(LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC | LX_PROT_SEM)) return -LX_EINVAL;
    prot &= LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC;
    if (pkey != -1) return -LX_EINVAL; /* no protection key is ever allocated (pkey_alloc: ENOSPC) */
    if (t && t->proc && (t->proc->personality & LKD_READ_IMPLIES_EXEC) && (prot & LX_PROT_READ)) prot |= LX_PROT_EXEC;
    if (!mm) return -LX_ENOMEM;
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    mm_vma *v = vt_first_end_after(&mm->vt, start);
    int r;
    if (!v || v->start >= end) r = -LX_ENOMEM;
    else if (grows) r = -LX_EINVAL;           /* no VM_GROWSDOWN / VM_GROWSUP VMA exists */
    else if (v->start > start) r = -LX_ENOMEM;
    else r = mm_protect_locked(mm, start, end, (uint32_t)prot, &iv);
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return r;
}

int lkd_mm_protect(lkd_mm *mm, uint64_t addr, uint64_t len, int prot)
{
    return do_mprotect(lkd_current(), mm, addr, len, (uint64_t)(uint32_t)prot, -1);
}

LKD_SYSCALL(mprotect) { return do_mprotect(t, mm_of(t), LKD_ARGU(0), LKD_ARGU(1), LKD_ARGU(2), -1); }
LKD_SYSCALL(pkey_mprotect) { return do_mprotect(t, mm_of(t), LKD_ARGU(0), LKD_ARGU(1), LKD_ARGU(2), LKD_ARGI(3)); }

/* The virtual CPU has no PKU (FEX does not expose OSPKE): arch_max_pkey() == 1, nothing to allocate. */
LKD_SYSCALL(pkey_alloc)
{
    if (LKD_ARGU(0)) return -LX_EINVAL;
    if (LKD_ARGU(1) & ~3ull) return -LX_EINVAL; /* PKEY_ACCESS_MASK */
    return -LX_ENOSPC;
}
LKD_SYSCALL(pkey_free) { return -LX_EINVAL; }

/* No shadow stack (CET) on the virtual CPU (arch/x86/kernel/shstk.c). */
LKD_SYSCALL(map_shadow_stack) { return -LX_EOPNOTSUPP; }

/* ---- brk --------------------------------------------------------------------------------------- */

int64_t lkd_mm_brk(lkd_mm *mm, uint64_t brk)
{
    if (!mm) return 0;
    lkd_task *t = lkd_current();
    mm_lock(mm);
    if (!mm->start_brk && t && t->proc && t->proc->layout && t->proc->layout->start_brk)
        mm->start_brk = mm->brk = t->proc->layout->start_brk;
    uint64_t orig = mm->brk;
    int64_t ret = (int64_t)orig;
    if (!mm->start_brk || brk < mm->start_brk) goto out;
    uint64_t rl = rlim_cur(t, LX_RLIMIT_DATA);
    if (rl != LX_RLIM_INFINITY) {
        const struct lkd_proc_layout *l = t && t->proc ? t->proc->layout : NULL;
        uint64_t data = l && l->end_data > l->start_data ? l->end_data - l->start_data : 0;
        if ((brk - mm->start_brk) + data > rl) goto out; /* check_data_rlimit */
    }
    uint64_t newbrk = GP_ALIGN(brk), oldbrk = GP_ALIGN(mm->brk);
    if (oldbrk == newbrk) { mm->brk = brk; ret = (int64_t)brk; goto out; }
    struct mm_inval iv = { 0 };
    if (brk <= mm->brk) {
        mm_vma *v = vt_first_end_after(&mm->vt, newbrk);
        if (!v || v->start >= oldbrk) goto out;
        mm->brk = brk;
        mm_unmap_locked(mm, newbrk, oldbrk, &iv);
        ret = (int64_t)brk;
        goto out_inval;
    }
    if (newbrk > mm_win.g_hi || !mlock_future_ok(t, mm, mm->def_flags, newbrk - oldbrk)) goto out;
    mm_vma *next = vt_first_end_after(&mm->vt, oldbrk);
    if (next && newbrk + GP > next->start) goto out;
    uint32_t prot = LX_PROT_READ | LX_PROT_WRITE;
    if (t && t->proc && (t->proc->personality & LKD_READ_IMPLIES_EXEC)) prot |= LX_PROT_EXEC;
    struct mm_mapreq rq = { .addr = oldbrk, .len = newbrk - oldbrk, .prot = prot, .vflags = VF_MAYWRITE, .fixed = 1 };
    if (mm_map_locked(mm, &rq, &iv) < 0) goto out;
    mm->brk = brk;
    ret = (int64_t)brk;
out_inval:
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return ret;
out:
    mm_unlock(mm);
    return ret;
}

LKD_SYSCALL(brk) { return lkd_mm_brk(mm_of(t), LKD_ARGU(0)); }

/* ---- madvise ----------------------------------------------------------------------------------- */

static int behavior_valid(int b)
{
    switch (b) {
    case LX_MADV_DOFORK: case LX_MADV_DONTFORK: case LX_MADV_NORMAL: case LX_MADV_SEQUENTIAL: case LX_MADV_RANDOM:
    case LX_MADV_REMOVE: case LX_MADV_WILLNEED: case LX_MADV_DONTNEED: case LX_MADV_DONTNEED_LOCKED: case LX_MADV_FREE:
    case LX_MADV_COLD: case LX_MADV_PAGEOUT: case LX_MADV_POPULATE_READ: case LX_MADV_POPULATE_WRITE:
    case LX_MADV_MERGEABLE: case LX_MADV_UNMERGEABLE: case LX_MADV_HUGEPAGE: case LX_MADV_NOHUGEPAGE:
    case LX_MADV_COLLAPSE: case LX_MADV_DONTDUMP: case LX_MADV_DODUMP: case LX_MADV_WIPEONFORK:
    case LX_MADV_KEEPONFORK: case LX_MADV_SOFT_OFFLINE: case LX_MADV_HWPOISON:
        return 1;
    }
    return 0;
}

/* Discard the pages of [s, e) of v (private): anonymous memory reads zeros again, private file pages
 * read the file again (zap_page_range_single). Whole host pages with no other VMA get fresh memory. */
static int discard(lkd_mm *mm, mm_vma *v, uint64_t s, uint64_t e)
{
    mm_watch_lock();
    int r = mm_discard_range(mm, v, s, e);
    int r2 = mm_host_refresh(mm, s, e);
    mm_watch_unlock();
    return r ? r : r2;
}

static void set_flags(lkd_mm *mm, uint64_t s, uint64_t e, uint32_t set, uint32_t clear)
{
    mm_split(mm, s);
    mm_split(mm, e);
    for (mm_vma *v = vt_find(&mm->vt, s); v && v->start < e; v = vt_next(&mm->vt, v))
        v->vflags = (v->vflags | set) & ~clear;
    mm_merge_around(mm, s, e);
}

static int populate(mm_vma *v, uint64_t s, uint64_t e, int write)
{
    if (write ? !(v->prot & LX_PROT_WRITE) : !v->prot) return -LX_EFAULT;
    mm_watch_lock(); /* a watched page is resident and must not be written: skipped */
    for (uint64_t a = s; a < e; a += GP) {
        if (write && !mm_watched(HP_DOWN(a))) __atomic_fetch_add((uint8_t *)mm_h(a), 0, __ATOMIC_RELAXED);
        else (void)*(volatile uint8_t *)mm_h(a);
    }
    mm_watch_unlock();
    return 0;
}

static int madvise_one(lkd_task *t, lkd_mm *mm, mm_vma *v, uint64_t s, uint64_t e, int b, struct mm_inval *iv)
{
    int locked = (v->vflags & VF_LOCKED) != 0;
    int anon_private = !v->bk && !(v->vflags & VF_SHARED);
    switch (b) {
    case LX_MADV_DONTNEED:
    case LX_MADV_DONTNEED_LOCKED:
        if (locked && b == LX_MADV_DONTNEED) return -LX_EINVAL;
        if (v->vflags & VF_HOSTSHARED) return 0; /* shared pages refault with the same content */
        if (v->prot & LX_PROT_EXEC) mm_inval_add(iv, s, e);
        return discard(mm, v, s, e);
    case LX_MADV_FREE:
        if (locked) return -LX_EINVAL;
        if (!anon_private) return -LX_EINVAL;
        return 0; /* lazy free: content may stay until written or reclaimed; nothing is reclaimed here */
    case LX_MADV_REMOVE:
        if (locked) return -LX_EINVAL;
        if (!v->bk) return -LX_EINVAL;
        if (!(v->vflags & VF_SHARED) || !(v->vflags & VF_MAYWRITE)) return -LX_EACCES;
        if (v->bk->kind == MB_FILE) return -LX_EOPNOTSUPP; /* hole punching of host files is not done */
        mm_watch_lock();
        if (mm_temp_access(mm, s, e, 1)) { mm_watch_unlock(); return -LX_EIO; }
        memset(mm_h(s), 0, e - s); /* shmem_fallocate(PUNCH_HOLE): the range reads zeros everywhere */
        mm_host_refresh(mm, s, e);
        mm_watch_unlock();
        if (v->prot & LX_PROT_EXEC) mm_inval_add(iv, s, e);
        return 0;
    case LX_MADV_WILLNEED:
    case LX_MADV_NORMAL:
        if (b == LX_MADV_NORMAL) set_flags(mm, s, e, 0, VF_RANDOM | VF_SEQ);
        return 0;
    case LX_MADV_RANDOM: set_flags(mm, s, e, VF_RANDOM, VF_SEQ); return 0;
    case LX_MADV_SEQUENTIAL: set_flags(mm, s, e, VF_SEQ, VF_RANDOM); return 0;
    case LX_MADV_COLD:
    case LX_MADV_PAGEOUT:
        return locked ? -LX_EINVAL : 0;
    case LX_MADV_DONTFORK: set_flags(mm, s, e, VF_DONTFORK, 0); return 0;
    case LX_MADV_DOFORK: set_flags(mm, s, e, 0, VF_DONTFORK); return 0;
    case LX_MADV_WIPEONFORK:
        if (v->bk || (v->vflags & VF_SHARED)) return -LX_EINVAL;
        set_flags(mm, s, e, VF_WIPEONFORK, 0);
        return 0;
    case LX_MADV_KEEPONFORK: set_flags(mm, s, e, 0, VF_WIPEONFORK); return 0;
    case LX_MADV_DONTDUMP: set_flags(mm, s, e, VF_DONTDUMP, 0); return 0;
    case LX_MADV_DODUMP:
        if (v->lkind == LBL_SPECIAL) return -LX_EINVAL; /* VM_SPECIAL */
        set_flags(mm, s, e, 0, VF_DONTDUMP);
        return 0;
    case LX_MADV_MERGEABLE:
        if (v->vflags & VF_SHARED) return 0; /* ksm_madvise ignores shared mappings */
        set_flags(mm, s, e, VF_MERGEABLE, 0);
        return 0;
    case LX_MADV_UNMERGEABLE: set_flags(mm, s, e, 0, VF_MERGEABLE); return 0;
    case LX_MADV_HUGEPAGE: set_flags(mm, s, e, VF_HUGEPAGE, VF_NOHUGEPAGE); return 0;
    case LX_MADV_NOHUGEPAGE: set_flags(mm, s, e, VF_NOHUGEPAGE, VF_HUGEPAGE); return 0;
    case LX_MADV_COLLAPSE:
        return -LX_EINVAL; /* no huge pages exist to collapse into */
    }
    (void)t;
    return -LX_EINVAL;
}

static int do_madvise(lkd_task *t, lkd_mm *mm, uint64_t start, uint64_t len_in, int b)
{
    if (!behavior_valid(b)) return -LX_EINVAL;
    if (start & (GP - 1)) return -LX_EINVAL;
    uint64_t len = GP_ALIGN(len_in);
    if (len_in && !len) return -LX_EINVAL;
    uint64_t end = start + len;
    if (end < start) return -LX_EINVAL;
    if (end == start) return 0;
    if (b == LX_MADV_HWPOISON || b == LX_MADV_SOFT_OFFLINE)
        return mm_capable(t, LKD_CAP_SYS_ADMIN) ? -LX_EINVAL : -LX_EPERM; /* madvise_inject_error */
    if (!mm) return -LX_ENOMEM;
    struct mm_inval iv = { 0 };
    int err = 0, unmapped = 0;
    mm_lock(mm);
    if (b == LX_MADV_POPULATE_READ || b == LX_MADV_POPULATE_WRITE) {
        for (uint64_t cur = start; cur < end && !err;) {
            mm_vma *v = vt_find(&mm->vt, cur);
            if (!v) { err = -LX_ENOMEM; break; }
            uint64_t pe = v->end < end ? v->end : end;
            err = populate(v, cur, pe, b == LX_MADV_POPULATE_WRITE);
            cur = pe;
        }
    } else {
        uint64_t cur = start;
        while (cur < end) {
            mm_vma *v = vt_first_end_after(&mm->vt, cur);
            if (!v || v->start >= end) { unmapped = 1; break; }
            if (v->start > cur) { unmapped = 1; cur = v->start; }
            uint64_t pe = v->end < end ? v->end : end;
            int r = madvise_one(t, mm, v, cur, pe, b, &iv);
            if (r) { err = r; break; }
            cur = pe;
        }
    }
    mm_unlock(mm);
    mm_inval_flush(&iv);
    if (err) return err;
    return unmapped ? -LX_ENOMEM : 0;
}

LKD_SYSCALL(madvise) { return do_madvise(t, mm_of(t), LKD_ARGU(0), LKD_ARGU(1), LKD_ARGI(2)); }

/* ---- mincore ----------------------------------------------------------------------------------- */

LKD_SYSCALL(mincore)
{
    uint64_t start = LKD_ARGU(0), len = LKD_ARGU(1), vec = LKD_ARGP(2);
    lkd_mm *mm = mm_of(t);
    if (start & (GP - 1)) return -LX_EINVAL;
    uint64_t pages = (len >> 12) + ((len & (GP - 1)) != 0);
    if (start + pages * GP < start || start + pages * GP > mm_win.g_hi) return -LX_ENOMEM; /* access_ok */
    if (!mm) return -LX_ENOMEM;
    uint8_t out[256];
    while (pages) {
        uint64_t n = pages < sizeof out ? pages : sizeof out;
        mm_lock(mm);
        mm_vma *v = vt_find(&mm->vt, start);
        if (!v) { mm_unlock(mm); return -LX_ENOMEM; }
        uint64_t vmax = (v->end - start) >> 12;
        if (n > vmax) n = vmax;
        char hv[256 / 4 + 2];
        uint64_t hs = HP_DOWN(start), he = HP_UP(start + n * GP);
        int r = mincore(mm_h(hs), he - hs, hv);
        mm_unlock(mm);
        if (r < 0) return LKD_HOST_ERR();
        for (uint64_t i = 0; i < n; i++) {
            uint64_t a = start + i * GP;
            out[i] = (hv[(HP_DOWN(a) - hs) / mm_hps] & MINCORE_INCORE) ? 1 : 0;
        }
        if (lkd_user_write(vec, out, n)) return -LX_EFAULT;
        vec += n;
        start += n * GP;
        pages -= n;
    }
    return 0;
}

/* ---- msync ------------------------------------------------------------------------------------- */

LKD_SYSCALL(msync)
{
    uint64_t start = LKD_ARGU(0), len = LKD_ARGU(1);
    int flags = LKD_ARGI(2);
    lkd_mm *mm = mm_of(t);
    if (flags & ~(LX_MS_ASYNC | LX_MS_INVALIDATE | LX_MS_SYNC)) return -LX_EINVAL;
    if (start & (GP - 1)) return -LX_EINVAL;
    if ((flags & LX_MS_ASYNC) && (flags & LX_MS_SYNC)) return -LX_EINVAL;
    len = (len + GP - 1) & ~(GP - 1);
    uint64_t end = start + len;
    if (end < start) return -LX_ENOMEM;
    if (end == start) return 0;
    if (!mm) return -LX_ENOMEM;
    int err = 0, unmapped = 0;
    mm_lock(mm);
    mm_vma *v = vt_first_end_after(&mm->vt, start);
    for (;;) {
        if (!v) { err = -LX_ENOMEM; break; }
        if (start < v->start) {
            if (flags == LX_MS_ASYNC) { err = -LX_ENOMEM; break; }
            start = v->start;
            if (start >= end) { err = -LX_ENOMEM; break; }
            unmapped = 1;
        }
        uint64_t fend = v->end < end ? v->end : end;
        if ((flags & LX_MS_INVALIDATE) && (v->vflags & VF_LOCKED)) { err = -LX_EBUSY; break; }
        if ((flags & LX_MS_SYNC) && v->bk && v->bk->kind == MB_FILE && (v->vflags & VF_SHARED)) {
            if (msync(mm_h(HP_DOWN(start)), HP_UP(fend) - HP_DOWN(start), MS_SYNC) < 0) { err = (int)LKD_HOST_ERR(); break; }
            if (fsync(v->bk->host_fd) < 0) { err = (int)LKD_HOST_ERR(); break; }
        }
        start = fend;
        if (start >= end) break;
        v = vt_first_end_after(&mm->vt, start);
    }
    mm_unlock(mm);
    if (err) return err;
    return unmapped ? -LX_ENOMEM : 0;
}

/* ---- mlock family ------------------------------------------------------------------------------ */

/* apply_vma_lock_flags over [s, e): every page must be mapped. */
static int lock_range(lkd_mm *mm, uint64_t s, uint64_t e, uint32_t lockflags)
{
    if (!mm_range_mapped(mm, s, e)) return -LX_ENOMEM;
    mm_split(mm, s);
    mm_split(mm, e);
    for (mm_vma *v = vt_find(&mm->vt, s); v && v->start < e; v = vt_next(&mm->vt, v)) {
        int was = (v->vflags & VF_LOCKED) != 0;
        uint64_t sz = v->end - v->start;
        if (lockflags & VF_LOCKED) {
            if (!was && !(lockflags & VF_LOCKONFAULT) && v->prot) {
                /* populate and wire the host pages (__mm_populate); an honest failure if the host refuses */
                if (mlock(mm_h(HP_DOWN(v->start)), HP_UP(v->end) - HP_DOWN(v->start)) < 0) {
                    int64_t err = LKD_HOST_ERR();
                    mm_merge_around(mm, s, e);
                    return err == -LX_ENOMEM ? -LX_ENOMEM : -LX_EAGAIN;
                }
            }
            if (!was) mm->locked_bytes += sz;
            v->vflags = (v->vflags & ~(VF_LOCKED | VF_LOCKONFAULT)) | lockflags;
        } else {
            if (was) {
                mm->locked_bytes -= sz;
                munlock(mm_h(HP_DOWN(v->start)), HP_UP(v->end) - HP_DOWN(v->start));
            }
            v->vflags &= ~(VF_LOCKED | VF_LOCKONFAULT);
        }
    }
    mm_merge_around(mm, s, e);
    return 0;
}

static uint64_t locked_in(lkd_mm *mm, uint64_t s, uint64_t e)
{
    uint64_t n = 0;
    for (mm_vma *v = vt_first_end_after(&mm->vt, s); v && v->start < e; v = vt_next(&mm->vt, v))
        if (v->vflags & VF_LOCKED) {
            uint64_t a = v->start > s ? v->start : s, z = v->end < e ? v->end : e;
            n += z - a;
        }
    return n;
}

static int do_mlock(lkd_task *t, uint64_t start, uint64_t len, uint32_t lockflags)
{
    lkd_mm *mm = mm_of(t);
    if (!can_do_mlock(t)) return -LX_EPERM;
    len = GP_ALIGN(len + (start & (GP - 1)));
    start &= ~(GP - 1);
    if (!mm) return -LX_ENOMEM;
    uint64_t end = start + len;
    if (end < start) return -LX_EINVAL;
    if (end == start) return 0;
    uint64_t lim = rlim_cur(t, LX_RLIMIT_MEMLOCK);
    mm_lock(mm);
    int r;
    uint64_t locked = len + mm->locked_bytes;
    if (lim != LX_RLIM_INFINITY && locked > lim && !mm_capable(t, LKD_CAP_IPC_LOCK)) locked -= locked_in(mm, start, end);
    if (lim == LX_RLIM_INFINITY || locked <= lim || mm_capable(t, LKD_CAP_IPC_LOCK)) r = lock_range(mm, start, end, lockflags);
    else r = -LX_ENOMEM;
    mm_unlock(mm);
    return r;
}

LKD_SYSCALL(mlock) { return do_mlock(t, LKD_ARGU(0), LKD_ARGU(1), VF_LOCKED); }

LKD_SYSCALL(mlock2)
{
    unsigned flags = LKD_ARGUI(2);
    if (flags & ~(unsigned)LX_MLOCK_ONFAULT) return -LX_EINVAL;
    return do_mlock(t, LKD_ARGU(0), LKD_ARGU(1), VF_LOCKED | (flags ? VF_LOCKONFAULT : 0));
}

LKD_SYSCALL(munlock)
{
    uint64_t start = LKD_ARGU(0), len = LKD_ARGU(1);
    lkd_mm *mm = mm_of(t);
    len = GP_ALIGN(len + (start & (GP - 1)));
    start &= ~(GP - 1);
    uint64_t end = start + len;
    if (end < start) return -LX_EINVAL;
    if (end == start) return 0;
    if (!mm) return -LX_ENOMEM;
    mm_lock(mm);
    int r = lock_range(mm, start, end, 0);
    mm_unlock(mm);
    return r;
}

LKD_SYSCALL(mlockall)
{
    unsigned flags = LKD_ARGUI(0);
    lkd_mm *mm = mm_of(t);
    if (!flags || (flags & ~(unsigned)(LX_MCL_CURRENT | LX_MCL_FUTURE | LX_MCL_ONFAULT)) || flags == LX_MCL_ONFAULT)
        return -LX_EINVAL;
    if (!can_do_mlock(t)) return -LX_EPERM;
    if (!mm) return -LX_ENOMEM;
    uint64_t lim = rlim_cur(t, LX_RLIMIT_MEMLOCK);
    int r = 0;
    mm_lock(mm);
    uint64_t total = 0;
    for (mm_vma *v = vt_first(&mm->vt); v; v = vt_next(&mm->vt, v)) total += v->end - v->start;
    if ((flags & LX_MCL_CURRENT) && lim != LX_RLIM_INFINITY && total > lim && !mm_capable(t, LKD_CAP_IPC_LOCK)) {
        mm_unlock(mm);
        return -LX_ENOMEM;
    }
    uint32_t lf = VF_LOCKED | ((flags & LX_MCL_ONFAULT) ? VF_LOCKONFAULT : 0);
    mm->def_flags = (flags & LX_MCL_FUTURE) ? lf : 0;
    if (flags & LX_MCL_CURRENT) {
        /* apply_mlockall_flags: lock every VMA. Linux ignores population errors; a host refusal to wire
         * is reported (EAGAIN) because the lock would otherwise be fake. */
        uint64_t cur = mm_win.g_lo;
        mm_vma *v;
        while (!r && (v = vt_first_end_after(&mm->vt, cur))) {
            cur = v->end;
            r = lock_range(mm, v->start, cur, lf);
        }
    }
    mm_unlock(mm);
    return r;
}

LKD_SYSCALL(munlockall)
{
    lkd_mm *mm = mm_of(t);
    if (!mm) return 0;
    mm_lock(mm);
    mm->def_flags = 0;
    for (mm_vma *v = vt_first(&mm->vt); v;) {
        uint64_t e = v->end;
        lock_range(mm, v->start, e, 0);
        v = vt_first_end_after(&mm->vt, e);
    }
    mm_unlock(mm);
    return 0;
}

int64_t mm_sys_mremap(lkd_task *t, const lkd_sysreq *rq);
LKD_SYSCALL(mremap) { return mm_sys_mremap(t, rq); }

void mm_sys_register(void)
{
    LKD_REGISTER(mmap);
    LKD_REGISTER(munmap);
    LKD_REGISTER(mprotect);
    LKD_REGISTER(pkey_mprotect);
    LKD_REGISTER(pkey_alloc);
    LKD_REGISTER(pkey_free);
    LKD_REGISTER(map_shadow_stack);
    LKD_REGISTER(brk);
    LKD_REGISTER(mremap);
    LKD_REGISTER(madvise);
    LKD_REGISTER(mincore);
    LKD_REGISTER(msync);
    LKD_REGISTER(mlock);
    LKD_REGISTER(mlock2);
    LKD_REGISTER(munlock);
    LKD_REGISTER(mlockall);
    LKD_REGISTER(munlockall);
}

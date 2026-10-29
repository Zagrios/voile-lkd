/* remap.c: mremap (mm/mremap.c v6.12: SYSCALL_DEFINE5(mremap), vma_to_resize, mremap_to, move_vma).
 * Owner: mm.
 *
 * Moving private memory: whole host pages are moved with mach_vm_remap(copy=TRUE) when the old and new
 * addresses are congruent modulo 16 KiB (the free-range search prefers such an address), the rest is
 * copied. Shared mappings are re-mapped with mach_vm_remap(copy=FALSE, VM_INHERIT_SHARE), so the new
 * range maps the same object and stays shared with forked children. A shared anonymous or SysV mapping
 * cannot grow beyond its object (the host object has a fixed size): -ENOMEM, documented. */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

#include "mm_priv.h"

static uint64_t rlim_memlock(lkd_task *t)
{
    return t && t->proc ? t->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur : LX_RLIM_INFINITY;
}

static mm_vma *to_resize(lkd_task *t, lkd_mm *mm, uint64_t addr, uint64_t old_len, uint64_t new_len,
                         uint64_t flags, int *err)
{
    mm_vma *v = vt_find(&mm->vt, addr);
    *err = 0;
    if (!v) { *err = -LX_EFAULT; return NULL; }
    if (!old_len && !(v->vflags & VF_SHARED)) { *err = -LX_EINVAL; return NULL; }
    if ((flags & LX_MREMAP_DONTUNMAP) && v->lkind == LBL_SPECIAL) { *err = -LX_EINVAL; return NULL; }
    if (old_len > v->end - addr) { *err = -LX_EFAULT; return NULL; }
    if (new_len == old_len) return v;
    if (v->lkind == LBL_SPECIAL) { *err = -LX_EFAULT; return NULL; } /* VM_DONTEXPAND */
    if ((v->vflags & VF_LOCKED) && !mm_capable(t, LKD_CAP_IPC_LOCK)) {
        uint64_t lim = rlim_memlock(t);
        if (lim != LX_RLIM_INFINITY && mm->locked_bytes + (new_len - old_len) > lim) { *err = -LX_EAGAIN; return NULL; }
    }
    return v;
}

/* Can the shared object behind v back the guest range [v->start, v->start + len)? */
static int shared_fits(const mm_vma *v, uint64_t from, uint64_t len)
{
    if (!(v->vflags & VF_HOSTSHARED) || v->bk->kind == MB_FILE) return 1;
    uint64_t off = v->pgoff + (from - v->start);
    return off + len <= v->bk->obj_size;
}

static int expandable(lkd_mm *mm, const mm_vma *v, uint64_t delta)
{
    uint64_t end = v->end + delta;
    if (end < v->end || end > mm_win.g_hi) return 0;
    mm_vma *n = vt_first_end_after(&mm->vt, v->end);
    if (n && n->start < end) return 0;
    if (v->vflags & VF_HOSTSHARED) {
        if (n && HP_DOWN(n->start) < HP_UP(end) && (n->vflags & VF_HOSTSHARED)) return 0;
        if (n && HP_DOWN(n->start) < HP_UP(end)) return 0;
    } else if (n && (n->vflags & VF_HOSTSHARED) && HP_DOWN(n->start) < end) {
        return 0;
    }
    return 1;
}

/* Extend v in place to v->end + delta (vma_merge_extend). */
static int expand(lkd_mm *mm, mm_vma *v, uint64_t delta, struct mm_inval *iv)
{
    uint64_t s = v->end, e = v->end + delta;
    if (v->vflags & VF_HOSTSHARED) {
        if (!shared_fits(v, v->start, e - v->start)) return -LX_ENOMEM;
        if (HP_UP(e) > HP_UP(s)) {
            /* file: map the additional host pages of the file */
            uint64_t hs = HP_UP(s), off = v->pgoff + v->bk->hbias + (hs - v->start);
            void *p = mmap(mm_h(hs), HP_UP(e) - hs, mm_host_prot_of(v->prot), MAP_SHARED | MAP_FIXED, v->bk->host_fd, (off_t)off);
            if (p == MAP_FAILED) { int64_t r = LKD_HOST_ERR(); mm_host_reset(hs, HP_UP(e) - hs); return (int)r; }
        }
        mm_wlock_range(mm, v->start, e);
        vt_set_end(&mm->vt, v, e);
        mm_wunlock_range(mm, v->start, e);
        if (v->vflags & VF_LOCKED) mm->locked_bytes += delta;
        mm_host_refresh(mm, s, e);
        return 0;
    }
    struct mm_mapreq rq = {
        .addr = s, .len = delta, .prot = v->prot, .vflags = v->vflags, .bk = v->bk,
        .pgoff = v->bk ? v->pgoff + (s - v->start) : 0, .label = v->label, .lkind = v->lkind, .fixed = 1,
    };
    int64_t r = mm_map_locked(mm, &rq, iv);
    return r < 0 ? (int)r : 0;
}

/* Copy or move host content of [old, old+len) to [nw, nw+len) for a private VMA. Destination pages that
 * hold no other VMA are replaced; shared boundary pages are written sub-page by sub-page. */
static int move_private(lkd_mm *mm, uint64_t old, uint64_t nw, uint64_t len)
{
    int congruent = ((old - nw) & (mm_hps - 1)) == 0;
    uint64_t d = nw, de = nw + len;
    while (d < de) {
        uint64_t p = HP_DOWN(d), pe = p + mm_hps;
        uint64_t a = d, z = pe < de ? pe : de;
        uint64_t src = old + (a - nw);
        if (congruent && a == p && z == pe && !(vt_first_end_after(&mm->vt, p) && vt_first_end_after(&mm->vt, p)->start < pe)) {
            /* run of whole host pages */
            uint64_t q = pe;
            while (q + mm_hps <= de) {
                mm_vma *o = vt_first_end_after(&mm->vt, q);
                if (o && o->start < q + mm_hps) break;
                q += mm_hps;
            }
            mach_vm_address_t target = (mach_vm_address_t)(uintptr_t)mm_h(p);
            vm_prot_t cur, max;
            kern_return_t kr = mach_vm_remap(mach_task_self(), &target, q - p, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                             mach_task_self(), (mach_vm_address_t)(uintptr_t)mm_h(src), TRUE, &cur, &max,
                                             VM_INHERIT_COPY);
            if (kr != KERN_SUCCESS) return -LX_ENOMEM;
            d = q;
            continue;
        }
        if (mm_temp_access(mm, src, src + (z - a), 0) || mm_temp_access(mm, a, z, 1)) return -LX_ENOMEM;
        memcpy(mm_h(a), mm_h(src), z - a);
        d = z;
    }
    return 0;
}

static int move_shared(uint64_t old, uint64_t nw, uint64_t len)
{
    /* host pages touched by [old, old+len); old and nw have the same position inside their host page */
    uint64_t so = HP_DOWN(old), n = HP_UP(old + len) - so;
    mach_vm_address_t target = (mach_vm_address_t)(uintptr_t)mm_h(HP_DOWN(nw));
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &target, n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                                     mach_task_self(), (mach_vm_address_t)(uintptr_t)mm_h(so), FALSE, &cur, &max,
                                     VM_INHERIT_SHARE);
    return kr == KERN_SUCCESS ? 0 : -LX_ENOMEM;
}

/* move_vma. [addr, addr+old_len) lies inside v; the destination range is free. */
static int64_t move_vma(lkd_task *t, lkd_mm *mm, mm_vma *v, uint64_t addr, uint64_t old_len, uint64_t new_len,
                        uint64_t nw, uint64_t flags, struct mm_inval *iv)
{
    (void)t;
    int hs = (v->vflags & VF_HOSTSHARED) != 0;
    if (hs) {
        if ((addr - nw) & (mm_hps - 1)) {
            lkd_log_once("mm: mremap of a MAP_SHARED mapping to an address with another position in the 16 KiB host page: EINVAL (host page limit)");
            return -LX_EINVAL;
        }
        if (mm_host_conflict(mm, nw, nw + new_len, 1, v->prot, v->bk, v->bk->kind == MB_SHANON ? 0 : v->pgoff + (addr - v->start))) {
            lkd_log_once("mm: mremap destination shares a 16 KiB host page with an accessible mapping: EINVAL");
            return -LX_EINVAL;
        }
        if (new_len > old_len && !shared_fits(v, addr, new_len)) return -LX_ENOMEM;
    }
    mm_vma *nv = mm_vma_clone(v);
    if (!nv) return -LX_ENOMEM;
    nv->start = nw;
    nv->end = nw + new_len;
    if (nv->bk) nv->pgoff = v->pgoff + (addr - v->start);
    int r;
    if (hs) {
        r = move_shared(addr, nw, old_len > new_len ? new_len : old_len);
        if (!r && v->bk->kind == MB_FILE && HP_UP(nw + new_len) > HP_UP(nw + old_len)) {
            uint64_t hsx = HP_UP(nw + old_len), off = nv->pgoff + v->bk->hbias + (hsx - nw);
            void *p = mmap(mm_h(hsx), HP_UP(nw + new_len) - hsx, mm_host_prot_of(v->prot), MAP_SHARED | MAP_FIXED,
                           v->bk->host_fd, (off_t)off);
            if (p == MAP_FAILED) r = (int)LKD_HOST_ERR();
        }
    } else {
        mm_watch_lock();
        r = move_private(mm, addr, nw, old_len);
        if (!r && new_len > old_len) {
            uint64_t es = nw + old_len, ee = nw + new_len;
            r = mm_temp_access(mm, es, ee, 1);
            if (!r) r = mm_fill(mm, nv, es, ee);
        }
        mm_host_refresh(mm, addr, addr + old_len); /* source pages opened for reading */
        mm_watch_unlock();
    }
    if (r) {
        mm_host_cleanup(mm, nw, nw + new_len); /* no VMA there: resets or zeroes the touched host pages */
        mm_host_refresh(mm, addr, addr + old_len);
        mm_vma_free(nv);
        return r;
    }
    mm_wlock_range(mm, nv->start, nv->end);
    vt_insert(&mm->vt, nv);
    mm_wunlock_range(mm, nv->start, nv->end);
    if (nv->bk && nv->bk->kind == MB_SYSV) mm_shm_attach_vma(nv);
    if (nv->vflags & VF_LOCKED) {
        mm->locked_bytes += new_len;
        mm_host_refresh(mm, nw, nw + new_len);
        mlock(mm_h(HP_DOWN(nw)), HP_UP(nw + new_len) - HP_DOWN(nw));
    }
    if (v->prot & LX_PROT_EXEC) {
        mm_inval_add(iv, addr, addr + old_len);
        mm_inval_add(iv, nw, nw + new_len);
    }
    if (flags & LX_MREMAP_DONTUNMAP) {
        /* The old VMA stays with its pages gone: anonymous memory reads zeros, a private file mapping
         * reads the file, a shared mapping still maps the object. It loses VM_LOCKED. */
        mm_split(mm, addr);
        mm_split(mm, addr + old_len);
        mm_vma *o = vt_find(&mm->vt, addr);
        if (o->vflags & VF_LOCKED) {
            mm->locked_bytes -= old_len;
            munlock(mm_h(HP_DOWN(addr)), HP_UP(addr + old_len) - HP_DOWN(addr));
            o->vflags &= ~(VF_LOCKED | VF_LOCKONFAULT);
        }
        if (!hs) {
            mm_watch_lock();
            mm_discard_range(mm, o, addr, addr + old_len);
            mm_host_refresh(mm, addr, addr + old_len);
            mm_watch_unlock();
        }
        mm_merge_around(mm, addr, addr + old_len);
        mm_host_refresh(mm, addr, addr + old_len);
    } else {
        mm_unmap_locked(mm, addr, addr + old_len, iv);
    }
    mm_merge_around(mm, nw, nw + new_len);
    mm_host_refresh(mm, nw, nw + new_len);
    return (int64_t)nw;
}

static int64_t remap_to(lkd_task *t, lkd_mm *mm, uint64_t addr, uint64_t old_len, uint64_t nw, uint64_t new_len,
                        uint64_t flags, struct mm_inval *iv)
{
    if (nw & (GP - 1)) return -LX_EINVAL;
    if (new_len > mm_win.g_hi - mm_win.g_lo || nw < mm_win.g_lo || nw > mm_win.g_hi - new_len) return -LX_EINVAL;
    if (addr + old_len > nw && nw + new_len > addr) return -LX_EINVAL;
    if (mm->vt.n + 2 >= 65530 - 3) return -LX_ENOMEM;
    int r;
    if (flags & LX_MREMAP_FIXED) {
        if ((r = mm_unmap_locked(mm, nw, nw + new_len, iv))) return r;
    }
    if (old_len > new_len) {
        if ((r = mm_unmap_locked(mm, addr + new_len, addr + old_len, iv))) return r;
        old_len = new_len;
    }
    mm_vma *v = to_resize(t, mm, addr, old_len, new_len, flags, &r);
    if (!v) return r;
    int hs = (v->vflags & VF_HOSTSHARED) != 0;
    if (!(flags & LX_MREMAP_FIXED)) {
        int64_t err;
        uint64_t pg = hs ? addr : (v->bk ? v->pgoff + v->bk->hbias + (addr - v->start) : addr);
        nw = mm_get_unmapped(mm, nw, new_len, pg, 0, hs, !hs, &err);
        if (err) return err;
    }
    return move_vma(t, mm, v, addr, old_len, new_len, nw, flags, iv);
}

int64_t lkd_mm_remap_task(lkd_task *t, lkd_mm *mm, uint64_t addr, uint64_t old_len, uint64_t new_len,
                          uint64_t flags, uint64_t new_addr)
{
    if (flags & ~(uint64_t)(LX_MREMAP_FIXED | LX_MREMAP_MAYMOVE | LX_MREMAP_DONTUNMAP)) return -LX_EINVAL;
    if ((flags & LX_MREMAP_FIXED) && !(flags & LX_MREMAP_MAYMOVE)) return -LX_EINVAL;
    if ((flags & LX_MREMAP_DONTUNMAP) && (!(flags & LX_MREMAP_MAYMOVE) || old_len != new_len)) return -LX_EINVAL;
    if (addr & (GP - 1)) return -LX_EINVAL;
    old_len = GP_ALIGN(old_len);
    new_len = GP_ALIGN(new_len);
    if (!new_len) return -LX_EINVAL;
    if (!mm) return -LX_EFAULT;
    struct mm_inval iv = { 0 };
    int64_t ret;
    int r;
    mm_lock(mm);
    mm_vma *v = vt_find(&mm->vt, addr);
    if (!v) { ret = -LX_EFAULT; goto out; }
    if (flags & (LX_MREMAP_FIXED | LX_MREMAP_DONTUNMAP)) {
        ret = remap_to(t, mm, addr, old_len, new_addr, new_len, flags, &iv);
        goto out;
    }
    if (old_len >= new_len) {
        if (old_len == new_len) { ret = (int64_t)addr; goto out; }
        r = mm_unmap_locked(mm, addr + new_len, addr + old_len, &iv);
        ret = r ? r : (int64_t)addr;
        goto out;
    }
    v = to_resize(t, mm, addr, old_len, new_len, flags, &r);
    if (!v) { ret = r; goto out; }
    if (old_len == v->end - addr && expandable(mm, v, new_len - old_len)) {
        r = expand(mm, v, new_len - old_len, &iv);
        ret = r ? r : (int64_t)addr;
        goto out;
    }
    ret = -LX_ENOMEM;
    if (flags & LX_MREMAP_MAYMOVE) {
        int hs = (v->vflags & VF_HOSTSHARED) != 0;
        int64_t err;
        uint64_t pg = hs ? addr : (v->bk ? v->pgoff + v->bk->hbias + (addr - v->start) : addr); /* congruent with addr */
        uint64_t nw = mm_get_unmapped(mm, 0, new_len, pg, 0, hs, 1, &err);
        if (err) { ret = err; goto out; }
        ret = move_vma(t, mm, v, addr, old_len, new_len, nw, flags, &iv);
    }
out:
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return ret;
}

int64_t lkd_mm_remap(lkd_mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint32_t flags, uint64_t new_addr)
{
    return lkd_mm_remap_task(lkd_current(), mm, old_addr, old_len, new_len, flags, new_addr);
}

int64_t mm_sys_mremap(lkd_task *t, const lkd_sysreq *rq)
{
    return lkd_mm_remap_task(t, mm_of(t), LKD_ARGU(0), LKD_ARGU(1), LKD_ARGU(2), LKD_ARGU(3), LKD_ARGU(4));
}

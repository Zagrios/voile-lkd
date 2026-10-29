/* mm.c: address spaces, mapping primitives, validation, maps rendering, fork and exec. Owner: mm.
 * Linux references are to v6.12 (mm/mmap.c, mm/vma.c, mm/mprotect.c, fs/proc/task_mmu.c). */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <strings.h>
#include <unistd.h>

#include "mm_priv.h"
#include "lkd_fs.h"

#define MAX_MAP_COUNT 65530 /* sysctl vm.max_map_count default (include/linux/mm.h DEFAULT_MAX_MAP_COUNT) */

static _Atomic uint64_t id_counter;
static uint64_t new_identity(void) { return ((uint64_t)getpid() << 24) | (atomic_fetch_add(&id_counter, 1) + 1); }

void mm_lock(lkd_mm *mm) { pthread_mutex_lock(&mm->lock); }
void mm_unlock(lkd_mm *mm) { pthread_mutex_unlock(&mm->lock); }
_Atomic uint64_t *mm_guard_gen_of(lkd_mm *mm, uint64_t hp)
{
    return &mm->guard_gen[((hp / mm_hps) * 0x9E3779B97F4A7C15ull >> 40) & (MM_GUARD_GEN_SLOTS - 1)];
}
void mm_guard_gen_bump_all(lkd_mm *mm)
{
    for (unsigned i = 0; i < MM_GUARD_GEN_SLOTS; i++) atomic_fetch_add_explicit(&mm->guard_gen[i], 1, memory_order_release);
}
void mm_guard_gen_bump(lkd_mm *mm, uint64_t s, uint64_t e)
{
    uint64_t p = HP_DOWN(s), end = HP_UP(e);
    if (end <= p) return;
    if ((end - p) / mm_hps >= MM_GUARD_GEN_SLOTS) { mm_guard_gen_bump_all(mm); return; }
    for (; p < end; p += mm_hps) atomic_fetch_add_explicit(mm_guard_gen_of(mm, p), 1, memory_order_release);
}
void mm_wlock(lkd_mm *mm)
{
    mrw_wrlock(&mm->vlock);
    mm_guard_gen_bump_all(mm);
}
void mm_wunlock(lkd_mm *mm)
{
    mm_guard_gen_bump_all(mm);
    mrw_wrunlock(&mm->vlock);
}
/* The same for a change confined to [s, e): only the generations of its host pages move, so the load stubs of other
 * guarded pages stay valid (an mmap or munmap anywhere used to send every stub of the process back to the fault). */
void mm_wlock_range(lkd_mm *mm, uint64_t s, uint64_t e)
{
    mrw_wrlock(&mm->vlock);
    mm_guard_gen_bump(mm, s, e);
}
void mm_wunlock_range(lkd_mm *mm, uint64_t s, uint64_t e)
{
    mm_guard_gen_bump(mm, s, e);
    mrw_wrunlock(&mm->vlock);
}
void mm_wlock_same(lkd_mm *mm) { mrw_wrlock(&mm->vlock); }
void mm_wunlock_same(lkd_mm *mm) { mrw_wrunlock(&mm->vlock); }
lkd_mm *mm_of(lkd_task *t) { return t ? t->mm : NULL; }

int mm_capable(lkd_task *t, int cap)
{
    return t && t->proc && (t->proc->cred.cap_eff >> cap) & 1;
}

/* ---- Invalidation list ------------------------------------------------------------------------- */

void mm_inval_add(struct mm_inval *iv, uint64_t s, uint64_t e)
{
    if (!iv || s >= e) return;
    for (int i = 0; i < iv->n; i++)
        if (s <= iv->e[i] && e >= iv->s[i]) {
            if (s < iv->s[i]) iv->s[i] = s;
            if (e > iv->e[i]) iv->e[i] = e;
            return;
        }
    if (iv->n < MM_INVAL_MAX) { iv->s[iv->n] = s; iv->e[iv->n] = e; iv->n++; return; }
    /* Too many pieces: widen the last one (over-invalidation is always safe). */
    iv->overflow = 1;
    if (s < iv->s[MM_INVAL_MAX - 1]) iv->s[MM_INVAL_MAX - 1] = s;
    if (e > iv->e[MM_INVAL_MAX - 1]) iv->e[MM_INVAL_MAX - 1] = e;
}

/* ops->invalidate_code. The engine drops the translations for every thread, also when the caller is a
 * host thread without a guest task (the remote memory service serving a ptrace POKE). */
void mm_invalidate(uint64_t s, uint64_t len)
{
    const lkd_engine_ops *ops = lkd_engine();
    if (ops && ops->invalidate_code) ops->invalidate_code(s, len);
}

void mm_inval_flush(struct mm_inval *iv)
{
    for (int i = 0; i < iv->n; i++) mm_invalidate(iv->s[i], iv->e[i] - iv->s[i]);
    iv->n = 0;
}

/* ---- Backing objects --------------------------------------------------------------------------- */

struct mm_backing *mb_new(int kind)
{
    struct mm_backing *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    atomic_init(&b->refs, 1);
    b->kind = kind;
    b->host_fd = -1;
    b->shm_slot = -1;
    return b;
}

struct mm_backing *mb_ref(struct mm_backing *b)
{
    if (b) atomic_fetch_add(&b->refs, 1);
    return b;
}

void mb_put(struct mm_backing *b)
{
    if (!b || atomic_fetch_sub(&b->refs, 1) != 1) return;
    if (b->file) lkd_fdobj_put(b->file);
    uint8_t *alias = atomic_load(&b->alias);
    if (alias) munmap(alias, atomic_load(&b->alias_len));
    free(b->name);
    free(b);
}

void mm_vma_free(mm_vma *v)
{
    if (!v) return;
    mb_put(v->bk);
    free(v->label);
    free(v);
}

mm_vma *mm_vma_clone(const mm_vma *v)
{
    mm_vma *n = malloc(sizeof *n);
    if (!n) return NULL;
    *n = *v;
    n->l = n->r = NULL;
    n->label = v->label ? strdup(v->label) : NULL;
    if (v->label && !n->label) { free(n); return NULL; }
    mb_ref(n->bk);
    return n;
}

/* ---- Address space objects --------------------------------------------------------------------- */

static uint64_t default_mmap_base(void)
{
    uint64_t size = mm_win.g_hi - mm_win.g_lo;
    uint64_t gap = size >= (1ull << 30) ? (128ull << 20) : size / 8; /* mmap_base() MIN_GAP = 128 MiB */
    return HP_DOWN(mm_win.g_hi - gap);
}

lkd_mm *lkd_mm_create(void)
{
    if (!mm_win.reserved) {
        const lkd_config *c = lkd_config_get();
        uint64_t sz = c->guest_window_bytes;
        if (sz > (4ull << 30)) sz = 4ull << 30; /* identity (test) window: 4 GiB at most */
        int e = mm_window_reserve(0, sz, 1);
        if (e < 0) { errno = ENOMEM; return NULL; }
    }
    if (mm_win.owner) { errno = EBUSY; return NULL; } /* one guest address space per host process */
    lkd_mm *mm = calloc(1, sizeof *mm);
    if (!mm) { errno = ENOMEM; return NULL; }
    atomic_init(&mm->refs, 1);
    pthread_mutex_init(&mm->lock, NULL);
    atomic_init(&mm->vlock.w, 0);
    vt_init(&mm->vt, mm_win.g_lo);
    mm->identity = new_identity();
    mm->mmap_base = default_mmap_base();
    mm_win.owner = mm;
    if (!mm_win.identity) mm_remote_start();
    return mm;
}

lkd_mm *lkd_mm_get(lkd_mm *mm)
{
    if (mm) atomic_fetch_add(&mm->refs, 1);
    return mm;
}

static void mm_destroy(lkd_mm *mm)
{
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    mm_unmap_locked(mm, mm_win.g_lo, mm_win.g_hi, &iv);
    mm_unlock(mm);
    mm_inval_flush(&iv);
    if (mm_win.owner == mm) {
        mm_remote_stop();
        mm_win.owner = NULL;
    }
    pthread_mutex_destroy(&mm->lock);
    free(mm->vt.views);
    free(mm);
}

void lkd_mm_put(lkd_mm *mm)
{
    if (mm && atomic_fetch_sub(&mm->refs, 1) == 1) mm_destroy(mm);
}

/* CLONE_VM shares the address space. Without CLONE_VM the child is a host fork(): it already owns a
 * copy-on-write copy of this object, re-identified by lkd_mm_fork_child; the reference is the child's. */
lkd_mm *lkd_mm_clone(lkd_mm *parent, uint64_t clone_flags)
{
    (void)clone_flags;
    return lkd_mm_get(parent);
}

uint64_t lkd_mm_identity(const lkd_mm *mm) { return mm ? mm->identity : 0; }

/* Engine and loader helpers (declared in src/mm/lkd_mm_ext.h, see docs/requests/mm-2.md). */
void lkd_mm_set_brk_start(lkd_mm *mm, uint64_t start_brk)
{
    mm_lock(mm);
    mm->start_brk = mm->brk = start_brk;
    mm_unlock(mm);
}

void lkd_mm_set_stack_start(lkd_mm *mm, uint64_t start_stack)
{
    mm_lock(mm);
    mm->start_stack = start_stack;
    mm_unlock(mm);
}

void lkd_mm_set_mmap_base(lkd_mm *mm, uint64_t base)
{
    mm_lock(mm);
    mm->mmap_base = base;
    mm_unlock(mm);
}

uint64_t lkd_mm_window_guest_lo(void) { return mm_win.g_lo; }
uint64_t lkd_mm_window_size(void) { return mm_win.reserved ? mm_win.size : 0; }
uint64_t lkd_mm_window_guest_hi(void) { return mm_win.g_hi; }

void lkd_mm_reset_for_exec(lkd_mm *mm)
{
    if (!mm) return;
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    mm_unmap_locked(mm, mm_win.g_lo, mm_win.g_hi, &iv);
    mm->start_brk = mm->brk = 0;
    mm->start_stack = 0;
    mm->def_flags = 0;
    mm->locked_bytes = 0;
    mm->hiwater_vm = mm->hiwater_rss = 0;   /* a new mm at exec */
    mm->rss_ns = 0;
    mm->rss_anon = mm->rss_file = mm->rss_shmem = 0;
    mm->mmap_base = default_mmap_base();
    vt_views_clear(&mm->vt);
    mm_watch_clear_all();
    mm_unlock(mm);
    mm_inval_flush(&iv);
}

/* ---- Tree helpers ------------------------------------------------------------------------------ */

static int mergeable(const mm_vma *a, const mm_vma *b)
{
    if (a->end != b->start || a->prot != b->prot || a->vflags != b->vflags || a->lkind != b->lkind) return 0;
    if (a->lkind == LBL_SPECIAL || a->lkind == LBL_STACK) return 0;
    if (a->lkind == LBL_ANON && strcmp(a->label, b->label)) return 0;
    if (!a->bk || !b->bk) return !a->bk && !b->bk;
    if (a->bk != b->bk && !(a->bk->kind == MB_FILE && b->bk->kind == MB_FILE && a->bk->file == b->bk->file))
        return 0;
    return a->pgoff + (a->end - a->start) == b->pgoff;
}

/* Merge VMAs that touch [start, end] (vma_merge in mm/vma.c). Caller holds mmap_lock. */
void mm_merge_around(lkd_mm *mm, uint64_t start, uint64_t end)
{
    mm_vma *v = vt_last_start_before(&mm->vt, start);
    if (!v) v = vt_first(&mm->vt);
    mm_wlock_same(mm);
    while (v && v->start <= end) {
        mm_vma *n = vt_next(&mm->vt, v);
        if (n && mergeable(v, n)) {
            /* another backing of the same file: n's goes with n, and its file view (guard_alias) with it */
            if (n->bk != v->bk) mm_guard_gen_bump(mm, n->start, n->end);
            vt_remove(&mm->vt, n);
            vt_set_end(&mm->vt, v, n->end);
            if (n->bk && n->bk->kind == MB_SYSV) mm_shm_detach_vma(n);
            mm_vma_free(n);
            continue;
        }
        v = n;
    }
    mm_wunlock_same(mm);
}

int mm_split(lkd_mm *mm, uint64_t addr)
{
    mm_vma *v = vt_find(&mm->vt, addr);
    if (!v || v->start == addr) return 0;
    if (mm->vt.n >= MAX_MAP_COUNT) return -LX_ENOMEM;
    mm_vma *n = mm_vma_clone(v);
    if (!n) return -LX_ENOMEM;
    n->start = addr;
    if (n->bk) n->pgoff += addr - v->start;
    mm_wlock_same(mm); /* the two pieces map what v mapped, with its protection and backing */
    vt_set_end(&mm->vt, v, addr);
    vt_insert(&mm->vt, n);
    mm_wunlock_same(mm);
    if (n->bk && n->bk->kind == MB_SYSV) mm_shm_attach_vma(n);
    return 0;
}

int mm_range_mapped(lkd_mm *mm, uint64_t start, uint64_t end)
{
    uint64_t cur = start;
    while (cur < end) {
        mm_vma *v = vt_find(&mm->vt, cur);
        if (!v) return 0;
        cur = v->end;
    }
    return 1;
}

/* ---- Host page maintenance --------------------------------------------------------------------- */

static int page_has_vma(lkd_mm *mm, uint64_t p)
{
    mm_vma *v = vt_first_end_after(&mm->vt, p);
    return v && v->start < p + mm_hps;
}

static int page_hostshared(lkd_mm *mm, uint64_t p)
{
    for (mm_vma *v = vt_first_end_after(&mm->vt, p); v && v->start < p + mm_hps; v = vt_next(&mm->vt, v))
        if (v->vflags & VF_HOSTSHARED) return 1;
    return 0;
}

int mm_page_union(lkd_mm *mm, uint64_t p)
{
    int h = PROT_NONE;
    for (mm_vma *v = vt_first_end_after(&mm->vt, p); v && v->start < p + mm_hps; v = vt_next(&mm->vt, v))
        h |= mm_host_prot_of(v->prot);
    return h;
}

_Atomic int mm_any_guarded; /* a guarded page was seen in this process: LKD copies look for them (mm_guard_open_range) */

/* A Windows module's file. Wine maps a module that needs its pages edited (relocations, an image section that is not
 * file backed) from a temporary file of its own: /tmp/.wine-UID/server-.../tmpmap-N. */
static int pe_name(const char *n)
{
    size_t l = n ? strlen(n) : 0;
    return (l > 4 && (!strcasecmp(n + l - 4, ".exe") || !strcasecmp(n + l - 4, ".dll") || !strcasecmp(n + l - 4, ".sys"))) ||
           (n && strstr(n, "/tmpmap-"));
}

/* Guarded host page: part of one file mapping, with guest sub-pages that are PROT_NONE next to accessible ones.
 * I2 (union) would let the guest read and write the PROT_NONE sub-pages, so the host protection is PROT_NONE and every
 * access faults; lkd_mm_guard_fault (watch.c) then decides per 4 KiB. Windows packers keep code and data encrypted
 * under PAGE_NOACCESS and decrypt them in the access violation handler: THEIA (HELLDIVERS 2, THE FINALS) read the
 * jump table of a switch from an encrypted page that shared its host page with decrypted code, and jumped to garbage.
 * Only sub-pages of the same file object count, shared or private pieces of it (Wine maps a tmpmap file MAP_SHARED):
 * the PROT_NONE tail of an anonymous reservation after a module does not make the module's last page slow.
 * VMA lock held. */
int mm_page_guarded(lkd_mm *mm, uint64_t p)
{
    const struct mm_backing *bk = NULL;
    int none = 0, acc = 0;
    for (mm_vma *v = vt_first_end_after(&mm->vt, p); v && v->start < p + mm_hps; v = vt_next(&mm->vt, v)) {
        /* Windows modules only: the gaps between the segments of a Linux shared object are PROT_NONE file pages too,
         * next to the hot data of the object. All the pieces on the page belong to one object, shared or private. */
        if (!v->bk || v->bk->kind != MB_FILE || !pe_name(v->bk->name)) return 0;
        if (bk && !mm_same_object(bk, v->bk)) return 0;
        bk = v->bk;
        if (v->prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) acc = 1; else none = 1;
    }
    if (!(none && acc)) return 0;
    atomic_store_explicit(&mm_any_guarded, 1, memory_order_relaxed);
    return 1;
}

static int page_prot(lkd_mm *mm, uint64_t p)
{
    return mm_page_guarded(mm, p) ? PROT_NONE : mm_page_union(mm, p);
}

/* I2 for every host page of [start, end); pages without VMA go back to PROT_NONE. */
static int page_prot_w(lkd_mm *mm, uint64_t p)
{
    int h = page_prot(mm, p);
    return mm_watched(p) ? h & ~PROT_WRITE : h; /* translated code: writes must fault (watch.c) */
}

struct prun { uint64_t s, e; int prot, err; };

static void prun_emit(struct prun *r, uint64_t a, uint64_t b, int prot)
{
    if (a >= b) return;
    if (r->e == a && r->prot == prot) { r->e = b; return; }
    if (r->e > r->s) {
        int x = mm_host_protect(r->s, r->e - r->s, r->prot);
        if (x && !r->err) r->err = x;
    }
    r->s = a; r->e = b; r->prot = prot;
}

/* I2 for every host page of [start, end); pages without VMA go back to PROT_NONE. The walk is by VMA
 * and by run: an empty gap or the interior of one VMA is one mprotect, whatever its size (the guest
 * window is 2^46 bytes, 2^32 host pages). Watched pages inside a writable run are split off. */
int mm_host_refresh(lkd_mm *mm, uint64_t start, uint64_t end)
{
    uint64_t p = HP_DOWN(start), e = HP_UP(end);
    struct prun r = { 0, 0, -1, 0 };
    mm_watch_lock();
    while (p < e) {
        mm_vma *v = vt_first_end_after(&mm->vt, p);
        uint64_t q;
        if (!v || v->start >= p + mm_hps) {
            q = v ? HP_DOWN(v->start) : e;
            if (q > e) q = e;
            if (q <= p) q = p + mm_hps;
            prun_emit(&r, p, q, PROT_NONE);
        } else if (v->start <= p && v->end >= p + mm_hps) {
            q = HP_DOWN(v->end);
            if (q > e) q = e;
            int pr = mm_host_prot_of(v->prot);
            if (!(pr & PROT_WRITE)) {
                prun_emit(&r, p, q, pr);
            } else {
                for (uint64_t x = p; x < q;) {
                    uint64_t w = mm_next_watched(x, q);
                    prun_emit(&r, x, w, pr);
                    if (w >= q) break;
                    prun_emit(&r, w, w + mm_hps, pr & ~PROT_WRITE);
                    x = w + mm_hps;
                }
            }
        } else {
            q = p + mm_hps;
            prun_emit(&r, p, q, page_prot_w(mm, p));
        }
        p = q;
    }
    if (r.e > r.s && r.prot >= 0) {
        int x = mm_host_protect(r.s, r.e - r.s, r.prot);
        if (x && !r.err) r.err = x;
    }
    /* mprotect changes v->prot without the VMA write lock: this is where the change becomes visible */
    mm_guard_gen_bump(mm, start, end);
    mm_watch_unlock();
    return r.err;
}

int mm_temp_access(lkd_mm *mm, uint64_t s, uint64_t e, int write)
{
    (void)mm;
    return mm_host_protect(HP_DOWN(s), HP_UP(e) - HP_DOWN(s), PROT_READ | (write ? PROT_WRITE : 0));
}

/* Zero or file content into [s, e) of v (private VMA). Host pages must be writable. */
int mm_fill(lkd_mm *mm, const mm_vma *v, uint64_t s, uint64_t e)
{
    (void)mm;
    uint8_t *h = mm_h(s);
    uint64_t n = e - s;
    if (!v->bk || v->bk->kind != MB_FILE) {
        memset(h, 0, n);
        return 0;
    }
    uint64_t off = v->pgoff + (s - v->start), done = 0;
    uint64_t fsz = v->bk->file_size;
    uint64_t avail = fsz > off ? fsz - off : 0;
    if (avail > n) avail = n;
    while (done < avail) {
        ssize_t r = pread(v->bk->host_fd, h + done, avail - done, (off_t)(off + done + v->bk->hbias));
        if (r < 0) {
            if (errno == EINTR) continue;
            return (int)LKD_HOST_ERR();
        }
        if (r == 0) break; /* file shrank: the rest reads as zeros */
        done += (uint64_t)r;
    }
    if (done < n) memset(h + done, 0, n - done);
    return 0;
}

/* [s, e) of the private VMA v reads zeros again (anonymous) or the file again (file mapping): madvise
 * DONTNEED, WIPEONFORK in a fork child, the old range of mremap DONTUNMAP. Host pages fully inside v
 * and [s, e) are replaced in one reset (no per-page loop over a large range); the boundary pages, which
 * other VMAs may share, and the pages that hold file data are written. Caller holds the watch lock and
 * calls mm_host_refresh afterwards. */
int mm_discard_range(lkd_mm *mm, const mm_vma *v, uint64_t s, uint64_t e)
{
    uint64_t is = HP_UP(s), ie = HP_DOWN(e);
    int r = 0;
    if (is >= ie) { /* at most two host pages, none of them whole */
        for (uint64_t p = HP_DOWN(s); p < e && !r; p += mm_hps) {
            uint64_t a = p > s ? p : s, z = p + mm_hps < e ? p + mm_hps : e;
            if (page_hostshared(mm, p)) continue; /* owned by a shared object: never written from here */
            r = mm_temp_access(mm, p, p + mm_hps, 1);
            if (!r) r = mm_fill(mm, v, a, z);
        }
        return r;
    }
    if (s < is && !page_hostshared(mm, HP_DOWN(s)) && !(r = mm_temp_access(mm, HP_DOWN(s), is, 1))) r = mm_fill(mm, v, s, is);
    if (!r && ie < e && !page_hostshared(mm, ie) && !(r = mm_temp_access(mm, ie, ie + mm_hps, 1))) r = mm_fill(mm, v, ie, e);
    if (r) return r;
    uint64_t fill_end = is;
    if (v->bk && v->bk->kind == MB_FILE) {
        uint64_t off = v->pgoff + (is - v->start);
        uint64_t data = v->bk->file_size > off ? v->bk->file_size - off : 0;
        fill_end = is + (data < ie - is ? HP_UP(data) : ie - is);
        if (fill_end > ie) fill_end = ie;
        if (fill_end > is && !(r = mm_temp_access(mm, is, fill_end, 1))) r = mm_fill(mm, v, is, fill_end);
    }
    if (!r && fill_end < ie) r = mm_host_reset(fill_end, ie - fill_end);
    return r;
}

/* ---- Unmap ------------------------------------------------------------------------------------- */

static uint64_t resident_bytes(const mm_vma *v, uint64_t *dirty);

/* RSS of this guest process: the host process's resident memory (TASK_VM_INFO, what ps shows), anonymous (internal)
 * and the rest. It includes the engine's own memory (FEX's code cache, LKD), and shared anonymous memory counts as
 * anonymous or file by its host backing (RssShmem stays 0). A walk of the guest's VMAs gave guest pages only but
 * cost one mach_vm_region per host region: MESURE about 20 ms for 40000 regions, against 0.5 us here; Dead by
 * Daylight reads /proc/self/status (GetProcessMemoryInfo) every frame with that many. */
static void host_rss(uint64_t *anon, uint64_t *rest)
{
    task_vm_info_data_t vi;
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    *anon = *rest = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) != KERN_SUCCESS) return;
    *anon = vi.internal < vi.resident_size ? vi.internal : vi.resident_size;
    *rest = vi.resident_size - *anon;
}

/* update_hiwater_vm and update_hiwater_rss: called before VMAs go away. mmap lock held. */
static void hiwater_before_unmap(lkd_mm *mm, uint64_t start, uint64_t end)
{
    if (start <= mm_win.g_lo && end >= mm_win.g_hi) return; /* exec reset or destruction: the mm restarts */
    uint64_t anon, rest;
    host_rss(&anon, &rest);
    if (mm->vt.total > mm->hiwater_vm) mm->hiwater_vm = mm->vt.total;
    if (anon + rest > mm->hiwater_rss) mm->hiwater_rss = anon + rest;
}

int mm_unmap_locked(lkd_mm *mm, uint64_t start, uint64_t end, struct mm_inval *iv)
{
    if (start >= end) return 0;
    mm_vma *v = vt_first_end_after(&mm->vt, start);
    if (!v || v->start >= end) return 0;
    hiwater_before_unmap(mm, start, end);
    mm->map_gen++;
    /* Splitting in the middle of one VMA needs one more slot (do_vmi_align_munmap). */
    if (v->start < start && v->end > end && mm->vt.n >= MAX_MAP_COUNT) return -LX_ENOMEM;
    int r = mm_split(mm, start);
    if (!r) r = mm_split(mm, end);
    if (r) return r;
    mm_vma *list = NULL;
    mm_wlock_range(mm, start, end);
    while ((v = vt_first_end_after(&mm->vt, start)) && v->start < end) {
        vt_remove(&mm->vt, v);
        v->l = list;
        list = v;
    }
    mm_wunlock_range(mm, start, end);
    uint64_t pg_lo = HP_DOWN(start), pg_hi = HP_DOWN(end - 1);
    int was_lo = 0, was_hi = 0;
    while (list) {
        v = list;
        list = v->l;
        if (v->vflags & VF_HOSTSHARED) { /* which edge host pages were mapped from a shared object */
            if (HP_DOWN(v->start) <= pg_lo) was_lo = 1;
            if (HP_DOWN(v->end - 1) >= pg_hi) was_hi = 1;
        }
        if (v->prot & LX_PROT_EXEC) mm_inval_add(iv, v->start, v->end);
        if (v->vflags & VF_LOCKED) {
            mm->locked_bytes -= v->end - v->start;
            munlock(mm_h(HP_DOWN(v->start)), HP_UP(v->end) - HP_DOWN(v->start));
        }
        if (v->bk && v->bk->kind == MB_SYSV) mm_shm_detach_vma(v);
        mm_vma_free(v);
    }
    /* A host page whose shared VMA is gone but that still holds inert PROT_NONE private siblings goes
     * back to fresh private memory: the object must not stay visible behind them. */
    for (int i = 0; i < 2; i++) {
        uint64_t p = i ? pg_hi : pg_lo;
        if ((i ? was_hi : was_lo) && !(i && pg_hi == pg_lo) && page_has_vma(mm, p) && !page_hostshared(mm, p))
            mm_host_reset(p, mm_hps);
    }
    mm_host_cleanup(mm, start, end);
    return 0;
}

/* After VMAs left [start, end): free host pages become fresh reservation (I1); a private page still used
 * by another VMA keeps its other sub-pages and gets the freed ones zeroed, so a later mapping reads
 * zeros; protections follow I2. */
void mm_host_cleanup(lkd_mm *mm, uint64_t start, uint64_t end)
{
    uint64_t p = HP_DOWN(start), pe = HP_UP(end);
    mm_watch_lock();
    while (p < pe) {
        mm_vma *v = vt_first_end_after(&mm->vt, p);
        if (!v || v->start >= p + mm_hps) {
            /* no VMA from p to the host page of the next VMA: one reset, whatever the size (task-4: this
             * walk used to advance one host page at a time, 2^32 steps over the production window).
             * A freed page loses its watch (its code VMAs were invalidated by the caller). */
            uint64_t q = v ? HP_DOWN(v->start) : pe;
            if (q > pe) q = pe;
            if (q <= p) q = p + mm_hps;
            mm_host_reset(p, q - p);
            mm_unwatch_range(p, q);
            p = q;
            continue;
        }
        if (v->start <= p && v->end >= p + mm_hps) {
            /* fully covered by a VMA that stays: nothing was freed here */
            uint64_t q = HP_DOWN(v->end);
            if (q > pe) q = pe;
            p = q > p ? q : p + mm_hps;
            continue;
        }
        /* a page still used by another VMA keeps its watch (remaining translations stay protected) */
        if (!page_hostshared(mm, p)) {
            uint64_t zs = p > start ? p : start, ze = p + mm_hps < end ? p + mm_hps : end;
            for (uint64_t z = zs; z < ze;) { /* zero only the sub-pages that no VMA covers */
                mm_vma *o = vt_find(&mm->vt, z);
                if (o) { z = o->end; continue; }
                mm_vma *n = vt_first_end_after(&mm->vt, z);
                uint64_t zz = n && n->start < ze ? n->start : ze;
                if (mm_temp_access(mm, p, p + mm_hps, 1) == 0) memset(mm_h(z), 0, zz - z);
                z = zz;
            }
        }
        mm_host_refresh(mm, p, p + mm_hps);
        p += mm_hps;
    }
    mm_watch_unlock();
}

/* ---- Map --------------------------------------------------------------------------------------- */

/* Same shared object? File by open description or (device, inode), anonymous shared and SysV by identity. */
int mm_same_object(const struct mm_backing *a, const struct mm_backing *b)
{
    if (!a || !b) return 0;
    if (a == b) return 1;
    if (a->kind != b->kind) return 0;
    if (a->kind == MB_FILE) return a->file == b->file || (a->ino == b->ino && a->dev_major == b->dev_major && a->dev_minor == b->dev_minor);
    if (a->kind == MB_SYSV) return a->shm_id == b->shm_id;
    return 0;
}

/* Host page sharing rules (I3), docs/status/mm.md. A host page of a MAP_SHARED VMA is mapped from the
 * shared object; the guest 4 KiB pages around the VMA in that page ("siblings") alias the object's
 * bytes there. That is only acceptable when no guest page can reach them: siblings must be unmapped, or
 * private PROT_NONE, or further pieces of the SAME object at the same relative position. Would a mapping
 * of [s, e) with (shared, prot, bk, pgoff) break this in a host page it touches? bk == NULL means
 * unknown (hint probe): conservative. */
int mm_host_conflict(lkd_mm *mm, uint64_t s, uint64_t e, int shared, uint32_t prot,
                     const struct mm_backing *bk, uint64_t pgoff)
{
    uint64_t pages[2] = { HP_DOWN(s), HP_DOWN(e - 1) };
    for (int i = 0; i < 2; i++) {
        uint64_t p = pages[i];
        if (i && p == pages[0]) break;
        for (mm_vma *v = vt_first_end_after(&mm->vt, p); v && v->start < p + mm_hps; v = vt_next(&mm->vt, v)) {
            uint64_t a = v->start > p ? v->start : p, b = v->end < p + mm_hps ? v->end : p + mm_hps;
            if (a >= s && b <= e) continue; /* replaced by the new mapping */
            int vs = (v->vflags & VF_HOSTSHARED) != 0;
            if (shared) {
                if (vs) {
                    if (!mm_same_object(v->bk, bk)) return 1;
                    int rel = bk->kind == MB_SHANON ? (v->start & (mm_hps - 1)) == (s & (mm_hps - 1))
                                                    : v->start - v->pgoff == s - pgoff;
                    if (!rel) return 1;
                } else if (v->prot != 0) {
                    return 1; /* an accessible private page would alias the object */
                }
            } else if (vs && prot != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int direct_ok(const struct mm_mapreq *rq)
{
    return rq->bk && rq->bk->kind == MB_FILE && !(rq->vflags & VF_HOSTSHARED) && rq->bk->host_fd >= 0 &&
           ((rq->pgoff + rq->bk->hbias - rq->addr) & (mm_hps - 1)) == 0;
}

/* Shared objects are mapped on the host pages the VMA touches; the VMA may cover only part of them. The
 * file offset of the first host page is pgoff minus the position of the VMA in it (congruence checked). */
static int host_map_shared(const struct mm_mapreq *rq)
{
    uint64_t p = HP_DOWN(rq->addr), len = HP_UP(rq->addr + rq->len) - p;
    int hp = mm_host_prot_of(rq->prot);
    void *want = mm_h(p), *r;
    if (rq->bk->kind == MB_SHANON)
        r = mmap(want, len, hp, MAP_SHARED | MAP_ANON | MAP_FIXED, -1, 0);
    else
        r = mmap(want, len, hp, MAP_SHARED | MAP_FIXED, rq->bk->host_fd, (off_t)(rq->pgoff + rq->bk->hbias - (rq->addr - p)));
    if (r == MAP_FAILED) {
        int64_t e = LKD_HOST_ERR();
        mm_host_reset(p, len);
        return (int)e;
    }
    return 0;
}

int64_t mm_map_locked(lkd_mm *mm, struct mm_mapreq *rq, struct mm_inval *iv)
{
    uint64_t s = rq->addr, e = rq->addr + rq->len;
    int hostshared = (rq->vflags & VF_HOSTSHARED) != 0;
    if (hostshared) {
        /* The host page must map the object at the right place: address and offset congruent modulo the
         * host page (anonymous shared memory has no offset to respect). */
        int anon = rq->bk->kind == MB_SHANON;
        uint64_t ho = rq->pgoff + rq->bk->hbias;   /* host offset (memfd phase) */
        if (!anon && ((s - ho) & (mm_hps - 1) || ho < (s & (mm_hps - 1)))) {
            lkd_log_once("mm: MAP_SHARED at 0x%llx offset 0x%llx: address and offset are not congruent modulo "
                         "the 16 KiB host page: EINVAL (host page limit)",
                         (unsigned long long)s, (unsigned long long)rq->pgoff);
            return -LX_EINVAL;
        }
    }
    if (mm_host_conflict(mm, s, e, hostshared, rq->prot, rq->bk, rq->pgoff)) {
        lkd_log_once("mm: %s at 0x%llx len 0x%llx prot %u offset 0x%llx of %s would put an accessible mapping "
                     "and a MAP_SHARED mapping in one 16 KiB host page (siblings must be unmapped or PROT_NONE): "
                     "EINVAL", hostshared ? "MAP_SHARED" : "mapping", (unsigned long long)s,
                     (unsigned long long)rq->len, rq->prot, (unsigned long long)rq->pgoff,
                     rq->bk && rq->bk->name ? rq->bk->name : rq->bk ? "a shared object" : "anonymous memory");
        return -LX_EINVAL;
    }
    if (mm->vt.n + 1 > MAX_MAP_COUNT) return -LX_ENOMEM;
    mm_vma *nv = calloc(1, sizeof *nv);
    if (!nv) return -LX_ENOMEM;
    nv->start = s;
    nv->end = e;
    nv->prot = rq->prot;
    nv->vflags = rq->vflags | mm->def_flags;
    nv->bk = mb_ref(rq->bk);
    nv->pgoff = rq->bk ? rq->pgoff : 0;
    nv->lkind = rq->lkind;
    if (rq->label && !(nv->label = strdup(rq->label))) { mm_vma_free(nv); return -LX_ENOMEM; }

    int r = mm_unmap_locked(mm, s, e, iv);
    if (r) { mm_vma_free(nv); return r; }

    if (hostshared) {
        r = host_map_shared(rq);
        if (r) { mm_vma_free(nv); return r; }
    } else {
        mm_watch_lock(); /* the pages written below must not lose PROT_WRITE meanwhile */
        int direct = direct_ok(rq);
        /* Interior host pages of the new range hold no other VMA (I1: fresh zero memory), so per-page work
         * is limited to the pages with file data and the two boundary pages: a huge anonymous or
         * PROT_NONE reservation costs O(1) here. */
        uint64_t data_end = s;
        if (rq->bk) {
            uint64_t avail = rq->bk->file_size > rq->pgoff ? rq->bk->file_size - rq->pgoff : 0;
            data_end = s + (avail < e - s ? avail : e - s);
        }
        uint64_t p = HP_DOWN(s), pe = HP_UP(data_end);
        if (pe < p + mm_hps) pe = p + mm_hps;
        uint64_t last = HP_DOWN(e - 1);
        while (p < pe) {
            int full = p >= s && p + mm_hps <= e && !page_has_vma(mm, p);
            uint64_t fo = rq->pgoff + (p - s);
            if (direct && full && fo < rq->bk->file_size) {
                /* Run of whole host pages backed directly by the file, copy-on-write. */
                uint64_t q = p + mm_hps;
                while (q + mm_hps <= e && !page_has_vma(mm, q) && rq->pgoff + (q - s) < rq->bk->file_size) q += mm_hps;
                void *m = mmap(mm_h(p), q - p, mm_host_prot_of(rq->prot), MAP_PRIVATE | MAP_FIXED,
                               rq->bk->host_fd, (off_t)(fo + rq->bk->hbias));
                if (m == MAP_FAILED) {
                    r = (int)LKD_HOST_ERR();
                    mm_host_reset(p, q - p);
                    break;
                }
                p = q;
                continue;
            }
            uint64_t fs = p > s ? p : s, fe = p + mm_hps < e ? p + mm_hps : e;
            int fresh = !page_has_vma(mm, p);
            if ((!fresh || rq->bk) && !page_hostshared(mm, p)) {
                /* Shared host page (other sub-pages in use) or file content: write it explicitly. */
                r = mm_temp_access(mm, p, p + mm_hps, 1);
                if (!r) r = mm_fill(mm, nv, fs, fe);
                if (r) break;
            }
            p += mm_hps;
        }
        if (!r && last >= pe && page_has_vma(mm, last) && !page_hostshared(mm, last)) {
            /* last host page shared with another VMA: its sub-pages of the new range read zeros */
            r = mm_temp_access(mm, last, last + mm_hps, 1);
            if (!r) r = mm_fill(mm, nv, last > s ? last : s, e);
        }
        if (r) {
            mm_host_cleanup(mm, s, e); /* nv was never inserted: its sub-pages are free again */
            mm_watch_unlock();
            mm_vma_free(nv);
            return r;
        }
    }
    mm_wlock_range(mm, nv->start, nv->end);
    vt_insert(&mm->vt, nv);
    mm_wunlock_range(mm, nv->start, nv->end);
    mm->map_gen++;
    if (hostshared) mm_watch_alias_sync(mm, s, e);   /* another view of these bytes holds translated code */
    if (nv->bk && nv->bk->kind == MB_SYSV) mm_shm_attach_vma(nv);
    if (nv->vflags & VF_LOCKED) {
        mm->locked_bytes += e - s;
        if (!(nv->vflags & VF_LOCKONFAULT) && (rq->prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC))) {
            mm_host_refresh(mm, s, e);
            mlock(mm_h(HP_DOWN(s)), HP_UP(e) - HP_DOWN(s)); /* populate and wire; MAP_LOCKED failures are not reported by Linux either */
        }
    }
    mm_merge_around(mm, s, e);
    mm_host_refresh(mm, s, e);
    if (!hostshared) mm_watch_unlock();
    if (rq->prot & LX_PROT_EXEC) mm_inval_add(iv, s, e);
    return (int64_t)s;
}

/* ---- Protect ----------------------------------------------------------------------------------- */

/* mprotect_fixup over [start, end): partial changes stay when a hole or EACCES is met (do_mprotect_pkey). */
int mm_protect_locked(lkd_mm *mm, uint64_t start, uint64_t end, uint32_t prot, struct mm_inval *iv)
{
    mm_vma *v = vt_find(&mm->vt, start);
    if (!v) return -LX_ENOMEM;
    uint64_t cur = start;
    int err = 0;
    while (cur < end) {
        v = vt_find(&mm->vt, cur);
        if (!v) { err = -LX_ENOMEM; break; }
        uint64_t pe = v->end < end ? v->end : end;
        if ((prot & LX_PROT_WRITE) && !(v->vflags & VF_MAYWRITE)) { err = -LX_EACCES; break; }
        if ((prot & LX_PROT_WRITE) && (v->vflags & VF_SHARED) && v->bk && v->bk->kind == MB_FILE && v->bk->file) {
            /* a sealed memfd (F_SEAL_WRITE, F_SEAL_FUTURE_WRITE) never becomes writable through a shared
             * mapping (seal_check_write clears VM_MAYWRITE; request fs-mm point 2) */
            int seals = lkd_fs_memfd_seals(v->bk->file);
            if (seals > 0 && (seals & (LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE))) { err = -LX_EACCES; break; }
        }
        if (prot && !(v->vflags & VF_HOSTSHARED) && (page_hostshared(mm, HP_DOWN(cur)) || page_hostshared(mm, HP_DOWN(pe - 1)))) {
            /* the sibling would alias the bytes of a shared object (see mm_host_conflict) */
            lkd_log_once("mm: mprotect at 0x%llx would make a page accessible inside a 16 KiB host page that "
                         "maps a MAP_SHARED object: ENOMEM", (unsigned long long)cur);
            err = -LX_ENOMEM;
            break;
        }
        if (v->prot != prot) {
            if ((err = mm_split(mm, cur)) || (err = mm_split(mm, pe))) break;
            v = vt_find(&mm->vt, cur);
            if ((v->prot | prot) & LX_PROT_EXEC) mm_inval_add(iv, cur, pe);
            vt_account(&mm->vt, v, -1);
            v->prot = prot;
            vt_account(&mm->vt, v, 1);
        }
        cur = pe;
    }
    if (cur > start) {
        mm_merge_around(mm, start, cur);
        mm_host_refresh(mm, start, cur);
    }
    return err;
}

/* ---- Free range search ------------------------------------------------------------------------- */

/* Free memory left above a file mapping placed here, such as a library ld.so maps. Linux packs them, and a program
 * that needs a page beside a library finds one in the tail of its last page or between them; here that tail shares
 * a host page with the library, where no MAP_SHARED view fits. Easy Anti-Cheat maps its trampolines there (a 4 KiB
 * memfd, MAP_SHARED|MAP_FIXED_NOREPLACE, at 64 KiB steps up to about 1.25 MiB around the libc function it hooks):
 * HELLDIVERS 2 found no room and stopped on an error (SOS G4P4-HCAY). Over 64 KiB plus a host page, every such scan
 * that crosses the gap meets a whole free host page. */
#define FILE_MAP_GAP (128ull << 10)

struct fitctx {
    lkd_mm *mm;
    uint64_t len, align, off, gap;
    int hostshared, topdown;
};

static uint64_t fit(void *c, uint64_t glo, uint64_t ghi, mm_vma *prev, mm_vma *next)
{
    struct fitctx *f = c;
    uint64_t lo = glo, hi = ghi;
    if (f->hostshared || (prev && (prev->vflags & VF_HOSTSHARED))) lo = HP_UP(lo);
    if (f->hostshared || (next && (next->vflags & VF_HOSTSHARED))) hi = HP_DOWN(hi);
    if (next && f->gap) hi = hi > lo + f->gap ? HP_DOWN(hi) - f->gap : lo;
    if (hi <= lo || hi - lo < f->len) return 0;
    uint64_t a;
    if (f->topdown) {
        a = hi - f->len;
        a = ((a - f->off) & ~(f->align - 1)) + f->off;
        if (a > hi - f->len) a -= f->align;
        if (a < lo || a > hi) return 0;
    } else {
        a = ((lo - f->off + f->align - 1) & ~(f->align - 1)) + f->off;
        if (a < lo) a += f->align;
        if (a + f->len > hi || a < lo) return 0;
    }
    return a;
}

static uint64_t search(lkd_mm *mm, uint64_t len, uint64_t low, uint64_t high, int topdown, int hostshared,
                       uint64_t align, uint64_t off, uint64_t gap)
{
    struct fitctx f = { mm, hostshared ? HP_UP(len) : len, align, off & (align - 1), gap, hostshared, topdown };
    if (high <= low || f.len > high - low) return 0;
    return vt_search(&mm->vt, f.len, low, high, mm_win.g_hi, topdown, fit, &f);
}

static uint64_t search_any(lkd_mm *mm, uint64_t len, uint64_t low, uint64_t high, int topdown, int hostshared,
                           int filemap, uint64_t pgoff)
{
    uint64_t a = 0;
    if (hostshared) {
        /* whole host pages are reserved for the object; the VMA sits at the position the offset needs */
        uint64_t intra = pgoff & (mm_hps - 1);
        a = search(mm, len + intra, low, high, topdown, 1, mm_hps, 0, 0);
        return a ? a + intra : 0;
    }
    /* Prefer an address congruent with the file offset modulo the host page so the file can be mapped
     * directly; any 4 KiB address otherwise (Linux has no alignment constraint here). A file mapping leaves
     * FILE_MAP_GAP free above it while there is room for it. */
    uint64_t gap = filemap ? FILE_MAP_GAP : 0;
    if (filemap) a = search(mm, len, low, high, topdown, 0, mm_hps, pgoff, gap);
    if (!a) a = search(mm, len, low, high, topdown, 0, GP, 0, gap);
    if (!a && gap) a = search(mm, len, low, high, topdown, 0, GP, 0, 0);
    return a;
}

static int hint_free(lkd_mm *mm, uint64_t a, uint64_t len, int hostshared, uint64_t pgoff)
{
    if (hostshared && (a - pgoff) & (mm_hps - 1)) return 0;
    mm_vma *v = vt_first_end_after(&mm->vt, a);
    if (v && a + len > v->start) return 0;
    /* unknown prot and object: conservative */
    return !mm_host_conflict(mm, a, a + len, hostshared, LX_PROT_READ, NULL, pgoff);
}

/* get_unmapped_area for a non-fixed mapping (arch/x86/kernel/sys_x86_64.c, mm/mmap.c). */
uint64_t mm_get_unmapped(lkd_mm *mm, uint64_t hint, uint64_t len, uint64_t pgoff, uint32_t lxflags,
                         int hostshared, int filemap, int64_t *err)
{
    uint64_t lo = mm_win.g_lo, hi = mm_win.g_hi;
    *err = 0;
    if (len > hi - lo) { *err = -LX_ENOMEM; return 0; }
    if (lxflags & LX_MAP_32BIT) {
        uint64_t b = lo + 0x40000000ull, e = lo + 0x80000000ull;
        if (e > hi) e = hi;
        if (len > e - lo) { *err = -LX_ENOMEM; return 0; }
        if (hint) {
            uint64_t a = GP_ALIGN(hint);
            if (e >= len && e - len >= a && hint_free(mm, a, len, hostshared, pgoff)) return a;
        }
        uint64_t a = search_any(mm, len, b, e, 0, hostshared, filemap, pgoff);
        if (!a) *err = -LX_ENOMEM;
        return a;
    }
    if (hint) {
        uint64_t a = hint & ~(GP - 1);
        if (a >= lo && hi - len >= a && hint_free(mm, a, len, hostshared, pgoff)) return a;
    }
    uint64_t low = lo + GP;
    if (lxflags & 0x80 /* MAP_ABOVE4G */) low = lo + (4ull << 30);
    uint64_t a = 0;
    if (!filemap && !hostshared && (len & ((2ull << 20) - 1)) == 0) {
        /* Larger anonymous mappings are THP aligned (thp_get_unmapped_area_vmflags, mm/huge_memory.c). */
        uint64_t size = 2ull << 20;
        uint64_t r = search_any(mm, len + size, low, mm->mmap_base, 1, 0, 0, 0);
        if (r) {
            uint64_t off_sub = (0 - r) & (size - 1);
            a = off_sub ? r + off_sub : r + size;
        }
    }
    if (!a) a = search_any(mm, len, low, mm->mmap_base, 1, hostshared, filemap, pgoff);
    if (!a) a = search_any(mm, len, HP_UP(lo + (hi - lo) / 3), hi, 0, hostshared, filemap, pgoff); /* bottom-up fallback */
    if (!a) *err = -LX_ENOMEM;
    return a;
}

/* ---- Validation hook --------------------------------------------------------------------------- */

int mm_check_range_mm(lkd_mm *mm, uint64_t gva, uint64_t len, int need_write, int force)
{
    if (len == 0) return 0;
    uint64_t end = gva + len;
    if (end < gva) return -LX_EFAULT;
    int r = 0;
    mrw_rdlock(&mm->vlock);
    uint64_t cur = gva;
    while (cur < end) {
        mm_vma *v = vt_find(&mm->vt, cur);
        if (!v) { r = -LX_EFAULT; break; }
        if (!force) {
            if (need_write ? !(v->prot & LX_PROT_WRITE) : !v->prot) { r = -LX_EFAULT; break; }
        } else if (need_write && (v->vflags & VF_SHARED) && !(v->vflags & VF_MAYWRITE)) {
            r = -LX_EFAULT; /* FOLL_FORCE cannot write a read-only shared mapping */
            break;
        }
        cur = v->end;
    }
    mrw_rdunlock(&mm->vlock);
    return r;
}

/* Strong definition of the core hook (src/include/mm.h). A thread without a task or a task without an
 * address space (unit tests of other modules, the engine's helper threads) is not checked: the fault
 * recovery of lkd_user_* stays the safety net. In identity mode, host memory outside the window is not
 * guest memory managed here and is not checked either. */
int lkd_mm_check_range(uint64_t gva, size_t len, int need_write)
{
    lkd_task *t = lkd_current();
    lkd_mm *mm = t ? t->mm : NULL;
    if (!mm) return 0;
    if (mm_win.identity && (gva + len <= mm_win.g_lo || gva >= mm_win.g_hi)) return 0;
    int r = mm_check_range_mm(mm, gva, len, need_write, 0);
    if (!r) mm_guard_open_range(mm, gva, len, need_write);
    if (r || !need_write || !len) return r;
    /* An LKD write into a page that holds translated code: drop the translations and make the page
     * writable before the copy (the same path as a guest store, lkd_mm_code_write_fault). */
    for (uint64_t p = HP_DOWN(gva); p < gva + len; p += mm_hps)
        if (mm_watched(p)) mm_unwatch_for_write(mm, p > gva ? p : gva, 1, NULL);
    return 0;
}

/* ---- Queries ----------------------------------------------------------------------------------- */

static int heap_vma(const lkd_mm *mm, const mm_vma *v)
{
    return !v->bk && mm->brk && v->start < mm->brk && v->end > mm->start_brk; /* vma_is_initial_heap */
}

static void fill_info(const lkd_mm *mm, const mm_vma *v, struct lkd_vma_info *o)
{
    memset(o, 0, sizeof *o);
    o->start = v->start;
    o->end = v->end;
    o->prot = v->prot;
    o->flags = (v->vflags & VF_SHARED) ? LX_MAP_SHARED : LX_MAP_PRIVATE;
    if (v->vflags & VF_LOCKED) o->flags |= LX_MAP_LOCKED;
    if (v->bk) {
        o->file_offset = v->pgoff;
        o->dev_major = v->bk->dev_major;
        o->dev_minor = v->bk->dev_minor;
        o->ino = v->bk->ino;
        if (v->bk->name) snprintf(o->name, sizeof o->name, "%s", v->bk->name);
        o->deleted = v->bk->deleted;
        struct stat st; /* d_path: an unlinked file shows " (deleted)" from then on */
        if (!o->deleted && v->bk->kind == MB_FILE && v->bk->host_fd >= 0 && fstat(v->bk->host_fd, &st) == 0 &&
            S_ISREG(st.st_mode) && st.st_nlink == 0)
            o->deleted = 1;
        if (v->bk->kind == MB_SHANON || v->bk->kind == MB_SYSV) o->flags |= LX_MAP_ANONYMOUS;
    } else {
        o->flags |= LX_MAP_ANONYMOUS;
        if (v->lkind == LBL_SPECIAL && v->label) snprintf(o->name, sizeof o->name, "%s", v->label);
        else if (heap_vma(mm, v)) snprintf(o->name, sizeof o->name, "[heap]");
        else if (v->lkind == LBL_STACK || (mm->start_stack && v->start <= mm->start_stack && v->end >= mm->start_stack))
            snprintf(o->name, sizeof o->name, "[stack]");
        else if (v->lkind == LBL_ANON && v->label) snprintf(o->name, sizeof o->name, "%s", v->label);
    }
}

int lkd_mm_find(lkd_mm *mm, uint64_t addr, int next, struct lkd_vma_info *out)
{
    if (!mm) return -LX_ENOENT;
    mrw_rdlock(&mm->vlock);
    mm_vma *v = next ? vt_first_end_after(&mm->vt, addr) : vt_find(&mm->vt, addr);
    if (v) fill_info(mm, v, out);
    mrw_rdunlock(&mm->vlock);
    return v ? 0 : -LX_ENOENT;
}

int lkd_mm_find_nowait(lkd_mm *mm, uint64_t addr, struct lkd_vma_info *out)
{
    if (!mm) return -LX_ENOENT;
    if (mrw_tryrdlock(&mm->vlock) != 0) return -LX_EBUSY;
    mm_vma *v = vt_find(&mm->vt, addr);
    if (v) fill_info(mm, v, out);
    mrw_rdunlock(&mm->vlock);
    return v ? 0 : -LX_ENOENT;
}


static int same_file(const struct mm_backing *b, struct lkd_fdobj *file, uint64_t ino, uint64_t dev_major,
                     uint64_t dev_minor, int have_id)
{
    if (b->kind != MB_FILE) return 0;
    if (b->file == file) return 1;
    return have_id && b->ino == ino && b->dev_major == dev_major && b->dev_minor == dev_minor;
}

int lkd_mm_has_writable_shared_mapping(lkd_mm *mm, struct lkd_fdobj *file)
{
    if (!mm || !file) return 0;
    struct mm_backing probe = { 0 };
    int have_id = 0;
    struct stat st;
    int fd = file->ops && file->ops->mmap_fd ? file->ops->mmap_fd(file) : file->host_fd;
    if (fd >= 0 && fstat(fd, &st) == 0) {
        uint64_t d = lkd_dev_to_linux(st.st_dev);
        probe.ino = (uint64_t)st.st_ino;
        probe.dev_major = (uint32_t)((d & 0xfff00) >> 8);
        probe.dev_minor = (uint32_t)((d & 0xff) | ((d >> 12) & 0xfff00));
        have_id = 1;
    }
    int found = 0;
    mrw_rdlock(&mm->vlock);
    for (mm_vma *v = vt_first(&mm->vt); v && !found; v = vt_next(&mm->vt, v))
        if (v->bk && (v->vflags & VF_SHARED) && (v->vflags & VF_MAYWRITE) &&
            same_file(v->bk, file, probe.ino, probe.dev_major, probe.dev_minor, have_id))
            found = 1;
    mrw_rdunlock(&mm->vlock);
    return found;
}

int lkd_mm_set_vma_name(lkd_mm *mm, uint64_t addr, uint64_t len, const char *name)
{
    if (!mm || (addr & (GP - 1))) return -LX_EINVAL;
    len = GP_ALIGN(len);
    if (!len) return -LX_EINVAL;
    uint8_t kind = LBL_NONE;
    if (name && *name) {
        if (!strcmp(name, "[stack]")) kind = LBL_STACK;
        else if (!strncmp(name, "[anon:", 6)) kind = LBL_ANON;
        else kind = LBL_SPECIAL;
    }
    mm_lock(mm);
    if (!mm_range_mapped(mm, addr, addr + len)) { mm_unlock(mm); return -LX_ENOMEM; }
    int r = mm_split(mm, addr);
    if (!r) r = mm_split(mm, addr + len);
    for (uint64_t cur = addr; !r && cur < addr + len;) {
        mm_vma *v = vt_find(&mm->vt, cur);
        if (v->bk && v->bk->kind == MB_FILE) { r = -LX_EBADF; break; } /* PR_SET_VMA_ANON_NAME: anonymous only */
        char *dup = kind ? strdup(name) : NULL;
        if (kind && !dup) { r = -LX_ENOMEM; break; }
        free(v->label);
        v->label = dup;
        vt_account(&mm->vt, v, -1);
        v->lkind = kind;
        vt_account(&mm->vt, v, 1);
        if (kind == LBL_STACK && !mm->start_stack) mm->start_stack = v->end - 1;
        cur = v->end;
    }
    mm_merge_around(mm, addr, addr + len);
    mm_unlock(mm);
    return r;
}

/* ---- /proc/<pid>/maps ---------------------------------------------------------------------------- */

struct sbuf { char *p; size_t n, cap; int oom; };

static void sb_need(struct sbuf *b, size_t more)
{
    if (b->oom || b->n + more + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->n + more + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { b->oom = 1; return; }
    b->p = np;
    b->cap = nc;
}

static void sb_putc(struct sbuf *b, char c)
{
    sb_need(b, 1);
    if (!b->oom) { b->p[b->n++] = c; b->p[b->n] = 0; }
}

static void sb_puts(struct sbuf *b, const char *s) { while (*s) sb_putc(b, *s++); }

/* seq_put_hex_ll: lowercase hex, at least `width` digits. */
static void sb_hex(struct sbuf *b, uint64_t v, int width)
{
    char tmp[17];
    int len = 0;
    do { tmp[len++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    for (int i = len; i < width; i++) sb_putc(b, '0');
    while (len) sb_putc(b, tmp[--len]);
}

static void sb_printf(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(struct sbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_puts(b, tmp);
}

/* imported from HL proc_emulation.c (proc_maps_fd: name column 73, newline escaped as \012), adapted:
 * addresses padded to 8 digits as seq_put_hex_ll does, the header always ends with a space (Linux
 * show_vma_header_prefix), " (deleted)" for unlinked files (d_path). */
#define MAPS_NAME_COLUMN 73
static void render_line(struct sbuf *b, const struct lkd_vma_info *o)
{
    size_t line0 = b->n;
    sb_hex(b, o->start, 8);
    sb_putc(b, '-');
    sb_hex(b, o->end, 8);
    sb_putc(b, ' ');
    sb_putc(b, o->prot & LX_PROT_READ ? 'r' : '-');
    sb_putc(b, o->prot & LX_PROT_WRITE ? 'w' : '-');
    sb_putc(b, o->prot & LX_PROT_EXEC ? 'x' : '-');
    sb_putc(b, (o->flags & LX_MAP_TYPE) == LX_MAP_SHARED ? 's' : 'p');
    sb_putc(b, ' ');
    sb_hex(b, o->file_offset, 8);
    sb_putc(b, ' ');
    sb_hex(b, o->dev_major, 2);
    sb_putc(b, ':');
    sb_hex(b, o->dev_minor, 2);
    sb_printf(b, " %llu ", (unsigned long long)o->ino);
    if (o->name[0]) {
        while (b->n - line0 < MAPS_NAME_COLUMN - 1 && !b->oom) sb_putc(b, ' ');
        sb_putc(b, ' ');
        for (const char *p = o->name; *p; p++) {
            if (*p == '\n') sb_puts(b, "\\012");
            else sb_putc(b, *p);
        }
        if (o->deleted) sb_puts(b, " (deleted)");
    }
    sb_putc(b, '\n');
}

char *lkd_mm_render_maps(lkd_mm *mm, size_t *len_out)
{
    struct sbuf b = { 0 };
    if (len_out) *len_out = 0;
    if (!mm) return NULL;
    sb_need(&b, 0);
    struct lkd_vma_info *o = malloc(sizeof *o);
    if (!o) { free(b.p); return NULL; }
    mrw_rdlock(&mm->vlock);
    for (mm_vma *v = vt_first(&mm->vt); v && !b.oom; v = vt_next(&mm->vt, v)) {
        fill_info(mm, v, o);
        render_line(&b, o);
    }
    mrw_rdunlock(&mm->vlock);
    free(o);
    if (b.oom) { free(b.p); return NULL; }
    if (len_out) *len_out = b.n;
    return b.p;
}

/* smaps: sizes from the VMA table, residency from the host (mincore of the 16 KiB host pages, so a
 * sub-page is reported resident when its host page is). Pss is reported equal to Rss for private
 * pages and is not computed for shared pages (the host gives no share count): such lines say so by
 * reporting Pss = Rss, documented in docs/status/mm.md. */
static void vmflags(struct sbuf *b, const mm_vma *v)
{
    sb_puts(b, "VmFlags:");
    if (v->prot & LX_PROT_READ) sb_puts(b, " rd");
    if (v->prot & LX_PROT_WRITE) sb_puts(b, " wr");
    if (v->prot & LX_PROT_EXEC) sb_puts(b, " ex");
    if (v->vflags & VF_SHARED) sb_puts(b, " sh");
    sb_puts(b, " mr");
    if (v->vflags & VF_MAYWRITE) sb_puts(b, " mw");
    sb_puts(b, " me");
    if (v->vflags & VF_SHARED) sb_puts(b, " ms");
    if (v->vflags & VF_LOCKED) sb_puts(b, " lo");
    if (v->vflags & VF_SEQ) sb_puts(b, " sr");
    if (v->vflags & VF_RANDOM) sb_puts(b, " rr");
    if (v->vflags & VF_DONTFORK) sb_puts(b, " dc");
    if (!(v->vflags & VF_NORESERVE) && !(v->vflags & VF_SHARED) && (v->prot & LX_PROT_WRITE)) sb_puts(b, " ac");
    if (v->vflags & VF_NORESERVE) sb_puts(b, " nr");
    if (v->vflags & VF_HUGEPAGE) sb_puts(b, " hg");
    if (v->vflags & VF_NOHUGEPAGE) sb_puts(b, " nh");
    if (v->vflags & VF_DONTDUMP) sb_puts(b, " dd");
    if (v->vflags & VF_WIPEONFORK) sb_puts(b, " wf");
    if (v->vflags & VF_MERGEABLE) sb_puts(b, " mg");
    sb_putc(b, '\n');
}

/* Large VMAs (Wine reserves terabytes PROT_NONE): residency from the host VM regions, O(regions); the
 * counts of a region that extends past the VMA are prorated by the overlap (documented approximation). */
static uint64_t resident_regions(const mm_vma *v, uint64_t *dirty)
{
    uint64_t res = 0, hs = (uint64_t)(uintptr_t)mm_h(v->start), he = (uint64_t)(uintptr_t)mm_h(v->end);
    mach_vm_address_t a = hs;
    *dirty = 0;
    while (a < he) {
        mach_vm_size_t sz = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &cnt, &obj) !=
                KERN_SUCCESS || a >= he || !sz)
            break;
        uint64_t lo = a > hs ? a : hs, hi = a + sz < he ? a + sz : he;
        if (hi > lo) {
            res += (uint64_t)info.pages_resident * mm_hps * (hi - lo) / sz;
            *dirty += (uint64_t)info.pages_dirtied * mm_hps * (hi - lo) / sz;
        }
        a += sz;
    }
    return res;
}

/* Resident bytes of [s, e) (host page aligned) by mincore. */
static uint64_t resident_mincore(const mm_vma *v, uint64_t s, uint64_t e, uint64_t *dirty)
{
    uint64_t res = 0;
    char vec[256];
    for (uint64_t p = s; p < e; p += sizeof vec * mm_hps) {
        uint64_t n = (e - p) / mm_hps;
        if (n > sizeof vec) n = sizeof vec;
        if (mincore(mm_h(p), n * mm_hps, vec) < 0) break;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t hs = p + i * mm_hps, he = hs + mm_hps;
            uint64_t a = hs > v->start ? hs : v->start, z = he < v->end ? he : v->end;
            if (a >= z) continue;
            if (vec[i] & MINCORE_INCORE) res += z - a;
            if (vec[i] & (MINCORE_MODIFIED | MINCORE_MODIFIED_OTHER)) *dirty += z - a;
        }
    }
    return res;
}

/* Resident (and dirty) bytes of v. A host region that lies wholly inside v is counted by its own resident page
 * count (one mach_vm_region call, exact: MESURE, same totals as mincore); the host pages of v that share a
 * region with something else (edges) are counted by mincore. Huge VMAs (more than 256 MiB) keep the
 * proportional estimate of resident_regions for partial regions. */
static uint64_t resident_bytes(const mm_vma *v, uint64_t *dirty)
{
    *dirty = 0;
    if (v->end - v->start > (256ull << 20)) return resident_regions(v, dirty);
    uint64_t res = 0, s = HP_DOWN(v->start), e = HP_UP(v->end);
    uint64_t hs = (uint64_t)(uintptr_t)mm_h(s), he = (uint64_t)(uintptr_t)mm_h(e);
    mach_vm_address_t a = hs;
    uint64_t done = s;                       /* guest address up to which pages are counted */
    while (done < e) {
        mach_vm_size_t sz = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        a = (mach_vm_address_t)(uintptr_t)mm_h(done);
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &cnt, &obj) !=
                KERN_SUCCESS || !sz || a >= he) {
            res += resident_mincore(v, done, e, dirty);
            break;
        }
        uint64_t ra = lkd_h2g((const void *)(uintptr_t)a), rb = ra + sz;
        if (ra > done) { res += resident_mincore(v, done, ra < e ? ra : e, dirty); done = ra < e ? ra : e; continue; }
        uint64_t end = rb < e ? rb : e;
        if (ra >= v->start && rb <= v->end) {
            res += (uint64_t)info.pages_resident * mm_hps;
            *dirty += (uint64_t)info.pages_dirtied * mm_hps;
        } else {
            res += resident_mincore(v, done, end, dirty);
        }
        done = end;
    }
    return res;
}

char *lkd_mm_render_smaps(lkd_mm *mm, size_t *len_out)
{
    struct sbuf b = { 0 };
    if (len_out) *len_out = 0;
    if (!mm) return NULL;
    sb_need(&b, 0);
    struct lkd_vma_info *o = malloc(sizeof *o);
    if (!o) { free(b.p); return NULL; }
    mm_lock(mm); /* host pages must not change under mincore */
    for (mm_vma *v = vt_first(&mm->vt); v && !b.oom; v = vt_next(&mm->vt, v)) {
        fill_info(mm, v, o);
        render_line(&b, o);
        uint64_t dirty, rss = resident_bytes(v, &dirty);
        uint64_t clean = rss > dirty ? rss - dirty : 0;
        int sh = (v->vflags & VF_SHARED) != 0, anon = !v->bk || v->bk->kind != MB_FILE;
        sb_printf(&b, "Size:           %8llu kB\n", (unsigned long long)((v->end - v->start) >> 10));
        sb_printf(&b, "KernelPageSize: %8d kB\nMMUPageSize:    %8d kB\n", 4, 4);
        sb_printf(&b, "Rss:            %8llu kB\n", (unsigned long long)(rss >> 10));
        sb_printf(&b, "Pss:            %8llu kB\n", (unsigned long long)(rss >> 10));
        sb_printf(&b, "Pss_Dirty:      %8llu kB\n", (unsigned long long)(dirty >> 10));
        sb_printf(&b, "Shared_Clean:   %8llu kB\n", (unsigned long long)((sh ? clean : 0) >> 10));
        sb_printf(&b, "Shared_Dirty:   %8llu kB\n", (unsigned long long)((sh ? dirty : 0) >> 10));
        sb_printf(&b, "Private_Clean:  %8llu kB\n", (unsigned long long)((sh ? 0 : clean) >> 10));
        sb_printf(&b, "Private_Dirty:  %8llu kB\n", (unsigned long long)((sh ? 0 : dirty) >> 10));
        sb_printf(&b, "Referenced:     %8llu kB\n", (unsigned long long)(rss >> 10));
        sb_printf(&b, "Anonymous:      %8llu kB\n", (unsigned long long)((anon && !sh ? rss : 0) >> 10));
        sb_puts(&b, "KSM:                   0 kB\nLazyFree:              0 kB\nAnonHugePages:         0 kB\n"
                    "ShmemPmdMapped:        0 kB\nFilePmdMapped:         0 kB\nShared_Hugetlb:        0 kB\n"
                    "Private_Hugetlb:       0 kB\nSwap:                  0 kB\nSwapPss:               0 kB\n");
        sb_printf(&b, "Locked:         %8llu kB\n", (unsigned long long)((v->vflags & VF_LOCKED ? rss : 0) >> 10));
        sb_puts(&b, "THPeligible:    0\n");
        vmflags(&b, v);
    }
    mm_unlock(mm);
    free(o);
    if (b.oom) { free(b.p); return NULL; }
    if (len_out) *len_out = b.n;
    return b.p;
}

int lkd_mm_stats(lkd_mm *mm, struct lkd_mm_stats *out)
{
    if (!mm || !out) return -LX_EINVAL;
    memset(out, 0, sizeof *out);
    mm_lock(mm);
    out->total_vm = mm->vt.total;
    out->nmaps = mm->vt.n;
    out->locked_vm = mm->locked_bytes;
    out->stack_vm = mm->vt.stack;
    out->exec_vm = mm->vt.exec;
    out->data_vm = mm->vt.data;
    /* is_stack_mapping also takes the VMA holding start_stack or ending there (show_map_vma names it [stack]):
     * out of the class the tree counts it in */
    uint64_t ss = mm->start_stack;
    mm_vma *at[2] = { ss ? vt_find(&mm->vt, ss) : NULL, ss ? vt_find(&mm->vt, ss - 1) : NULL };
    if (at[1] && (at[1] == at[0] || at[1]->end != ss)) at[1] = NULL;
    for (int i = 0; i < 2; i++) {
        if (!at[i]) continue;
        vtree one = { 0 };
        vt_account(&one, at[i], 1);
        out->stack_vm += one.total - one.stack;
        out->exec_vm -= one.exec;
        out->data_vm -= one.data;
    }
    int64_t now = lkd_host_mono_ns();
    if (!mm->rss_ns || mm->rss_gen != mm->map_gen || now - mm->rss_ns >= MM_RSS_TTL_NS) {
        host_rss(&mm->rss_anon, &mm->rss_file);
        mm->rss_shmem = 0;
        mm->rss_ns = now;
        mm->rss_gen = mm->map_gen;
    }
    out->rss_anon = mm->rss_anon; out->rss_file = mm->rss_file; out->rss_shmem = mm->rss_shmem;
    uint64_t rss = out->rss_anon + out->rss_file + out->rss_shmem;
    if (out->total_vm > mm->hiwater_vm) mm->hiwater_vm = out->total_vm;
    if (rss > mm->hiwater_rss) mm->hiwater_rss = rss;
    out->hiwater_vm = mm->hiwater_vm;
    out->hiwater_rss = mm->hiwater_rss;
    mm_unlock(mm);
    out->pte_bytes = 0; /* no guest page tables exist; reported as 0 */
    return 0;
}

/* ---- Futex keys -------------------------------------------------------------------------------- */

int lkd_mm_futex_key(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key)
{
    if (gva & 3) return -LX_EINVAL;
    lkd_mm *mm = mm_of(t);
    if (!mm || private_hint) {
        key->kind = LKD_FK_PRIVATE;
        key->a = mm ? mm->identity : (t && t->proc ? (uint64_t)t->proc->pid : 0);
        key->b = gva;
        return 0;
    }
    int r = 0;
    mrw_rdlock(&mm->vlock);
    mm_vma *v = vt_find(&mm->vt, gva);
    if (!v) r = -LX_EFAULT;
    else if ((v->vflags & VF_SHARED) && v->bk) {
        uint64_t off = v->pgoff + (gva - v->start);
        key->kind = LKD_FK_SHARED;
        switch (v->bk->kind) {
        case MB_SHANON: key->a = (1ull << 62) | v->bk->obj_id; key->b = off; break;
        case MB_SYSV: key->a = (3ull << 62) | (uint64_t)(uint32_t)v->bk->shm_id; key->b = off; break;
        default: /* file: (device, inode) hashed into a, page offset in b; equal in every process */
            key->a = (2ull << 62) | ((v->bk->ino * 0x9e3779b97f4a7c15ull ^ ((uint64_t)v->bk->dev_major << 20 | v->bk->dev_minor))
                                     & ((1ull << 62) - 1));
            key->b = off;
            break;
        }
    } else {
        key->kind = LKD_FK_PRIVATE;
        key->a = mm->identity;
        key->b = gva;
    }
    mrw_rdunlock(&mm->vlock);
    return r;
}

/* ---- Module hooks ------------------------------------------------------------------------------ */

void mm_sys_register(void);
void mm_shm_register(void);
void mm_shm_init(void);
void mm_remote_register(void);

void lkd_mm_init(void)
{
    const lkd_config *c = lkd_config_get();
    if (c->guest_window_base && !mm_win.reserved) {
        int e = mm_window_reserve(c->guest_window_base, c->guest_window_bytes, 0);
        if (e < 0) lkd_log("mm: cannot reserve the guest window of %llu bytes: %d",
                           (unsigned long long)c->guest_window_bytes, e);
    }
    mm_shm_init();
    mm_sys_register();
    mm_shm_register();
    mm_remote_register();
}

void lkd_mm_fork_prepare(lkd_task *t, uint64_t clone_flags)
{
    (void)clone_flags;
    lkd_mm *mm = mm_of(t);
    if (!mm) return;
    mm_lock(mm);
    mm_watch_lock();
    mrw_wrlock(&mm->vlock);
}

void lkd_mm_fork_parent(lkd_task *t, pid_t host_child)
{
    (void)host_child;
    lkd_mm *mm = mm_of(t);
    if (!mm) return;
    mrw_wrunlock(&mm->vlock);
    mm_watch_unlock();
    mm_unlock(mm);
}

/* Child of a host fork (kernel/fork.c dup_mmap): private memory is a copy-on-write copy, shared mappings
 * stay shared. VM_DONTCOPY VMAs are absent in the child, VM_WIPEONFORK ones are empty, memory locks and
 * mlockall(MCL_FUTURE) are not inherited. */
void lkd_mm_fork_child(lkd_task *t)
{
    lkd_mm *mm = mm_of(t);
    if (!mm) return;
    /* First thing in the child, before anything can take them: the three locks taken by fork_prepare are
     * rebuilt, never unlocked. A thread of the parent that was waiting for one of them at fork time (a
     * JIT thread in lkd_mm_code_write_fault waiting for the VMA lock) leaves kernel wait state in the
     * lock word, and the child's unlock or next lock then blocks for ever (MESURE: w18_forkjit, child in
     * mm_wlock). lkd_fork_reinit_mutex zeroes the memory first (pthread_mutex_init on a held lock is EBUSY);
     * the VMA lock is one word: its waiters' counts go with their threads. */
    mm_watch_fork_child();
    atomic_store(&mm->vlock.w, 0);
    lkd_fork_reinit_mutex(&mm->lock, 0);
    atomic_store(&mm->refs, 1);
    mm->identity = new_identity();
    mm->def_flags = 0;
    mm->locked_bytes = 0;
    mm_win.owner = mm;
    struct mm_inval iv = { 0 };
    mm_lock(mm);
    mm_shm_fork_child(mm);
    for (mm_vma *v = vt_first(&mm->vt); v;) {
        mm_vma *n = vt_next(&mm->vt, v);
        v->vflags &= ~(VF_LOCKED | VF_LOCKONFAULT);
        if (v->vflags & VF_DONTFORK) {
            uint64_t s = v->start, e = v->end;
            mm_unmap_locked(mm, s, e, &iv);
            n = vt_first_end_after(&mm->vt, e);
        } else if ((v->vflags & VF_WIPEONFORK) && !v->bk) {
            mm_watch_lock();
            mm_discard_range(mm, v, v->start, v->end);
            mm_host_refresh(mm, v->start, v->end);
            mm_watch_unlock();
        }
        v = n;
    }
    mm_merge_around(mm, mm_win.g_lo, mm_win.g_hi);
    /* the child inherits no translations: no watch, every page gets its VMA protection back */
    mm_watch_clear_all();
    for (mm_vma *v = vt_first(&mm->vt); v; v = vt_next(&mm->vt, v)) mm_host_refresh(mm, v->start, v->end);
    mm_unlock(mm);
    iv.n = 0; /* the child has no translated code of its own yet */
    mm_remote_fork_child();
}

void lkd_mm_exec_commit(lkd_task *t) { lkd_mm_reset_for_exec(mm_of(t)); }

/* The thread drops its reference to the address space (CLONE_VM threads share it). */
void lkd_mm_task_exit(lkd_task *t)
{
    if (t && t->mm) {
        lkd_mm *mm = t->mm;
        t->mm = NULL;
        lkd_mm_put(mm);
    }
}

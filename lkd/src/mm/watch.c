/* watch.c: host pages that hold translated guest code (FEX "mtrack" self-modifying-code detection).
 * Owner: mm.
 *
 * The engine calls lkd_mm_watch_code() for every range it translated. From then on the host page (16 KiB)
 * of that range has no PROT_WRITE, even when the guest VMA is writable. The first write faults; the
 * engine's SIGSEGV/SIGBUS handler (and LKD's own copies) call lkd_mm_code_write_fault(), which drops
 * the translations of the host page through ops->invalidate_code, clears the watch and makes the page
 * writable again, so the faulting store is retried.
 *
 * State: one bit per host page of the window, in an anonymous MAP_NORESERVE bitmap (512 MiB of address
 * space for a 2^46 window, only touched words use memory), read and written with atomic operations.
 *
 * Callers of lkd_mm_code_write_fault pass store faults (ESR data abort with WnR set, any fault status):
 * a page that is already writable then means another thread restored it first, and the retry succeeds.
 *
 * Lock: the watch lock (recursive mutex, rank between LKD_RANK_MM and LKD_RANK_VMA, taken before the VMA
 * read lock) serializes every change of a host page protection with the watch bits: mm_host_refresh,
 * the temporary write windows of mm itself, lkd_mm_watch_code and lkd_mm_code_write_fault. It is never
 * held while guest code runs, while ops->invalidate_code runs, or during an lkd_user_* copy, so a guest
 * thread that faults (holding no LKD lock) can take it from its signal handler. pthread mutexes are not
 * async-signal-safe in general; here the interrupted code is guest code or an LKD copy routine, never
 * the mutex implementation itself, which is the case that matters (documented in docs/status/mm.md).
 *
 * Limit: the watch is per process. A write by another process into a MAP_SHARED or SysV page that this
 * process translated is not seen here (no fault in this process).
 */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <time.h>

#include "mm_priv.h"
#include "lkd_mm_watch.h"

static _Atomic uint64_t *wbits;
static uint64_t wbits_bytes;
static pthread_mutex_t wlk;
static int wlk_ready;

static void wlk_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&wlk, &a);
    pthread_mutexattr_destroy(&a);
    wlk_ready = 1;
}

/* Fork child: the recursive watch lock taken by fork_prepare cannot be unlocked by the child's thread
 * (ownership check, MESURE: test_mm_fork hung), so it is rebuilt from scratch. */
void mm_watch_fork_child(void)
{
    lkd_fork_reinit_mutex(&wlk, 1);
    wlk_ready = 1;
}

void mm_watch_lock(void) { if (wlk_ready) pthread_mutex_lock(&wlk); }
void mm_watch_unlock(void) { if (wlk_ready) pthread_mutex_unlock(&wlk); }

static void *bitmap_map(void *at)
{
    return mmap(at, wbits_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | (at ? MAP_FIXED : 0), -1, 0);
}

/* Called once the window exists (window.c). */
int mm_watch_init(void)
{
    if (!wlk_ready) wlk_init();
    if (wbits) return 0;
    uint64_t pages = mm_win.size / mm_hps;
    wbits_bytes = HP_UP(((pages + 63) / 64) * 8);
    void *p = bitmap_map(NULL);
    if (p == MAP_FAILED) return (int)LKD_HOST_ERR();
    wbits = p;
    return 0;
}

static int in_window(uint64_t g) { return mm_win.reserved && g >= mm_win.g_lo && g < mm_win.g_hi; }
static uint64_t idx(uint64_t g) { return (g - mm_win.g_lo) / mm_hps; }

int mm_watched(uint64_t p)
{
    if (!wbits || !in_window(p)) return 0;
    uint64_t i = idx(p);
    return (atomic_load_explicit(&wbits[i >> 6], memory_order_acquire) >> (i & 63)) & 1;
}

static int set_bit(uint64_t p)
{
    uint64_t i = idx(p), m = 1ull << (i & 63);
    return (atomic_fetch_or(&wbits[i >> 6], m) & m) != 0;
}

int mm_unwatch_bit(uint64_t p)
{
    if (!wbits || !in_window(p)) return 0;
    uint64_t i = idx(p), m = 1ull << (i & 63);
    return (atomic_fetch_and(&wbits[i >> 6], ~m) & m) != 0;
}

/* Clear the watch of host pages [p, q), any size: whole bitmap pages (16 KiB of bitmap = 2 GiB of guest
 * space) are replaced by fresh zero memory, the edges are cleared word by word without touching words
 * that are already zero (an untouched bitmap page stays unallocated). */
void mm_unwatch_range(uint64_t p, uint64_t q)
{
    if (!wbits || p >= q) return;
    if (p < mm_win.g_lo) p = mm_win.g_lo;
    if (q > mm_win.g_hi) q = mm_win.g_hi;
    if (p >= q) return;
    uint64_t i = idx(p), n = (q - mm_win.g_lo + mm_hps - 1) / mm_hps;
    const uint64_t bpp = mm_hps * 8; /* bits per bitmap page */
    while (i < n) {
        if ((i & (bpp - 1)) == 0 && n - i >= bpp) {
            uint64_t k = (n - i) / bpp * bpp;
            mmap((char *)wbits + i / 8, k / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
            i += k;
            continue;
        }
        if ((i & 63) == 0 && n - i >= 64) {
            if (atomic_load(&wbits[i >> 6])) atomic_store(&wbits[i >> 6], 0);
            i += 64;
            continue;
        }
        uint64_t m = 1ull << (i & 63);
        if (atomic_load(&wbits[i >> 6]) & m) atomic_fetch_and(&wbits[i >> 6], ~m);
        i++;
    }
}

/* First watched host page in [p, q), or q. Zero words are skipped 64 pages at a time. */
uint64_t mm_next_watched(uint64_t p, uint64_t q)
{
    if (!wbits || p >= q) return q;
    uint64_t i = idx(p), n = (q - mm_win.g_lo + mm_hps - 1) / mm_hps;
    while (i < n) {
        uint64_t w = atomic_load_explicit(&wbits[i >> 6], memory_order_acquire) >> (i & 63);
        if (!w) { i = (i | 63) + 1; continue; }
        i += (uint64_t)__builtin_ctzll(w);
        break;
    }
    if (i >= n) return q;
    uint64_t r = mm_win.g_lo + i * mm_hps;
    return r < q ? r : q;
}

/* Drop every watch (fork child, exec): a fresh zero bitmap at the same address. */
void mm_watch_clear_all(void)
{
    if (wbits) bitmap_map((void *)wbits);
}

static lkd_mm *cur_mm(void)
{
    lkd_task *t = lkd_current();
    return t && t->mm ? t->mm : mm_win.owner;
}

/* Host protection the page should have now (I2, or PROT_NONE for a guarded page, minus PROT_WRITE when watched).
 * Watch lock held. */
static int wanted_prot(lkd_mm *mm, uint64_t p)
{
    int h;
    mrw_rdlock(&mm->vlock);
    h = mm_page_guarded(mm, p) ? PROT_NONE : mm_page_union(mm, p);
    mrw_rdunlock(&mm->vlock);
    if (mm_watched(p)) h &= ~PROT_WRITE;
    return h;
}

/* ---- Aliases (request wine-7) -------------------------------------------------------------------------
 * A shared object (memfd, file, SysV segment) can be mapped twice in one process: written through a RW view
 * and executed through an RX view. Translations are made from the RX view's addresses; a store through the
 * RW view must drop them. So a watch covers every host page of this process that maps the same object
 * bytes: they are all write-protected together, and a write fault on any of them unwatches all of them and
 * invalidates each one's range. Gap: views in OTHER processes of the family are not covered (as before for
 * MAP_SHARED). */

/* Object offset mapped at host page p by the shared VMA v (congruent by construction). */
static uint64_t host_off_of(const mm_vma *v, uint64_t p) { return v->pgoff + v->bk->hbias + p - v->start; }

int mm_alias_pages(lkd_mm *mm, uint64_t p, uint64_t *out, int cap)
{
    int n = 0;
    mrw_rdlock(&mm->vlock);
    mm_vma *v = vt_first_end_after(&mm->vt, p);
    while (v && v->start < p + mm_hps && !(v->vflags & VF_HOSTSHARED)) v = vt_next(&mm->vt, v);
    if (!v || v->start >= p + mm_hps || !v->bk) {
        mrw_rdunlock(&mm->vlock);
        if (cap > 0) out[n++] = p;
        return n;
    }
    uint64_t ho = host_off_of(v, p);
    for (mm_vma *w = vt_first(&mm->vt); w && n < cap; w = vt_next(&mm->vt, w)) {
        if (!(w->vflags & VF_HOSTSHARED) || !mm_same_object(w->bk, v->bk)) continue;
        uint64_t q = w->start - (w->pgoff + w->bk->hbias) + ho;   /* where ho would be in w (may be outside) */
        if (q & (mm_hps - 1)) continue;
        if (q < HP_DOWN(w->start) || q >= HP_UP(w->end)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (out[i] == q) dup = 1;
        if (!dup) out[n++] = q;
    }
    mrw_rdunlock(&mm->vlock);
    if (n == 0 && cap > 0) out[n++] = p;
    return n;
}

#define MAX_ALIASES 16

/* Watch lock held. */
static void watch_page(lkd_mm *mm, uint64_t p)
{
    if (set_bit(p)) return;
    mm_host_protect(p, mm_hps, wanted_prot(mm, p));
}

/* Watch lock held. */
static void alias_sync_page(lkd_mm *mm, uint64_t p)
{
    uint64_t al[MAX_ALIASES];
    int n = mm_alias_pages(mm, p, al, MAX_ALIASES), any = 0;
    for (int i = 0; i < n; i++) if (al[i] != p && mm_watched(al[i])) any = 1;
    if (any) watch_page(mm, p);
}

#define MAX_VIEWS 32

/* A new shared VMA [s, e): its host pages whose bytes another view of the object holds as translated code
 * start watched. The interior host pages belong to the new VMA alone, so another view of the same object
 * holds their bytes at one constant shift, or does not hold them: one walk of the VMAs, then a scan of the
 * watch bits of each other view (64 pages per word). The two edge pages may be shared with neighbouring
 * VMAs and keep the page-by-page lookup. The old loop walked every VMA for every host page: a 4 GiB view
 * cost 13.6 s in a process with 2,000 mappings (witness w32), with the watch lock held all along, which
 * stalled every thread that translated code meanwhile. */
void mm_watch_alias_sync(lkd_mm *mm, uint64_t s, uint64_t e)
{
    if (!wbits) return;
    uint64_t is = HP_UP(s), ie = HP_DOWN(e);
    mm_watch_lock();
    if (is >= ie) {
        for (uint64_t p = HP_DOWN(s); p < HP_UP(e); p += mm_hps) alias_sync_page(mm, p);
        mm_watch_unlock();
        return;
    }
    if (s < is) alias_sync_page(mm, HP_DOWN(s));
    if (ie < e) alias_sync_page(mm, ie);
    struct { uint64_t lo, hi; int64_t d; } sh[MAX_VIEWS];
    int n = 0, slow = 0;
    mrw_rdlock(&mm->vlock);
    mm_vma *nv = vt_first_end_after(&mm->vt, is);
    if (!nv || nv->start > is || nv->end < ie || !(nv->vflags & VF_HOSTSHARED) || !nv->bk) {
        slow = 1;
    } else {
        int64_t base = (int64_t)(nv->start - (nv->pgoff + nv->bk->hbias));   /* address of host offset 0 */
        for (mm_vma *w = vt_first(&mm->vt); w && !slow; w = vt_next(&mm->vt, w)) {
            if (w == nv || !(w->vflags & VF_HOSTSHARED) || !mm_same_object(w->bk, nv->bk)) continue;
            int64_t d = (int64_t)(w->start - (w->pgoff + w->bk->hbias)) - base;   /* page p of nv is q = p + d in w */
            if (!d || (d & (int64_t)(mm_hps - 1))) continue;
            int64_t lo = (int64_t)HP_DOWN(w->start) - d, hi = (int64_t)HP_UP(w->end) - d;
            if (lo < (int64_t)is) lo = (int64_t)is;
            if (hi > (int64_t)ie) hi = (int64_t)ie;
            if (lo >= hi) continue;
            if (n == MAX_VIEWS) { slow = 1; break; }
            sh[n].lo = (uint64_t)lo; sh[n].hi = (uint64_t)hi; sh[n].d = d;
            n++;
        }
    }
    mrw_rdunlock(&mm->vlock);
    if (slow) {
        for (uint64_t p = is; p < ie; p += mm_hps) alias_sync_page(mm, p);
    } else {
        for (int i = 0; i < n; i++) {
            uint64_t qe = sh[i].hi + (uint64_t)sh[i].d;
            for (uint64_t q = mm_next_watched(sh[i].lo + (uint64_t)sh[i].d, qe); q < qe; q = mm_next_watched(q + mm_hps, qe))
                watch_page(mm, q - (uint64_t)sh[i].d);
        }
    }
    mm_watch_unlock();
}

int lkd_mm_watch_code(uint64_t gva, uint64_t len)
{
    lkd_mm *mm = cur_mm();
    if (!mm || !wbits) return -LX_ESRCH;
    if (!len) return 0;
    uint64_t end = gva + len;
    if (end < gva || !in_window(gva) || end > mm_win.g_hi) return -LX_EFAULT;
    uint64_t p0 = HP_DOWN(gva), pe = HP_UP(end);
    int all = 1;
    for (uint64_t p = p0; p < pe && all; p += mm_hps) all = mm_watched(p);
    if (all) return 0; /* idempotent, lock-free */
    mm_watch_lock();
    for (uint64_t p = p0; p < pe; p += mm_hps) {
        if (mm_watched(p)) continue;
        uint64_t al[MAX_ALIASES];
        int n = mm_alias_pages(mm, p, al, MAX_ALIASES);   /* p itself, and every view of the same bytes */
        for (int i = 0; i < n; i++) watch_page(mm, al[i]);
    }
    mm_watch_unlock();
    return 0;
}

/* Current host protection of the page, -1 if unknown. */
static int host_prot_now(uint64_t p)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)mm_h(p);
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) !=
            KERN_SUCCESS || a > (mach_vm_address_t)(uintptr_t)mm_h(p))
        return -1;
    return info.protection & (VM_PROT_READ | VM_PROT_WRITE);
}

/* Unwatch the host page of gva: invalidate its translations (watch lock NOT held), then restore the
 * protection. need_vma_write: the guest VMA at gva must allow the write (a guest store); 0 for FOLL_FORCE
 * writes of LKD itself. Returns 0 when the write is not allowed, 2 when this call found the page watched
 * or changed its protection (the fault was a watch fault: retry), 1 when it found the page already
 * writable and unwatched and did nothing (another thread restored it first, or a fault the watch does
 * not explain). */
int mm_unwatch_for_write(lkd_mm *mm, uint64_t gva, int need_vma_write, struct mm_inval *iv)
{
    uint64_t p = HP_DOWN(gva);
    if (need_vma_write) {
        mrw_rdlock(&mm->vlock);
        mm_vma *v = vt_find(&mm->vt, gva);
        int ok = v && (v->prot & LX_PROT_WRITE);
        mrw_rdunlock(&mm->vlock);
        if (!ok) return 0;
    }
    uint64_t al[MAX_ALIASES];
    int na = mm_alias_pages(mm, p, al, MAX_ALIASES), dropped[MAX_ALIASES] = { 0 };
    mm_watch_lock();
    int was = mm_unwatch_bit(p);
    for (int i = 0; i < na; i++) {
        if (al[i] == p) continue;
        dropped[i] = mm_unwatch_bit(al[i]);
        if (dropped[i]) mm_host_protect(al[i], mm_hps, wanted_prot(mm, al[i]));   /* the other views get their rights back */
    }
    mm_watch_unlock();
    /* a caller that holds the mmap lock collects the ranges and invalidates after unlocking: the engine takes
     * its own locks in invalidate_code, which its fork_prepare holds before LKD's (CONVENTIONS 3) */
    for (int i = 0; i < na; i++) {
        uint64_t q = al[i];
        if (!(q == p ? was : dropped[i])) continue;
        if (iv) mm_inval_add(iv, q, q + mm_hps);
        else mm_invalidate(q, mm_hps);
    }
    if (!was) for (int i = 0; i < na; i++) if (dropped[i]) was = 1;   /* the write hit a view of watched code */
    if (!need_vma_write) return was; /* FOLL_FORCE writer: it opens the page itself (mm_temp_access) */
    mm_watch_lock();
    if (mm_watched(p)) { mm_watch_unlock(); return 2; } /* translated again meanwhile: the retry faults again */
    int want = wanted_prot(mm, p);
    int ret = 0;
    if (!(want & PROT_WRITE)) {
        ret = 0;                                        /* the write is not allowed after all */
    } else if (!was && host_prot_now(p) > 0 && (host_prot_now(p) & PROT_WRITE)) {
        ret = 1;                                        /* another thread already restored the page */
    } else {
        ret = mm_host_protect(p, mm_hps, want) == 0 ? 2 : 0; /* was watched, or a stale protection */
    }
    mm_watch_unlock();
    return ret;
}

/* Retry bound: a store that keeps faulting on a page that is no longer watched (a file mapped past its
 * end, a stale protection) must become a real guest fault, not an endless retry loop. Per thread, the
 * count of consecutive answers "already writable, nothing to do" for the same host page; any answer where
 * the page was found watched (at the fault or while handling it) or had its protection changed resets it.
 * Such a fault always belongs to the watch: a Linux store to a writable VMA never fails because of it.
 * The old bound (4 consecutive retries, watched state sampled only before handling) refused legitimate
 * stores when other threads kept translating code of the same host page: nptunix.so has its .plt and its
 * .data/.bss (a pthread mutex) in one 16 KiB host page; each PLT call re-watched the page, a store to the
 * mutex got SIGSEGV, Wine aborted the unix call with the mutex held and the next present deadlocked
 * (Rocket League runs 20261003T014348 to 022545, witness w23_codedatapage). */
#define MAX_RETRIES 1000
static _Thread_local uint64_t retry_page = ~0ull;
static _Thread_local int retry_count;

int lkd_mm_code_write_fault(uint64_t gva)
{
    lkd_mm *mm = mm_win.owner;
    if (!mm || !wbits || !in_window(gva)) return 0;
    uint64_t p = HP_DOWN(gva);
    int was_watched = mm_watched(p);
    int r = mm_unwatch_for_write(mm, gva, 1, NULL);
    if (!r) { retry_page = ~0ull; return 0; }
    if (was_watched || r == 2 || p != retry_page) { retry_page = p; retry_count = 1; return 1; }
    if (++retry_count > MAX_RETRIES) { retry_page = ~0ull; return 0; }
    return 1;
}

int lkd_mm_code_watched(uint64_t gva) { return mm_watched(HP_DOWN(gva)); }

/* lkd_mm_file_written (mm.h): fs wrote file bytes [off, off + len) through a descriptor. Every shared view of
 * them in this process that holds translated code loses its translations (Linux: the page cache is the
 * memory the code runs from). */
void lkd_mm_file_written(struct lkd_fdobj *file, uint64_t off, uint64_t len)
{
    lkd_mm *mm = cur_mm();
    if (!mm || !wbits || !file || !len) return;
    uint64_t pages[64];
    int n = 0;
    mrw_rdlock(&mm->vlock);
    for (mm_vma *v = vt_first(&mm->vt); v && n < 64; v = vt_next(&mm->vt, v)) {
        if (!(v->vflags & VF_HOSTSHARED) || !v->bk || v->bk->kind != MB_FILE || !v->bk->file) continue;
        lkd_fdobj *f = v->bk->file;
        if (f != file && !(f->ino == file->ino && f->dev == file->dev)) continue;
        uint64_t vs = v->pgoff, ve = v->pgoff + (v->end - v->start);
        uint64_t a = off > vs ? off : vs, b = off + len < ve ? off + len : ve;
        if (a >= b) continue;
        for (uint64_t g = HP_DOWN(v->start + (a - vs)); g < v->start + (b - vs) && n < 64; g += mm_hps)
            if (mm_watched(g)) pages[n++] = g;
    }
    mrw_rdunlock(&mm->vlock);
    for (int i = 0; i < n; i++) mm_unwatch_for_write(mm, pages[i], 0, NULL);
    for (int i = 0; i < n; i++) {
        mm_watch_lock();
        mm_host_protect(pages[i], mm_hps, wanted_prot(mm, pages[i]));
        mm_watch_unlock();
    }
}


/* ---- Guarded host pages (mm.c, mm_page_guarded) ---------------------------------------------------------
 * The host protection of a guarded page is PROT_NONE, so the guest's accesses fault and are decided here per 4 KiB
 * (lkd_mm_watch.h has the return values). A page opened for a store stays open until something sets its host
 * protection again (a protection change, a translation of its code, another thread's close): a packer writes the
 * decrypted bytes of a page it has just made writable, one store at a time, and stepping each store would be too slow.
 * Meanwhile its PROT_NONE neighbours are open too. A page opened for a read is closed again by the caller after one
 * instruction (lkd_mm_guard_close) or, for the LKD and the translator, by lkd_mm_guard_close_all at the end of the
 * operation: the PROT_NONE neighbours must not be readable meanwhile, or the guest reads the encrypted bytes.
 * The decision looks at the 4 KiB of the faulting address only: an access that runs from an allowed 4 KiB into a
 * forbidden one of the same host page completes once the page is open, where x86 faults at the boundary. */

#define GUARD_OPEN_MAX 16
static _Thread_local uint64_t guard_open[GUARD_OPEN_MAX];
static _Thread_local int guard_open_n;
static _Thread_local uint64_t guard_last_page = ~0ull;
static _Thread_local int guard_last_count;

static void guard_forget(uint64_t p)
{
    for (int i = 0; i < guard_open_n; i++)
        if (guard_open[i] == p) { guard_open[i] = guard_open[--guard_open_n]; return; }
}

/* A full list closes its oldest page first: an operation still using it faults and opens it again. */
static void guard_note_open(uint64_t p)
{
    for (int i = 0; i < guard_open_n; i++) if (guard_open[i] == p) return;
    if (guard_open_n == GUARD_OPEN_MAX) lkd_mm_guard_close(guard_open[0]);
    guard_open[guard_open_n++] = p;
}

/* A page that this thread reads again and again (the PE header of a module, next to its encrypted first pages) costs a
 * fault and a single-stepped instruction per read. After GUARD_HOT_READS reads in GUARD_HOT_NS it stays open for
 * GUARD_BURST_NS: its PROT_NONE neighbours are readable meanwhile, which the closing timer (any later guard fault, end
 * of a system call or of a compilation of this thread) bounds. */
#define GUARD_HOT_READS 6
#define GUARD_HOT_NS 20000000ull
#define GUARD_BURST_NS 4000000ull
#define GUARD_BURST_MAX 8
#define GUARD_HIST 8
static _Thread_local struct { uint64_t page, t0; unsigned n; } guard_hist[GUARD_HIST];
static _Thread_local unsigned guard_hist_next;
static _Thread_local struct { uint64_t page, expiry; } guard_burst[GUARD_BURST_MAX];
static _Thread_local int guard_burst_n;

static uint64_t guard_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void guard_expire(uint64_t now)
{
    for (int i = 0; i < guard_burst_n;) {
        if (guard_burst[i].expiry > now) { i++; continue; }
        uint64_t p = guard_burst[i].page;
        guard_burst[i] = guard_burst[--guard_burst_n];
        lkd_mm_guard_close(p);
    }
}

static int guard_hot(uint64_t p)
{
    uint64_t now = guard_now();
    int h = -1;
    for (int i = 0; i < GUARD_HIST; i++) if (guard_hist[i].page == p) { h = i; break; }
    if (h < 0) { h = (int)(guard_hist_next++ % GUARD_HIST); guard_hist[h].page = p; guard_hist[h].t0 = now; guard_hist[h].n = 0; }
    if (now - guard_hist[h].t0 >= GUARD_HOT_NS) { guard_hist[h].t0 = now; guard_hist[h].n = 0; }
    if (++guard_hist[h].n < GUARD_HOT_READS) return 0;
    for (int i = 0; i < guard_burst_n; i++) if (guard_burst[i].page == p) { guard_burst[i].expiry = now + GUARD_BURST_NS; return 1; }
    if (guard_burst_n == GUARD_BURST_MAX) return 0;
    guard_burst[guard_burst_n].page = p;
    guard_burst[guard_burst_n++].expiry = now + GUARD_BURST_NS;
    return 1;
}

int lkd_mm_guard_fault(uint64_t gva, int write, uint64_t *page_out)
{
    lkd_mm *mm = cur_mm();
    if (!mm || !in_window(gva)) return 0;
    uint64_t p = HP_DOWN(gva);
    if (guard_burst_n) guard_expire(guard_now());
    mrw_rdlock(&mm->vlock);
    int guarded = mm_page_guarded(mm, p);
    mm_vma *v = guarded ? vt_find(&mm->vt, gva) : NULL;
    int allowed = v && (write ? (v->prot & LX_PROT_WRITE) : (v->prot & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)));
    mrw_rdunlock(&mm->vlock);
    if (!guarded) return 0;
    if (!allowed) return -1;
    /* the same access failing again and again although the page is already open: stop and make it a guest fault */
    int hprot = host_prot_now(p);
    if (hprot >= 0 && (hprot & (write ? PROT_WRITE : PROT_READ))) {
        if (p == guard_last_page) { if (++guard_last_count > 64) { guard_last_page = ~0ull; return -1; } }
        else { guard_last_page = p; guard_last_count = 1; }
    } else guard_last_page = ~0ull;
    if (write && mm_watched(p)) mm_unwatch_for_write(mm, gva, 0, NULL); /* translated code: drop it before the store */
    mm_watch_lock();
    int prot;
    mrw_rdlock(&mm->vlock);
    prot = mm_page_union(mm, p);
    mrw_rdunlock(&mm->vlock);
    if (mm_watched(p)) prot &= ~PROT_WRITE;
    mm_host_protect(p, mm_hps, prot);
    mm_watch_unlock();
    if (page_out) *page_out = p;
    if (write) return 1;
    if (guard_hot(p)) return 2; /* open for a while: nothing to close after the access */
    guard_note_open(p);
    return 1;
}

void lkd_mm_guard_close(uint64_t page)
{
    lkd_mm *mm = cur_mm();
    guard_last_page = ~0ull;
    guard_forget(page);
    if (!mm) return;
    mm_watch_lock();
    mm_host_protect(page, mm_hps, wanted_prot(mm, page));
    mm_watch_unlock();
}

void lkd_mm_guard_close_all(void)
{
    while (guard_open_n > 0) lkd_mm_guard_close(guard_open[--guard_open_n]);
    if (guard_burst_n) guard_expire(guard_now());
}

/* LKD copies of [gva, gva+len): open the guarded pages for the duration of the syscall. */
void mm_guard_open_range(lkd_mm *mm, uint64_t gva, uint64_t len, int write)
{
    if (!atomic_load_explicit(&mm_any_guarded, memory_order_relaxed) || !len) return;
    for (uint64_t p = HP_DOWN(gva); p < gva + len; p += mm_hps) {
        uint64_t a = p > gva ? p : gva;
        mrw_rdlock(&mm->vlock);
        int guarded = mm_page_guarded(mm, p);
        mrw_rdunlock(&mm->vlock);
        if (guarded) lkd_mm_guard_fault(a, write, NULL);
    }
}

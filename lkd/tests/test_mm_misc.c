/* test_mm_misc.c: brk, mremap, madvise, mincore, msync, mlock family, pkeys and the exact
 * /proc/<pid>/maps format. Linux references: brk(2), mremap(2), madvise(2), mincore(2), msync(2),
 * mlock(2), pkeys(7), proc_pid_maps(5); sources mm/mmap.c, mm/mremap.c, mm/madvise.c, mm/mincore.c,
 * mm/msync.c, mm/mlock.c and fs/proc/task_mmu.c show_map_vma of v6.12. */
#include <sys/mman.h>

#include "ltest.h"
#include "fake_engine.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

static lkd_task *T;
static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define MMAP(a, l, p, f) sc(LKD_NR_mmap, (a), (l), (p), (f), (uint64_t)-1, 0)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)
#define BRK(x) sc(LKD_NR_brk, (x), 0, 0, 0, 0, 0)
#define MREMAP(a, o, n, f, na) sc(LKD_NR_mremap, (a), (o), (n), (f), (na), 0)
#define MADV(a, l, b) sc(LKD_NR_madvise, (a), (l), (uint64_t)(b), 0, 0, 0)

static void up(void)
{
    if (!T) { fake_engine_init(); T = fake_make_task(); }
    T->mm = lkd_mm_create();
    fake_engine_reset();
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }
static uint64_t LO(void) { return lkd_mm_window_guest_lo(); }

LT_TEST(brk_semantics)
{
    up();
    /* no heap configured yet: brk reports 0, like a failed brk glibc then avoids */
    LT_EQ(BRK(0), 0);
    uint64_t base = LO() + 0x10001000;
    lkd_mm_set_brk_start(T->mm, base);
    LT_EQ(BRK(0), base);                    /* brk(2): returns the current break on failure/query */
    LT_EQ(BRK(base + 0x1800), base + 0x1800);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, base, 0, &vi), 0);
    LT_EQ(vi.start, base);
    LT_EQ(vi.end, base + 0x2000);           /* PAGE_ALIGN(brk) */
    LT_STREQ(vi.name, "[heap]");
    LT_EQ(vi.prot, RW);
    LT_EQ(H(base + 0x1fff)[0], 0);
    H(base + 0x10)[0] = 9;
    LT_EQ(BRK(base + 0x5000), base + 0x5000);  /* growth extends the same VMA */
    LT_EQ(T->mm->vt.n, 1);
    LT_EQ(H(base + 0x10)[0], 9);
    LT_EQ(BRK(base + 0x800), base + 0x800);    /* shrinking is always allowed */
    LT_EQ(lkd_mm_find(T->mm, base, 0, &vi), 0);
    LT_EQ(vi.end, base + 0x1000);
    LT_EQ(BRK(base - 0x1000), base + 0x800);   /* below start_brk: unchanged */
    /* growth stops one page before the next VMA (brk: next && newbrk + PAGE_SIZE > vm_start_gap(next)) */
    LT_EQ(MMAP(base + 0x10000, 0x1000, RW, PA | LX_MAP_FIXED), base + 0x10000);
    LT_EQ(BRK(base + 0xf800), base + 0x800);
    LT_EQ(BRK(base + 0xf000), base + 0xf000);
    /* shrinking when the heap VMA was unmapped by hand fails (no brk VMA in the range) */
    LT_EQ(BRK(base + 0x1000), base + 0x1000);
    sc(LKD_NR_munmap, base, 0x1000, 0, 0, 0, 0);
    LT_EQ(BRK(base), base + 0x1000);
    down();
}

LT_TEST(mremap_semantics)
{
    up();
    int64_t a = MMAP(0, 0x4000, RW, PA);
    for (int i = 0; i < 0x4000; i++) H(a)[i] = (uint8_t)i;
    /* argument errors (mremap(2) EINVAL / EFAULT) */
    LT_ERRNO(MREMAP(a, 0x4000, 0x8000, 0x10, 0), LX_EINVAL);
    LT_ERRNO(MREMAP(a, 0x4000, 0x8000, LX_MREMAP_FIXED, a + 0x100000), LX_EINVAL);
    LT_ERRNO(MREMAP(a, 0x4000, 0x8000, LX_MREMAP_MAYMOVE | LX_MREMAP_DONTUNMAP, 0), LX_EINVAL);
    LT_ERRNO(MREMAP(a + 1, 0x4000, 0x8000, 0, 0), LX_EINVAL);
    LT_ERRNO(MREMAP(a, 0x4000, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(MREMAP(a - 0x100000, 0x1000, 0x2000, 0, 0), LX_EFAULT);   /* not mapped */
    LT_ERRNO(MREMAP(a, 0x5000, 0x8000, LX_MREMAP_MAYMOVE, 0), LX_EFAULT); /* old range crosses the VMA end */
    LT_ERRNO(MREMAP(a, 0, 0x2000, LX_MREMAP_MAYMOVE, 0), LX_EINVAL);   /* duplicating a private mapping */
    LT_EQ(MREMAP(a, 0x4000, 0x4000, 0, 0), a);
    /* shrink in place */
    LT_EQ(MREMAP(a, 0x4000, 0x3000, 0, 0), a);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_EQ(vi.end, a + 0x3000);
    /* grow in place when the space after is free */
    LT_EQ(MREMAP(a, 0x3000, 0x6000, 0, 0), a);
    LT_EQ(H(a)[0x2fff], 0xff);
    LT_EQ(H(a)[0x3000], 0);      /* the new part reads zeros */
    LT_EQ(H(a)[0x5fff], 0);
    /* blocked growth without MAYMOVE: ENOMEM; with MAYMOVE the content moves */
    LT_EQ(MMAP(a + 0x6000, 0x1000, LX_PROT_READ, PA | LX_MAP_FIXED), a + 0x6000);
    LT_ERRNO(MREMAP(a, 0x6000, 0x10000, 0, 0), LX_ENOMEM);
    int64_t b = MREMAP(a, 0x6000, 0x10000, LX_MREMAP_MAYMOVE, 0);
    LT_CHECK(b > 0 && b != a);
    LT_EQ((b - a) & 0x3fff, 0);  /* congruent address chosen: whole host pages moved */
    LT_EQ(H(b)[0x1234], 0x34);
    LT_EQ(H(b)[0x2fff], 0xff);
    LT_EQ(H(b)[0xffff], 0);
    LT_ERRNO(lkd_mm_find(T->mm, a, 0, &vi), LX_ENOENT); /* the old range is gone */
    /* MREMAP_FIXED to a chosen, non congruent address: copied */
    uint64_t f = LO() + 0x30001000;
    LT_EQ(MREMAP(b, 0x10000, 0x10000, LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED, f), f);
    LT_EQ(H(f)[0x1234], 0x34);
    LT_ERRNO(MREMAP(f, 0x2000, 0x2000, LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED, f + 0x1000), LX_EINVAL); /* overlap */
    /* MREMAP_DONTUNMAP: the old private anonymous range stays mapped and reads zeros */
    uint64_t g = LO() + 0x31000000;
    LT_EQ(MREMAP(f, 0x4000, 0x4000, LX_MREMAP_MAYMOVE | LX_MREMAP_DONTUNMAP | LX_MREMAP_FIXED, g), g);
    LT_EQ(H(g)[0x1234], 0x34);
    LT_EQ(lkd_mm_find(T->mm, f, 0, &vi), 0);
    LT_EQ(H(f)[0x1234], 0);
    down();
}

LT_TEST(mremap_shared_stays_shared)
{
    up();
    int64_t s = MMAP(0, 0x4000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);
    H(s)[1] = 42;
    int64_t n = MREMAP(s, 0x4000, 0x4000, LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED, LO() + 0x32000000);
    LT_EQ(n, LO() + 0x32000000);
    LT_EQ(H(n)[1], 42);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, n, 0, &vi), 0);
    LT_STREQ(vi.name, "/dev/zero");
    LT_ERRNO(MREMAP(n, 0x4000, 0x8000, LX_MREMAP_MAYMOVE, 0), LX_ENOMEM); /* object cannot grow (documented) */
    down();
}

LT_TEST(madvise_semantics)
{
    up();
    int64_t a = MMAP(0, 0xc000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    memset(H(a), 0x5c, 0xc000);
    /* MADV_DONTNEED on private anonymous memory: "subsequent accesses ... zero-fill-on-demand pages" */
    LT_EQ(MADV(hp, 0x4000, LX_MADV_DONTNEED), 0);   /* a whole host page */
    LT_EQ(H(hp)[0], 0);
    LT_EQ(H(hp)[0x3fff], 0);
    uint64_t other = hp + 0x4000; /* inside the mapping: hp - a < 0x4000 */
    LT_EQ(H(other)[0], 0x5c);
    LT_EQ(MADV(other, 0x1000, LX_MADV_DONTNEED), 0); /* one sub-page */
    LT_EQ(H(other)[0], 0);
    LT_EQ(H(other + 0x1000)[0], 0x5c);
    LT_ERRNO(MADV(a, 0x1000, 77), LX_EINVAL);           /* unknown advice */
    LT_ERRNO(MADV(a + 1, 0x1000, LX_MADV_DONTNEED), LX_EINVAL);
    LT_EQ(MADV(a, 0, LX_MADV_DONTNEED), 0);
    LT_ERRNO(MADV(a - 0x100000, 0x1000, LX_MADV_NORMAL), LX_ENOMEM); /* unmapped range */
    LT_ERRNO(MADV(a, 0x1000, 100 /* MADV_HWPOISON */), LX_EPERM);     /* needs CAP_SYS_ADMIN */
    LT_ERRNO(MADV(a, 0x1000, LX_MADV_COLLAPSE), LX_EINVAL);
    /* WIPEONFORK only on private anonymous memory */
    int64_t s = MMAP(0, 0x1000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);
    LT_ERRNO(MADV(s, 0x1000, LX_MADV_WIPEONFORK), LX_EINVAL);
    LT_ERRNO(MADV(s, 0x1000, LX_MADV_FREE), LX_EINVAL);
    LT_EQ(MADV(a, 0x1000, LX_MADV_WIPEONFORK), 0);
    LT_EQ(MADV(a, 0x1000, LX_MADV_DONTFORK), 0);
    LT_EQ(MADV(a, 0x8000, LX_MADV_FREE), 0);
    /* MADV_REMOVE on shared anonymous memory punches a hole: zeros */
    H(s)[5] = 1;
    LT_EQ(MADV(s, 0x1000, LX_MADV_REMOVE), 0);
    LT_EQ(H(s)[5], 0);
    LT_ERRNO(MADV(a, 0x1000, LX_MADV_REMOVE), LX_EINVAL);
    /* POPULATE_WRITE on a read-only mapping: EFAULT */
    int64_t r = MMAP(0, 0x2000, LX_PROT_READ, PA);
    LT_ERRNO(MADV(r, 0x2000, LX_MADV_POPULATE_WRITE), LX_EFAULT);
    LT_EQ(MADV(r, 0x2000, LX_MADV_POPULATE_READ), 0);
    /* the flags show up in smaps VmFlags */
    size_t len;
    char *sm = lkd_mm_render_smaps(T->mm, &len);
    LT_CHECK(sm && strstr(sm, " dc") && strstr(sm, " wf"));
    free(sm);
    /* DONTNEED on a locked mapping: EINVAL (madvise_dontneed_free_valid_vma) */
    LT_EQ(sc(LKD_NR_mlock, r, 0x1000, 0, 0, 0, 0), 0);
    LT_ERRNO(MADV(r, 0x1000, LX_MADV_DONTNEED), LX_EINVAL);
    LT_EQ(MADV(r, 0x1000, LX_MADV_DONTNEED_LOCKED), 0);
    down();
}

LT_TEST(mincore_semantics)
{
    up();
    int64_t a = MMAP(0, 0x10000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(MADV(hp, 0x8000, LX_MADV_DONTNEED), 0);
    H(hp)[0] = 1; /* touch the first host page only */
    uint64_t vec = (uint64_t)(uintptr_t)malloc(16);
    LT_EQ(sc(LKD_NR_mincore, hp, 0x8000, vec, 0, 0, 0), 0);
    uint8_t *v = (uint8_t *)(uintptr_t)vec;
    /* residency is known per 16 KiB host page: the four guest pages of the touched page are resident */
    LT_EQ(v[0], 1); LT_EQ(v[1], 1); LT_EQ(v[2], 1); LT_EQ(v[3], 1);
    LT_EQ(v[4], 0); LT_EQ(v[7], 0);
    LT_ERRNO(sc(LKD_NR_mincore, hp + 1, 0x1000, vec, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_mincore, a - 0x100000, 0x1000, vec, 0, 0, 0), LX_ENOMEM);
    LT_ERRNO(sc(LKD_NR_mincore, hp, 0x1000, a - 0x100000, 0, 0, 0), LX_EFAULT);
    free((void *)(uintptr_t)vec);
    down();
}

LT_TEST(msync_semantics)
{
    up();
    int64_t a = MMAP(0, 0x3000, RW, PA);
    LT_ERRNO(sc(LKD_NR_msync, a, 0x1000, LX_MS_ASYNC | LX_MS_SYNC, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_msync, a, 0x1000, 8, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_msync, a + 1, 0x1000, LX_MS_SYNC, 0, 0, 0), LX_EINVAL);
    LT_EQ(sc(LKD_NR_msync, a, 0x3000, LX_MS_SYNC, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_msync, a, 0, LX_MS_SYNC, 0, 0, 0), 0);
    LT_ERRNO(sc(LKD_NR_msync, a - 0x2000, 0x3000, LX_MS_ASYNC, 0, 0, 0), LX_ENOMEM); /* hole: ENOMEM */
    LT_ERRNO(sc(LKD_NR_msync, a - 0x2000, 0x3000, LX_MS_SYNC, 0, 0, 0), LX_ENOMEM);
    LT_EQ(sc(LKD_NR_mlock, a, 0x1000, 0, 0, 0, 0), 0);
    LT_ERRNO(sc(LKD_NR_msync, a, 0x1000, LX_MS_INVALIDATE, 0, 0, 0), LX_EBUSY);
    down();
}

LT_TEST(mlock_semantics)
{
    up();
    int64_t a = MMAP(0, 0x4000, RW, PA);
    LT_EQ(sc(LKD_NR_mlock, a + 0x100, 0x1000, 0, 0, 0, 0), 0); /* rounded to [a, a+0x2000) */
    struct lkd_mm_stats st;
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.locked_vm, 0x2000);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_CHECK(vi.flags & LX_MAP_LOCKED);
    LT_EQ(sc(LKD_NR_munlock, a, 0x4000, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.locked_vm, 0);
    LT_ERRNO(sc(LKD_NR_mlock, a - 0x100000, 0x1000, 0, 0, 0, 0), LX_ENOMEM);  /* unmapped */
    LT_ERRNO(sc(LKD_NR_mlock2, a, 0x1000, 2, 0, 0, 0), LX_EINVAL);           /* unknown flag */
    LT_EQ(sc(LKD_NR_mlock2, a, 0x1000, LX_MLOCK_ONFAULT, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_munlock, a, 0x1000, 0, 0, 0, 0), 0);
    /* RLIMIT_MEMLOCK: ENOMEM above the limit, EPERM when the limit is 0 (no CAP_IPC_LOCK) */
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = 0x1000;
    LT_ERRNO(sc(LKD_NR_mlock, a, 0x2000, 0, 0, 0, 0), LX_ENOMEM);
    LT_EQ(sc(LKD_NR_mlock, a, 0x1000, 0, 0, 0, 0), 0);
    LT_ERRNO(MMAP(0, 0x2000, RW, PA | LX_MAP_LOCKED), LX_EAGAIN);           /* mlock_future_ok */
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = 0;
    LT_ERRNO(sc(LKD_NR_mlock, a, 0x1000, 0, 0, 0, 0), LX_EPERM);
    LT_ERRNO(sc(LKD_NR_mlockall, LX_MCL_CURRENT, 0, 0, 0, 0, 0), LX_EPERM);
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
    LT_ERRNO(sc(LKD_NR_mlockall, 0, 0, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_mlockall, LX_MCL_ONFAULT, 0, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_mlockall, 8, 0, 0, 0, 0, 0), LX_EINVAL);
    /* MCL_FUTURE: later mappings are locked */
    LT_EQ(sc(LKD_NR_mlockall, LX_MCL_FUTURE, 0, 0, 0, 0, 0), 0);
    int64_t b = MMAP(0, 0x1000, RW, PA);
    LT_EQ(lkd_mm_find(T->mm, b, 0, &vi), 0);
    LT_CHECK(vi.flags & LX_MAP_LOCKED);
    LT_EQ(sc(LKD_NR_munlockall, 0, 0, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.locked_vm, 0);
    int64_t c = MMAP(0, 0x1000, RW, PA);
    LT_EQ(lkd_mm_find(T->mm, c, 0, &vi), 0);
    LT_EQ(vi.flags & LX_MAP_LOCKED, 0);
    LT_EQ(sc(LKD_NR_mlockall, LX_MCL_CURRENT, 0, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.locked_vm, st.total_vm);
    down();
}

/* VmPeak and VmHWM (request procfs-2): mm->hiwater_vm and hiwater_rss are updated before every unmap
 * (update_hiwater_vm, update_hiwater_rss in mm/mmap.c) and at reads; /proc shows max(hiwater, current). */
LT_TEST(high_water_marks)
{
    up();
    struct lkd_mm_stats st;
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    uint64_t vm0 = st.total_vm;
    int64_t a = MMAP(0, 8 << 20, RW, PA);
    LT_CHECK(a > 0);
    memset(H((uint64_t)a), 1, 8 << 20);                      /* resident */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);                       /* the mapping changed: a fresh RSS */
    LT_CHECK(st.rss_anon >= (8u << 20));
    uint64_t r8 = st.rss_anon;
    int64_t b = MMAP(0, 4 << 20, RW, PA);
    LT_CHECK(b > 0);
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)a, 8 << 20, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.total_vm, vm0 + (4 << 20));
    LT_EQ(st.hiwater_vm, vm0 + (12 << 20));                   /* the peak before the unmap */
    LT_CHECK(st.hiwater_rss >= r8);                           /* with the 8 MiB that were resident */
    LT_CHECK(st.rss_anon + (7u << 20) <= r8);                 /* they went back to the host */
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)b, 4 << 20, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.hiwater_vm, vm0 + (12 << 20));                   /* never decreases */
    lkd_mm_reset_for_exec(T->mm);                             /* a new mm at exec */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.hiwater_vm, st.total_vm);
    down();
}

/* RSS of /proc/<pid>/status: the host process's resident memory (host_rss in mm.c; a walk of the VMAs cost 20 ms
 * per read with 40000 host regions, Dead by Daylight reads it every frame). It is cached for MM_RSS_TTL_NS
 * (100 ms) unless a mapping is added or removed. */
LT_TEST(rss_counting_and_cache)
{
    up();
    struct lkd_mm_stats st;
    int64_t a = MMAP(0, 8 << 20, RW, PA);
    LT_CHECK(a > 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    uint64_t r0 = st.rss_anon;
    memset(H((uint64_t)a), 1, 3 << 20);                       /* 3 MiB of 8 MiB resident, no mapping change */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.rss_anon, r0);                                   /* within the validity: the cached value */
    usleep(120000);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_CHECK(st.rss_anon >= r0 + (3u << 20));                 /* refreshed after 100 ms */
    uint64_t r1 = st.rss_anon;
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)a, 8 << 20, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_CHECK(st.rss_anon + (2u << 20) <= r1);                 /* a mapping change refreshes at once */
    down();
}

/* VmSize, VmData, VmExe and VmStk come from counters the VMA tree keeps (vm_stat_account): after maps, splits,
 * merges, mprotect, mremap, names and unmaps they still equal a walk of the VMAs (vt_check). */
LT_TEST(vm_counters_follow_the_tree)
{
    up();
    struct lkd_mm_stats st;
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    uint64_t vm0 = st.total_vm, data0 = st.data_vm, exec0 = st.exec_vm, stk0 = st.stack_vm;
    int64_t a = MMAP(0, 0x10000, RW, PA);                                   /* data */
    int64_t x = MMAP(0, 0x4000, LX_PROT_READ | LX_PROT_EXEC, PA);          /* exec */
    int64_t s = MMAP(0, 0x4000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);     /* shared: neither */
    LT_CHECK(a > 0 && x > 0 && s > 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.total_vm, vm0 + 0x18000);
    LT_EQ(st.data_vm, data0 + 0x10000);
    LT_EQ(st.exec_vm, exec0 + 0x4000);
    LT_EQ(sc(LKD_NR_mprotect, (uint64_t)a + 0x4000, 0x4000, LX_PROT_READ | LX_PROT_EXEC, 0, 0, 0), 0); /* split */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.data_vm, data0 + 0xc000);
    LT_EQ(st.exec_vm, exec0 + 0x8000);
    LT_EQ(vt_check(&T->mm->vt), 0);
    LT_EQ(sc(LKD_NR_mprotect, (uint64_t)a + 0x4000, 0x4000, RW, 0, 0, 0), 0);                          /* merge */
    LT_EQ(vt_check(&T->mm->vt), 0);
    int64_t m = MREMAP(a, 0x10000, 0x20000, LX_MREMAP_MAYMOVE, 0);                                     /* grow */
    LT_CHECK(m > 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.data_vm, data0 + 0x20000);
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)m + 0x8000, 0x4000, 0, 0, 0, 0), 0);                             /* a hole */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.total_vm, vm0 + 0x24000);
    LT_EQ(st.data_vm, data0 + 0x1c000);
    LT_EQ(vt_check(&T->mm->vt), 0);
    LT_EQ(lkd_mm_set_vma_name(T->mm, (uint64_t)m, 0x4000, "[stack]"), 0);                               /* a stack */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.stack_vm, stk0 + 0x4000);
    LT_EQ(st.data_vm, data0 + 0x18000);
    LT_EQ(vt_check(&T->mm->vt), 0);
    lkd_mm_set_stack_start(T->mm, (uint64_t)x + 0x100);                    /* the VMA holding start_stack */
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.stack_vm, stk0 + 0x8000);
    LT_EQ(st.exec_vm, exec0);
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)m, 0x20000, 0, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)x, 0x4000, 0, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_munmap, (uint64_t)s, 0x4000, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.total_vm, vm0);
    LT_EQ(st.data_vm, data0);
    LT_EQ(vt_check(&T->mm->vt), 0);
    down();
}

LT_TEST(pkeys_and_shadow_stack)
{
    up();
    int64_t a = MMAP(0, 0x1000, RW, PA);
    LT_ERRNO(sc(LKD_NR_pkey_alloc, 0, 0, 0, 0, 0, 0), LX_ENOSPC); /* no PKU on the virtual CPU */
    LT_ERRNO(sc(LKD_NR_pkey_alloc, 1, 0, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_pkey_free, 1, 0, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_pkey_mprotect, a, 0x1000, LX_PROT_READ, 1, 0, 0), LX_EINVAL);
    LT_EQ(sc(LKD_NR_pkey_mprotect, a, 0x1000, LX_PROT_READ, (uint64_t)-1, 0, 0), 0);
    LT_ERRNO(sc(LKD_NR_map_shadow_stack, 0, 0x1000, 0, 0, 0, 0), LX_EOPNOTSUPP);
    down();
}

/* show_vma_header_prefix + show_map_vma: "%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu " then, when there is
 * a name, spaces up to column 73 and the name. Expected strings are built here from the literal format. */
LT_TEST(maps_exact_format)
{
    up();
    uint64_t lo = LO();
    int64_t a = MMAP(lo + 0x40000000, 0x2000, LX_PROT_READ | LX_PROT_EXEC, PA | LX_MAP_FIXED);
    int64_t s = MMAP(lo + 0x40010000, 0x1000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED);
    LT_EQ(lkd_mm_set_vma_name(T->mm, a, 0x1000, "[vdso]"), 0);
    lkd_mm_set_brk_start(T->mm, lo + 0x40020000);
    BRK(lo + 0x40021000);
    int64_t st = MMAP(lo + 0x40030000, 0x3000, RW, PA | LX_MAP_FIXED);
    lkd_mm_set_stack_start(T->mm, st + 0x2ff0);
    int64_t nm = MMAP(lo + 0x40040000, 0x1000, RW, PA | LX_MAP_FIXED);
    LT_EQ(lkd_mm_set_vma_name(T->mm, nm, 0x1000, "[anon:wine heap]"), 0);
    size_t len;
    char *m = lkd_mm_render_maps(T->mm, &len);
    LT_CHECK(m != NULL);
    char want[1024], line[200];
    int ino = 0;
    struct lkd_vma_info vi;
    lkd_mm_find(T->mm, s, 0, &vi);
    ino = (int)vi.ino;
    want[0] = 0;
#define PAD(pre, name) do { snprintf(line, sizeof line, "%s", pre); while (strlen(line) < 73) strcat(line, " "); \
        strcat(line, name); strcat(line, "\n"); strcat(want, line); } while (0)
    snprintf(line, sizeof line, "%08llx-%08llx r-xp 00000000 00:00 0 ", (unsigned long long)a, (unsigned long long)a + 0x1000);
    { char pre[100]; strcpy(pre, line); PAD(pre, "[vdso]"); }
    snprintf(line, sizeof line, "%08llx-%08llx r-xp 00000000 00:00 0 \n", (unsigned long long)a + 0x1000, (unsigned long long)a + 0x2000);
    strcat(want, line);
    { char pre[100]; snprintf(pre, sizeof pre, "%08llx-%08llx rw-s 00000000 00:01 %d ", (unsigned long long)s, (unsigned long long)s + 0x1000, ino); PAD(pre, "/dev/zero (deleted)"); }
    { char pre[100]; snprintf(pre, sizeof pre, "%08llx-%08llx rw-p 00000000 00:00 0 ", (unsigned long long)lo + 0x40020000, (unsigned long long)lo + 0x40021000); PAD(pre, "[heap]"); }
    { char pre[100]; snprintf(pre, sizeof pre, "%08llx-%08llx rw-p 00000000 00:00 0 ", (unsigned long long)st, (unsigned long long)st + 0x3000); PAD(pre, "[stack]"); }
    { char pre[100]; snprintf(pre, sizeof pre, "%08llx-%08llx rw-p 00000000 00:00 0 ", (unsigned long long)nm, (unsigned long long)nm + 0x1000); PAD(pre, "[anon:wine heap]"); }
    LT_STREQ(m, want);
    LT_EQ(len, strlen(want));
    /* the name column is 73 (0-based) on every named line */
    LT_EQ(strchr(m, '[') - m, 73);
    free(m);
    /* names only on anonymous memory (prctl PR_SET_VMA_ANON_NAME) and only on mapped ranges */
    LT_ERRNO(lkd_mm_set_vma_name(T->mm, lo + 0x50000000, 0x1000, "[anon:x]"), LX_ENOMEM);
    down();
}

LT_TEST(futex_keys)
{
    up();
    int64_t p = MMAP(0, 0x1000, RW, PA);
    int64_t s = MMAP(0, 0x1000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);
    struct lkd_futex_key k1, k2;
    LT_ERRNO(lkd_mm_futex_key(T, p + 2, 0, &k1), LX_EINVAL);        /* misaligned */
    LT_EQ(lkd_mm_futex_key(T, p + 8, 0, &k1), 0);
    LT_EQ(k1.kind, LKD_FK_PRIVATE);
    LT_EQ(k1.a, lkd_mm_identity(T->mm));
    LT_EQ(k1.b, p + 8);
    LT_EQ(lkd_mm_futex_key(T, s + 8, 0, &k2), 0);
    LT_EQ(k2.kind, LKD_FK_SHARED);
    LT_EQ(k2.b, 8);                                                  /* offset in the shared object */
    LT_EQ(lkd_mm_futex_key(T, s + 8, 1, &k2), 0);                    /* FUTEX_PRIVATE_FLAG */
    LT_EQ(k2.kind, LKD_FK_PRIVATE);
    LT_ERRNO(lkd_mm_futex_key(T, p - 0x100000, 0, &k1), LX_EFAULT);
    down();
}

int main(void)
{
    LT_RUN(brk_semantics);
    LT_RUN(mremap_semantics);
    LT_RUN(mremap_shared_stays_shared);
    LT_RUN(madvise_semantics);
    LT_RUN(mincore_semantics);
    LT_RUN(msync_semantics);
    LT_RUN(mlock_semantics);
    LT_RUN(high_water_marks);
    LT_RUN(rss_counting_and_cache);
    LT_RUN(vm_counters_follow_the_tree);
    LT_RUN(pkeys_and_shadow_stack);
    LT_RUN(maps_exact_format);
    LT_RUN(futex_keys);
    return lt_finish();
}

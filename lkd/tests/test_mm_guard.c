/* test_mm_guard.c: guarded host pages, the host page of a Windows module's file mapping whose 4 KiB guest pages are not
 * all accessible (mm.c mm_page_guarded). The host protection is PROT_NONE, and nothing the engine does for one thread may
 * open it: every other thread would read the pieces the guest forbids (a packer's encrypted code) without a fault.
 * lkd_mm_guard_store is the decision for a store the engine runs through an alias of the page (watch.c). */
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"
#include "../src/mm/lkd_mm_watch.h"

static lkd_task *T;
static char path[512];
static int hostfd = -1;
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PG 0x1000u

static int t_mmap_fd(lkd_fdobj *o) { return o->host_fd; }
static int t_describe(lkd_fdobj *o, char *buf, size_t cap) { return snprintf(buf, cap, "%s", o->gpath); }
static const lkd_fd_ops file_ops = { .name = "file", .mmap_fd = t_mmap_fd, .describe = t_describe };

static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c)
{
    lkd_sysreq rq = { nr, { a, b, c, 0, 0, 0 } };
    return lkd_dispatch(T, &rq);
}

static void up(void)
{
    if (!T) {
        fake_engine_init();
        T = fake_make_task();
        const char *d = getenv("TMPDIR");
        snprintf(path, sizeof path, "%s/lkd_mm_guard_XXXXXX", d && *d ? d : "/tmp");
        hostfd = mkstemp(path);
        uint8_t zero[0x10000] = { 0 };
        write(hostfd, zero, sizeof zero);
    }
    T->mm = lkd_mm_create();
    fake_engine_reset();
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }

/* A private mapping of a file the engine sees as a Windows module, two host pages, at a host page boundary. */
static uint64_t map_module(const char *gpath)
{
    int fd = open(path, O_RDONLY);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_FILE, &file_ops, fd, LX_O_RDONLY, NULL);
    o->gpath = strdup(gpath);
    int64_t a = lkd_mm_map(T->mm, 0, 4 * mm_hps, RW, LX_MAP_PRIVATE, o, 0, NULL);
    lkd_fdobj_put(o);
    return a > 0 ? ((uint64_t)a + mm_hps - 1) & ~(mm_hps - 1) : 0;
}

/* The host protection of the host page of guest address g, VM_PROT_*; -1 when macOS does not say. */
static int host_prot(uint64_t g)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)lkd_g2h(g);
    mach_vm_size_t sz;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return -1;
    return info.protection;
}

LT_TEST(store_decision_and_page_stays_closed)
{
    up();
    uint64_t base = map_module("/opt/game/module.dll");
    LT_CHECK(base != 0);
    /* piece 1 of the first host page is forbidden, its neighbours stay accessible: a guarded page */
    LT_EQ(sc(LKD_NR_mprotect, base + PG, PG, LX_PROT_NONE), 0);
    LT_EQ(host_prot(base), VM_PROT_NONE);
    LT_EQ(lkd_mm_guard_store(base + 0x10), 1);        /* the guest allows a store here */
    LT_EQ(lkd_mm_guard_store(base + PG + 0x10), -1);  /* and forbids it there */
    LT_EQ(lkd_mm_guard_store(base + mm_hps + 0x10), 0); /* the next host page has no forbidden piece: not guarded */
    /* the decisions did not open the page for the other threads */
    LT_EQ(host_prot(base), VM_PROT_NONE);
    down();
}

LT_TEST(store_to_a_read_only_piece_is_forbidden)
{
    up();
    uint64_t base = map_module("/opt/game/readonly.dll");
    LT_CHECK(base != 0);
    LT_EQ(sc(LKD_NR_mprotect, base + PG, PG, LX_PROT_NONE), 0);
    LT_EQ(sc(LKD_NR_mprotect, base + 2 * PG, PG, LX_PROT_READ), 0);
    LT_EQ(lkd_mm_guard_store(base + 2 * PG), -1);
    LT_EQ(lkd_mm_guard_store(base), 1);
    down();
}

LT_TEST(not_a_module_is_not_guarded)
{
    up();
    uint64_t base = map_module("/opt/game/data.bin");
    LT_CHECK(base != 0);
    LT_EQ(sc(LKD_NR_mprotect, base + PG, PG, LX_PROT_NONE), 0);
    LT_EQ(lkd_mm_guard_store(base), 0); /* only Windows modules have their pieces guarded */
    down();
}

LT_TEST(aliases_of_a_guarded_page)
{
    up();
    uint64_t base = map_module("/opt/game/aliases.dll");
    LT_CHECK(base != 0);
    LT_EQ(sc(LKD_NR_mprotect, base + PG, PG, LX_PROT_NONE), 0);
    const void *alias[4];
    const uint64_t *gen;
    uint64_t now;
    LT_EQ(lkd_mm_guard_aliases(base, alias, &gen, &now), 1);
    LT_CHECK(alias[1] == NULL); /* forbidden: no view */
    LT_EQ(lkd_mm_guard_aliases(base + mm_hps, alias, &gen, &now), 0);
    down();
}

int main(void)
{
    LT_RUN(store_decision_and_page_stays_closed);
    LT_RUN(store_to_a_read_only_piece_is_forbidden);
    LT_RUN(not_a_module_is_not_guarded);
    LT_RUN(aliases_of_a_guarded_page);
    unlink(path);
    return lt_finish();
}

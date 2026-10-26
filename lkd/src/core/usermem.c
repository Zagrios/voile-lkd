/* usermem.c: fault-safe access to guest memory. Owner: core.
 *
 * lkd_user_read/write/strn/zero validate the range with lkd_mm_check_range (weak hook, the mm module
 * overrides it) and then copy with lkd_uc_copy, a leaf assembly routine. A SIGSEGV/SIGBUS whose PC is
 * inside that routine is not a crash: the handler sets x0 = -1 and PC = LR (the routine never touches
 * LR), so the routine "returns" -1 and the caller reports -EFAULT. Faults anywhere else go to the
 * handler that was installed before lkd_init (chained), or kill the process by default.
 *
 * Engine coexistence: an engine that installs its own SIGSEGV/SIGBUS handler after lkd_init must call
 * lkd_fault_try_recover(ucontext) first. See include/lkd/lkd.h.
 */
#include <signal.h>
#include <string.h>
#include <sys/ucontext.h>
#include <mach/thread_status.h>
#include <unistd.h>

#include "lkd_internal.h"
#include "mm.h"

uint64_t lkd_guest_base;

#define LKD_USER_TOP 0x800000000000ull /* guest addresses are 47-bit */

#if !defined(__aarch64__)
#error "LKD usermem fault recovery is implemented for arm64 only"
#endif

extern int lkd_uc_copy(void *dst, const void *src, size_t n);
extern const char lkd_uc_copy_begin[], lkd_uc_copy_end[];
extern int lkd_uc_cas32(uint32_t *p, uint32_t *expected, uint32_t desired);

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _lkd_uc_copy\n"
    ".globl _lkd_uc_copy_begin\n"
    ".globl _lkd_uc_copy_end\n"
    "_lkd_uc_copy:\n"
    "_lkd_uc_copy_begin:\n"
    "    cbz x2, 9f\n"
    "    cmp x2, #16\n"
    "    b.lo 3f\n"
    "1:  ldp x3, x4, [x1], #16\n"
    "    stp x3, x4, [x0], #16\n"
    "    sub x2, x2, #16\n"
    "    cmp x2, #16\n"
    "    b.hs 1b\n"
    "    cbz x2, 9f\n"
    "3:  ldrb w3, [x1], #1\n"
    "    strb w3, [x0], #1\n"
    "    subs x2, x2, #1\n"
    "    b.ne 3b\n"
    "9:  mov x0, #0\n"
    "    ret\n"
    /* int lkd_uc_cas32(uint32_t *p, uint32_t *expected, uint32_t desired): 0 swapped, 1 different (*expected
     * updated), -1 by the fault handler. A leaf that never touches LR, inside the recoverable range. */
    ".p2align 2\n"
    ".globl _lkd_uc_cas32\n"
    "_lkd_uc_cas32:\n"
    "    ldr w3, [x1]\n"
    "1:  ldaxr w4, [x0]\n"
    "    cmp w4, w3\n"
    "    b.ne 2f\n"
    "    stlxr w5, w2, [x0]\n"
    "    cbnz w5, 1b\n"
    "    mov x0, #0\n"
    "    ret\n"
    "2:  clrex\n"
    "    str w4, [x1]\n"
    "    mov x0, #1\n"
    "    ret\n"
    "_lkd_uc_copy_end:\n"
);

/* The range check is lkd_mm_check_range (weak in stubs_mm.c, strong in the mm module). Tests of core
 * that use raw host memory as guest memory replace it, see lkd_usermem_set_check_fn. */
static int (*g_check)(uint64_t, size_t, int);

void lkd_usermem_set_check_fn(int (*fn)(uint64_t gva, size_t len, int need_write))
{
    g_check = fn;
}

static inline int check_range(uint64_t gva, size_t len, int need_write)
{
    return g_check ? g_check(gva, len, need_write) : lkd_mm_check_range(gva, len, need_write);
}

int lkd_fault_try_recover(void *ctx)
{
    ucontext_t *uc = ctx;
    uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    if (pc >= (uintptr_t)lkd_uc_copy_begin && pc < (uintptr_t)lkd_uc_copy_end) {
        /* A store into a page whose write permission mm removed because it holds translated code
         * (src/mm/watch.c, request mm-5): mm drops the translations and restores the permission; return
         * without touching the context so the store is executed again. Only data aborts with WnR set (a
         * store). The fault status is NOT filtered: macOS reports a store to a non-resident read-only page
         * as a translation fault (DFSC 0x06, MESURE), a resident one as a permission fault (0x0f). */
        uint32_t esr = uc->uc_mcontext->__es.__esr;
        uint32_t ec = esr >> 26;
        if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6)) &&
            lkd_mm_code_write_fault(lkd_h2g((const void *)(uintptr_t)uc->uc_mcontext->__es.__far)) == 1)
            return 1;
        /* A guarded host page (src/mm/mm.c): PROT_NONE on the host, the guest's rights decide. Opened until the end of
         * the system call; the copy is executed again. */
        if (ec == 0x24 || ec == 0x25) {
            int g = lkd_mm_guard_fault(lkd_h2g((const void *)(uintptr_t)uc->uc_mcontext->__es.__far), (esr & (1u << 6)) != 0, NULL);
            if (g == 1 || g == 2) return 1;
        }
        void *lr = (void *)arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
        arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, lr);
        uc->uc_mcontext->__ss.__x[0] = (uint64_t)-1;
        return 1;
    }
    return 0;
}

static struct sigaction prev_segv, prev_bus;
static _Atomic int handlers_installed;

static void chain(const struct sigaction *prev, int sig, siginfo_t *si, void *ctx)
{
    if (prev->sa_flags & SA_SIGINFO) {
        if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, ctx); return; }
    } else if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN) {
        prev->sa_handler(sig);
        return;
    }
    /* No previous handler: restore the default action and return so that the instruction faults again
     * and the process dies with the original signal. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
}

static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
    if (lkd_fault_try_recover(ctx)) return;
    chain(sig == SIGBUS ? &prev_bus : &prev_segv, sig, si, ctx);
}

/* Called by lkd_init. Idempotent. */
int lkd_usermem_install_handlers(void)
{
    if (atomic_exchange(&handlers_installed, 1)) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &prev_segv) < 0) return -1;
    if (sigaction(SIGBUS, &sa, &prev_bus) < 0) return -1;
    return 0;
}

static inline int range_ok(uint64_t gva, size_t n)
{
    return gva < LKD_USER_TOP && n <= LKD_USER_TOP - gva;
}

int lkd_user_read(uint64_t gva, void *dst, size_t n)
{
    if (n == 0) return 0;
    if (!range_ok(gva, n)) return -LX_EFAULT;
    int r = check_range(gva, n, 0);
    if (r) return r;
    return lkd_uc_copy(dst, lkd_g2h(gva), n) ? -LX_EFAULT : 0;
}

int lkd_user_write(uint64_t gva, const void *src, size_t n)
{
    if (n == 0) return 0;
    if (!range_ok(gva, n)) return -LX_EFAULT;
    int r = check_range(gva, n, 1);
    if (r) return r;
    return lkd_uc_copy(lkd_g2h(gva), src, n) ? -LX_EFAULT : 0;
}

int lkd_user_zero(uint64_t gva, size_t n)
{
    static const char zeros[256];
    while (n) {
        size_t c = n < sizeof zeros ? n : sizeof zeros;
        int r = lkd_user_write(gva, zeros, c);
        if (r) return r;
        gva += c;
        n -= c;
    }
    return 0;
}

int64_t lkd_user_strn(uint64_t gva, char *dst, size_t max)
{
    if (max == 0) return -LX_EINVAL;
    size_t copied = 0;
    while (copied < max) {
        uint64_t a = gva + copied;
        if (!range_ok(a, 1)) return -LX_EFAULT;
        size_t chunk = LX_PAGE_SIZE - (a & (LX_PAGE_SIZE - 1)); /* never cross a 4 KiB guest page */
        if (chunk > max - copied) chunk = max - copied;
        int r = check_range(a, chunk, 0);
        if (r) return r;
        if (lkd_uc_copy(dst + copied, lkd_g2h(a), chunk)) return -LX_EFAULT;
        const char *nul = memchr(dst + copied, 0, chunk);
        if (nul) return (int64_t)(copied + (size_t)(nul - (dst + copied)));
        copied += chunk;
    }
    dst[max - 1] = 0;
    return -LX_ENAMETOOLONG;
}

int lkd_user_cmpxchg32(uint64_t gva, uint32_t *expected, uint32_t desired)
{
    if (gva & 3) return -LX_EINVAL;
    if (!range_ok(gva, 4)) return -LX_EFAULT;
    int r = check_range(gva, 4, 1);
    if (r) return r;
    uint32_t e = *expected;
    int c = lkd_uc_cas32((uint32_t *)lkd_g2h(gva), &e, desired);
    if (c < 0) return -LX_EFAULT;
    *expected = e;
    return c;
}

/* seccomp.c: seccomp(2), prctl(PR_SET_SECCOMP, PR_GET_SECCOMP) and the filters every syscall of a thread
 * runs through. Owner: task.
 *
 * Models kernel/seccomp.c and the classic BPF checks of net/core/filter.c (bpf_check_classic,
 * check_load_and_stores) of Linux v6.12. A filter is a checked copy of the guest's program, refcounted, linked
 * to the filters installed before it. Each syscall runs every filter of the thread's chain and the action of
 * lowest precedence value wins (seccomp_run_filters). SECCOMP_RET_TRAP skips the syscall and forces SIGSYS
 * with SYS_SECCOMP, the call address, the number and the arch: Proton's Wine installs such a filter to catch
 * the system calls Windows code makes itself, below the addresses of the native libraries.
 *
 * Not modeled: user notification (SECCOMP_FILTER_FLAG_NEW_LISTENER is refused, so SECCOMP_RET_USER_NOTIF
 * finds no listener) and PTRACE_EVENT_SECCOMP (SECCOMP_RET_TRACE finds no tracer). Both answer -ENOSYS, as
 * Linux does without a listener or a tracer. no_new_privs stays per process, as the rest of the creds here.
 */
#include <stdlib.h>
#include <string.h>

#include "task_priv.h"
#include "sig.h"

#define CAP_SYS_ADMIN 21
#define MAX_ERRNO 4095
/* MAX_INSNS_PER_PATH: (1 << 18) / sizeof(struct sock_filter), with a 4 instruction penalty per filter. */
#define MAX_INSNS_PER_PATH ((1u << 18) / sizeof(struct lx_sock_filter))

struct lkd_seccomp_filter {
    _Atomic int refs;
    struct lkd_seccomp_filter *prev; /* the filter installed before it, whose reference it holds */
    uint8_t log;                     /* SECCOMP_FILTER_FLAG_LOG */
    uint16_t len;
    struct lx_sock_filter insns[];
};

/* Classic BPF (include/uapi/linux/bpf_common.h, filter.h). */
#define BPF_CLASS(c) ((c) & 0x07)
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_OP(c) ((c) & 0xf0)
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_SRC(c) ((c) & 0x08)
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10
#define BPF_TAX 0x00
#define BPF_TXA 0x80

/* ---- Checking a program --------------------------------------------------------------------------- */

static int is_cond_jump(uint16_t code)
{
    switch (code) {
    case BPF_JMP | BPF_JEQ | BPF_K: case BPF_JMP | BPF_JEQ | BPF_X:
    case BPF_JMP | BPF_JGT | BPF_K: case BPF_JMP | BPF_JGT | BPF_X:
    case BPF_JMP | BPF_JGE | BPF_K: case BPF_JMP | BPF_JGE | BPF_X:
    case BPF_JMP | BPF_JSET | BPF_K: case BPF_JMP | BPF_JSET | BPF_X:
        return 1;
    }
    return 0;
}

/* seccomp_check_filter: only these codes, the loads of struct seccomp_data rewritten as in the kernel. */
static int seccomp_codes(struct lx_sock_filter *f, unsigned len)
{
    for (unsigned pc = 0; pc < len; pc++) {
        struct lx_sock_filter *i = &f[pc];
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS:
            if (i->k >= sizeof(struct lx_seccomp_data) || (i->k & 3)) return -LX_EINVAL;
            continue;
        case BPF_LD | BPF_W | BPF_LEN:
            i->code = BPF_LD | BPF_IMM;
            i->k = sizeof(struct lx_seccomp_data);
            continue;
        case BPF_LDX | BPF_W | BPF_LEN:
            i->code = BPF_LDX | BPF_IMM;
            i->k = sizeof(struct lx_seccomp_data);
            continue;
        case BPF_RET | BPF_K: case BPF_RET | BPF_A:
        case BPF_ALU | BPF_ADD | BPF_K: case BPF_ALU | BPF_ADD | BPF_X:
        case BPF_ALU | BPF_SUB | BPF_K: case BPF_ALU | BPF_SUB | BPF_X:
        case BPF_ALU | BPF_MUL | BPF_K: case BPF_ALU | BPF_MUL | BPF_X:
        case BPF_ALU | BPF_DIV | BPF_K: case BPF_ALU | BPF_DIV | BPF_X:
        case BPF_ALU | BPF_AND | BPF_K: case BPF_ALU | BPF_AND | BPF_X:
        case BPF_ALU | BPF_OR | BPF_K: case BPF_ALU | BPF_OR | BPF_X:
        case BPF_ALU | BPF_XOR | BPF_K: case BPF_ALU | BPF_XOR | BPF_X:
        case BPF_ALU | BPF_LSH | BPF_K: case BPF_ALU | BPF_LSH | BPF_X:
        case BPF_ALU | BPF_RSH | BPF_K: case BPF_ALU | BPF_RSH | BPF_X:
        case BPF_ALU | BPF_NEG:
        case BPF_LD | BPF_IMM: case BPF_LDX | BPF_IMM:
        case BPF_MISC | BPF_TAX: case BPF_MISC | BPF_TXA:
        case BPF_LD | BPF_MEM: case BPF_LDX | BPF_MEM:
        case BPF_ST: case BPF_STX:
        case BPF_JMP | BPF_JA:
            continue;
        default:
            if (is_cond_jump(i->code)) continue;
            return -LX_EINVAL;
        }
    }
    return 0;
}

/* bpf_check_classic: operands and jump targets in range, a return last, and no scratch cell read before
 * every path to it has written it (check_load_and_stores). */
static int check_classic(const struct lx_sock_filter *f, unsigned len)
{
    for (unsigned pc = 0; pc < len; pc++) {
        const struct lx_sock_filter *i = &f[pc];
        switch (i->code) {
        case BPF_ALU | BPF_DIV | BPF_K:
            if (!i->k) return -LX_EINVAL;
            break;
        case BPF_ALU | BPF_LSH | BPF_K: case BPF_ALU | BPF_RSH | BPF_K:
            if (i->k >= 32) return -LX_EINVAL;
            break;
        case BPF_LD | BPF_MEM: case BPF_LDX | BPF_MEM: case BPF_ST: case BPF_STX:
            if (i->k >= LX_BPF_MEMWORDS) return -LX_EINVAL;
            break;
        case BPF_JMP | BPF_JA:
            if (i->k >= len - pc - 1) return -LX_EINVAL;
            break;
        default:
            if (is_cond_jump(i->code) && (pc + i->jt + 1 >= len || pc + i->jf + 1 >= len)) return -LX_EINVAL;
            break;
        }
    }
    uint16_t last = f[len - 1].code;
    if (last != (BPF_RET | BPF_K) && last != (BPF_RET | BPF_A)) return -LX_EINVAL;

    uint16_t *masks = malloc(len * sizeof *masks);
    if (!masks) return -LX_ENOMEM;
    memset(masks, 0xff, len * sizeof *masks);
    uint16_t memvalid = 0; /* one bit per scratch cell */
    int r = 0;
    for (unsigned pc = 0; pc < len && !r; pc++) {
        const struct lx_sock_filter *i = &f[pc];
        memvalid &= masks[pc];
        switch (i->code) {
        case BPF_ST: case BPF_STX:
            memvalid |= (uint16_t)(1u << i->k);
            break;
        case BPF_LD | BPF_MEM: case BPF_LDX | BPF_MEM:
            if (!(memvalid & (1u << i->k))) r = -LX_EINVAL;
            break;
        case BPF_JMP | BPF_JA:
            masks[pc + 1 + i->k] &= memvalid;
            memvalid = 0xffff;
            break;
        default:
            if (is_cond_jump(i->code)) {
                masks[pc + 1 + i->jt] &= memvalid;
                masks[pc + 1 + i->jf] &= memvalid;
                memvalid = 0xffff;
            }
            break;
        }
    }
    free(masks);
    return r;
}

/* ---- Running the filters --------------------------------------------------------------------------- */

/* The classic BPF machine as the kernel's conversion runs it: 32-bit A and X, both 0 at entry, shifts by X
 * modulo 32, a division by a zero X returns 0. */
static uint32_t run_one(const struct lkd_seccomp_filter *f, const struct lx_seccomp_data *d)
{
    const uint8_t *data = (const uint8_t *)d;
    uint32_t A = 0, X = 0, M[LX_BPF_MEMWORDS];
    for (unsigned pc = 0; pc < f->len; pc++) {
        const struct lx_sock_filter *i = &f->insns[pc];
        uint32_t s = BPF_SRC(i->code) == BPF_X ? X : i->k;
        switch (i->code) {
        case BPF_LD | BPF_W | BPF_ABS: memcpy(&A, data + i->k, sizeof A); continue;
        case BPF_LD | BPF_IMM: A = i->k; continue;
        case BPF_LDX | BPF_IMM: X = i->k; continue;
        case BPF_LD | BPF_MEM: A = M[i->k]; continue;
        case BPF_LDX | BPF_MEM: X = M[i->k]; continue;
        case BPF_ST: M[i->k] = A; continue;
        case BPF_STX: M[i->k] = X; continue;
        case BPF_MISC | BPF_TAX: X = A; continue;
        case BPF_MISC | BPF_TXA: A = X; continue;
        case BPF_RET | BPF_K: return i->k;
        case BPF_RET | BPF_A: return A;
        case BPF_JMP | BPF_JA: pc += i->k; continue;
        case BPF_ALU | BPF_NEG: A = 0u - A; continue;
        }
        if (BPF_CLASS(i->code) == BPF_ALU) {
            switch (BPF_OP(i->code)) {
            case BPF_ADD: A += s; break;
            case BPF_SUB: A -= s; break;
            case BPF_MUL: A *= s; break;
            case BPF_DIV: if (!s) return 0; A /= s; break;
            case BPF_OR: A |= s; break;
            case BPF_AND: A &= s; break;
            case BPF_XOR: A ^= s; break;
            case BPF_LSH: A <<= (s & 31); break;
            case BPF_RSH: A >>= (s & 31); break;
            }
            continue;
        }
        int taken = 0;
        switch (BPF_OP(i->code)) {
        case BPF_JEQ: taken = A == s; break;
        case BPF_JGT: taken = A > s; break;
        case BPF_JGE: taken = A >= s; break;
        case BPF_JSET: taken = (A & s) != 0; break;
        }
        pc += taken ? i->jt : i->jf;
    }
    return 0; /* unreachable: a checked program ends with a return */
}

static int32_t action_only(uint32_t ret) { return (int32_t)(ret & LX_SECCOMP_RET_ACTION_FULL); }

static uint32_t run_filters(const struct lkd_seccomp_filter *f, const struct lx_seccomp_data *d)
{
    uint32_t ret = LX_SECCOMP_RET_ALLOW;
    for (; f; f = f->prev) {
        uint32_t cur = run_one(f, d);
        if (action_only(cur) < action_only(ret)) ret = cur;
    }
    return ret;
}

/* ---- Filter objects ------------------------------------------------------------------------------- */

static void filter_get(struct lkd_seccomp_filter *f)
{
    if (f) atomic_fetch_add(&f->refs, 1);
}

static void filter_put(struct lkd_seccomp_filter *f)
{
    while (f && atomic_fetch_sub(&f->refs, 1) == 1) {
        struct lkd_seccomp_filter *prev = f->prev;
        free(f);
        f = prev;
    }
}

static int is_ancestor(const struct lkd_seccomp_filter *parent, const struct lkd_seccomp_filter *child)
{
    if (!parent) return 1; /* NULL is the ancestor of every chain */
    for (; child; child = child->prev)
        if (child == parent) return 1;
    return 0;
}

static int filter_count(const struct lkd_seccomp_filter *f)
{
    int n = 0;
    for (; f; f = f->prev) n++;
    return n;
}

void lkd_seccomp_fork(lkd_task *child, lkd_task *parent)
{
    if (!child || !parent) return;
    struct lkd_seccomp_filter *f = atomic_load(&parent->seccomp_filter);
    filter_get(f);
    atomic_store(&child->seccomp_filter, f);
    atomic_store(&child->seccomp_mode, atomic_load(&parent->seccomp_mode));
}

void lkd_seccomp_release(lkd_task *t)
{
    if (!t) return;
    filter_put(atomic_exchange(&t->seccomp_filter, NULL));
}

void lkd_seccomp_status(lkd_task *t, int *mode, int *filters)
{
    *mode = atomic_load(&t->seccomp_mode);
    *filters = filter_count(atomic_load(&t->seccomp_filter));
}

/* ---- Enforcement ----------------------------------------------------------------------------------- */

static int core_dumps(struct lkd_proc *p)
{
    pthread_mutex_lock(&p->lock);
    int core = p->rlimits[LX_RLIMIT_CORE].rlim_cur != 0 && p->dumpable;
    pthread_mutex_unlock(&p->lock);
    return core;
}

static int live_threads(struct lkd_proc *p)
{
    pthread_mutex_lock(&p->lock);
    int n = p->nthreads;
    pthread_mutex_unlock(&p->lock);
    return n;
}

/* The filter kills: the thread alone (do_exit) unless the action names the process or the thread is the
 * last one, which dies by signo as force_sig_seccomp(force_coredump) makes it. */
static int64_t kill_task(lkd_task *t, int signo, int whole_process)
{
    if (whole_process || live_threads(t->proc) <= 1) {
        int status = signo;
        if (signo == LX_SIGSYS && core_dumps(t->proc)) status |= 0x80;
        return lkd_task_do_exit_group(t, status);
    }
    return lkd_task_do_exit(t, signo);
}

/* __secure_computing_strict: read, write, exit and rt_sigreturn only. */
static int strict_allows(uint64_t nr)
{
    return nr == LKD_NR_read || nr == LKD_NR_write || nr == LKD_NR_exit || nr == LKD_NR_rt_sigreturn;
}

int lkd_seccomp_enter(lkd_task *t, const lkd_sysreq *rq, int64_t *ret)
{
    int mode = atomic_load(&t->seccomp_mode);
    if (mode == LX_SECCOMP_MODE_DISABLED) return 0;
    if (mode == LX_SECCOMP_MODE_STRICT) {
        if (strict_allows(rq->nr)) return 0;
        *ret = kill_task(t, LX_SIGKILL, 0);
        return 1;
    }

    struct lx_seccomp_data d;
    memset(&d, 0, sizeof d);
    d.nr = (int32_t)rq->nr;
    d.arch = LX_AUDIT_ARCH_X86_64;
    memcpy(d.args, rq->a, sizeof d.args);
    const lkd_engine_ops *ops = lkd_engine();
    lkd_regs regs;
    if (ops && ops->get_regs && ops->get_regs(t, &regs) == 0) d.instruction_pointer = regs.rip;

    uint32_t r = run_filters(atomic_load(&t->seccomp_filter), &d);
    uint32_t data = r & LX_SECCOMP_RET_DATA;
    switch (r & LX_SECCOMP_RET_ACTION_FULL) {
    case LX_SECCOMP_RET_ALLOW:
        return 0;
    case LX_SECCOMP_RET_LOG:
        lkd_log_once("seccomp: a filter logged syscall %d (SECCOMP_RET_LOG), which runs", d.nr);
        return 0;
    case LX_SECCOMP_RET_ERRNO:
        *ret = -(int64_t)(data > MAX_ERRNO ? MAX_ERRNO : data);
        return 1;
    case LX_SECCOMP_RET_TRAP: {
        /* syscall_rollback leaves the number in RAX; force_sig_seccomp(this_syscall, data, false) */
        struct lx_siginfo info;
        memset(&info, 0, sizeof info);
        info.si_signo = LX_SIGSYS;
        info.si_errno = (int32_t)data;
        info.si_code = LX_SYS_SECCOMP;
        info.u.sys.call_addr = d.instruction_pointer;
        info.u.sys.syscall = d.nr;
        info.u.sys.arch = LX_AUDIT_ARCH_X86_64;
        lkd_sig_force_sync(t, &info);
        *ret = (int64_t)rq->nr;
        return 1;
    }
    case LX_SECCOMP_RET_TRACE:
        if (atomic_load(&t->ptrace.flags) & LX_PTRACE_O_TRACESECCOMP)
            lkd_log_once("seccomp: SECCOMP_RET_TRACE with PTRACE_O_TRACESECCOMP: no PTRACE_EVENT_SECCOMP stop is modeled");
        *ret = -LX_ENOSYS;
        return 1;
    case LX_SECCOMP_RET_USER_NOTIF:
        *ret = -LX_ENOSYS; /* no listener can exist */
        return 1;
    case LX_SECCOMP_RET_KILL_THREAD:
        *ret = kill_task(t, LX_SIGSYS, 0);
        return 1;
    default: /* SECCOMP_RET_KILL_PROCESS and the actions Linux does not know */
        *ret = kill_task(t, LX_SIGSYS, 1);
        return 1;
    }
}

/* ---- Installing ----------------------------------------------------------------------------------- */

/* seccomp_may_assign_mode: a thread keeps the mode it has. */
static int may_assign_mode(lkd_task *t, int mode)
{
    int cur = atomic_load(&t->seccomp_mode);
    return cur == LX_SECCOMP_MODE_DISABLED || cur == mode;
}

static int64_t set_mode_strict(lkd_task *t)
{
    if (!may_assign_mode(t, LX_SECCOMP_MODE_STRICT)) return -LX_EINVAL;
    atomic_store(&t->seccomp_mode, LX_SECCOMP_MODE_STRICT);
    return 0;
}

#define FILTER_FLAGS (LX_SECCOMP_FILTER_FLAG_TSYNC | LX_SECCOMP_FILTER_FLAG_LOG | LX_SECCOMP_FILTER_FLAG_SPEC_ALLOW | \
                      LX_SECCOMP_FILTER_FLAG_NEW_LISTENER | LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH | \
                      LX_SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV)

/* seccomp_prepare_user_filter: a checked copy of the guest's program. */
static int64_t prepare_filter(lkd_task *t, uint64_t uprog, struct lkd_seccomp_filter **out)
{
    struct lx_sock_fprog prog;
    if (lkd_user_read(uprog, &prog, sizeof prog)) return -LX_EFAULT;
    if (prog.len == 0 || prog.len > LX_BPF_MAXINSNS) return -LX_EINVAL;
    /* no_new_privs, or CAP_SYS_ADMIN, keeps a filter from steering a more privileged program. */
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    int allowed = p->no_new_privs || (p->cred.cap_eff >> CAP_SYS_ADMIN) & 1;
    pthread_mutex_unlock(&p->lock);
    if (!allowed) return -LX_EACCES;
    struct lkd_seccomp_filter *f = malloc(sizeof *f + prog.len * sizeof(struct lx_sock_filter));
    if (!f) return -LX_ENOMEM;
    if (lkd_user_read(prog.filter, f->insns, prog.len * sizeof(struct lx_sock_filter))) {
        free(f);
        return -LX_EFAULT;
    }
    /* bpf_prog_create_from_user runs seccomp_check_filter, then bpf_check_classic */
    int r = seccomp_codes(f->insns, prog.len);
    if (!r) r = check_classic(f->insns, prog.len);
    if (r) {
        free(f);
        return r;
    }
    atomic_init(&f->refs, 1);
    f->prev = NULL;
    f->log = 0;
    f->len = prog.len;
    *out = f;
    return 0;
}

/* seccomp_can_sync_threads: every other thread runs an ancestor of the caller's filters. Returns 0, or the
 * tid of the first that does not. */
static int32_t sync_blocker(lkd_task *t, const struct lkd_seccomp_filter *mine)
{
    for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc) {
        if (x == t || (atomic_load(&x->flags) & LKD_TF_EXITING)) continue;
        int mode = atomic_load(&x->seccomp_mode);
        if (mode == LX_SECCOMP_MODE_STRICT || !is_ancestor(atomic_load(&x->seccomp_filter), mine)) return x->tid;
    }
    return 0;
}

static int64_t set_mode_filter(lkd_task *t, unsigned flags, uint64_t uprog)
{
    if (flags & ~FILTER_FLAGS) return -LX_EINVAL;
    if (flags & LX_SECCOMP_FILTER_FLAG_NEW_LISTENER) {
        lkd_log_once("seccomp: SECCOMP_FILTER_FLAG_NEW_LISTENER refused: user notification is not modeled");
        return -LX_EINVAL;
    }
    if (flags & LX_SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV) return -LX_EINVAL; /* only with NEW_LISTENER */
    struct lkd_seccomp_filter *f = NULL;
    int64_t r = prepare_filter(t, uprog, &f);
    if (r) return r;
    f->log = (flags & LX_SECCOMP_FILTER_FLAG_LOG) != 0;
    if (!may_assign_mode(t, LX_SECCOMP_MODE_FILTER)) {
        filter_put(f);
        return -LX_EINVAL;
    }

    struct lkd_seccomp_filter *mine = atomic_load(&t->seccomp_filter);
    unsigned total = f->len;
    for (const struct lkd_seccomp_filter *w = mine; w; w = w->prev) total += w->len + 4;
    if (total > MAX_INSNS_PER_PATH) {
        filter_put(f);
        return -LX_ENOMEM;
    }

    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    if (flags & LX_SECCOMP_FILTER_FLAG_TSYNC) {
        int32_t tid = sync_blocker(t, mine);
        if (tid) {
            pthread_mutex_unlock(&p->lock);
            filter_put(f);
            return (flags & LX_SECCOMP_FILTER_FLAG_TSYNC_ESRCH) ? -LX_ESRCH : tid;
        }
    }
    /* seccomp_attach_filter: the thread's reference to its chain moves into the new filter. */
    f->prev = mine;
    atomic_store(&t->seccomp_filter, f);
    atomic_store(&t->seccomp_mode, LX_SECCOMP_MODE_FILTER);
    if (flags & LX_SECCOMP_FILTER_FLAG_TSYNC) {
        /* seccomp_sync_threads. The filter another thread drops is an ancestor of f, which keeps it. */
        for (lkd_task *x = p->threads; x; x = x->next_in_proc) {
            if (x == t || (atomic_load(&x->flags) & LKD_TF_EXITING)) continue;
            filter_get(f);
            filter_put(atomic_exchange(&x->seccomp_filter, f));
            atomic_store(&x->seccomp_mode, LX_SECCOMP_MODE_FILTER);
        }
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}

static int64_t action_avail(uint64_t uaction)
{
    uint32_t action;
    if (lkd_user_read(uaction, &action, sizeof action)) return -LX_EFAULT;
    switch (action) {
    case LX_SECCOMP_RET_KILL_PROCESS: case LX_SECCOMP_RET_KILL_THREAD: case LX_SECCOMP_RET_TRAP:
    case LX_SECCOMP_RET_ERRNO: case LX_SECCOMP_RET_TRACE: case LX_SECCOMP_RET_LOG: case LX_SECCOMP_RET_ALLOW:
        return 0;
    default: /* SECCOMP_RET_USER_NOTIF: no listener can be created */
        return -LX_EOPNOTSUPP;
    }
}

static int64_t do_seccomp(lkd_task *t, unsigned op, unsigned flags, uint64_t uargs)
{
    switch (op) {
    case LX_SECCOMP_SET_MODE_STRICT:
        if (flags || uargs) return -LX_EINVAL;
        return set_mode_strict(t);
    case LX_SECCOMP_SET_MODE_FILTER:
        return set_mode_filter(t, flags, uargs);
    case LX_SECCOMP_GET_ACTION_AVAIL:
        if (flags) return -LX_EINVAL;
        return action_avail(uargs);
    case LX_SECCOMP_GET_NOTIF_SIZES: {
        if (flags) return -LX_EINVAL;
        /* sizeof struct seccomp_notif, seccomp_notif_resp and seccomp_data */
        struct lx_seccomp_notif_sizes sizes = { 80, 24, sizeof(struct lx_seccomp_data) };
        return lkd_user_write(uargs, &sizes, sizeof sizes) ? -LX_EFAULT : 0;
    }
    default:
        return -LX_EINVAL;
    }
}

int64_t task_seccomp_prctl_get(lkd_task *t)
{
    return atomic_load(&t->seccomp_mode);
}

/* prctl(PR_SET_SECCOMP, mode, filter): the strict mode, or a filter with no flag. */
int64_t task_seccomp_prctl_set(lkd_task *t, uint64_t mode, uint64_t ufilter)
{
    switch (mode) {
    case LX_SECCOMP_MODE_STRICT: return do_seccomp(t, LX_SECCOMP_SET_MODE_STRICT, 0, ufilter);
    case LX_SECCOMP_MODE_FILTER: return do_seccomp(t, LX_SECCOMP_SET_MODE_FILTER, 0, ufilter);
    default: return -LX_EINVAL;
    }
}

LKD_SYSCALL(seccomp)
{
    return do_seccomp(t, LKD_ARGUI(0), LKD_ARGUI(1), LKD_ARGP(2));
}

void task_seccomp_register(void)
{
    LKD_REGISTER(seccomp);
}

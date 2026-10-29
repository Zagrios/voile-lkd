/* dispatch.c: syscall table, registration, lkd_syscall. Owner: core.
 *
 * The table is filled during lkd_init (single thread) and read-only afterwards, so the hot path takes no
 * lock. Unimplemented numbers return -ENOSYS, counted per number, logged once per number when tracing
 * or LKD_LOG_ENOSYS is on.
 */
#include <string.h>

#include "lkd_internal.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task.h"

static lkd_sysfn g_table[LKD_NR_TABLE_SIZE];
static const char *g_owner[LKD_NR_TABLE_SIZE];
static unsigned g_registered;
static int g_conflicts;
static const char *g_init_module = "";

static _Atomic uint64_t g_enosys[LKD_NR_TABLE_SIZE];
static _Atomic uint64_t g_enosys_oob;
static _Atomic unsigned char g_enosys_logged[LKD_NR_TABLE_SIZE];

int lkd_trace_level(void);
int lkd_enosys_logging(void);
void lkd_trace_entry(lkd_task *t, const lkd_sysreq *rq);
void lkd_trace_exit(lkd_task *t, const lkd_sysreq *rq, int64_t ret);

const char *lkd_current_init_module(void) { return g_init_module; }
void lkd_dispatch_set_init_module(const char *name) { g_init_module = name; }
int lkd_dispatch_conflicts(void) { return g_conflicts; }
unsigned lkd_registered_count(void) { return g_registered; }

int lkd_register_syscall(unsigned nr, const char *name, lkd_sysfn fn)
{
    if (nr >= LKD_NR_TABLE_SIZE || !fn || !name) return -LX_EINVAL;
    if (g_table[nr]) {
        g_conflicts++;
        lkd_log("syscall %u (%s) registered by %s is already owned by %s", nr, name, g_init_module,
                g_owner[nr] ? g_owner[nr] : "?");
        return -LX_EEXIST;
    }
    g_table[nr] = fn;
    g_owner[nr] = g_init_module[0] ? g_init_module : "?";
    g_registered++;
    return 0;
}

const char *lkd_syscall_owner(unsigned nr)
{
    return nr < LKD_NR_TABLE_SIZE && g_table[nr] ? g_owner[nr] : NULL;
}

/* Forget every registration (unit tests of the dispatcher itself). */
void lkd_dispatch_reset(void)
{
    memset(g_table, 0, sizeof g_table);
    memset(g_owner, 0, sizeof g_owner);
    g_registered = 0;
    g_conflicts = 0;
}

uint64_t lkd_enosys_count(unsigned nr)
{
    return nr < LKD_NR_TABLE_SIZE ? atomic_load(&g_enosys[nr]) : atomic_load(&g_enosys_oob);
}

static int64_t enosys(const lkd_sysreq *rq)
{
    if (rq->nr < LKD_NR_TABLE_SIZE) {
        atomic_fetch_add(&g_enosys[rq->nr], 1);
        if ((lkd_trace_level() || lkd_enosys_logging()) && !atomic_exchange(&g_enosys_logged[rq->nr], 1)) {
            const char *n = lkd_syscall_name(rq->nr);
            lkd_log("unimplemented syscall %llu (%s) -> ENOSYS", (unsigned long long)rq->nr, n ? n : "unassigned");
        }
    } else {
        atomic_fetch_add(&g_enosys_oob, 1);
    }
    return -LX_ENOSYS;
}

int64_t lkd_dispatch(lkd_task *t, const lkd_sysreq *rq)
{
    lkd_sysfn fn = rq->nr < LKD_NR_TABLE_SIZE ? g_table[rq->nr] : NULL;
    return fn ? fn(t, rq) : enosys(rq);
}

/* restart_syscall(2): run the continuation stored by the interrupted syscall, else -EINTR (what
 * do_no_restart_syscall returns). The continuation is cleared when a signal handler runs (sig module). */
LKD_SYSCALL(restart_syscall)
{
    if (!t->restart.fn) return -LX_EINTR;
    return t->restart.fn(t);
}

void lkd_core_register_syscalls(void)
{
    LKD_REGISTER(restart_syscall);
}

/* syscall_user_dispatch (kernel/entry/syscall_user_dispatch.c), checked before ptrace as Linux does: is the
 * syscall to be turned into SIGSYS instead of run? The selector byte is read at each syscall: 0 ALLOW, 1
 * BLOCK, anything else kills the process with SIGSYS, an unreadable one with SIGSEGV. Returns 0 to run the
 * syscall, 1 when it was dispatched, 2 when the engine completed it instead of the SIGSYS (ops->sud_shortcut,
 * never for a traced task: its tracer sees every signal). */
static int user_dispatch(lkd_task *t, uint64_t nr)
{
    const lkd_engine_ops *ops = lkd_engine();
    lkd_regs r;
    if (!ops || !ops->get_regs || ops->get_regs(t, &r) < 0) return 0;
    if (r.rip - t->sud_offset < t->sud_len) return 0; /* the address after the syscall instruction */
    if (t->sud_selector) {
        uint8_t state;
        if (lkd_user_read(t->sud_selector, &state, 1)) { lkd_sig_force_exit(t, LX_SIGSEGV); return 1; }
        if (state == 0) return 0;
        if (state != 1) { lkd_sig_force_exit(t, LX_SIGSYS); return 1; }
    }
    if (ops->sud_shortcut && !(atomic_load(&t->ptrace.flags) & LKD_PT_TRACED) && ops->sud_shortcut(t, nr)) return 2;
    lkd_sig_user_dispatch(t, r.rip, (int)nr);
    return 1;
}

lkd_disp lkd_syscall(lkd_task *t, const lkd_sysreq *in, int64_t *rax)
{
    /* A dispatched syscall is not run and RAX keeps its number (syscall_rollback). */
    if (t->sud_on) {
        int d = user_dispatch(t, in->nr);
        if (d == 2) return LKD_CONTEXT_REPLACED;
        if (d) {
            *rax = (int64_t)in->nr;
            return LKD_RETURN;
        }
    }
    lkd_sysreq rq = *in;
    uint64_t orig_nr = in->nr;
    lkd_disp disp = LKD_RETURN;
    int64_t ret = 0;

    atomic_store(&t->cur_nr, (int64_t)rq.nr);
    memcpy(t->cur_args, rq.a, sizeof t->cur_args);

    int traced = (atomic_load(&t->ptrace.flags) & LKD_PT_SYSCALL_TRACE) != 0;
    int skipped = 0;
    if (traced) {
        int64_t skip_ret = 0;
        int e = lkd_ptrace_syscall_enter(t, &rq, &skip_ret);
        if (e < 0) { atomic_store(&t->cur_nr, -1); *rax = -LX_EINTR; return LKD_RETURN; }
        if (e > 0) { skipped = 1; ret = skip_ret; }
        else { atomic_store(&t->cur_nr, (int64_t)rq.nr); memcpy(t->cur_args, rq.a, sizeof t->cur_args); }
    }
    if (lkd_trace_level() >= 2) lkd_trace_entry(t, &rq);

    if (!skipped) ret = lkd_dispatch(t, &rq);

    if (ret == LKD_RET_CONTEXT_REPLACED) { disp = LKD_CONTEXT_REPLACED; ret = 0; }
    else if (ret == LKD_RET_EXIT_THREAD) { disp = LKD_EXIT_THREAD; ret = 0; }
    else if (ret == LKD_RET_EXIT_GROUP) { disp = LKD_EXIT_GROUP; ret = 0; }
    else if (ret == -LX_ENOIOCTLCMD) ret = -LX_ENOTTY;
    else if (ret <= -LX_ERESTARTSYS && ret >= -LX_ERESTART_RESTARTBLOCK) {
        /* Interrupted wait: restart or EINTR depending on the signal that will be delivered. */
        switch (lkd_sig_restart_decision(t, (int)-ret)) {
        case LKD_RESTART_SAME: disp = LKD_RESTART; ret = (int64_t)orig_nr; break;
        case LKD_RESTART_BLOCK: disp = LKD_RESTART; ret = LKD_NR_restart_syscall; break;
        default: ret = -LX_EINTR; break;
        }
    }

    /* syscall_exit_work() reads the work flags again: PTRACE_SYSCALL given at a stop inside the call (the
     * exec event) makes an exit stop. It also reports the exit of execve and rt_sigreturn (ptrace-1). */
    traced = (atomic_load(&t->ptrace.flags) & LKD_PT_SYSCALL_TRACE) != 0;
    if (disp == LKD_RETURN && traced) lkd_ptrace_syscall_exit(t, &ret);
    else if (disp == LKD_CONTEXT_REPLACED && traced) lkd_ptrace_syscall_exit_replaced(t);
    if (lkd_trace_level()) lkd_trace_exit(t, &rq, disp == LKD_RESTART ? -LX_ERESTARTSYS : ret);
    atomic_store(&t->cur_nr, -1);
    lkd_mm_guard_close_all(); /* guarded host pages opened for this call's copies (src/mm/watch.c) */
    *rax = ret;
    return disp;
}

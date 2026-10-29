/* lkd.h: public API between a CPU engine (FEXCore host) and LKD, the Linux kernel on Darwin.
 *
 * LKD is the x86-64 Linux syscall layer. The engine executes guest x86-64 code and calls LKD for
 * every guest SYSCALL; LKD calls back into the engine through lkd_engine_ops for things only the
 * engine can do (read and write registers, interrupt a running thread, build a signal frame...).
 *
 * Link rule: liblkd.a holds weak default hooks (core) and strong module code. Link it with
 * -Wl,-force_load,liblkd.a (or -all_load); a plain archive link may keep the weak placeholders.
 *
 * Conventions used in the comments below:
 *   OWNER   = the host thread that currently runs the guest task (the only caller allowed).
 *   ANY     = any host thread, any time.
 *   Locks   = see docs/CONVENTIONS.md for the global lock order. "takes no lock" means the call is
 *             safe while the engine holds its own locks, "may block" means it can sleep.
 *   The engine must never hold an engine lock that a kick handler or ops callback takes while it
 *   calls into LKD, and LKD never holds a kernel lock while it runs guest code or calls the ops
 *   marked "called without LKD locks".
 */
#ifndef LKD_LKD_H
#define LKD_LKD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <lkd/abi_x64.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 1. Guest memory ------------------------------------------------------------------------- */

/* The guest address space is a window of the host address space: guest address g is at host address
 * lkd_guest_base + g. The mm module reserves the window and sets lkd_guest_base in lkd_init().
 * Unit tests that use host memory as guest memory leave it at 0 (guest pointer == host pointer).
 * Written once before the first guest instruction runs, read-only afterwards. */
extern uint64_t lkd_guest_base;

static inline void *lkd_g2h(uint64_t gva) { return (void *)(uintptr_t)(lkd_guest_base + gva); }
static inline uint64_t lkd_h2g(const void *hva) { return (uint64_t)(uintptr_t)hva - lkd_guest_base; }

/* Fault-safe copies between guest and kernel memory. ANY thread, takes only a leaf lock briefly
 * (lkd_mm_check_range), safe under any other lock, never blocks. Return 0 or -LX_EFAULT.
 * A partial copy may have happened before a fault. Guest pointers must NEVER be dereferenced by any
 * other means in LKD code. Do not keep a host pointer obtained with lkd_g2h across a blocking call.
 * lkd_user_strn copies a NUL-terminated string (dst capacity max, includes the NUL) and returns its
 * length without the NUL, -LX_EFAULT, or -LX_ENAMETOOLONG when no NUL fits in max bytes (dst is then
 * NUL-terminated at max-1). lkd_user_zero writes n zero bytes. */
int lkd_user_read(uint64_t gva, void *dst, size_t n);
int lkd_user_write(uint64_t gva, const void *src, size_t n);
int64_t lkd_user_strn(uint64_t gva, char *dst, size_t max);
int lkd_user_zero(uint64_t gva, size_t n);

/* Fault recovery for the user copies uses a SIGSEGV/SIGBUS handler installed by lkd_init() and
 * chained to any previous handler. Engines that install their own SIGSEGV/SIGBUS handlers AFTER
 * lkd_init() must call lkd_fault_try_recover(ucontext) first thing in their handler: it returns 1
 * (and has redirected the context to the -EFAULT path, the engine just returns from its handler)
 * when the faulting PC is inside an LKD user copy, else 0 and the engine continues. Async-signal-safe. */
int lkd_fault_try_recover(void *ucontext);

/* ---- 2. Objects and data types ---------------------------------------------------------------- */

typedef struct lkd_task lkd_task; /* one guest thread; opaque here, see src/include/task.h */
struct lkd_exec_image;

typedef struct lx_user_regs lkd_regs; /* struct user_regs_struct: 27 words, order of ptrace GETREGS */

/* x86-64 syscall request: rax number, rdi rsi rdx r10 r8 r9 arguments. RCX/R11 are the engine's. */
typedef struct lkd_sysreq {
    uint64_t nr;
    uint64_t a[6];
} lkd_sysreq;

typedef enum lkd_disp {
    LKD_RETURN = 0,           /* write *rax into guest RAX and continue */
    LKD_CONTEXT_REPLACED = 1, /* registers were replaced through ops->set_regs (exec, rt_sigreturn):
                                 continue at the new context, do not write RAX */
    LKD_EXIT_THREAD = 2,      /* this thread has exited: unwind, then lkd_task_release() */
    LKD_EXIT_GROUP = 3,       /* the whole process exits: same, every other thread was kicked */
    LKD_RESTART = 4           /* syscall interrupted by a signal and restartable: set RIP -= 2 and
                                 RAX = *rax (the number to restart with: the original number, or
                                 LKD_NR_restart_syscall), keep the six argument registers, then
                                 deliver pending signals (the frame saves the restarted context) */
} lkd_disp;

/* Description of one signal delivery, filled by LKD, consumed by ops->frame_build. */
typedef struct lkd_delivery {
    int signo;
    struct lx_siginfo info;   /* copied into the frame (rt_sigframe.info) */
    uint64_t handler;         /* guest handler address */
    uint32_t flags;           /* LX_SA_* of the action (SA_SIGINFO, SA_RESTORER, ...) */
    uint64_t restorer;        /* sa_restorer, becomes pretcode */
    uint64_t old_mask;        /* mask before delivery: stored in uc_sigmask */
    uint64_t new_mask;        /* mask now in force for the handler (already applied to the task) */
    int use_altstack;         /* build the frame on the alternate signal stack */
    struct lx_stack_t altstack; /* task sigaltstack with ss_flags computed for uc_stack */
} lkd_delivery;

/* FP state kinds for ops->get_fp / set_fp. */
enum { LKD_FP_FXSAVE = 1, /* 512 bytes, struct lx_user_fpregs (NT_PRFPREG) */
       LKD_FP_XSAVE = 2   /* full XSAVE image (NT_X86_XSTATE), size reported in *len */ };

/* ---- 3. Engine services ----------------------------------------------------------------------- */

/* Registered once with lkd_init(). Every pointer except the optional ones is mandatory. The struct is
 * copied. Callbacks marked [owner] are only called on the host thread that runs the task, at a safe
 * point (inside lkd_syscall, lkd_signal_next, lkd_exec_commit...). Callbacks marked [any] may be called
 * from any host thread, including the helper thread of another process' cooperative channel. Nothing
 * here is called with an LKD lock held unless noted. */
typedef struct lkd_engine_ops {
    size_t size;              /* sizeof(lkd_engine_ops), for forward compatibility */

    /* Read or write the guest registers of t. [owner] for t == the calling task. [any] for a task that
     * is stopped (ptrace stop, exit, not yet started): its state is in ordinary memory then. For a
     * running task other than the caller the call must return -LX_ESRCH rather than guess. */
    int (*get_regs)(lkd_task *t, lkd_regs *out);
    int (*set_regs)(lkd_task *t, const lkd_regs *in);
    /* FP state, same rules. *len is the capacity on input and the full size on output (at most
     * capacity bytes are copied). kind is LKD_FP_*. Return -LX_EINVAL for a kind it does not have. */
    int (*get_fp)(lkd_task *t, int kind, void *buf, size_t *len);
    int (*set_fp)(lkd_task *t, int kind, const void *buf, size_t len);

    /* Make a RUNNING task reach a safe point soon, then call lkd_signal_next() and re-check its state:
     * the engine implements this with one reserved host signal (pthread_kill) plus a flag it tests at
     * block boundaries. Must be async-signal-safe-ish, not block, and be harmless when the task is
     * already at a safe point or blocked in a syscall (that wait must then return EINTR/interrupted).
     * [any] */
    void (*kick)(lkd_task *t);

    /* Create the host thread for a new guest thread. child is fully formed (tid, tgid, signal mask
     * inherited). start holds the initial registers (RAX = 0, RSP, RIP, FS base from CLONE_SETTLS...).
     * The thread must call lkd_set_current(child) first and run child until it exits. Return 0, or
     * -LX_EAGAIN/-LX_ENOMEM. [owner of the parent] */
    int (*spawn_thread)(lkd_task *child, const lkd_regs *start);

    /* Guest code in [gva, gva+len) may have changed (mprotect, munmap, mmap over code, ptrace POKE,
     * file mapping of code): drop translated code. Must complete before the caller continues. [any] */
    void (*invalidate_code)(uint64_t gva, uint64_t len);

    /* Signal frames. frame_build pushes an x86-64 rt_sigframe on the stack (or the alternate stack),
     * sets RIP = handler, RSP, RDI = signo, RSI/RDX = &info/&uc when SA_SIGINFO, RAX = 0, clears DF
     * and TF, writes d->old_mask and d->altstack into the ucontext. Return 0 or -LX_EFAULT when the
     * stack is not writable (LKD then forces SIGSEGV as Linux does). frame_restore is the body of
     * rt_sigreturn: pop the frame at RSP, restore all registers and FP state, return the saved mask
     * and stack_t. Return 0 or -LX_EFAULT (LKD kills the task with SIGSEGV). [owner] */
    int (*frame_build)(lkd_task *t, const lkd_delivery *d);
    int (*frame_restore)(lkd_task *t, uint64_t *saved_mask, struct lx_stack_t *saved_stack);

    /* execve: load the image described by img (map segments, build stack and auxv, set registers and
     * FS base). Either returns 0 with the new context installed (LKD then returns
     * LKD_CONTEXT_REPLACED) or never returns because the host re-executes the runtime (state needed
     * across the host exec travels through the family shared memory and inherited fds; see
     * docs/CONVENTIONS.md, exec). Return -errno to fail execve before the point of no return.
     * Called after lkd_exec_prepare and lkd_exec_commit. [owner] */
    int (*exec_image)(lkd_task *t, const struct lkd_exec_image *img);

    /* Engine locks around fork(), the FEX LockBeforeFork/UnlockAfterFork protocol. Called by
     * lkd_fork_host(); optional if the engine runs fork() itself with lkd_fork_prepare/finish. */
    void (*fork_prepare)(lkd_task *t);
    void (*fork_parent)(lkd_task *t, pid_t host_child);
    void (*fork_child)(lkd_task *t);

    /* Optional (NULL = unsupported, ptrace answers honestly): enable or disable hardware single-step of
     * one guest instruction for t at its next resume. Return -LX_ENOSYS if unsupported. */
    int (*set_single_step)(lkd_task *t, int enable);

    /* Optional: a syscall that syscall user dispatch is about to turn into SIGSYS (selector BLOCK, the
     * task not traced). Return 1 when the engine has completed it the way the task's SIGSYS handler
     * would (registers set, RIP left after the syscall instruction): no signal is sent and lkd_syscall
     * returns LKD_CONTEXT_REPLACED. Return 0 to have SIGSYS sent. [owner] */
    int (*sud_shortcut)(lkd_task *t, uint64_t nr);
} lkd_engine_ops;

/* ---- 4. Setup --------------------------------------------------------------------------------- */

typedef struct lkd_config {
    size_t size;                  /* sizeof(lkd_config) */
    const char *guest_root;       /* host path of the guest root file system; NULL = "/" (tests) */
    uint32_t first_pid;           /* first pid of the family; 0 = 2. The first process gets this pid */
    uint32_t uid, gid;            /* identity presented to the guest; 0 = 1000 */
    const char *hostname;         /* NULL = "obelisk" */
    uint64_t guest_window_bytes;  /* size of the reserved guest window; 0 = 2^46, tests use less */
    uint64_t guest_window_base;   /* host address wanted for the window. 0 = test identity mode: lkd_guest_base
                                   * stays 0 and the first lkd_mm_create reserves a window (at most 4 GiB)
                                   * whose guest addresses are host addresses. The engine passes a base
                                   * (0x80000000000 recommended); lkd_init reserves the window there or at
                                   * the next free candidate and sets lkd_guest_base (request mm-2). */
} lkd_config;

/* Optional, before lkd_init(): override defaults. The struct is copied. Environment (read in
 * lkd_init): LKD_TRACE=1 traces every syscall to stderr, and the signal that ends a process (sender, or faulting
 * address and rip); LKD_LOG_ENOSYS=1 logs the first call of every unimplemented syscall number. Returns 0 or
 * -LX_EINVAL. */
int lkd_init_config(const lkd_config *cfg);

/* Initialize LKD once per host process (also in a fork child: not needed, state is inherited).
 * Order: copy ops, install the SIGSEGV/SIGBUS fault-recovery handlers, read the environment, call
 * lkd_<module>_init() of mm, fs, procfs, task, sig, net, ptrace (each registers its syscalls).
 * Returns 0, or a negative Linux errno (-LX_EEXIST when two modules registered the same syscall).
 * Not thread-safe against itself; call before creating guest threads. */
int lkd_init(const lkd_engine_ops *ops);

/* The operations registered with lkd_init (never NULL after a successful init). */
const lkd_engine_ops *lkd_engine(void);

/* Specification of the first guest process of a family. */
typedef struct lkd_main_spec {
    size_t size;                  /* sizeof(lkd_main_spec) */
    const char *exe_path;         /* guest path of the executable, for /proc/self/exe */
    const char *const *argv;      /* NULL-terminated; cmdline snapshot for /proc/<pid>/cmdline */
    const char *const *envp;      /* NULL-terminated; snapshot for /proc/<pid>/environ (may be NULL) */
    const char *cwd;              /* guest working directory; NULL = "/" */
    int32_t pid;                  /* requested pid; 0 = allocate from the family counter */
    int32_t ppid;                 /* parent pid to present; 0 = the registry default (init) */
    void *engine_data;            /* stored in the task, see lkd_task_engine_data */
} lkd_main_spec;

/* Create the first task of this host process (the main thread: tid == pid, pgid/sid set so it can
 * become a session leader if the spec says so). Also creates the shared registry when this is the
 * first process of the family. Returns NULL with errno set to a host errno on failure. OWNER-less:
 * call it on the thread that will run the task, then lkd_set_current(). */
lkd_task *lkd_task_create_main(const lkd_main_spec *spec);

/* Per-thread binding and accessors. ANY, takes no lock. */
void lkd_set_current(lkd_task *t);          /* thread-local; the engine calls it when a host thread starts a task */
lkd_task *lkd_current(void);
void *lkd_task_engine_data(const lkd_task *t);
void lkd_task_set_engine_data(lkd_task *t, void *p);
int32_t lkd_task_tid(const lkd_task *t);
int32_t lkd_task_pid(const lkd_task *t);   /* tgid */

/* ---- 5. Syscalls ------------------------------------------------------------------------------ */

/* Execute one guest syscall. OWNER, may block (interruptibly). Never takes an LKD lock across its
 * return. Results:
 *   LKD_RETURN: guest RAX = *rax (a value or -errno).
 *   other dispositions: see lkd_disp.
 * Numbers >= LKD_NR_TABLE_SIZE and unregistered numbers return -LX_ENOSYS. Internal restart codes
 * (-LX_ERESTART*) are resolved here: the result is -LX_EINTR or LKD_RESTART. When the task is
 * ptrace-traced with PTRACE_SYSCALL, the syscall-entry and syscall-exit stops happen inside this call
 * (the tracer may change the registers; LKD re-reads them through ops->get_regs). */
lkd_disp lkd_syscall(lkd_task *t, const lkd_sysreq *rq, int64_t *rax);

/* ---- 6. Signals ------------------------------------------------------------------------------- */

/* Cheap check, ANY, lock-free: nonzero if a signal may be deliverable to t or t must die. The engine
 * tests it at every safe point (after lkd_syscall, after a kick, after a fault) and then calls
 * lkd_signal_next. A false positive is allowed, a false negative is a bug. */
int lkd_signal_pending(const lkd_task *t);

/* Deliver at most one signal to t now. OWNER, at a safe point (all guest registers are current in the
 * engine), may block (ptrace signal-delivery-stop, job-control stop). Dequeues the next deliverable
 * signal and, depending on its action:
 *   returns 1  a handler frame was built through ops->frame_build; the engine resumes at the new context
 *              and calls lkd_signal_next again at the next safe point.
 *   returns 0  nothing to do (none pending, ignored, default-ignored, stop already handled).
 *   returns -1 the task must die: the process or thread exit has been recorded (fatal default action
 *              or SIGKILL); the engine unwinds the host thread and calls lkd_task_release().
 * Loop until it returns 0 before resuming guest code. */
int lkd_signal_next(lkd_task *t);

/* The engine restored a signal frame itself (instead of the rt_sigreturn syscall path): tell LKD the
 * mask and stack to reinstall. OWNER. rt_sigreturn through lkd_syscall needs no extra call. */
void lkd_signal_returned(lkd_task *t, uint64_t restored_mask, const struct lx_stack_t *restored_stack);

/* Synchronous fault of the guest, translated by the engine from its own fault (page fault, #UD,
 * #DE, int3, single step). OWNER. Queues a forced synchronous signal for t (signo SIGSEGV, SIGBUS,
 * SIGILL, SIGFPE or SIGTRAP; si_code and si_addr exact: SEGV_MAPERR/SEGV_ACCERR, FPE_INTDIV,
 * SI_KERNEL for int3; err and trapno go into the frame's sigcontext). If the signal is blocked or
 * ignored the process dies (force_sig semantics). The engine then calls lkd_signal_next. */
void lkd_fault(lkd_task *t, int signo, int si_code, uint64_t addr, uint64_t err, uint64_t trapno);

/* The thread t is done running guest code (after LKD_EXIT_THREAD, LKD_EXIT_GROUP or lkd_signal_next
 * == -1): the engine frees its host thread state and then calls this. OWNER, last call on t, which is
 * invalid afterwards. Returns 1 when t was the last thread of its process: the engine must then end
 * the host process with _exit(status) (the exit status is already published in the family registry for
 * the parent's wait4) after flushing its own output. Returns 0 otherwise. */
int lkd_task_release(lkd_task *t, int *exit_status_out);

/* ---- 7. fork ---------------------------------------------------------------------------------- */

/* Guest fork/vfork/clone-without-CLONE_VM is a host fork(). macOS fork() keeps only the calling thread
 * and orphans locks held by other threads, so every LKD module takes its locks in
 * lkd_<module>_fork_prepare (ascending lock order), the host fork() happens, then the parent releases
 * (lkd_<module>_fork_parent) and the child resets (lkd_<module>_fork_child) in descending order.
 *
 * lkd_fork_host() does the whole sequence: ops->fork_prepare, module prepare hooks, fork(), module
 * hooks, ops->fork_parent/child. OWNER, may block on module locks. Returns the host pid in the parent
 * (> 0), 0 in the child (the caller is now the only thread of the new guest process; task module has
 * already switched t to the child identity), or a negative Linux errno. Used by the task module's
 * clone/fork; engines that must call fork() themselves use the two halves below instead. */
pid_t lkd_fork_host(lkd_task *t, uint64_t clone_flags);
int lkd_fork_prepare(lkd_task *t, uint64_t clone_flags); /* returns 0 or -errno; on 0 the caller MUST fork() then finish */
void lkd_fork_finish(lkd_task *t, pid_t host_child);       /* host_child == 0 in the child */

/* ---- 8. exec ---------------------------------------------------------------------------------- */

#define LKD_EXEC_MAX_ARGS_BYTES (2u << 20) /* argv + envp limit, like ARG_MAX/4 on Linux */

/* Result of lkd_exec_prepare: the file to load, ready for the engine's ELF loader. */
typedef struct lkd_exec_image {
    char *guest_path;     /* resolved guest path of the file to load (the script interpreter for #!) */
    char *host_path;      /* host path of the same file */
    int host_fd;          /* read-only host fd of the file, >= 0, CLOEXEC; closed by lkd_exec_image_free */
    int argc;
    char **argv;          /* NULL-terminated copies in host memory, after #! and execveat handling */
    char **envp;
    int interp_depth;     /* number of #! levels followed (limit 5, like Linux) */
    int set_uid_ignored;  /* 1 if the file had a set-id bit that LKD ignores (documented, never faked) */
    char *original_path;  /* the path as given to execve, for AT_EXECFN */
} lkd_exec_image;

/* Resolve path through the vfs, check execute permission, follow #! scripts, copy argv and envp.
 * argv and envp are host arrays (NULL-terminated). OWNER, may block on file I/O. Returns 0 or
 * -errno (-LX_ENOENT, -LX_EACCES, -LX_ENOEXEC for a file that is neither ELF nor script...). On
 * failure nothing is allocated. The caller owns img and frees it with lkd_exec_image_free. */
int lkd_exec_prepare(lkd_task *t, const char *path, char *const argv[], char *const envp[], lkd_exec_image *img);

/* Point of no return. OWNER. Kills the other threads of the process, resets signal handlers to default
 * (keeping ignored ones), closes O_CLOEXEC descriptors, tears down the mm, resets dumpable, renames
 * comm/exe/cmdline in the registry, delivers the ptrace TRACEEXEC stop after ops->exec_image() has
 * installed the new context (the stop is reported from lkd_exec_finish). */
void lkd_exec_commit(lkd_task *t, const lkd_exec_image *img);

/* Called after ops->exec_image() returned 0: the new image is in place. OWNER. Reports the
 * PTRACE_EVENT_EXEC stop (or the SIGTRAP after exec for a plain PTRACE_TRACEME tracee). */
void lkd_exec_finish(lkd_task *t);

void lkd_exec_image_free(lkd_exec_image *img);

/* ---- 9. ptrace -------------------------------------------------------------------------------- */

typedef enum lkd_stop_kind {
    LKD_STOP_SYSCALL_ENTRY = 1, /* done by lkd_syscall itself; listed for completeness */
    LKD_STOP_SYSCALL_EXIT,
    LKD_STOP_SIGNAL,            /* signal-delivery-stop (done inside lkd_signal_next) */
    LKD_STOP_EXEC,              /* PTRACE_EVENT_EXEC / post-exec SIGTRAP */
    LKD_STOP_EVENT,             /* other PTRACE_EVENT_* stop: signo carries the event number */
    LKD_STOP_GROUP              /* group-stop */
} lkd_stop_kind;

/* Park the calling task in a ptrace stop of the given kind and block until its tracer resumes it.
 * OWNER, may block, called with no LKD lock held. If the task is not traced it returns 0 at once
 * without stopping. On return *resume_signo is the signal the tracer asks to deliver (0 = none) and
 * the registers may have been changed by the tracer (the caller re-reads them if it needs them).
 * Returns 0, or -1 if the task was killed while stopped (caller unwinds like lkd_signal_next == -1).
 * Normally called by LKD itself; the engine uses it only for stops LKD cannot see. */
int lkd_ptrace_stop(lkd_task *t, lkd_stop_kind kind, int signo, int *resume_signo);

/* ---- 10. Diagnostics -------------------------------------------------------------------------- */

/* Syscall tracing (also enabled by LKD_TRACE=1 at init): one line per syscall on stderr,
 * "[tid] name(0xa0, 0xa1, ...) = ret", trailing zero arguments omitted, errors as "-2 (ENOENT)".
 * Never prints guest memory contents. */
void lkd_trace_enable(int on);

/* Number of times syscall nr hit the unimplemented path (-ENOSYS) since init. ANY, lock-free. */
uint64_t lkd_enosys_count(unsigned nr);

#ifdef __cplusplus
}
#endif
#endif /* LKD_LKD_H */

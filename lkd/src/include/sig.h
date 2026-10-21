/* sig.h: signal state, sending and delivery queries. OWNER: sig (signals) and futex.h (futex).
 * Core provides weak placeholders (src/core/stubs_sig.c): nothing is ever pending.
 *
 * Model. Signals are software queues (Darwin has no real-time signals, tgkill or sigqueue). The
 * per-thread and per-process pending sets live in the task structures (task.h); the actions live in
 * the shared struct lkd_sighand. A single host signal, chosen by the engine, only interrupts a running
 * thread (ops->kick). Cross-process delivery goes through a mailbox serviced by a helper thread of the
 * target process; the helper queues the signal in the target and kicks it.
 */
#ifndef LKD_SIG_H
#define LKD_SIG_H

#include "task.h"

/* Signal actions shared by the threads of a process (CLONE_SIGHAND). Defined by the sig module in
 * src/sig; other modules treat it as opaque and use the functions below. */
struct lkd_sighand;
struct lkd_sigproc;

/* ---- Sending (any module) ---------------------------------------------------------------------- */

/* Queue `signo` for the process `pid` (kill semantics: any thread that does not block it may take it;
 * the sig module picks the target thread). `info` may be NULL, meaning an SI_USER siginfo built from the
 * sender (pid and uid of `sender`, which may be NULL for kernel-originated signals: si_code SI_KERNEL).
 * Works across processes of the family. signo 0 only checks existence and permission. Returns 0 or
 * -LX_ESRCH, -LX_EPERM, -LX_EINVAL. ANY, may take sig and registry locks. */
int lkd_sig_send_pid(lkd_task *sender, int32_t pid, int signo, const struct lx_siginfo *info);
/* Thread-directed (tgkill/tkill): tgid 0 means "any thread group". */
int lkd_sig_send_tid(lkd_task *sender, int32_t tgid, int32_t tid, int signo, const struct lx_siginfo *info);
/* Process group (kill with pid <= 0 or killpg): pgrp > 0, or pgrp == -1 for "every process the caller
 * may signal". Returns the number of processes signalled, or -errno. */
int lkd_sig_send_pgrp(lkd_task *sender, int32_t pgrp, int signo, const struct lx_siginfo *info);
/* Directly to a local task that the caller holds (no permission check): SIGPIPE to self, SIGCHLD to a
 * thread of the parent, SIGALRM from a timer, the fault path. thread_directed selects t's own pending
 * set instead of the process set. */
void lkd_sig_send_task(lkd_task *target, int signo, const struct lx_siginfo *info, int thread_directed);

/* SIGCHLD to the parent of a child whose state changed (code is a CLD_*, status the exit status or
 * signal). Honors SA_NOCLDSTOP, SA_NOCLDWAIT and SIG_IGN for the parent (which can make the child
 * auto-reaped: returns 1 then, and the task module must not keep the zombie). */
int lkd_sig_child_event(int32_t parent_pid, int32_t child_pid, uint32_t child_uid, int code, int status);

/* ---- Queries ------------------------------------------------------------------------------------ */

/* Is t interrupted: a signal that is neither blocked nor ignored is pending, or LKD_TF_FATAL is set,
 * or a stop is requested. ANY, lock-free best effort (false positives allowed). This is the predicate
 * behind lkd_block_wait_*; it must be cheap and must not block. */
int lkd_sig_interrupted(const lkd_task *t);

/* Decision for a syscall that returned -LX_ERESTARTSYS/-LX_ERESTARTNOINTR/-LX_ERESTARTNOHAND/
 * -LX_ERESTART_RESTARTBLOCK. Looks at the signal that lkd_signal_next would deliver now (without
 * dequeuing it) and applies the Linux rules (arch/x86/kernel/signal.c, handle_signal and
 * arch_do_signal_or_restart): no signal to deliver -> restart (RESTARTBLOCK via restart_syscall);
 * handler with SA_RESTART -> restart for ERESTARTSYS; any other handler -> -EINTR;
 * ERESTARTNOINTR always restarts. Returns LKD_RESTART_NONE, LKD_RESTART_SAME or LKD_RESTART_BLOCK. */
enum { LKD_RESTART_NONE = 0, LKD_RESTART_SAME = 1, LKD_RESTART_BLOCK = 2 };
int lkd_sig_restart_decision(lkd_task *t, int restart_errno);

/* Temporary signal mask (sigsuspend, pselect6, ppoll, epoll_pwait, rt_sigtimedwait use it): installs
 * `mask` (SIGKILL and SIGSTOP removed), saves the previous one in t->saved_sigmask and sets
 * t->restore_sigmask. lkd_signal_next then either puts the saved mask in the frame as the handler's
 * saved mask (a signal is delivered, rt_sigreturn restores it) or restores it itself (nothing
 * delivered). OWNER. */
void lkd_sig_set_temp_mask(lkd_task *t, uint64_t mask);
/* Permanent mask change (rt_sigprocmask). Removes SIGKILL/SIGSTOP. */
void lkd_sig_set_mask(lkd_task *t, uint64_t mask);

/* A forced synchronous signal that is not an engine fault, with the complete siginfo (the SIGSYS of
 * seccomp's SECCOMP_RET_TRAP, force_sig_seccomp). OWNER. As lkd_fault: a blocked or ignored signal gets
 * its default action back, err and trapno of the frame's sigcontext are 0. */
void lkd_sig_force_sync(lkd_task *t, const struct lx_siginfo *info);

/* ---- Lifecycle hooks used by the task module --------------------------------------------------- */

/* New thread or process: copy the mask, share or copy the actions per clone flags (CLONE_SIGHAND shares;
 * otherwise a copy; CLONE_CLEAR_SIGHAND resets to default). Returns the object for child->sighand. */
struct lkd_sighand *lkd_sighand_clone(struct lkd_sighand *parent, uint64_t clone_flags);
void lkd_sighand_put(struct lkd_sighand *h);
/* Exec: handlers become SIG_DFL, ignored stay ignored, pending signals are kept (flush_signal_handlers
 * touches only the actions), sigaltstack cleared. */
void lkd_sig_exec_reset(lkd_task *t);
/* 'T' (LKD_PS_STOPPED) while t is parked in a job-control stop, else 0. ANY, lock-free. task uses it for
 * the registry state, procfs for /proc/<pid>/stat. */
int lkd_sig_task_stopped(lkd_task *t);
/* Host re-exec: serialize what execve keeps (blocked mask, ignored dispositions, pending signals with
 * their siginfo). Returns the size needed; writes only when cap is enough. import returns 0 or -EINVAL. */
size_t lkd_sig_exec_export(lkd_task *t, void *buf, size_t cap);
int lkd_sig_exec_import(lkd_task *t, const void *buf, size_t len);
/* The process is dying with the given fatal condition: mark every thread LKD_TF_FATAL and kick it. */
void lkd_sig_kill_all_threads(lkd_task *t);
/* Fill `out` with the sets for /proc/<pid>/status: SigPnd (thread), ShdPnd (process), SigBlk, SigIgn,
 * SigCgt, SigQ (queued count) of the local task. Returns 0. */
struct lkd_sig_status { uint64_t pnd, shdpnd, blk, ign, cgt; uint32_t queued, queue_max; };
int lkd_sig_status(lkd_task *t, struct lkd_sig_status *out);

/* ---- Fault delivery -------------------------------------------------------------------------- */
/* lkd_fault() and lkd_signal_next() are declared in <lkd/lkd.h> and implemented by sig. */

#endif /* LKD_SIG_H */

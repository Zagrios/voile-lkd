# Interrupt (kick) contract between the engine and LKD, as seen by sig

Requester: sig. Recipients: lead, engine (`src/fexdarwin/src/LkdGlue.cpp`). Date: 2026-10-02.

## What sig does

1. When a signal is queued for a task that can take it (`complete_signal`), sig sets `LKD_TF_SIGPENDING`, then, with locks released: `lkd_task_wake(t)` (wake pipe, wakes any LKD wait), rings the shared futex wait if there is one (`sig_ring_futex`), and calls `ops->kick(t)` if `t` is not the current task. A fatal signal (SIGKILL, or a terminating default action) marks all threads `LKD_TF_FATAL` and does the same for each one.
2. A blocking wait in LKD never relies on the host signal to end: wake pipe, then test of `lkd_sig_interrupted` before sleeping. The kick is only for JIT code.
3. Futex waits on a shared key sleep in `os_sync_wait_on_address(SHARED)` in 50 ms slices; the kick (SIGUSR2 without SA_RESTART) makes them return with EINTR (MEASURED darwin-memory-facts-b.md:96), and sig rings them directly.
4. `lkd_signal_pending` is also true when a temporary mask (sigsuspend, ppoll, pselect6) is waiting to be restored: the engine loop `while (lkd_signal_pending(t)) lkd_signal_next(t)` restores it.

## What the engine must do (already done according to a reading of LkdGlue.cpp:118-197)

1. SIGUSR2 reserved for the kick, installed without SA_RESTART, never used for anything else; the handler only raises a flag and, in JIT code, redirects to a safe point that calls `lkd_signal_next`.
2. After each `lkd_syscall`, after a kick, after `lkd_fault`: loop on `lkd_signal_next` while `lkd_signal_pending`; on -1, unwind the thread and call `lkd_task_release`.
3. `lkd_fault`: SIGSEGV with SEGV_MAPERR/SEGV_ACCERR and the exact address, SIGBUS, SIGILL, SIGFPE/FPE_INTDIV with the instruction's address, SIGTRAP from int3 with SI_KERNEL (sig then does not fill si_addr, like `force_sig`), single step with TRAP_TRACE and the address. err and trapno go into the sigcontext built by the engine.
4. Documented limit: a thread in JIT code is reached only at its next syscall as long as the interrupt page does not exist; the engine's 1 ms re-kick covers the unsafe areas.
5. Keep `LKD_FAMSVC_DIR` and `LKD_FAMSVC_FD` (and the descriptor) across the host re-execution (see sig-famsvc.md).

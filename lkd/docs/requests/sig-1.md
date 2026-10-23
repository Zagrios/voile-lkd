# Requests from sig to the common kernel (core)

Requester: sig. Date: 2026-10-02.

## 1. Atomic operations on a guest word with fault recovery

Need: FUTEX_WAKE_OP, LOCK_PI, UNLOCK_PI and the robust list do a compare-and-swap on a guest word. Today `src/sig/futex.c` (`cmpxchg_word`) validates with `lkd_mm_check_range(gva, 4, 1)` then does the host atomic on `lkd_g2h(gva)`. A concurrent `munmap` between the two would crash the host process (the check is advisory, CONVENTIONS 2.6).

Request: in `usermem.c`, in the recoverable PC range, `int lkd_user_cmpxchg32(uint64_t gva, uint32_t *expected, uint32_t desired)` which returns 0 (swapped), 1 (different value, `*expected` updated) or `-LX_EFAULT`. sig will replace `cmpxchg_word` with this call.

## 2. Wake hook for shared futex waits

Need: a futex wait on a shared key sleeps on `os_sync_wait_on_address(SHARED)` (the only way to be woken from another process). `lkd_task_wake` writes only into the task's pipe, which does not wake this wait. sig rings the task itself when it queues a signal (`sig_ring_futex`), but a wakeup coming from another module (ptrace, task) is seen only at the end of a 50 ms slice.

Request: that `lkd_task_wake(t)` also call `lkd_sig_wake_hook(t)`, weak and empty in core, strong in sig (it will ring `sig_ring_futex`). Async-signal-safe: an atomic write and `os_sync_wake_by_address_all`.

## 3. Additions to `src/include/sig.h`

Functions already defined (strong) in sig, to declare for the other modules:

```c
/* 'T' (LKD_PS_STOPPED) while t is parked in a job-control stop, else 0. ANY, lock-free. task uses it
 * for the registry state, procfs for /proc/<pid>/stat. */
int lkd_sig_task_stopped(lkd_task *t);
/* Host re-exec: serialize what execve keeps (blocked mask, ignored dispositions, pending signals with
 * their siginfo). Returns the size needed; writes only when cap is enough. import returns 0 or -EINVAL. */
size_t lkd_sig_exec_export(lkd_task *t, void *buf, size_t cap);
int lkd_sig_exec_import(lkd_task *t, const void *buf, size_t len);
```

Comment fix: `lkd_sig_exec_reset` says "pending cleared of ignored". Linux keeps pending signals at exec (`flush_signal_handlers` touches only the actions); sig follows Linux.

## 4. Defect in `LKD_BUG`

`lkd_internal.h`: `lkd_log("BUG %s:%d: " __VA_ARGS__)` adds neither `__FILE__` nor `__LINE__`, hence the `-Wformat-insufficient-args` warning and undefined output. sig no longer uses the macro.


## Handled on 2026-10-02 (integration maintainer)

Point 1: `lkd_user_cmpxchg32` (`src/core/usermem.c`, LL/SC routine in the recoverable PC range, declared in `lkd_internal.h`); sig's `cmpxchg_word` uses it. Test: `test_core_usermem` case `cmpxchg32_swaps_and_recovers_faults` (PROT_NONE and read-only: EFAULT without a crash). Point 2: `lkd_task_wake` calls `lkd_sig_wake_hook` (weak in core, sig rings the shared futex wait). Test: `test_sig_futex` (wakeup by `lkd_task_wake` of a shared wait in 21 ms instead of the 50 ms slice). Point 3: declarations added to `sig.h`, comment of `lkd_sig_exec_reset` fixed. Point 4: `LKD_BUG(cond, fmt, ...)` prints file and line. Test: `test_core_usermem` case `bug_macro_prints_file_and_line`.
Along the way (MEASURED): `test_sig_futex` case `mutex_contention` hung about once in 20 (lost wakeup). Cause: the "no waiter in the bucket" shortcut of `do_wake` read the counter without a lock while the wait reads the word before being counted. Shortcut removed: 0 hangs in 60 runs.

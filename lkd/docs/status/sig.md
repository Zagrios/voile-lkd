# Status of the sig module (LKD)

Date: 2026-10-02. Milestones 1 to 3 reached: signals, futex, signalfd, family IPC service (famsvc), single- and multi-process tests. `./check.sh`: 26 programs, 26 PASS (MEASURED, including the mm, fs, task and core tests against the real sig module).

## Files

| File | Content |
| --- | --- |
| `src/sig/sig_priv.h` | private structures, lock order of the module |
| `src/sig/state.c` | lazy state per process (`lkd_sigproc`) and per thread (`sig_task` on `task->pending.queue`), action objects, wakeups outside the lock, init/fork/exec/exit hooks, export and import for the host re-exec |
| `src/sig/queue.c` | siginfo queues, generation (prepare_signal, legacy_queue, RLIMIT_SIGPENDING, complete_signal), dequeue (next_signal, collect_signal), retarget on mask change, force_sig |
| `src/sig/deliver.c` | `lkd_signal_pending`, `lkd_sig_interrupted`, `lkd_signal_next`, `lkd_sig_restart_decision`, `lkd_fault`, `lkd_signal_returned`, temporary masks, alternate stack, group stop, `lkd_sig_status`, `lkd_sig_task_stopped` |
| `src/sig/send.c` | kill, tkill, tgkill, kill(0), kill(-pgrp), kill(-1), permissions, inter-process sending, SIGCHLD to the parent, `lkd_sig_kill_all_threads` |
| `src/sig/syscalls.c` | signal system calls |
| `src/sig/signalfd.c` | signalfd objects (fd.h), readability through a notification pipe |
| `src/sig/futex.c` | futex, futex_waitv, futex2, PI, robust lists, private table and shared table |
| `src/sig/famsvc.c`, `src/include/famsvc.h` | family IPC service (decision of the project lead), announced in `docs/requests/sig-famsvc.md` |
| `src/sig/NOTICE` | no code imported from HyperLinux |

## Registered system calls (21)

rt_sigaction, rt_sigprocmask, rt_sigreturn, rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo, rt_sigsuspend, rt_tgsigqueueinfo, sigaltstack, kill, tkill, tgkill, pause, signalfd, signalfd4, pidfd_send_signal, futex, futex_waitv, futex_wake, futex_wait, futex_requeue. All those of `OWNERS.md` for sig.

## Tests (MEASURED, `tools/runtest.sh`)

| Program | Assertions | Cases | Content |
| --- | --- | --- | --- |
| `test_sig_basic` | 385 | 26 | rt_sigaction (UAPI flags, KILL/STOP), masks, delivery and frame (SI_USER, old/new masks, NODEFER, RESETHAND), ignored signals, coalescing and FIFO real-time queue, sigqueueinfo EPERM, arguments of kill/tkill/tgkill, permissions (uid, suid, SIGCONT same session, CAP_KILL), thread choice, restart rules, faults (si_code, si_addr, SI_KERNEL, force_sig on a blocked or ignored action, core status), group fatal signal, sigaltstack (ENOMEM, EINVAL, EPERM, SS_ONSTACK, SA_ONSTACK, SS_AUTODISARM, restore), frame failure then SIGSEGV, blocking rt_sigtimedwait, rt_sigsuspend, pause, group stop and resume, SIGCHLD (NOCLDSTOP, NOCLDWAIT, SIG_IGN), group kill, signalfd, exec, re-exec export/import, trace line of the fatal signal (absent without `LKD_TRACE`) |
| `test_sig_more` | 83 | 8 | SIGKILL during a stop, tgkill(SIGKILL) kills the group, SIGPIPE, RLIMIT_SIGPENDING limit (EAGAIN, silent loss), cloning of the actions (CLONE_SIGHAND, CLONE_CLEAR_SIGHAND), retarget on blocking and on thread exit, state of the child of a real fork, 4 concurrent senders x 500 real-time signals (none lost, FIFO per sender), path of w15 (`lkd_task_create_main`, `rt_sigaction`, real `clone` of the task module, `tgkill`, delivery on the new thread) |
| `test_sig_futex` | 147 | 13 | WAIT errors, WAIT/WAKE with 8 threads (nr = 0 wakes one), bitset, absolute MONOTONIC and REALTIME timeouts, REQUEUE and CMP_REQUEUE, WAKE_OP (5 operations, OPARG_SHIFT, ENOSYS), PI protocol (EDEADLK, EWOULDBLOCK, EPERM, ownership handoff, ESRCH, timeout, OWNER_DIED), interruption by a signal (ERESTARTSYS, ERESTART_RESTARTBLOCK and restart_syscall up to the original deadline), futex_waitv, futex2 calls, robust list (OWNER_DIED, list_op_pending, bounded cycle), futex mutex with 16 threads, futex shared between two processes (fork), shared PI |
| `test_sig_famsvc` | 93 | 9 | PING, payloads of 3 and 6 MiB, descriptor passing, caller identity, unknown opcode ENOSYS, nested call refused, ESRCH, timeout then resume, service of the child of a fork, signals child to parent (kill, tgkill) and parent to child, EPERM between processes, adoption of the socket after a real host re-exec (`execl` of the test binary), shutdown and removal of the directory |

Total: 708 assertions, 56 cases, including multi-thread cases (up to 16 threads) and multi-process cases (real fork through `lkd_fork_host`, real exec).

## Measurements (MEASURED, M5 Max, macOS 27)

- famsvc, 4-byte PING, 2000 calls: to another process p50 8.4 to 10.4 us, p99 22 to 32 us; to itself p50 8.4 to 12.7 us. First version with `poll`: p50 19 us; switch to kqueue on the service side and blocking sockets with `SO_RCVTIMEO` on the client side.
- Futex wait on a shared key interrupted by a signal sent at 20 ms: return at 22.5 to 25 ms (the direct ring avoids the 50 ms slice).
- Private futex mutex, 16 threads x 3000 sections with `sched_yield` inside: 7.4 ms, 1579 FUTEX_WAIT calls.

## Engine witnesses (MEASURED, 19:25, details in `docs/requests/sig-4.md`)

Reproduction: liblkd.a built in a work directory, `fexdarwin-run` host built against it (recipe of the project lead), `FEXDARWIN_LKD=1`, `--trace-signals`, `run-with-timeout.sh`.

1. Cause of the death of w15_async (status 138, `lkd_signal_next = -1`). A temporary trace (removed) in `rt_sigaction` and in `complete_signal` shows that the CLONE_SIGHAND sharing is correct: tid 2 and tid 3 see the same action object. The action read in guest memory is `handler = 0x0, flags = 0x4000004`: the handler is 0 BEFORE entering LKD. The binaries of `witnesses/elf-x86_64/bin` (dated 15:17, unchanged at 19:23) are static PIEs (`ET_DYN`, `PT_DYNAMIC`, no `PT_INTERP`) whose `R_X86_64_RELATIVE` relocations are never applied (neither by the engine's loader, nor by `rt.h`, nor by a `_dlstart`, absent with `-nostdlib`). Relocations not applied: w15_async 2 (0x99a0 handler, 0x99b0 restorer), w02_int 4, w11_badjump 2, w14_overflow 3, w07_fault 1, w10_threads 0. The `ksigaction` structures initialized with `(u64)handler` are read with the field at 0, so SIG_DFL, so SIGUSR1 kills the process. Remedy on the witness or loader side: apply the RELATIVE relocations (base + addend) at load time, or link as ET_EXEC. The test `clone_thread_shares_handler` (`tests/test_sig_more.c`) reproduces the path of w15 with the real task module and passes.
2. With copies relocated for base 0x2aaaaaaaa000 (8 bytes of each RELATIVE written by a Python script, copies outside `witnesses/`): w02_int PASS 9524/9524, w14_overflow PASS 7/7 (stack overflow caught on the alternate stack, twice), w10_threads PASS 13/13, w07_fault passes up to the "non-exec fetch" case.
3. w11_badjump and the end of w07_fault: FEXCore repeats `[FEXCore E] NoExec instruction in entry block: 9` (or 3FFFF7FF0000) without `lkd_fault` being called (no signal line in the trace). On the engine side: `GuestSignals.cpp:112` is never reached for a jump to a non-executable address. The log reaches hundreds of MiB per minute: always bound the output.
4. w15_async relocated: 15/16 or 14/16 depending on the run. Correct delivery (`lkd_signal_next = 1`, handler on the targeted thread, SI_TKILL and SI_USER right, mask restored), but "spinner registers intact" and sometimes "made progress" fail: registers or xmm of the thread interrupted in translated code badly saved or restored by `FrameBuild` and `FrameRestore` (INFERRED, on the engine side: LKD touches no register).
5. Outside sig: `test_task_bigwin` hangs in the child process of `lkd_fork_host` (MEASURED with `sample`: `lkd_mm_fork_child` -> `mm_host_refresh` (mm.c:305) -> `mm_watch_lock` (watch.c:50) -> `__psynch_mutexwait`). The recursive lock of `watch.c` is taken by `lkd_mm_fork_prepare` in the parent; the child reinitializes it in `mm_watch_fork_child`, but `mm_host_refresh` takes it before that call in `lkd_mm_fork_child`. Defect of the mm module, which is being modified, reported to the project lead. The sig tests are not affected (none uses the production window).

## Design choices (READ in the code)

1. A single lock per process (`lkd_sigproc.lock`, rank 40) for all the queues; actions modified under this lock and the one of the action object. The wakeups (pipe, futex ring, `ops->kick`) are done with the locks released.
2. Generation following Linux: an ignored signal is dropped at generation unless it is blocked (or waited for by rt_sigtimedwait, `real_blocked`) or the task is traced; a standard signal already pending is not queued again; a signal fatal by default (other than core) marks the whole group `LKD_TF_FATAL` immediately; the first thread at the safe point calls `lkd_task_do_exit_group` only once.
3. Dequeue order: synchronous signals (SEGV, BUS, ILL, TRAP, FPE, SYS) first, then the lowest number, the thread's queue before the process's.
4. "core" status: bit 0x80 if the soft RLIMIT_CORE is non-zero and the process is dumpable. No core file is written.
5. Group stop: the first thread that dequeues a stop signal marks all threads; each one parks at its safe point; the last one reports CLD_STOPPED to the parent; SIGCONT releases, reports CLD_CONTINUED (or CLD_STOPPED if the stop was not finished, like Linux) at the next pass through `lkd_signal_next`. A traced task goes through `lkd_ptrace_stop(LKD_STOP_GROUP)`.
6. Futex: private table per process (`os_unfair_lock` locks) and shared table in the `futex.shm` file of the family directory (per-word locks holding the host pid, taken over if the holder is dead). Private waits sleep on the task's wakeup pipe; a wait with a shared key sleeps on `os_sync_wait_on_address(SHARED)` in 50 ms slices, rung by signals. A REQUEUE between the private table and the shared table wakes instead of moving (spurious wakeup allowed).
7. famsvc: see `docs/requests/sig-famsvc.md`.
8. Diagnostics: with `LKD_TRACE`, the signal that ends a process gives a line `lkd: pid N dies of signal S in tid T: ...`, with the sender (si_code <= 0) or the faulting address and the rip (fault of the dying thread). It comes from `complete_signal` (immediate death of the group) or from `lkd_signal_next` (default action with core, forced signal). Without `LKD_TRACE`, a relaxed read of an integer and nothing else.

## Known gaps

1. WAIT_REQUEUE_PI and CMP_REQUEUE_PI: `-ENOSYS` (glibc 2.25 and later no longer use them for condition variables). No priority inheritance. The death of a PI owner without a robust list is not detected (Linux does it through the kernel's PI state).
2. Atomic operations on guest words (WAKE_OP, PI, robust): range check then host atomic; a concurrent `munmap` would crash (request sig-1.md, point 1).
3. wait4 does not see a child's job control stops and resumes yet (request sig-2.md, point 1); the SIGCHLD and the wakeup of the waiters are sent.
4. Orphaned process group not detected: SIGTSTP, SIGTTIN, SIGTTOU always stop.
5. `si_utime`/`si_stime` of SIGCHLD at 0. RLIMIT_SIGPENDING counted per process (Linux: per user), capped at 16384.
6. signalfd: `fstat` not provided (NULL, the default error of the fs module); state not shared across a fork (fd.h). Through SCM_RIGHTS (2026-10-10), it goes out as a record with its mask (`LKD_FDREC_SIGNALFD`): the receiver gets one in its own process and reads its own signals, like Linux; a mask changed afterwards by `signalfd(fd, ...)` is not seen on the other side.
7. The entries of the shared futex table of a process that died abruptly stay (wakeups counted wrongly); the family directories of crashed runs are cleaned up after one minute.
8. Interruption of JIT code: only at the next syscall as long as the engine has no interrupt page (engine limit, sig-3.md).
9. Absolute REALTIME timeouts are converted once: a later change of the time does not move them.

## Open requests

`docs/requests/sig-famsvc.md` (announcement of the service, opcodes of mm, procfs, ptrace), `sig-1.md` (core: guest CAS with fault recovery, wakeup hook, declarations in sig.h, LKD_BUG macro), `sig-2.md` (task: stop state for wait4, pidfd, clocks, transport of the signal state at re-exec), `sig-3.md` (kick contract with the engine).

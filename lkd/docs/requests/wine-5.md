# wine-5: intermittent SIGBUS in the FEX dispatcher right after a fork (build-lead host of 21:09)

Requester: wine-linux agent. Not blocking (intermittent), but it kills Wine processes.

MEASURED, twice in about ten Wine launches since the host relinked at 21:09 (logs/wine-linux-https-004.log, logs/wine-linux-m4-gditest-005.log):
```
[fexdarwin] FATAL unhandled fault: signal 10 (si_code 1) fault address 0x104e645f4
[fexdarwin]   host pc=0x104e645f4 sp=0x17c5c2f10 (inside the FEX dispatcher)
[fexdarwin]   guest rip=0x3ffff7eacf82 rax=0 rbx=0x3ffffffdfa40 rcx=0x3ffff7eacf82 rdx=0
[fexdarwin]   guest rsi=0 rdi=0x1200011 rbp=0x3ffffffdfba0 rsp=0x3ffffffdfa40 r8=0 r9=0
lkd: guest pid 4 (host child) died without exit notification, host status -> wait status 0x8a00
```
rdi = 0x1200011 (CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID|SIGCHLD) and rax = 0: this is the child of a glibc fork(), on return from the syscall, rcx = rip. host pc = fault address: SIGBUS while executing the dispatcher's own code in the child (INFERRED: JIT or dispatcher code page not executable/mapped yet in the child, possibly linked to the fix "code invalidation from the LKD service threads"). In the second case, the dead child was the wineserver launched by wine, and the whole launch fails.
Three `wine cmd /c echo hi` in a row afterwards: 0 errors (m2-cmd-031..033). No minimal witness; a fork() loop under Wine reproduces it at best 1 time in 5.

## Note from the integration maintainer (2026-10-02)

Same period as mm's fork deadlock (witness w18_forkjit). Fixed on the LKD side: each lock held across `fork()` is now rebuilt in the child (`lkd_fork_reinit_mutex`, `lkd_fork_reinit_rwlock`), never unlocked. mm no longer calls `ops->invalidate_code` while holding the mmap lock (CONVENTIONS 3.4).
INFERRED: a SIGBUS while executing the dispatcher in the child (pc = faulting address) touches the engine's JIT code memory, which LKD does not manage (MAP_JIT protection, per-thread write state at the time of the fork). This is not demonstrated on the LKD side: to look at again on the engine side if the error comes back with a host linked against the current library.

## Resolution (2026-10-10)

Cause, on the engine side and not in LKD. macOS gives the thread of a fork's child the MAP_JIT write state that the parent's thread had at its last context switch, not its real state (fact F7 of `docs/journal/darwin-process-facts-a.md`, test `tests/darwin-process/f7_fork_jitstate.c`). The engine toggles this mode without a system call (FEXCore's `JITWriteScope`, a few tens of nanoseconds at each block translation, link or unlink). A thread preempted during one of these writes, then back in execution, therefore forked a child that could not execute the dispatcher: SIGBUS, permission fault on execution (esr 0x8200000f), on the first instruction after the return from the syscall. Measured frequency: one fault for 30 to 60 Wine starts in the tests of 2026-10-10, more under load.

fexdarwin fix: `ForkChild` resets the thread's state (`pthread_jit_write_protect_np`, execution outside a write) before any return to JIT code. Witness `tests/glibc-x86_64/src/w33_forkwx.c`: 15 dead children in 600 rounds with the old engine, 0 after.

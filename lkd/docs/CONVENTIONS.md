# LKD conventions

Working document of the common kernel. The modules (mm, fs, procfs, task, sig, net, ptrace) follow these rules. What is not in this file is in the header comments (`include/lkd/lkd.h`, `src/include/*.h`), which are authoritative. A rule that gets in the way is discussed through a request in `docs/requests/<module>-<n>.md`, not through a workaround.

## 1. Error returns

1. A syscall handler returns the content of RAX: a value, or `-LX_E*` (negative Linux error). Never the Darwin `errno`. The `LX_E*` constants have the Linux values (`EAGAIN` is 11 on Linux and 35 on Darwin).
2. After a host call that fails, write `return LKD_HOST_ERR();` (translates `errno` with `lkd_errno_from_darwin`) or `LKD_HOST_RET(r)` for an `r < 0`. A Darwin code without a Linux equivalent becomes `LX_EIO` or the closest honest one (table in `src/core/xlate.c`).
3. No fabricated answer. An unimplemented function returns `-LX_ENOSYS` (or Linux's exact error for a specific case), never a success. A syscall that the module does not provide stays unregistered: the dispatcher answers `-ENOSYS`, counts the call (`lkd_enosys_count`) and names it once with `LKD_LOG_ENOSYS=1`.
4. The codes `-LX_ERESTARTSYS`, `-LX_ERESTARTNOINTR`, `-LX_ERESTARTNOHAND`, `-LX_ERESTART_RESTARTBLOCK` are internal: they never go out to the guest (the dispatcher resolves them into `-EINTR` or a restart). `-LX_ENOIOCTLCMD` becomes `-LX_ENOTTY`.
5. Special results of a handler: `LKD_RET_CONTEXT_REPLACED` (execve, rt_sigreturn: the context was set by `ops->set_regs`), `LKD_RET_EXIT_THREAD`, `LKD_RET_EXIT_GROUP`.

## 2. Guest pointers

1. Never dereference a guest pointer other than through `lkd_user_read`, `lkd_user_write`, `lkd_user_strn`, `lkd_user_zero`. They validate with `lkd_mm_check_range` then copy with fault recovery (`-LX_EFAULT`, never a crash).
2. Allowed fast path: after `lkd_mm_check_range(gva, n, need_write) == 0`, give `lkd_g2h(gva)` to a host call (`read`, `write`, `recv`). The host kernel returns `EFAULT` for an unmapped page. Do not do this for a buffer that can overlap the file being read itself or a page protected by 4 KiB subpages without going through validation.
3. Never keep a host pointer obtained from `lkd_g2h` beyond a blocking call, a possible `munmap` or a mutation of the VMA table. Copy into a kernel buffer first.
4. Structures containing pointers (`iovec`, `msghdr`, `sigaction`, `epoll_event`): read the structure with `lkd_user_read`, then handle each inner pointer the same way. The sizes and offsets are in `abi_x64.h`.
5. `lkd_guest_base` is 0 in the tests: a guest pointer is then a host pointer. In the engine, the guest address `g` is at the host address `lkd_guest_base + g`, the window is at most 2^46 bytes, the guest page is 4 KiB and the host page 16 KiB.
6. `lkd_mm_check_range` is advisory: a concurrent `munmap` can slip in between the check and the copy. Fault recovery is the real safety net.

## 3. Lock order

Take in increasing order, never take a lower rank while holding a higher rank. The constants are in `lkd_internal.h` (`enum lkd_lock_rank`). The list takes over the one from HyperLinux (`src/syscall_internal.h:10-38`) and extends it.

| Rank | Lock | Module |
| --- | --- | --- |
| 10 | `mmap_lock` (VMA map, brk, mmap) | mm |
| 11 | SysV shm table, taken under `mmap_lock` | mm |
| 20 | vfs mount table, `fsctx` (cwd, root, umask) | fs |
| 21 | `/dev` device registry | procfs |
| 30 | descriptor table | fs |
| 31 | fd object lock (`lkd_fdobj.lock`) | fs and any object owner |
| 40 | per-process signal state (actions, process pending) | sig |
| 41 | per-task siginfo queue | sig |
| 50 | thread registry and thread list of a process | task |
| 51 | shared process registry (`fcntl` byte lock) | task |
| 60 | children wait states (wait4) | task |
| 61 | tracer/tracee state | ptrace |
| 70 | futex bucket (by increasing index if several) | sig |
| 71 | epoll, timerfd, inotify tables | net |
| 80 | leaf: log, trace, counters | all |
| 90 | VMA read lock, taken by `lkd_mm_check_range` | mm |

Associated rules.

1. Rank 90 is a leaf that `lkd_mm_check_range` takes for reading, very briefly: `lkd_user_*` can therefore be called under any other lock. A module must never call `lkd_user_*` while holding this rank 90 itself (this is the only prohibition).
2. No lock is held during: a guest syscall, a blocking wait, an engine callback marked "no LKD lock", `lkd_signal_next`, `lkd_ptrace_stop`, nor during the execution of guest code.
3. A module that publishes to another module releases its lock before the call. ptrace publishes a stop (rank 61) then releases before waking wait4 (rank 60): `lkd_ptrace_wait_event` is called with rank 60 held, so ptrace's code must never take a rank lower than 61 while holding 61.
4. Engine locks (2026-10-02, integration). The engine's locks come BEFORE all of LKD's: `lkd_fork_prepare` calls `ops->fork_prepare` (ThreadsLock, FEX code locks) then the modules' hooks. In return, LKD never calls `ops->invalidate_code` (or any engine callback) while holding an LKD lock: mm collects the ranges in a `struct mm_inval` and invalidates them after releasing the mmap lock (`mm_inval_flush`), and removing a watch (`mm_unwatch_for_write`) invalidates outside the watch and VMA locks. The engine's SMC handler can therefore take ThreadsLock in `invalidate_code` without crossing a fork in progress.
5. A synchronization primitive shared between processes (registry lock, counter) must survive the death of its holder: `fcntl` byte lock (released by the kernel on death), or a lock-free algorithm. A `pthread` mutex in shared memory stays locked if the process dies holding it: to avoid. `os_unfair_lock` cannot be shared between processes.

## 4. Blocking calls and signals

1. Every blocking syscall waits through `lkd_block_wait_fd`, `lkd_block_poll` or `lkd_block_sleep` (or a primitive that follows the same protocol). The interruption test `lkd_sig_interrupted(t)` takes place before sleeping, an `lkd_task_wake(t)` writes into the task's wake pipe: a signal queued before the wait is never lost. Do not do a blocking host `read` on a pipe or socket descriptor: wait for readiness (`lkd_block_wait_fd`), then read without blocking.
2. On interruption, the wait returns `-LX_ERESTARTSYS`. The syscall returns it as is if it is restartable, otherwise replaces it with `-LX_EINTR`:
   - restartable (return `-LX_ERESTARTSYS`): `read`, `readv`, `write`, `writev`, `ioctl`, `accept`, `accept4`, `recvfrom`, `recvmsg`, `sendto`, `sendmsg`, `connect` (blocking), `flock`, `fcntl(F_SETLKW)`, `wait4`, `waitid`, `futex(FUTEX_WAIT)` without a timeout, `open` on a FIFO, `msgrcv`. Linux does not restart if an `SO_RCVTIMEO` or `SO_SNDTIMEO` timeout is set: return `-LX_EINTR`.
   - never restarted (return `-LX_EINTR`): `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`, `epoll_pwait`, `rt_sigtimedwait`, `rt_sigsuspend`, `sigsuspend`, `io_getevents`, `mq_timedreceive`.
   - `-LX_ERESTARTNOHAND`: `pause`, `rt_sigsuspend`, `epoll_pwait` with a mask (restarts only if no handler runs).
   - `-LX_ERESTART_RESTARTBLOCK` with a continuation stored in `t->restart`: `nanosleep`, `clock_nanosleep`, `futex(FUTEX_WAIT)` with a timeout, `poll`/`ppoll` with a timeout. The `restart_syscall` syscall (core) runs the continuation; the `sig` module sets `t->restart.fn` to NULL when a signal handler runs.
3. The dispatcher decides on the restart with `lkd_sig_restart_decision` (rules of `arch/x86/kernel/signal.c`). On `LKD_RESTART`, the engine moves RIP back by 2, sets RAX to the number returned in `*rax`, keeps the six argument registers, then calls `lkd_signal_next`: the signal frame saves the already restarted context.
4. A thread that executes JIT code is interrupted by `ops->kick(t)` (a reserved host signal, chosen by the engine). The engine's handler only raises a flag read at safe points, then the thread calls `lkd_signal_pending`, `lkd_signal_next`. A thread blocked in LKD is woken by `lkd_task_wake` (pipe), never by the host signal alone.
5. The waits that do not go through the pipe (futex on `os_sync_wait_on_address`) follow the contract of `futex.h`: register in the bucket, test for interruption, sleep, leave the bucket, return `-LX_ERESTARTSYS`. The `sig` module chooses the wake mechanism.

## 5. Contract with the engine (summary)

1. `lkd_syscall` at each guest SYSCALL; write RAX with `*rax` for `LKD_RETURN`. RCX and R11 are set by the engine, never by LKD.
2. After `lkd_syscall`, after a kick, after a fault report: `while (lkd_signal_pending(t) && lkd_signal_next(t) > 0) {}`; a `-1` return means that the task must die (unwind the host thread then `lkd_task_release`).
3. `ops->get_regs` and `set_regs` are valid for the calling task at a safe point, and for a stopped task (ptrace stop, exit). For a running task, they return `-LX_ESRCH`.
4. The engine calls `lkd_set_current(t)` at the start of each guest host thread.
5. An engine that installs its own SIGSEGV/SIGBUS handlers after `lkd_init` calls `lkd_fault_try_recover(ucontext)` first.
6. The engine links `liblkd.a` with `-Wl,-force_load,liblkd.a`.

## 6. fork

macOS keeps only the calling thread and leaves locked the locks held by the other threads (mutex, `os_unfair_lock`, stdio). The scheme is FEX's (`LockBeforeFork`/`UnlockAfterFork`).

1. `lkd_fork_host(t, flags)` does: `ops->fork_prepare`, `lkd_<m>_fork_prepare` in the order mm, fs, procfs, sig, task, ptrace, net (increasing ranks), `fork()`, then in the parent `lkd_<m>_fork_parent` and `ops->fork_parent`, in the child `lkd_<m>_fork_child` and `ops->fork_child`, in reverse order.
2. `fork_prepare` takes all the module's locks (in the order of its own ranks) and returns quickly; it does not block for long (a missing holder is a defect to report). `fork_parent` releases them. `fork_child` rebuilds them with `lkd_fork_reinit_mutex` or `lkd_fork_reinit_rwlock` (memory zeroed then init), never with an `unlock`: if another thread was waiting for the lock at the time of the fork, the lock word carries a kernel wait state (psynch) that the child does not have, and the child stays blocked at the next unlock or acquire (MEASURED: witness `w18_forkjit`, child blocked in `mm_wlock`; `test_mm_fork` case `fork_while_a_thread_churns_the_mm_locks`). Same rule for the locks that `fork_prepare` does not take but that another thread of the parent can hold (leaf locks): rebuild them in `fork_child`. The `os_unfair_lock`s are reset to `OS_UNFAIR_LOCK_INIT`. `fork_child` also forgets the missing threads, restarts the module's auxiliary threads (signal service thread, ptrace transport thread) and rebuilds the objects that `fork` does not inherit: the kqueue descriptors (MEASURED: `EBADF` in the child), hence epoll, timerfd and inotify.
3. Inherited by `fork`: private memory (copy on write), host descriptors (with their shared offset and flags), shared mappings (`MAP_SHARED`). Not inherited: kqueue, the other threads, the locks of missing threads.
4. mm: shared anonymous memory must be a shared memory object to stay shared with the child. task: `lkd_fork_child` switches `t` to the child's identity (new tid and pid taken from the family counter before `fork()`, ppid, single thread); the state of a missing thread is removed from the local registry.
5. `CLONE_VM|CLONE_VFORK` is not reproduced by a fork (documented FEX limit): task treats it as a fork followed by a wait for the child's exec or exit, and says so in its status.

## 7. exec

The guest exec is a re-execution of the host runtime. Order: `lkd_exec_prepare` (resolution through the vfs, permissions, `#!`, copy of argv and envp; no state change), then `lkd_exec_commit` (point of no return: hooks `lkd_<m>_exec_commit` in the order task, mm, fs, sig, ptrace, net, procfs), then `ops->exec_image`. If the host exec loses the memory, the state to keep (family registry, non-CLOEXEC descriptor tables) goes through the family's shared memory (`lkd_family_export`) and the inherited host descriptors. The `FD_CLOEXEC` flag of a guest descriptor is also set on the host descriptor. `lkd_exec_finish` reports the `PTRACE_EVENT_EXEC` stop.

## 8. Initialization order

`lkd_init(ops)`: copy of the operations, SIGSEGV/SIGBUS handlers, environment variables (`LKD_TRACE`, `LKD_LOG_ENOSYS`), common kernel syscalls (`restart_syscall`), then `lkd_mm_init`, `lkd_fs_init`, `lkd_procfs_init`, `lkd_task_init`, `lkd_sig_init`, `lkd_net_init`, `lkd_ptrace_init`. `lkd_init` fails with `-LX_EEXIST` if two modules register the same syscall. A module's initialization depends only on the modules that come before it. The first guest thread is created afterwards by `lkd_task_create_main`.

## 9. Adding a syscall in ten lines

```c
/* src/task/ids.c */
#include "lkd_internal.h"
#include "task.h"

LKD_SYSCALL(gettid)                       /* static int64_t lkd_sys_gettid(lkd_task *t, const lkd_sysreq *rq) */
{
    return t->tid;
}

void lkd_task_init(void)                  /* strong definition; core has a weak no-op */
{
    LKD_REGISTER(gettid);                 /* nr taken from LKD_NR_gettid, name "gettid" */
}
```

Everything else (`tests/test_task_ids.c`) is picked up by `file(GLOB)`: run `./check.sh` again. The arguments are `rq->a[0..5]` (macros `LKD_ARGI`, `LKD_ARGU`, `LKD_ARGP`). The test calls `lkd_syscall` with `fake_make_task()` and `fake_engine_init()` (see §10).

## 10. Tests

1. A `tests/test_<module>_<topic>.c` file is an executable; `tests/support/*.c` (fake engine) is linked into each one. Framework: `tests/ltest.h` (`LT_TEST`, `LT_RUN`, `LT_CHECK`, `LT_EQ`, `LT_ERRNO`, `LT_STREQ`, `LT_MEMEQ`, `lt_finish`). A program without an assertion fails.
2. Each behavior has a test that fails before and passes after, with the modeled Linux behavior in a comment (man page or kernel source file with line). The expected values are written as literals, not recomputed by the code under test.
3. Fake engine: `fake_engine_init()` calls `lkd_init` with operations that record kicks, invalidations, frame builds, spawns and fork events; `fake_make_task()` creates a published task with a fake CPU (`fake_cpu_of(t)->regs`); `fake_make_thread(t)` adds a thread of the same process. The settings `fake_spawn_result`, `fake_frame_build_result`, etc. simulate failures. For an extension of the fake engine, write a request.
4. The `fork` tests really fork the test process and check through a pipe or shared memory (`lt_run_child`). The thread tests create real threads, never more than 300 per process.
5. Machine safety: never more than 300 threads in a process, 64 simultaneous child processes, 8 GiB of touched memory, no unbounded fork or thread loop. `check.sh` runs each program under a 120 s limit (`LKD_TEST_TIMEOUT`). A test leaves behind neither a process nor a file outside its temporary directory.
6. The tests may define a strong symbol only if no module defines it (no redefinition of a module hook in a test).
7. Never launch the game, EAC, UTM, QEMU or anything with HVF.

## 11. Linking and weak symbols

`liblkd.a` contains weak symbols (hooks of `weak_modules.c`, replacements of `stubs_*.c`) and their strong definitions in the modules. An ordinary archive link does not necessarily pull in the strong member when the weak one has already resolved the symbol. The tests and the engine therefore link with `-Wl,-force_load,<path>/liblkd.a` (already done by `CMakeLists.txt`; `LINK_DEPENDS` relinks the tests when the library changes). A module that adds a source file simply drops it in `src/<module>/`: `file(GLOB_RECURSE)` picks it up at the next configuration (`./build.sh` always reconfigures).

## 12. HyperLinux import policy

HyperLinux (Apache-2.0, `rocket-hyperlinux-20260928/hyper-linux/src`) is read-only. A file or a function is copied into `src/<module>/`, adapting it (x86-64 syscall numbers, `lkd_user_*` instead of `guest_read`, `LX_*` values, no HVF type). Each imported block carries the comment `/* imported from HL <file> */`, the SPDX header and the original copyright stay, and the module lists its imported files in `src/<module>/NOTICE`. The root `NOTICE` contains the attribution text. Never edit the original. FEX's CLAUDE.md forbids submitting AI-generated code: this tree is a private research copy, nothing is sent upstream.

## 13. Style

Code comments in English, short, without decoration. Reports and status files in English too, simple sentences, no em dash, with `file:line`, MEASURED kept apart from READ and INFERRED. Names: `lkd_<module>_*` for a module's public functions, `lx_` and `LX_` for the Linux ABI, `LKD_` for the kernel's macros.

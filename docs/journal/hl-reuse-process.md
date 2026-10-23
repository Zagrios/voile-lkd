# Reuse of the HyperLinux process layer by an in-process FEXCore engine

Date: 2026-10-02. Status: complete version reviewed against the sources (draft 3). Second half of the HyperLinux reuse study: threads, fork, exec, wait, signals, futex, ptrace, network, polling. Memory, VMAs, the vfs and procfs are covered in `reports/hl-reuse-core.md`; I cite their symbols only to measure the coupling.

Conventions. MEASURED: output of a command or of a program launched in this session, with the log cited. READ: reading of a source, with `file:line`. INFERRED: deduction on my part, not executed. The paths `src/...` refer to `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/hyper-linux/src/` (HL). `EXP` refers to `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`. The paths `probes/...` refer to `rocket-hyperlinux-20260928/probes/...`.

## Result in brief

1. HyperLinux is not a kernel reusable as is. Its process layer is welded to the `vcpu_run_loop` loop (HVC, ESR_EL1, HVF registers) and to a fork through `posix_spawn` plus state transfer. Out of 18,527 lines of process-related files that I classified by line ranges, I estimate 9,500 lines to keep with mechanical changes (guest memory access, registers), 2,300 to rewrite and 6,700 to drop (HVC loop, fork IPC, Rosetta, GDB stub). INFERRED to within about 30%.
2. Eight modules have no HVF reference (`grep` count): `futex.c`, `syscall_poll.c`, `syscall_net.c`, `syscall_inotify.c`, `syscall_fd.c`, `proc_ptrace.c`, `proc_names.c`, `trace.c`. Their only runtime dependency on the engine is the guest memory copy (`guest_read`, `guest_write`, `guest_ptr`). Compile dependency: eleven headers include `<Hypervisor/Hypervisor.h>` (among them `syscall.h`, `guest.h`, `thread.h`, `syscall_signal.h`, `syscall_proc.h`), which seven of these eight modules include transitively (`trace.c` is exempt); they have to be purged of it (`hv_vcpu_t` types in the signal prototypes and in `guest_t`).
3. HL's dispatch is indexed by the arm64 syscall numbers (`src/syscall.h:17-170`). The 276 registrations of FEX's Linux layer that do a "passthrough" call the host kernel with these same arm64 numbers through `svc #0` (`Syscalls/Passthrough.cpp:18-150`). HL can therefore play the role of host kernel behind FEX's x86-64 front end, which avoids redoing the x86-64 conversion (numbers, packed `epoll_event`, `stat`). Coverage by name is 128 syscalls out of 241 FEX passthrough names (53%); 113 are missing.
4. `proc_ptrace.c` is already a user-space emulation of the tracer/tracee relation between two processes of the runtime, without kernel ptrace: shared state file, `fcntl` lock, notification socket, cooperative stop of the tracee. I recompiled it in the working copy: 79 PASS checks, 0 FAIL (`logs/host-test-proc-ptrace-host.log`). Its content is ARM64 (34 words): the x86 view (216 bytes, which EAC asks for) is to be written. FEXCore keeps the real x86 state in ordinary memory (`CPUState`), so the x86 view is exact by construction, unlike Rosetta's ARM context.
5. Five gaps break Wine even before EAC: no signal to another guest process other than SIGKILL/SIGCONT of a child (Wine server sends SIGUSR1 to the clients, `voile-wine/server/thread.c:961`); process-local futex; no SIGCHLD at the death of a child; `fork` ignores `CLONE_CHILD_SETTID`; `vfork` and `posix_spawn` lead to an unqualified vm-clone path.
6. MEASURED on this machine: a kqueue descriptor does not go through `SCM_RIGHTS` (`EINVAL`) and is not inherited by `fork` (`EBADF` in the child) (`logs/host-test-kq-fork-scm.log`). HL's fork would make `clone` fail if a timerfd is open. Epoll, timerfd and inotify have to be rebuilt after `fork`.
7. The native `fork()` of macOS 27 holds in a multithreaded process with `MAP_JIT` (teammate's log `evidence/darwin-process/f1_fork_mt.log`, 17 PASS, 0 FAIL). The 2,300 state transfer lines of `fork_ipc.c` become useless. The price: the locks held by the absent threads stay locked in the child (mutex, `os_unfair_lock`, stdio), so FEX's `LockBeforeFork`/`UnlockAfterFork` scheme is mandatory.
8. Test infrastructure: the full tree compiles without warnings in 2.9 s (39 objects, link successful, no execution); 8 host test programs pass in 2.7 s without HVF; the test `test-thread-vcpu-lifetime` does not link with the Makefile as is (symbol `proc_trace_tracer_exit`). 77 static x86-64 guest programs are already compiled (`test-binaries/amd64/bin`) and can serve as acceptance tests for a FEX host.
9. Side finding (out of scope but measured): `SYS_fgetxattr` is 16 and `SYS_fremovexattr` 18 in `src/syscall.h:166,169`, whereas the arm64 numbers are 10 and 16 (`FEX Arm64/SyscallsEnum.h:22,28`). The EAC logs contain 304 `unimplemented_syscall` events for number 10.

## 0. Sources, state of the tree, method

Tree read: HL in working copy state, HEAD 8048a96 ("docs: link the public examples sibling"). MEASURED (`git status --short`, `git diff --stat`, read-only): 45 entries, that is 33 modified tracked files (4475 insertions, 1560 deletions) and 12 untracked (11 sources plus `.DS_Store`). Untracked, therefore entirely private: `src/fault_dump.c/h`, `src/proc_names.c/h`, `src/proc_ptrace.c/h`, `src/signal_notify.h`, `src/syscall_filemap.c/h`, `src/syscall_reopen.c/h`.

Integrity: I reran `shasum -a 256 -c EXP/evidence/hl-source-manifest-after.sha256` from HL: 214 entries, no line not OK. I wrote nothing in HL, in the game, in the installed runtime or in the repositories. I launched neither the game, nor EAC, nor QEMU, nor UTM, nor HVF.

Full reading of the files: `thread.c/h`, `syscall_proc.c/h`, `syscall_signal.c/h`, `futex.c/h`, `fork_ipc.c/h`, `syscall_exec.c`, `proc_ptrace.c/h`, `syscall_internal.h`, `signal_notify.h`, `rosetta.h`, `gdb_stub.h`, `trace.h`, `crash_report.h`, `hv_util.h`, `proc_names.h`, `syscall_poll.h`, `syscall_net.h`, `syscall_inotify.h`. Targeted reading (functions, headers, tables): `syscall.c` (dispatch), `syscall_poll.c`, `syscall_net.c`, `syscall_inotify.c`, `syscall_fd.c`, `proc_names.c`, `trace.c`, `crash_report.c`, `fault_dump.c`, `hl.c`, `syscall_sys.c`. Documents read: `proc-ptrace-module.md`, `ptrace-runtime.md`, `ptrace-options-contract.md`, `ptrace-syscall-contract.md`, `ptrace-regset-contract.md`, `regset-architecture.md`, the README of `ptrace-remote-setregset-runtime-20261001`, `ptrace-x86-feasibility-20260929.md`, `eac-ptrace-target-role-20260929.md`, the READMEs of the EAC audits, as well as `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`.

Engine assumption, taken from the instructions: FEXCore in a native arm64 macOS process, guest addresses equal to host addresses, without Hypervisor.framework, without EL1 shim, without Rosetta. The target guest is a Linux x86-64 process (Linux Wine, Windows game, Linux EAC module). Caveat: the report `reports/fexcore-darwin-port.md` measures that an arm64 macOS process cannot reserve memory below 4 GiB nor above 0x7ffffe000000, whereas `wine64-preloader` asks for 0x10000-0x68000000 and 0x7f000000-0x7fff0000. Perfect address identity may therefore not be able to host Wine, and a guest base offset could be needed. I recommend keeping an explicit guest memory access layer (section 3), which makes this offset cheap.

## 1. Architecture as implemented

### 1.1 A guest process is a macOS process with its own HVF VM

READ: `src/fork_ipc.h:6-8` (HVF allows only one VM per process, so fork launches a new hl process). The guest PID is an integer from a counter shared by the whole fork family: unlinked one-page `mkstemp` file, mapped `MAP_SHARED`, atomic integer `next` initialized to 2, initial PID 1 (`src/syscall_proc.c:45-46,80-113`, `proc_alloc_pid` 516-529, export and import 531-562). PIDs stay below `0x3fffffff`, limit of the owner field of PI futexes (`src/syscall_proc.c:77-81`). The task registry (`comm`, `exe`, `cmdline`, published ptrace relation) is a second shared file indexed by TID, protected by an `fcntl` lock (`src/proc_names.c:1-80`, 479 lines), passed to the child by descriptor.

### 1.2 vCPU versus guest threads

READ. A guest thread is a `thread_entry_t` (`src/thread.h:28-102`) in a table of `MAX_THREADS = 64` entries (`src/thread.h:25`), under a global mutex `thread_lock` of rank 5 (`src/thread.c:34`), with a `_Thread_local current_thread` pointer (`src/thread.c:45`). Each guest thread owns an `hv_vcpu_t` created on its own host pthread, because HVF binds a vCPU to its creating thread (`src/fork_ipc.c:1442-1445`). It receives a 4 KiB EL1 stack taken from a bitmap of 64 slots (`src/thread.c:36-40,373-393`). The thread also carries: per-thread signal mask and alternate stack (`src/thread.h:42-51`), Rosetta's local ptrace state (`53-77`), GDB snapshot (`88-91`), state of `CLONE_VM` clones (`93-101`).

Interruption: `hv_vcpus_exit` under the table lock (`src/thread.c:283-296`, `454-472`). A guest signal or `exit_group` wakes all vCPUs; the loop sees `HV_EXIT_REASON_CANCELED` (`src/syscall_proc.c:2073-2136`). Blocking host calls are interruptible only on the `read`, `write`, `pread`, `pwrite` path (`interruptible_iov_io`, `src/syscall_io.c:304-324`): the thread publishes its pthread, a "waker" thread sends it host SIGUSR2 every 2 ms as long as the wakeup is pending (`src/thread.c:47-140`). `ppoll`, `pselect6` and `epoll_pwait` without timeout wake up through the `wakeup_pipe` pipe and the interruption epoch (`src/syscall_poll.c:221-265,436-497,846-911`); `wait4` loops in steps of 1 ms and `futex` in steps of 100 ms. On the other hand `nanosleep` and `clock_nanosleep` call the host `nanosleep` without an exit condition (`src/syscall_time.c`), and `syscall_net.c` contains neither `signal_pending`, nor `EINTR`, nor `poll(`: `accept`, `connect`, `recv*` and `send*` block on the host without being interrupted by a guest signal. INFERRED from these absences, not executed.

Thread end: write of 0 into `clear_child_tid` then `futex_wake_one`, after destruction of the vCPU (`src/fork_ipc.c:1515-1541`). `exit_group` sets `exit_group_requested`, wakes everything (`thread_interrupt_all`, wakeup pipe) and joins the workers for at most 50 ms before detaching them (`src/syscall_proc.c:125-138`, `src/thread.c:405-439`).

### 1.3 Clone variants

READ: the `sys_clone` dispatcher (`src/fork_ipc.c:2623-2642`) has only three branches.

| Flags | Path | Effect |
|---|---|---|
| `CLONE_THREAD` (pthread_create glibc, musl) | `sys_clone_thread`, `src/fork_ipc.c:1290-1440` | New 2 MiB host pthread, new vCPU, same page tables. Copy of the parent's 31 GPRs and system registers, X0=0, SP=`child_stack`, TPIDR_EL0=`tls` if `CLONE_SETTLS` (1484-1488). `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID` handled (1384-1407). |
| `CLONE_VM` without `CLONE_THREAD` | `sys_clone_vm`, `src/fork_ipc.c:1568-1701` | A detached host thread with `is_vm_clone=1`, own TID, waitable by wait4 through the thread table (`src/thread.c:783-869`). Designed for Rosetta's two-process JIT. `CLONE_VFORK` is not honored; if `child_stack` is 0 the "child" takes over the parent's stack (1633-1634). The exit of the last vm-clone triggers `exit_group` (1795-1808). |
| Everything else, including glibc `fork()` (flags 0x1200011, observed for EAC) | `sys_clone_fork` then `sys_clone_fork_inner`, `src/fork_ipc.c:1841-2617` | `posix_spawn` of an `hl --fork-child <fd>` (1873-1930) then state transfer (1.4). Only `CLONE_VFORK` is read there: the parent waits for the child's `waitpid` (2595). |

READ findings:

- `sys_clone_fork(vcpu, g, flags, verbose)` receives neither `child_stack`, nor `ptid`, nor `tls`, nor `ctid` (`src/fork_ipc.c:2612-2642`). A glibc `fork()` passes `CLONE_CHILD_SETTID` with `ctid = &THREAD_SELF->tid`: the child therefore does not receive its TID in this variable. INFERRED: any glibc code that reads the cached TID in the child sees the parent's (PI mutex, `pthread_kill(pthread_self())`).
- glibc's `vfork()` and `posix_spawn()` use `CLONE_VM|CLONE_VFORK` (0x4111 with SIGCHLD). INFERRED: they fall into `sys_clone_vm`, the parent is not suspended, and an `execve` from the "child" calls `guest_reset(g)` on the shared guest memory (`src/syscall_exec.c:373`). No HL test exercises vfork or posix_spawn (`grep` over `test/test-*.c`: only `test-shm.c:216` mentions it, in a comment).
- `clone3` returns ENOSYS (`src/syscall.c:2417-2420`); glibc falls back to `clone`. `unshare`, `setns`, `pidfd_open` are missing from the dispatch.

### 1.4 How fork copies guest memory, and the fork holes fixed

READ: two paths, IPC version 18 (`src/fork_ipc.c:57`).

1. COW path. If guest memory is a `mkstemp` `MAP_SHARED` file (`g->shm_fd >= 0`) and outside Rosetta, the parent sends the descriptor through `SCM_RIGHTS` and the child maps it `MAP_PRIVATE` (`src/fork_ipc.c:575-592,1954-1964,2041-2047`). Zero bytes copied. The parent never remaps, because HVF caches the host VA to PA mapping (1966-1978).
2. Copy path. Rosetta and fallback: enumeration of the used regions, 1 MiB blocks over the socket (`src/fork_ipc.c:2080-2135`).

State sent in order: header `ipc_header_t` (brk, stack, mmap, TTBR0/1, Rosetta placement, 62-91), PID allocator descriptor, names descriptor, two ptrace descriptors with ACK from the child (2014-2038), registers `ipc_registers_t` (93-108), memory, FD table through `SCM_RIGHTS` (dup of each host FD, 2251-2261), OSS objects recreated empty, procfd "reopen" states, epoll states (kqueues do not go through, 161-163 and 243-250), cwd and umask, VFS snapshot, SysV shm segments, shared file views, sysroot, ELF path, hl path, cmdline, semantic regions, high VA ranges, VMAs, VA aliases, pre-announced regions, signal state, shim blob, sentinel.

Fork holes fixed by the private fixes (READ, reasons in the comments): task identity and names shared in the family (`src/syscall_proc.c:76-81`: a local counter gave the child's first pthread the child's PID); import ACK of the ptrace descriptors (without it, Darwin reported an EOF on the received socket while the sending alias was closing, false death of the parent measured 3 times out of 100; `src/fork_ipc.c:313-336,2029-2038`, `probes/ptrace-scm-diagnosis.md`); epoll kqueues recreated in the child (`src/fork_ipc.c:164-243`, `src/syscall_poll.c:742-810`); procfd states reopened without closing aliases, because closing an alias releases POSIX locks (343-446); `MAP_SHARED` file mappings kept shared (448-504, `src/syscall_filemap.c`); SysV segments reattached (2431-2452); vDSO `[vvar]` page, whose publisher thread does not exist in the child (1233-1243); signal mask and alternate stack restored after `thread_register_main` (1119-1125, 1226-1231); FD buffers on the heap because a macOS worker's stack is 512 KiB (1832-1840, 2140-2150).

Remaining fork holes (READ or INFERRED):

- MEASURED with `EXP/scratch-hl/probe-src/kq_fork_scm.c`: `dup(kqueue)` succeeds, `sendmsg(SCM_RIGHTS)` of a kqueue fd fails with `EINVAL`, and after `fork()` the kqueue fd does not exist in the child (`fcntl` and `kevent` answer `EBADF`), the parent's kqueue staying intact (`logs/host-test-kq-fork-scm.log`).
- `FD_TIMERFD` has the kqueue itself as host fd (`src/syscall_fd.c:79-89`) and goes through the generic path `dup` then `SCM_RIGHTS` (`src/fork_ipc.c:2251-2261`). INFERRED: `send_fds` fails (2320-2334) and `clone` returns ENOMEM as long as a timerfd is open. Not executed.
- `FD_EVENTFD`, `FD_SIGNALFD` and `FD_INOTIFY` have a pipe as host fd, but their state (counter, mask, kqueue) lives in process-local arrays (`src/syscall_fd.c:389-397,626`, `src/syscall_inotify.c:69-94`). `src/fork_ipc.c` contains no occurrence of these three types: the child has the fd but not the state.
- An eventfd passed through `SCM_RIGHTS` to another process also loses its counter. This is the esync scheme in Wine (not enabled by default).

### 1.5 exec

READ (`src/syscall_exec.c:70-764`). The runtime does not relaunch itself: exec happens in the same macOS process. Sequence: reading of path, argv, envp in guest memory (limits 256 arguments, 4096 variables, 256 KiB, lines 100-102), VFS resolution, ELF or shebang loading (133-250), pre-validation of the interpreter or of Rosetta (251-308), point of no return (310), closing of `O_CLOEXEC` FDs with cleanup of the eventfd, signalfd, timerfd, inotify subsystems (316-370), `guest_reset(g)` which zeroes all guest memory (373), reset of `exit_group_requested` (379), `signal_reset_for_exec` (384), reloading of the shim, the segments and the interpreter (386-421), rebuild of the page tables (460-575), new stack (625-684), write of TTBR0, ELR, SP_EL0, SPSR, TPIDR_EL0 and zeroing of the 31 GPRs (696-736), then ptrace exec stop (`proc_trace_exec_stop`, 746-757).

READ limits: no call that stops the other threads (the file references only `current_thread` and `exit_group_requested`); exec from a non-leader thread (de_thread) is not handled, which `probes/ptrace-syscall-contract.md:33` confirms ("full nonleader exec ... remain outside this lot"). The ptrace link survives exec (`src/proc_ptrace.h:67-69`).

### 1.6 wait4, waitid, SIGCHLD

READ (`src/syscall_proc.c:1077-1196`). `proc_wait_event` polls in a loop: remote ptrace events (`proc_trace_wait`), the local thread table (`thread_ptrace_wait`, Rosetta clones) and the child table `proc_table[64]` with one host `wait4(WNOHANG)` per child. Without `WNOHANG` it sleeps 1 ms (`nanosleep`, 1150-1151) and tests `signal_pending` and `exit_group`. Stop statuses are re-encoded as Linux numbers (1121-1128). `waitid` accepts `P_ALL` and `P_PID` only (1170), with `WSTOPPED`, `WEXITED`, `WCONTINUED`, `WNOWAIT`. The options `__WALL` and `__WCLONE` are ignored.

READ findings:

- No SIGCHLD is queued at the death of an ordinary child. `LINUX_SIGCHLD` appears only in the ptrace stop notification (`src/syscall_signal.c:300-318`), the `exit_signal` field of vm-clones (never delivered) and the value of `waitid` (`src/syscall_proc.c:1179`). `SA_NOCLDWAIT` and auto-reaping are not handled.
- A guest process killed by a default signal exits with code `128 + signum` (`src/syscall_signal.c:1002-1007`), so the parent reads `WIFEXITED` with this code, not `WIFSIGNALED`. INFERRED from this code; not executed.
- No reparenting nor subreaper; `getppid` returns the PID of the original parent; `setpgid` and `setsid` are stubs (`src/syscall.c:2088-2093`).

### 1.7 Signals to the guest

READ (`src/syscall_signal.c/.h`).

- The actions (`actions[64]`) are unique per process, under `sig_lock` (rank 4). Directed signals (tkill, tgkill, `rt_tgsigqueueinfo`) have per-thread pending sets `directed_pending[MAX_THREADS]` (`src/syscall_signal.c:37-47,165-189`) with a full `siginfo`. Standard signals coalesced; real-time signals 32..64 queued up to 32 (`RT_SIGQUEUE_MAX`, `src/syscall_signal.h:177`).
- Delivery only at points of the loop: after each syscall (`src/syscall_proc.c:1380-1385`) or after cancellation of the vCPU (2124-2131). `signal_queue_info` wakes all vCPUs, the host I/O of the target thread, signalfd and the wakeup pipe (`src/syscall_signal.c:284-289`).
- Frame: arm64 `linux_rt_sigframe_t` on the guest stack (128-byte siginfo, `ucontext`, `sigcontext` with 31 GPRs, SP, PC, PSTATE), FPSIMD context (magic `0x46508001`, 528 bytes, 843-899), ESR context for synchronous faults, restorer always equal to the vDSO page (`src/syscall_signal.c:1153-1156`, `sa_restorer` ignored like the arm64 kernel).
- Per-thread masks and alternate stack. `SA_ONSTACK`, `SA_NODEFER`, `SA_RESETHAND`, `SA_SIGINFO`, `SA_RESTART` handled. The `SA_RESTART` restart saves x0..x5, x8 and the PC before each restartable call (`signal_syscall_begin`, `src/syscall_signal.c:389-396`, list `src/syscall.c:1185-1211`) then moves the PC back by 4 bytes (403, 1037).
- `sigsuspend` always returns EINTR without sleeping (`src/syscall_signal.c:727-777`): a `while (!flag) sigsuspend()` loop spins.
- Guest `ITIMER_REAL` is emulated by hand because the host `alarm()` serves the vCPU watchdog (80-90, 541-621).
- Signals between processes: `kill` accepts only itself, the "group" assumed equal to itself, or a registered child with SIGKILL or SIGCONT only (`src/syscall.c:1982-2001`, `src/syscall_proc.c:641-665`; any other signal returns EOPNOTSUPP). `tgkill` with a `tgid` different from the current PID returns ESRCH (`src/syscall.c:2025-2028`). `probes/ptrace-runtime.md:51` confirms it.
- Synchronous faults: the EL1 shim forwards them through HVC #11 (abort, undefined instruction) and HVC #10 (BRK); the loop builds `si_code`, `si_addr` and the ESR (`src/syscall_proc.c:1692-1887`).

### 1.8 futex

READ (`src/futex.c`, 905 lines). Table of 64 buckets indexed by the guest virtual address, waiter allocated on the caller's stack, one condvar per waiter (91-137). Operations: WAIT, WAKE, WAIT_BITSET, WAKE_BITSET, REQUEUE, CMP_REQUEUE, WAKE_OP, LOCK_PI, UNLOCK_PI, TRYLOCK_PI (857-893); the rest returns ENOSYS (895-898).

- The table is process-local. No wait or wake between processes, even on `MAP_SHARED` memory. This is the main gap for Wine, whose processes share sections.
- `FUTEX_WAIT` without timeout wakes up every 100 ms and returns EINTR after 1 s (`futex.c:230-269`): a substitute for signal delivery, not the Linux semantics.
- `FUTEX_WAIT_BITSET` uses `pthread_cond_timedwait` in CLOCK_REALTIME and treats the absolute timeout as real time, whether `FUTEX_CLOCK_REALTIME` is set or not (`futex.c:143-165`); Linux uses CLOCK_MONOTONIC by default. INFERRED: glibc's `pthread_cond_clockwait` and `sem_clockwait` misbehave.
- PI: ownership of the word under the bucket lock, recovery of a dead owner through `thread_tid_alive`, no priority inheritance, no `OWNER_DIED` (`futex.c:78-87,596-771`).
- `set_robust_list` returns 0 without doing anything (`src/syscall.c:1810-1814`). Missing: `get_robust_list`, `futex_waitv` (449), `futex_wake/wait/requeue` (454-456), `FUTEX_CMP_REQUEUE_PI`, `FUTEX_WAIT_REQUEUE_PI`, `FUTEX_LOCK_PI2`.

### 1.9 poll, select, epoll, eventfd, timerfd, signalfd

READ. `ppoll` and `pselect6`: `src/syscall_poll.c:117-553`. epoll over kqueue: `EVFILT_READ` and `EVFILT_WRITE`, `EV_CLEAR` for ET, `EV_ONESHOT` for ONESHOT, table of 256 kevents per call, EPOLLEXCLUSIVE not supported (`syscall_poll.c:554-990`, comment 561). Waking indefinite waits goes through a global pipe `wakeup_pipe` (`src/syscall_poll.h:46-52`). `epoll_event` follows the 16-byte arm64 layout (`syscall_poll.c:583-588`).

`src/syscall_fd.c` (814 lines): eventfd = non-blocking pipe plus counter in an array of 32 entries (`:384-444`); timerfd = one-shot `EVFILT_TIMER` kqueue rearmed on read, 32 entries, `TFD_TIMER_ABSTIME` ignored (`:30-60,121-126`); signalfd = pipe plus mask, 16 entries (`:626,686-720`). None of these objects can be shared between processes.

### 1.10 ptrace as it exists today

Two disjoint implementations behind `sys_ptrace` (`src/syscall_proc.c:870-1044`).

A. Local, specific to Rosetta (`src/thread.c:577-869`, `src/syscall_proc.c:909-1039`): SEIZE, CONT, INTERRUPT, GETSIGINFO, GETREGSET and SETREGSET `NT_PRSTATUS`, plus `NT_PRFPREG` and `NT_ARM_HW_WATCH`. The tracer and the tracee are two threads of the same process (`CLONE_VM` clones). The tracee stops by itself on BRK (HVC #10) or on INTERRUPT, copies its ARM registers into `ptrace_regs` and sleeps on a condvar (`src/thread.c:607-683`). ARM64 registers only. To drop.

B. Remote, module `proc_ptrace.c` (807 lines, untracked): parent to child relation created at fork (1.11).

Requests handled for the remote relation (`src/syscall_proc.c:873-893`): TRACEME (0), SETOPTIONS (0x4200, mask 0x11 only), GETEVENTMSG (0x4201), CONT (7), DETACH (17), SYSCALL (24), GETREGSET `NT_PRSTATUS` (0x4204, 34 ARM64 words). Everything else returns EINVAL or ESRCH.

Actual use by EAC (READ in `probes/`): TRACEME in the child PID 148, then in the parent SETOPTIONS(148, 0x11) and 66 GETREGSET of 216 bytes (`probes/eac-ptrace-audit-regset-linux-20260929/README.md`); PTRACE_SYSCALL (24) 146 times (`eac-ptrace-regset-transition`); in the run `eac-ptrace-poke-20260930/run-20260930T133949334669Z/progress-finding.json`: POKEDATA (5) of a word whose low byte is 0xCC, CONT (7), wait4 with status 1407 (0x57f, SIGTRAP), GETREGSET, PEEKDATA (2), restoring POKEDATA, kill; then a 216-byte SETREGSET that moves RIP back by one byte (`probes/ptrace-remote-setregset-runtime-20261001/README.md:3-9`). The EAC module contains 21 static call sites to `ptrace` (`probes/eac-current-module-static-20260929/verified-ptrace-call-sites.json`); only the requests above are observed at runtime, the other sites are unknown. The size 216 is that of the x86-64 `user_regs_struct` (27 words): EAC asks for an x86 view, which Rosetta fills with the first 27 ARM registers (`probes/regset-architecture.md:9`).

### 1.11 The tracer/tracee relation between two processes, in pure user space

READ (`src/proc_ptrace.c/h`, `probes/proc-ptrace-module.md`). This part uses no kernel ptrace.

- At fork, the parent creates an unlinked `mkstemp` metadata file (`trace_shared_t`, `src/proc_ptrace.c:28-40`), mapped `MAP_SHARED` in both processes, plus a pair of Unix notification sockets. These descriptors never appear in the guest FD table and go only to the direct child (`:321-389`, `src/fork_ipc.c:2014-2038`).
- Mutual exclusion between processes: POSIX lock `fcntl(F_SETLKW)` on the first byte plus a local mutex per process (`:117-131`). The lock is released at the death of the owner.
- The tracee stops itself, on its thread, at three points: signal delivery (`signal_deliver` to `proc_trace_stop`, `src/syscall_signal.c:937-985`), syscall entry and exit (`syscall_trace_boundary`, `src/syscall.c:1213-1235,1257,2496`), exec event (`src/syscall_exec.c:746-757`). It then captures its registers through the `proc_trace_capture_regs` callback (34 words, `src/syscall_proc.c:152-183`), publishes them in the shared file with a stop sequence number (`src/proc_ptrace.c:620-691`) and waits for resumption by `poll` of the socket with a 200 ms timeout (`:698-715`).
- The tracer reads the stop sequence in wait4 and waitid (`proc_trace_wait`, `:582-618`), controls through CONT, DETACH, SYSCALL (`proc_trace_control`, `:459-492`) and reads the captured registers (`proc_trace_getregs`, `:533-563`). Only the exact tracer TID can control (`:469-471`).
- Death of a peer: EOF of the notification socket (`peer_closed`, `:193-206`); the death of the tracer detaches and resumes the tracee with the signal suppressed (`:735-755`).

Limits (READ, `probes/ptrace-runtime.md:49-55`, `proc-ptrace-module.md:9`): no ATTACH nor SEIZE of a non-child, no remote memory access (`process_vm_readv` answers ENOSYS), no PEEK nor POKE, no register writes, no group stops, no reparenting, no asynchronous SIGCHLD for a tracer that is not waiting.

## 2. Table per file

LOC measured by `wc -l`. Columns A, R, D: lines to adapt or keep (A), to rewrite on the same principle (R), to drop (D), estimated by line ranges that I read (calculation: Python script in this session's history, ranges cited in the verdicts). Total of the 37 files in the table: 18,527 lines, A 9,483, R 2,316, D 6,728.

| File | LOC | Subsystem | Concrete HVF, EL1, Rosetta coupling | Assumed guest ABI | A | R | D | Verdict |
|---|---|---|---|---|---|---|---|---|
| thread.c | 885 | thread table, interruptible host I/O, local ptrace | `hv_vcpu_t`, `hv_vcpus_exit` (283-296, 454-472), `hv_vcpu_destroy` (276), `sp_el1` (373-393), `HV_SYS_REG_DBGW*` (508-552), `vcpu_get_gpr` (607-616) | `linux_user_pt_regs_t` ARM64 | 406 | 45 | 434 | adapt (table, `host_io_waker`, join); drop (local ptrace 473-885); rely on FEX's ThreadManager |
| thread.h | 226 | same | `Hypervisor.h`, `hv_vcpu_exit_t`, `ptrace_*` | ARM64 | 109 | 27 | 90 | adapt |
| syscall_proc.c | 2164 | PID, wait, vCPU loop, sys_ptrace | `vcpu_run_loop` (1223-2164), HVC 0/2/4/5/7/9/10/11/12, `ESR_EL1`, `KBUF_VA_BASE`, `dump_rosetta_state` (242-351), `ID_AA64*` override (1482-1540) | x0-x8, ESR, ARM64 | 619 | 409 | 1136 | adapt (PID, wait, child table); rewrite (`sys_ptrace`, capture); drop (HVC loop) |
| syscall_proc.h | 171 | same | `Hypervisor.h`, `vcpu_run_loop` | ARM64 | 125 | 0 | 46 | adapt |
| syscall_signal.c | 1255 | signals | `hv_vcpu_get/set_reg`, `HV_SYS_REG_ELR_EL1`, `HV_REG_FPSR`, Q0-Q31 (858-899, 1023-1165, 1190-1250) | arm64 frame, FPSIMD, ESR, vDSO restorer | 958 | 297 | 0 | adapt (state, queues, itimer, sigaction); rewrite (frame, sigreturn) |
| syscall_signal.h | 314 | same | `Hypervisor.h` | `linux_sigcontext_t`, `linux_ucontext_t` ARM64 | 274 | 40 | 0 | adapt; rewrite (structures 133-172) |
| futex.c / .h | 905 / 42 | local futex | no HVF; `guest_ptr` (7 sites) | generic | 947 | 0 | 0 | adapt (guest memory); extend (section 4) |
| fork_ipc.c / .h | 2642 / 28 | clone, fork through IPC | `posix_spawn` (1919), `hv_vcpu_create` (1167, 1454), TTBR0/1, SCTLR (1197), `guest_init_from_shm` (586), 109 HVF references | ARM64, `ipc_registers_t` | 0 | 330 | 2340 | drop (IPC 130-1255, 1561-2617); rewrite (clone thread 1256-1560, dispatcher) |
| syscall_exec.c / .h | 764 / 24 | execve | `guest_reset` (373), `guest_build_page_tables` (569), `rosetta_prepare/finalize` (479, 636), `hv_vcpu_set_sys_reg` (697-713) | ARM64, Rosetta | 336 | 138 | 314 | adapt (argv reading, shebang, CLOEXEC); drop (page table rebuild); rewrite (exec hook) |
| syscall_poll.c / .h | 1002 / 52 | poll, select, epoll | no HVF; `guest_*` (16 sites) | 16-byte `epoll_event` (583-588) | 863 | 191 | 0 | adapt; rewrite (epoll wait, x86 layout) |
| syscall_net.c / .h | 1701 / 122 | sockets | no HVF; `guest_*` (41 sites); `rosettad_is_socket` | LP64 `msghdr` identical to x86-64 | 1823 | 0 | 0 | adapt |
| syscall_inotify.c / .h | 667 / 44 | inotify over kqueue vnode | no HVF; `guest_*` (4 sites) | `inotify_event` identical | 711 | 0 | 0 | adapt |
| syscall_fd.c / .h | 814 / 55 | eventfd, timerfd, signalfd | no HVF; `guest_*` (8 sites) | identical | 432 | 437 | 0 | adapt; rewrite (states shareable between processes) |
| proc_ptrace.c / .h | 807 / 115 | remote ptrace, user space | `PROC_TRACE_REG_WORDS = 34` ARM64 (`:41`), callback `proc_trace_capture_regs` | NT_PRSTATUS ARM64 | 922 | 0 | 0 | adapt (arch-neutral register buffer, version 5) |
| proc_names.c / .h | 479 / 60 | family task registry | none | generic | 539 | 0 | 0 | adapt (as is for the most part) |
| signal_notify.h | 10 | ptrace stop notification | none | generic | 10 | 0 | 0 | as-is |
| rosetta.c / .h | 301 / 136 | Rosetta | everything (`TCR_EL1_VALUE`, VZ ioctl, rosettad) | ARM64 | 0 | 0 | 437 | drop |
| gdb_stub.c / .h | 1732 / 78 | GDB RSP AArch64 | `HV_SYS_REG_DBG*`, AArch64 XML | ARM64 | 0 | 0 | 1810 | drop; FEX has its GdbServer (`LinuxEmulation/GdbServer`, 1523 lines) |
| syscall_internal.h | 132 | lock ranks, FD | none | generic | 132 | 0 | 0 | as-is |
| trace.c / .h | 209 / 68 | traces per category | none (pure libc) | generic | 277 | 0 | 0 | as-is (already linked by the host tests) |
| crash_report.c / .h | 195 / 37 | crash report | 11 HVF references | ARM64 | 0 | 232 | 0 | rewrite (optional) |
| fault_dump.c / .h | 162 / 8 | fault diagnostic | `mach_vm_read`, VMA, `hv_vcpu_t` | ARM64 | 0 | 170 | 0 | rewrite (optional) |
| hv_util.h | 121 | HVF macros | `HV_CHECK`, SCTLR | ARM64 | 0 | 0 | 121 | drop |
| syscall.c / .h (outside total) | 2512 / 645 | dispatch and constants | x0-x5, x8, `hv_vcpu_set_reg(X0)` (2495), arm64 `SYS_*` (17-170) | ARM64 | n/a | n/a | n/a | the dispatch skeleton (1181-1303, 2463-2512) and about 600 lines of process cases stay; the rest belongs to the teammate |

### 2.1 Coupling measured per file

MEASURED by `grep -c` over HL/src: number of lines that touch guest memory (`guest_read`, `guest_write`, `guest_ptr`, `guest_read_str`, `guest_ptr_avail`), HVF (`hv_vcpu`, `hv_vm`, `HV_REG`, `HV_SYS_REG`, `HV_CHECK`, `vcpu_get`, `vcpu_set`) and `guest_t`.

| File | guest memory access | HVF references | `guest_t` |
|---|---|---|---|
| thread.c | 0 | 48 | 21 |
| syscall_proc.c | 19 | 82 | 22 |
| syscall_signal.c | 11 | 32 | 11 |
| syscall_poll.c | 16 | 0 | 4 |
| syscall_net.c | 41 | 0 | 19 |
| syscall_inotify.c | 4 | 0 | 2 |
| syscall_exec.c | 3 | 15 | 6 |
| fork_ipc.c | 7 | 109 | 23 |
| futex.c | 7 | 0 | 13 |
| proc_ptrace.c | 0 | 0 | 0 |
| syscall_fd.c | 8 | 0 | 7 |
| syscall.c | 15 | 15 | 19 |
| syscall_io.c | 29 | 0 | 20 |
| syscall_time.c | 12 | 0 | 7 |
| syscall_sys.c | 12 | 0 | 8 |
| syscall_fs.c | 58 | 0 | 33 |

### 2.2 Fixed limits in the code (READ)

`FD_TABLE_SIZE` 1024 (`src/syscall.h:601`); `MAX_THREADS` 64 (`src/thread.h:25`); `PROC_TABLE_SIZE` 64 children (`src/syscall_proc.h:92`); `FUTEX_BUCKETS` 64; `EVENTFD_MAX` 32, `TIMERFD_MAX` 32, `SIGNALFD_MAX` 16 (`src/syscall_fd.c`), `INOTIFY_MAX` 16; 64 AF_UNIX abstract name locks (`src/syscall_net.c:156`); 256 kevents per `epoll_pwait`. I did not measure the number of threads of Rocket League under Wine: 64 threads is a hard limit of unknown fit for this game.

## 3. Minimal interface between a CPU engine and the kernel emulation

### 3.1 Two integration modes

Mode A (recommended, INFERRED). Keep FEX's x86-64 front end (`Source/Tools/LinuxEmulation/LinuxSyscalls`, about 30,000 lines, including 406 syscall registrations, 276 of them pure passthrough) and replace the "host kernel" it calls. FEX calls this kernel in two ways: through the 7 templates `SyscallPassthroughN<arm64 number>` that contain an inline assembly `svc #0` (`Syscalls/Passthrough.cpp:18-150`), and through 64 `::syscall(` calls plus a few direct libc calls (`::poll` `Syscalls/FD.cpp:36`, `::select` and `::fcntl` `x64/FD.cpp:41-55`). Replacing the 7 templates with a call to a function `hlk_host_syscall(nr, a0..a5)` indexed by the arm64 numbers gives exactly HL's dispatch. The conversions from x86-64 to host layout (for example packed `epoll_event_x86` to the 16-byte `epoll_event`, `x64/EPoll.cpp:25-90`) stay done by FEX. FEX's direct libc calls assume Linux constants: they also have to go through the layer (for example `RemapFromX86Flags` assumes the `O_*` values of a Linux host).

Mode B. Keep HL's dispatch as is, index it by the x86-64 numbers and throw away FEX's Linux layer. One then has to redo in HL the x86-64 table (about 330 numbers of which ~100 are legacy), all the x86 structures (`stat`, `epoll_event`, `sigaction`, signal frames), the x86-64 vDSO (FEX has one, `VDSO_Emulation.cpp`, 906 lines) and the ptrace view. To avoid.

In both modes, FEX itself handles four areas that assume a Linux host kernel (READ, FEX source intact): clone and fork (`Syscalls/Thread.cpp:179-399`, which calls `::syscall(SYSCALL_DEF(clone), ...)` at line 258 and emulates `vfork` through a fork followed by a wait, 284-299); signals (`SignalDelegator` keeps the guest actions, but `kill`, `tkill`, `tgkill` are passthroughs to the host kernel, `Passthrough.cpp:232,295,305`); exec (`x64/Thread.cpp:286-315` then `Syscalls.cpp:199-440`: FEX relaunches the host on itself with the loader arguments, which naturally corresponds to a re-execution of the runtime under Darwin); ptrace (`Syscalls/Info.cpp:96-113`: PEEKTEXT, PEEKDATA, POKETEXT, POKEDATA, ATTACH, DETACH as passthrough "so that Wine launches the Ubisoft launcher", everything else returns EPERM). `prctl` sends all options except SECCOMP and GET_AUXV to the host libc (`Syscalls/Thread.cpp:479-516`), `personality` is remembered per thread (`Syscalls/Info.cpp:72`) and `seccomp` is emulated by a user-space `SeccompEmulator` (`Info.cpp:93`). These are the areas that HL's process layer has to replace; HL's gaps on `personality` and `seccomp` are therefore moot in mode A.

### 3.2 Sketch of a C API (narrow boundary)

Principle: the engine owns the CPU, the host thread stack and the signal frame; the kernel layer owns identity, tables, pending signals, ptrace stops and kernel objects. Seven entry points are enough.

```c
/* hlk.h: boundary between a CPU engine (FEXCore host) and the Linux-on-Darwin kernel layer */
typedef struct hlk_task hlk_task;          /* one guest thread: TID, mask, alternate stack, ptrace link */

/* 1. guest memory. Guest address = host address (or base + address, see 3.3). Never a direct dereference in the handlers. */
int     hlk_user_read (uint64_t gva, void *dst, size_t n);        /* 0 or -EFAULT, fault-safe */
int     hlk_user_write(uint64_t gva, const void *src, size_t n);
int64_t hlk_user_strn (uint64_t gva, char *dst, size_t max);      /* length, -EFAULT or -ENAMETOOLONG */

/* 2. syscall entry: called by the engine at each guest SYSCALL, owner thread */
typedef struct { uint64_t nr, a[6]; } hlk_sysreq;                  /* RAX, RDI, RSI, RDX, R10, R8, R9 */
typedef enum { HLK_RETURN, HLK_CONTEXT_REPLACED, HLK_EXIT_THREAD,
               HLK_EXIT_GROUP, HLK_RESTART } hlk_disp;
hlk_disp hlk_syscall(hlk_task *t, const hlk_sysreq *rq, int64_t *rax);
   /* RETURN: write rax. CONTEXT_REPLACED: exec or sigreturn have already set the context through engine->set_regs.
      RESTART: SA_RESTART, the engine moves RIP back by 2 and sets RAX = nr again. */

/* 3. asynchronous signals: the engine calls hlk_signal_next at safe points (syscall return, after kick, after fault) */
typedef struct { int signo; uint8_t info[128]; uint64_t handler, flags, restorer, new_mask;
                 uint64_t alt_sp, alt_size; int use_alt; } hlk_delivery;
int  hlk_signal_next(hlk_task *t, hlk_delivery *d);               /* 1 deliver, 0 nothing, -1 the process dies */
void hlk_signal_returned(hlk_task *t, uint64_t mask, uint64_t sp);/* after the engine's sigreturn */

/* 4. synchronous faults: the engine translates its fault into (signo, si_code, address, err, trapno) */
void hlk_fault(hlk_task *t, int signo, int si_code, uint64_t addr, uint64_t err, uint64_t trapno);

/* 5. engine services called by the kernel layer (registered once) */
typedef struct hlk_engine_ops {
  int  (*get_regs)(hlk_task*, hlk_x86_regs *r);      /* user_regs_struct, valid at a safe point or stopped thread */
  int  (*set_regs)(hlk_task*, const hlk_x86_regs *r);
  int  (*get_fp)(hlk_task*, void *xsave, size_t *n);  int (*set_fp)(hlk_task*, const void*, size_t);
  void (*kick)(hlk_task*);                            /* reach a safe point soon: pthread_kill on the reserved signal */
  int  (*spawn)(hlk_task *child, const hlk_x86_regs *start, uint64_t fs_base);
  void (*invalidate_code)(uint64_t start, uint64_t len); /* POKE, mprotect, write to a code page */
  int  (*frame_build)(hlk_task*, const hlk_delivery*);   /* pushes x86-64 rt_sigframe, sets RIP/RSP/RDI/RSI/RDX */
  int  (*frame_restore)(hlk_task*, uint64_t *mask, uint64_t *sp);
  void (*reset_for_exec)(void);                          /* or re-execution of the host */
} hlk_engine_ops;

/* 6. fork: the engine calls fork() itself between these two calls */
int  hlk_fork_prepare(hlk_task *t, uint64_t clone_flags);  /* locks, family registry, kqueues to rebuild */
void hlk_fork_finish (hlk_task *t, pid_t host_child);      /* host_child == 0 in the child */

/* 7. exec */
int  hlk_exec_prepare(hlk_task *t, const char *path, char *const argv[], char *const envp[], hlk_image *img);
void hlk_exec_commit (hlk_task *t, const hlk_image *img); /* image installed: TRACEEXEC stop, signal reset */

/* + ptrace stop (called by the engine at stop points) */
typedef enum { HLK_STOP_SYSCALL_ENTRY, HLK_STOP_SYSCALL_EXIT, HLK_STOP_SIGNAL, HLK_STOP_EXEC, HLK_STOP_FAULT } hlk_stop;
int  hlk_ptrace_stop(hlk_task *t, hlk_stop kind, int signo, int *resume_signo);
```

### 3.3 Ownership and lock rules

- Only the owner thread calls `hlk_syscall`, `hlk_signal_next` and `hlk_ptrace_stop`. A thread stopped for ptrace has its x86 state in the ordinary memory of the process (`FEXCore::Core::CPUState`, `CoreState.h`, `gregs[16]`, `rip`, `xmm`, `flags[48]`): a helper thread of the same process can read it without waking it. The HVF constraint "only the owner vCPU reads its registers" (`src/thread.c:607-616`) disappears.
- Lock order to keep (`src/syscall_internal.h:10-38`): mmap (1), fd (3), signal (4), thread table (5), pid (6), futex buckets (7). The engine must never hold a kernel lock while it executes guest code.
- Guest memory: keep the three `hlk_user_*` functions as the only access. With identity they do a fault-protected copy (FEX provides `FaultSafeUserMemAccess`: `CopyFromUser` and `CopyToUser` in arm64 assembly with a known fault PC, `FaultSafeUserMemAccess.cpp:1-80`). If a guest base offset becomes necessary for Wine (section 0), only this layer changes, plus the pointers nested in structures (`iovec`, `msghdr`, `sigaction`), for which HL already has converters (`append_guest_iov`, `src/syscall_io.c:279-301`).
- Signals: a single reserved host signal serves as "kick" (SIGUSR2 is already taken by HL's host I/O, `src/thread.c:52`); FEX reserves its own signals, an allocation plan is needed. Darwin has no real-time signals, no `tgkill` and no `rt_sigqueueinfo`: guest signals must stay software queues (like `src/syscall_signal.c:37-47`), the host signal serving only to interrupt.
- Identity: virtual PID and TID from the family counter, not macOS identifiers. The main thread's TID must equal the PID (a Linux expectation, which Mach identifiers do not satisfy).

### 3.4 What depends on ARM64 Linux (list of x86-64 conversions)

1. Syscall numbers and registers: `src/syscall.h:17-170`, `case SYS_*` in `src/syscall.c:1277-2463` (182 lines), reading of x0-x5 and x8 (`src/syscall.c:1249-1255`), result in X0 and abuse of X8 as the shim's TLBI flag (2495-2507, moot in x86-64). x86-64: RAX, RDI, RSI, RDX, R10, R8, R9; RCX receives the next RIP and R11 the RFLAGS.
2. Order of `clone` arguments: arm64 `(flags, stack, ptid, tls, ctid)` (`src/syscall.c:2227`, `src/fork_ipc.c:1290-1293`); x86-64 `(flags, stack, ptid, ctid, tls)`.
3. Syscall restart: PC minus 4 (`src/syscall_signal.c:403,1037`); x86-64: RIP minus 2 and RAX restored from `orig_rax`, which does not exist in HL.
4. Signal frame: `linux_rt_sigframe_t`, FPSIMD and ESR (`src/syscall_signal.h:133-172`, `.c:841-899,1021-1186,1190-1255`). x86-64: `rt_sigframe` with `pretcode` taken from `sa_restorer` (no vDSO restorer), skip of the 128-byte red zone, stack alignment, x86 `sigcontext` (r8..r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags, cs/gs/fs, err, trapno, cr2), xsave area with magic `FP_XSTATE_MAGIC1`. FEX has written it (`SignalDelegator/GuestFramesManagement.cpp`, 856 lines).
5. Fault translation: decoding of ESR_EL1, FSC, ACCERR/MAPERR (`src/syscall_proc.c:1767-1887`). x86-64: #PF (error code, cr2), #UD (SIGILL), #DE (SIGFPE, `FPE_INTDIV`), `int3` (SIGTRAP with `si_code = SI_KERNEL`, RIP after the byte), single step (`TRAP_TRACE`).
6. Thread pointer: TPIDR_EL0 (`src/fork_ipc.c:1484-1489`). x86-64: FS base set by `CLONE_SETTLS` or `arch_prctl(ARCH_SET_FS)`; GS base for Wine's TEB.
7. ptrace views: `linux_user_pt_regs_t` (34 words, `src/syscall.h:215-220`), `linux_user_fpsimd_state_t`, `NT_ARM_HW_WATCH`, X7 convention at syscall stops (`probes/regset-architecture.md:18`). x86-64: `user_regs_struct` 27 words, `user_fpregs_struct` 512 bytes, `NT_X86_XSTATE`, debug registers through PEEKUSER and POKEUSER (Wine server depends on it, `voile-wine/server/ptrace.c:643-721`), requests GETREGS, SETREGS, GETFPREGS, SETFPREGS, `PTRACE_ARCH_PRCTL`, and at the syscall stop: `rax = -ENOSYS`, `orig_rax` = number, `rcx` and `r11` according to the semantics of `syscall`.
8. `struct epoll_event`: 16 bytes in arm64 (`src/syscall_poll.c:583-588`), 12 bytes packed in x86-64.
9. Values of `O_*`, `struct stat`, `statfs`, `AT_*` and auxv (vfs and loader, teammate): `O_DIRECTORY`, `O_NOFOLLOW`, `O_DIRECT`, `O_LARGEFILE` differ (HL's `CLAUDE.md`, section "aarch64-linux Open Flag Values").
10. x86-64 syscalls without an arm64 equivalent: about a hundred legacy calls (`fork` 57, `vfork` 58, `pause` 34, `alarm` 37, `poll` 7, `select` 23, `dup2` 33, `pipe` 22, `epoll_create` 213, `epoll_wait` 232, `eventfd` 284, `signalfd` 282, `inotify_init` 253, `getpgrp` 111, `time` 201) and x86-specific ones (`arch_prctl` 158, `modify_ldt` 154, `iopl`, `ioperm`). x86 `fork` and `vfork` go through the same `ForkGuest` as in FEX (`Syscalls/Thread.cpp:401-436`).

## 4. Gaps against Wine and EAC

Legend: present (P), partial (PA), absent (A). "Status" concerns the HL tree as it stands.

### 4.1 What Wine and EAC actually use

MEASURED by script over 18 `eac.log`/`control.log` logs from 8 EAC trial directories (`probes/eac-*`): `unimplemented_syscall` events of the private runtime, with a budget of 5 per process (field `budget`) and therefore not exhaustive. arm64 numbers: 10 (`fgetxattr`, 304 events, 7 runs, see 8), 282 (`userfaultfd`, 120, 7 runs, legitimate ENOSYS), 153 (`times`, 26, 7 runs). The same run `eac-ptrace-poke-20260930` logs 24 `sys_exit_group`, 13 `exec_shim_diagnostic`, 65 `wait4`: several dozen guest processes.

Wine (READ in `voile-wine/server`, used as a proxy for the server code of the Debian Wine 10 that the HyperLinux trials ran; identity of the two sources not checked): `send_thread_signal` sends SIGUSR1 to the clients through `tkill(unix_pid, unix_tid, sig)` to suspend, interrupt and terminate threads (`thread.c:961,1451,1468`, `ptrace.c:258-266`), SIGQUIT at violent death (`thread.c:1628`), and `kill(process->unix_pid, signal)` (`process.c:787`). Cross-process memory read and write first use `process_vm_readv` and `process_vm_writev`, then, if ENOSYS or EPERM, ptrace ATTACH, waitpid(SIGSTOP, `__WALL`), PEEKDATA, POKEDATA, DETACH (`ptrace.c:182-300,336-416,474-492`). Debug registers go through ATTACH, PEEKUSER, POKEUSER, DETACH (`ptrace.c:625-725`). So, before EAC, Wine already requires cross-process `tgkill` and either `process_vm_*` or ptrace on non-children.

### 4.2 ptrace

| Request | Value | Evidence of use | HL remote status | Note for an x86 view |
|---|---|---|---|---|
| TRACEME | 0 | EAC (child 148) | P | link created at fork only |
| PEEKTEXT, PEEKDATA | 1, 2 | EAC (PEEKDATA, poke run); Wine server (fallback) | A (EINVAL) | cooperative agent or Mach task of the tracee |
| PEEKUSER, POKEUSER | 3, 6 | Wine server (DR0-DR7) | A | x86 `struct user`, offsets of the debug registers |
| POKETEXT, POKEDATA | 4, 5 | EAC (byte 0xCC); Wine server (fallback) | A | write into an RX page, then invalidation of the translation cache |
| CONT | 7 | EAC | P | |
| KILL | 8 | not observed (EAC uses kill) | A | |
| SINGLESTEP | 9 | not observed (defined in `voile-wine/server/ptrace.c:64-65`, never called) | A | TF and `TRAP_TRACE` |
| GETREGS, SETREGS | 12, 13 | not observed | A | x86 requests |
| GETFPREGS, SETFPREGS | 14, 15 | not observed | A | 512 bytes |
| ATTACH | 16 | Wine server | A (non-child) | family process registry |
| DETACH | 17 | Wine server, EAC possible | P (child) | |
| SYSCALL | 24 | EAC (146 times) | P | stops at the FEX boundaries |
| SETOPTIONS | 0x4200 | EAC (0x11) | PA (mask 0x11 only) | TRACECLONE, TRACEFORK, TRACEEXIT, EXITKILL missing |
| GETEVENTMSG | 0x4201 | EAC possible | P | |
| GETSIGINFO, SETSIGINFO | 0x4202, 0x4203 | not observed | A remote (local Rosetta only) | |
| GETREGSET | 0x4204 | EAC (66 times, 216 bytes) | PA (272 ARM64 bytes) | 216 bytes = x86 `user_regs_struct` |
| SETREGSET | 0x4205 | EAC (RIP minus 1) | A in the tree (unqualified candidate, 74/76) | |
| SEIZE, INTERRUPT | 0x4206, 0x4207 | not observed remotely | A remote | |
| `PTRACE_GET_SYSCALL_INFO` | 0x420e | not observed | A | |

### 4.3 Clone flags and process creation

| Item | Status | Detail |
|---|---|---|
| glibc `pthread_create` (0x3d0f00 plus SETTLS, PARENT_SETTID, CHILD_CLEARTID) | P | `sys_clone_thread` |
| glibc `fork()` (0x1200011) | PA | no write of `ctid`, no SIGCHLD, no `SETTLS` |
| `vfork`, `posix_spawn` (`CLONE_VM|CLONE_VFORK`) | A (vm-clone path) | INFERRED broken, not tested |
| `CLONE_PTRACE`, `CLONE_UNTRACED`, `CLONE_PARENT`, `CLONE_PIDFD`, `CLONE_NEW*` | A | ignored without error |
| `clone3` | A | ENOSYS, glibc fallback |
| Thread limit | PA | 64 per process |

### 4.4 Signals, kill, tgkill

| Item | Status | Detail |
|---|---|---|
| `rt_sigaction`, `rt_sigprocmask`, `sigaltstack`, `rt_sigpending`, `rt_sigreturn` | P | ARM64 frame |
| `rt_sigsuspend` | PA | returns EINTR without sleeping |
| `rt_sigtimedwait`, `rt_sigqueueinfo` | A | |
| `rt_tgsigqueueinfo` | PA | intra-process |
| intra-process `kill`, `tkill`, `tgkill` | P | `SI_TKILL` honored |
| `kill` to a child | PA | SIGKILL and SIGCONT only |
| `kill`, `tgkill` to another process | A | required by Wine server |
| SIGCHLD at the death of a child | A | |
| Real-time signals 32 to 64 | P | queue of 32 |
| `signalfd4` | PA | 16 slots, local |
| ITIMER_REAL | P | emulated; ITIMER_VIRTUAL and PROF not |
| Waits interrupted by a guest signal | PA | yes for read and write, poll, select, epoll without timeout, wait4, futex; no for `nanosleep`, `clock_nanosleep`, `accept`, `connect`, `recv*`, `send*` (1.2) |

### 4.5 prctl, rlimits, personality, seccomp, io_uring, pidfd

| Item | Status | Detail |
|---|---|---|
| `prctl` SET_NAME, GET_NAME | P | name registry |
| `prctl` SET_PDEATHSIG | PA | stub that returns 0 |
| `prctl` NO_NEW_PRIVS, DUMPABLE | PA | stubs (`src/syscall.c:1857-1882`) |
| `prctl` SET_MEM_MODEL | PA | EINVAL, intended for Rosetta |
| `prctl` SECCOMP, PTRACER (Yama), CHILD_SUBREAPER, TIMERSLACK, SYSCALL_USER_DISPATCH, CAPBSET | A | ENOSYS (Linux returns EINVAL for an unknown option) |
| `getrlimit`, `setrlimit`, `prlimit64` | PA | resources 0 to 9 to the macOS limits; `pid` ignored; 10 to 15 (LOCKS, SIGPENDING, MSGQUEUE, NICE, RTPRIO, RTTIME) return EINVAL (`syscall_sys.c`) |
| `personality` | A in HL | emulated by FEX itself (`Info.cpp:72`) in mode A |
| `seccomp` (syscall) and `prctl(PR_SET_SECCOMP)` | A in HL | emulated by FEX's `SeccompEmulator` (`Info.cpp:93`, `Thread.cpp:485-492`) in mode A |
| `io_uring_*` | A | FEX also answers "unimplemented safe" (`Passthrough.cpp:360-362`) |
| `pidfd_open`, `pidfd_send_signal`, `pidfd_getfd` | A | |
| `process_vm_readv`, `process_vm_writev` | A | first option of Wine server and of EAC |
| `unshare`, `setns`, namespaces | A | |
| `sched_setscheduler`, `sched_getscheduler`, `sched_getparam`, `sched_get_priority_*` | A | `sched_setaffinity` PA (stub 0), `sched_getaffinity` P |
| `getcpu`, `times`, `capget`, `capset`, `setgroups`, `getsid` | A | `times` observed in the EAC logs |

### 4.6 futex

| Operation | Status |
|---|---|
| WAIT, WAKE, REQUEUE, CMP_REQUEUE, WAKE_OP | P (process-local) |
| WAIT_BITSET, WAKE_BITSET | PA (absolute clock treated as real time) |
| LOCK_PI, UNLOCK_PI, TRYLOCK_PI | PA (no priority inheritance, no OWNER_DIED) |
| WAIT_REQUEUE_PI, CMP_REQUEUE_PI, LOCK_PI2 | A |
| `FUTEX_PRIVATE_FLAG` | PA (ignored; everything is private) |
| futex shared between processes | A |
| `set_robust_list` | PA (stub); `get_robust_list` A |
| `futex_waitv`, `futex_wake`, `futex_wait`, `futex_requeue` (449, 454-456) | A |

Candidate Darwin primitive for the shared case: `os_sync_wait_on_address` with the "shared" flag (the teammate is qualifying it in `g1_futex`, not executed to date).

### 4.7 Polling and notification objects

| Item | Status | Detail |
|---|---|---|
| `ppoll`, `pselect6` | P | |
| epoll IN, OUT, ERR, HUP, RDHUP, ET, ONESHOT | P | kqueue |
| epoll PRI, EXCLUSIVE, WAKEUP, `epoll_pwait2` | A | |
| eventfd (counter, semaphore, nonblock) | PA | local, 32 max, not shareable |
| timerfd | PA | kqueue, `ABSTIME` ignored, 32 max, not inheritable |
| signalfd | PA | local, 16 max |
| inotify | PA | kqueue vnode, 16 instances, no rename cookie, directory names sometimes missing, no recursion (`src/syscall_inotify.c:1-20`) |

### 4.8 Sockets

| Item | Status | Detail |
|---|---|---|
| AF_UNIX stream and datagram, `socketpair` | P | `SCM_RIGHTS` with guest to host fd translation and `cmsghdr` layout (`src/syscall_net.c:1296-1352,1564-1600`) |
| abstract AF_UNIX | PA | path derived by FNV hash plus `flock` lock (`:60-200`) |
| AF_INET, AF_INET6, TCP, UDP | P | option numbers translated (`src/syscall_net.h:36-62`) |
| DNS | PA | plain UDP and TCP sockets to the server from the vfs `resolv.conf`; no emulated resolution |
| `SOCK_SEQPACKET` | PA | simulated as a stream `socketpair` for rosettad only (`:538-570`) |
| `SO_PEERCRED`, `SCM_CREDENTIALS`, `SO_PASSCRED` | A | |
| AF_NETLINK, AF_PACKET | A | "best effort passthrough" (`:40-48`): the Linux numbers 16 and 17 designate `AF_APPLETALK` and `AF_ROUTE` in the macOS SDK (`sys/socket.h:383-384`), there is no `AF_NETLINK`; glibc's `getifaddrs` goes through netlink |
| socket options | PA | 17 `SOL_SOCKET` options, 4 TCP options, 3 IP and IPv6 options translated (`src/syscall_net.h:36-62`) |

### 4.9 Coverage of FEX's passthrough syscalls

INFERRED by name comparison (script: names from `Passthrough.cpp` against the `SYS_*` of `src/syscall.c` and `.h`): out of 241 passthrough names, 128 are covered, 113 not covered. Not covered, the most relevant for Wine and EAC: `capget`, `capset`, `futex_waitv`, `futex_wait`, `futex_wake`, `futex_requeue`, `get_robust_list`, `getcpu`, `getsid`, `mlock2`, `mlockall`, `pidfd_getfd`, `pidfd_open`, `pidfd_send_signal`, `process_vm_readv`, `process_vm_writev`, `rt_sigqueueinfo`, `sched_get_priority_max`, `sched_get_priority_min`, `sched_getparam`, `sched_getscheduler`, `sched_setscheduler`, `sched_rr_get_interval`, `setgroups`, `settimeofday`, `clock_settime`, `times`, `timer_create`, `timer_settime`, `timer_gettime`, `timer_delete`, `unshare`, `setns`, `msgget`, `msgsnd`, `msgrcv`, `msgctl`, `semget`, `semop`, `semtimedop`, `io_setup`, `io_submit`, `io_getevents`, `ioprio_set`, `ioprio_get`, `syncfs`, `sync_file_range`, `readahead`, `fanotify_init`. The full list is in my command history; the total depends on name normalization (for example `fadvise64`).

## 5. Test infrastructure

### 5.1 Compile

- What HL's Makefile does: `HL_SRCS` (39 sources), CFLAGS `-O2 -std=c11 -Werror=vla -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Wimplicit-fallthrough -Wundef -Wnull-dereference -Wno-unused-parameter` (`Makefile:74-77`), shim assembled by `as -arch arm64` then extracted into `shim_blob.h`, signing with `entitlements.plist` (`Makefile:30,169`). The guest tests require an `aarch64-unknown-linux-musl-` toolchain or prebuilt binaries (`GUEST_TEST_BINARIES`) provided by `nix develop`; `nix` is not installed here (`probes/ptrace-remote-setregset-runtime-20261001/README.md:29-30`).
- Compilation only in the working copy, without execution: `bash EXP/tools/build-hl-scratch.sh`. MEASURED: 39 objects, 0 failures, 0 warnings, 0 errors, link successful, 2.9 s (`logs/hl-compile.log` empty). The binary `_build/hl` is not signed with the HVF entitlement and was not launched.

### 5.2 Pure host tests (without HVF)

Run: `bash EXP/tools/build-run-host-tests.sh` (2.7 s). MEASURED, replayed in this session (`logs/host-run-all-rerun.log`):

| Program | Result | Link with the process layer |
|---|---|---|
| test-fd-object-lite | PASS, 13 OK lines | fd objects |
| test-vfs-unit | PASS, 16 OK lines | vfs (teammate) |
| test-app-open | PASS | unrelated |
| test-oss-abi | PASS | unrelated |
| test-fd-digest | PASS, 9 | unrelated |
| test-aot-format | PASS, 7 | Rosetta, unrelated |
| test-thread-vcpu-lifetime | PASS, 7, with `scratch-hl/stub-trace.c` | `thread.c`, interruption and destruction of vCPUs |
| test-page-table-pool | PASS, 7 | memory (teammate) |
| proc-ptrace-host (source `probes/proc-ptrace-host.c`, compiled by me in `EXP/scratch-hl/probe-bin`) | 79 PASS, 0 FAIL (`logs/host-test-proc-ptrace-host.log`) | full `proc_ptrace.c`: 23 checks of basics, options, syscalls, registers, concurrent stops |
| kq_fork_scm (written by me, `EXP/scratch-hl/probe-src/kq_fork_scm.c`) | see 1.4 (`logs/host-test-kq-fork-scm.log`) | kqueue, fork, `SCM_RIGHTS` |

Known failure: with the Makefile as is, `test-thread-vcpu-lifetime` does not link (`_proc_trace_tracer_exit` undefined; `thread.c:248` calls `proc_trace_tracer_exit`, which the `test-host-units` recipe does not link, `Makefile:409`), see `logs/host-build-test-thread-vcpu-lifetime-asis.log`. The workaround is a stub in the working directory only. Not executed: the four audio tests (CoreAudio) and `test-diagnostics.sh` (requires a guest).

The tests `test/test-*.c` (69 programs, plus 6 `helper-*.c` and 15 scripts) are guest programs: they require a signed `hl` and HVF, so they do not run here. For a FEX layer: 77 static musl x86-64 binaries are already compiled in `test-binaries/amd64/bin` (full list: `ls`), including the process tests `test-fork`, `test-cow-fork`, `test-fork-exec`, `test-exec`, `test-pthread`, `test-thread`, `test-futex-pi`, `test-signal`, `test-signal-thread`, `test-signalfd`, `test-timerfd`, `test-eventfd`, `test-epoll`, `test-poll`, `test-poll-ready`, `test-poll-wakeup`, `test-socket`, `test-net`, `test-inotify`, `test-tgkill-target`, `test-sigpipe-survival`, `test-sigill`, `test-stress`. The reference matrix (43 programs chosen per architecture, `test-binaries/TEST-REPORT.md`) is driven by `run-unit-matrix.sh` and `bounded-hl.py` (15 s per guest, own process group); replacing the `HL` argument with a `fexdarwin` executable is enough. Expected-failure policy: `test/matrix-xfails.sh` (`test-signal` and `test-signal-thread` fail under Rosetta because of `SA_RESETHAND`; will not apply to FEX).

### 5.3 Oracles

No Linux oracle can be executed here without a VM. Recorded oracles reusable offline: Linux ARM64 and x64 references under OrbStack (`probes/ptrace-lifecycle/results`, 7/7, 13/13, 9/9, 40/40, 24/24 depending on the fixture), native x86 references under QEMU TCG (`probes/ptrace-x86-full-context-control-20260929`, 272/272; `probes/ptrace-setregset-control-20261001/qemu-*`, 76/76 in five modes). The fixture sources are in `probes/ptrace-*/`. Local compilation of x86-64 guests: neither `zig`, nor `x86_64-linux-gnu-gcc`, nor `ld.lld` are in the `PATH` (measured by `which`) and Apple's linker does not produce ELF, but the shared venv contains `ziglang 0.16.0` (`EXP/tools/venv`, read-only on my part). MEASURED: `"$PY" -m ziglang cc -target x86_64-linux-musl -static -O2 -I<hyper-linux/test> <test>.c -o <output> -lpthread` produces a static x86-64 ELF for 5 programs out of 5 tried (`test-fork`, `test-pthread`, `test-futex-pi`, `test-signal-thread`, `test-epoll`), output in `EXP/scratch-hl/guest-x86_64/`, logs `logs/host-build-guest-*.log`, without Docker. None of these binaries was executed (no runtime). The 77 prebuilt binaries (Debian container, `test-binaries/build-guests.sh`) remain the comparison reference with the existing matrix. The oracle logs remain usable as expected values.

## 6. Estimate in lines of code and five risks

### 6.1 Lines

INFERRED, to within about 30%, computed from the line ranges of section 2 and the reading of the files.

| Item | Lines |
|---|---|
| To keep or adapt mechanically (A) | 9,500 |
| To rewrite on the same principle (R) | 2,300 |
| To drop (D) | 6,700 |
| New: fork, clone, exec glue on FEX (native fork, threads, host re-execution, vfork emulation, registry reset in the child) | 900 to 1,300 |
| New: signals (cross-process mailbox, SIGCHLD, blocking `sigsuspend`, `rt_sigtimedwait`, `rt_sigqueueinfo`, x86 frame bridge) | 1,100 to 1,600 |
| New: wait and status (registry of Linux exit statuses, `WIFSIGNALED`, minimal groups and sessions) | 400 to 700 |
| New: x86 ptrace (GETREGS/SETREGS/REGSET prstatus, fpregs, xstate, PEEK/POKE/USER, cooperative agent, invalidation, SINGLESTEP, KILL, ATTACH/SEIZE/INTERRUPT, CLONE/FORK/EXIT events) | 2,200 to 3,200 |
| New: shared futex, robust list, waitv, clocks | 500 to 900 |
| New: special objects shareable and fork-safe | 300 to 500 |
| New: missing syscalls (sched_*, getcpu, times, capget, process_vm_*, pidfd_*, personality) | 600 to 1,000 |
| New: host tests of the new modules | 1,200 to 2,000 |
| Total new | 7,200 to 11,200 |

These figures assume mode A (section 3.1) and do not count the port of FEX's Linux layer to Darwin (clone, host signals, allocator), which belongs to the FEXCore report.

### 6.2 The five heaviest risks

1. Cooperative x86 ptrace between two processes, with translation cache coherence (EAC). The only risk that depends on a kernel ptrace relation: EAC calls the ptrace of the guest world, and macOS offers no equivalent relation (Darwin's `ptrace(2)` sees only the host arm64 state, and `task_for_pid` requires an authorization that the instructions forbid triggering). The emulation described in 6.3 is therefore needed. Hard points: rebuilding an exact x86 context at stops that are in JIT code or in a handler (`ReconstructCompactedEFLAGS`, `RestoreRIPFromHostPC` in `Context.h`); the x86 semantics of RCX, R11 and `orig_rax` at syscall stops (measured on Rosetta: `probes/ptrace-x86-feasibility-20260929.md:13,47-51`); the invalidation of the tracee's translation cache after a POKE (failure seen with Rosetta: stale translation after the byte was restored, 74/76, `probes/ptrace-remote-setregset-runtime-20261001/README.md:32-44`); access to the tracee's memory from another process.
2. Asynchronous delivery of guest signals on Darwin. No real-time signals, no `tgkill`, no `rt_sigqueueinfo`, a single reserved host signal available to interrupt JIT code at an exact point. Wine server suspends, interrupts and terminates threads with cross-process SIGUSR1, glibc uses real-time signals for cancellation and `setxid`. Software queues are needed, a cross-process mailbox, and waits (`futex`, `poll`, `read`, `accept`, `wait4`) all interruptible with the right EINTR and restart semantics. Does not depend on a kernel ptrace.
3. Synchronization shared between processes: futex on shared sections, esync eventfds, wait and SIGCHLD. HL has nothing shared (futex, eventfd counters, child tables). Candidate: `os_sync_wait_on_address` in shared mode, to qualify. Does not depend on a kernel ptrace.
4. fork, exec and vfork on a process that contains a live JIT. Locks held by absent threads (MEASURED by the teammate: mutex, `os_unfair_lock` and stdio stay locked in the child and block a grandchild, `evidence/darwin-process/f1_fork_mt.log`), kqueues not inherited (MEASURED here), family registry to keep alive across a host exec, semantics of `CLONE_VM|CLONE_VFORK` that a fork does not reproduce (the exec error that glibc returns to the parent through the shared stack would never reach it; FEX has the same limit, `Syscalls/Thread.cpp:293-294`). Does not depend on a kernel ptrace.
5. Linux fidelity in polling and sockets, and hard limits. kqueue instead of epoll (EXCLUSIVE, PRI), packed `epoll_event`, AF_NETLINK missing, `SO_PEERCRED` missing, 64 threads, 1024 fds, special objects limited to 16 or 32, `SIGCHLD` missing. Each one is small, the sum decides whether Wine starts. Does not depend on a kernel ptrace.

### 6.3 User-space emulation of the tracer/tracee relation between two processes of the runtime

Principle taken from HL (already tested on the host: 79/79) and adapted to FEX.

1. When a child is created by `fork`, the parent creates an unlinked fixed-size state file, mapped `MAP_SHARED`, and a pair of Unix sockets. The child inherits them, nobody else (`proc_trace_create`, `proc_trace_import`). A family registry by TID (like `proc_names.c`) lets a third party find a relation; it will have to be extended for ATTACH and SEIZE of a non-child (new: rendezvous through a named socket in a family directory).
2. Mutual exclusion: `fcntl` byte lock plus local mutex. It is released at the death of the owner; MEASURED by the host test.
3. TRACEME, SETOPTIONS, CONT, DETACH, SYSCALL, GETEVENTMSG are writes into this file. The tracer never touches the tracee directly.
4. Stop: the tracee reaches a FEX safe point (return of `HandleSyscall` entry or exit, delivery of a signal by the `SignalDelegator`, SIGTRAP fault of an `int3`, end of exec). It rebuilds its x86 context from `CpuStateFrame` and, at asynchronous points, from the host context of the signal handler. It publishes in the file: stop sequence number, type, signal, `user_regs_struct` (27 words), and the fpregs if needed. It notifies the socket then waits for resumption (`poll`), without holding a lock.
5. The tracer waits through wait4 or waitid: the status `(event << 16) | (signal << 8) | 0x7f` is rebuilt from the sequence (`proc_trace_wait`).
6. GETREGSET, GETREGS, GETFPREGS are reads of the file. SETREGSET, SETREGS, POKEUSER are commands: the tracer writes the command into the file with the targeted stop sequence and notifies; the tracee, or a helper thread of the same process, executes and publishes the result, like HL's SETREGSET candidate which checks the sequence to reject stale writes.
7. PEEK and POKE: the tracee (or its helper thread) does the read or write in its address space, with a temporary `vm_protect` for an RX page, then calls the engine's code invalidation (FEX: `InvalidateGuestCodeRange` / `InvalidateThreadCachedCodeRange` from `Context.h`) before answering. This choice avoids both `task_for_pid` and cache incoherence. Alternative to qualify by the teammate (series H): obtain the tracee's task port through a Mach message sent by the tracee itself, without `task_for_pid` authorization.
8. Death of a peer: EOF of the socket (`peer_closed`), detach with signal suppressed, as on Linux (references `probes/ptrace-lifecycle`).

What this scheme does not give for free: stopping a running thread (PTRACE_INTERRUPT, SIGSTOP sent by the tracer) requires the "kick" of risk 2; group stop; reparenting.

## 7. Why the private fixes exist

READ (diff, hunk comments, `STATUS.md`, `probes/ptrace-*.md`). Lines changed = insertions plus deletions from `git diff --stat`.

| Subsystem | Files (lines changed) | Why |
|---|---|---|
| Threads | `thread.c/h` (392 plus 47) | Interrupt blocking host I/O with a private signal so that `tgkill` is not lost (`src/thread.c:47-140`); EL1 stack relocated (the shim moves to the top of memory to leave the low area to Wine); ptrace stop with siginfo, watchpoints, FP, explicit interruption; `WNOWAIT` and `waitid` semantics; cleanup of the tracer and names at exit |
| Processes | `syscall_proc.c/h` (991 plus 31) | Family PID allocator, name registry, `wait4` and `waitid` rewritten (stops, continuation, `WNOWAIT`, `SA_RESTART` restart, timer during the wait), remote `sys_ptrace`, register capture with provenance (`proc_regset_capture_context`) |
| Signals | `syscall_signal.c/h` (444 plus 16) | Per-thread directed signals, full `siginfo`, `SA_RESTART` restart through saved arguments, ptrace stop before the disposition, fork semantics (pending cleared), ptrace stop SIGCHLD |
| Dispatch | `syscall.c/h` (608 plus 21) | ptrace ENTRY and EXIT stop points (`syscall_trace_boundary`), list of restartable calls, `tkill`/`tgkill` with `SI_TKILL` (glibc `setxid`, musl cancellation), `rt_tgsigqueueinfo` (Rosetta sends back its faults), plus memory (teammate) |
| Fork | `fork_ipc.c` (457) | IPC version 18: ptrace links with ACK, PID allocator, names, reopen states, epoll rebuild, file views, SHM, vDSO |
| Exec | `syscall_exec.c` (70) | Shebang with intact argv, VFS resolution, vDSO mapped, TRACEEXEC event |
| Network | `syscall_net.c` (123) | Guest buffers scattered over several host mappings, introduced by the shared file views |
| New modules | `proc_ptrace.c/h`, `proc_names.c/h`, `signal_notify.h`, `fault_dump.c/h` | EAC needs: `/proc/<pid>` of another process, ptrace relation between processes, stop SIGCHLD notification, fault diagnostic |
| Startup | `hl.c` (60), `shim.S` (22), `guest.c` (562), `vdso.c`, `stack.c` | Shim moved to the top of guest memory so that Wine can map the low area ("Wine's DOS area", comment in `src/vdso.c:279`), vDSO mapped explicitly |
| GDB | `gdb_stub.c/h` (7) | `gdb_stub_owns_debug_regs`: GDB owns the debug registers, ptrace does not touch them |

## 8. Side findings

- `SYS_fgetxattr` is 16 and `SYS_fremovexattr` 18 in `src/syscall.h:166,169`; the arm64 numbers are 10 and 16 (`Arm64/SyscallsEnum.h:22,28` in FEX). Effect: call 10 is not implemented (304 events in the EAC logs) and call 16 (fremovexattr) would be handled as fgetxattr. The dispatch uses these constants (`src/syscall.c:2328-2339`).
- The Makefile's `test-host-units` recipe does not link `test-thread-vcpu-lifetime` on the working copy (5.2).
- HL's `CLAUDE.md` lags behind the code: it describes `sys_ptrace` as limited to Rosetta and PI futexes as not implemented, whereas `futex.c` implements them and `proc_ptrace.c` exists.
- `FUTEX_WAIT_BITSET` always treats the clock as real time (1.8).
- `fork` does not propagate `CLONE_CHILD_SETTID` (1.3).

## 9. Unknowns and measurements not done

- No guest execution: all the HL behaviors cited come from the code, from existing logs or from host tests. The INFERRED points (`clone` failure with an open timerfd, vfork, WIFSIGNALED, glibc cached TID) were not executed.
- I did not read in full `syscall_io.c`, `syscall_fs.c`, `syscall_time.c`, `guest.c`, `vdso.c`, `proc_emulation.c`, `stack.c`, `shim.S`. My coupling counts are based on their `grep` lines, not on reading them.
- I did not measure the number of threads of Rocket League under Wine; the limit of 64 is therefore an unquantified risk.
- FEX's behavior at syscall stops (RCX, R11, RFLAGS, `orig_rax`) is not checked; it belongs to the FEX teammate (witness (h)).
- I did not qualify `os_sync_wait_on_address` in shared mode, nor the exchange of Mach task ports between cooperating processes; they are in the G and H series of the Darwin teammate.
- The 21 EAC ptrace call sites are not all associated with a request: the set of requests needed may exceed the observed list.

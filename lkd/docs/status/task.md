# Status of the task module (LKD)

Date: 2026-10-02. Milestone 3 reached: all syscalls of the scope written and tested, in the private build (core + task, then core + task + fs + mm) and in the common build with all modules.

## What exists (src/task, 4755 lines with the tests)

| File | Content |
| --- | --- |
| `task_priv.h` | internal interfaces, safety caps (300 threads per process, 64 live children, 512 live processes per family) |
| `family.c` | shared family registry: temporary file deleted right away, `MAP_SHARED`, 1024 processes and 8192 threads; local mutex then `fcntl` byte lock (`family.c:146`); shared atomic pid counter, never reused (`family.c:138`); export and attach through `LKD_FAMILY_FD`; public lookups |
| `tasks.c` | task and process objects, local table, publication, `lkd_task_create_main` (`tasks.c:551`, takeover of a record after a host re-exec), thread exit, `exit_group`, `lkd_task_release` (`tasks.c:702`), `de_thread` (`tasks.c:747`), fork hooks |
| `ids.c` | pid, tid, ppid, pgid, sid, setpgid, setsid, uid/gid family, groups, capget/capset, personality |
| `sys.c` | uname, rlimits, getrusage, times, sysinfo, sched_*, priorities, getcpu, getrandom, set_tid_address, robust lists, exit, exit_group, arch_prctl, prctl, iopl, ioperm, unshare, setns |
| `time.c` | clocks, dynamic CPU clocks, nanosleep, clock_nanosleep, gettimeofday, time, read-only adjtimex, itimers, alarm |
| `clone.c` | clone, clone3, fork, vfork, `lkd_task_fork_child` hook (`clone.c:54`) |
| `wait.c` | death of a process (`wait.c:166`), reparenting, parent notification, wait4, waitid, host children |
| `exec.c` | execve, execveat, `lkd_exec_prepare`, `lkd_task_exec_commit` hook, `lkd_exec_finish` |

## Tests (MEASURED)

Common build `./build.sh -k 0` then `tools/run_tests.sh build/tests` (`test_task_proc` recounted on 2026-10-10, after the orphans):

| Program | Assertions | Cases |
| --- | --- | --- |
| `test_task_ids` | 228 | 16 |
| `test_task_time` | 118 | 10 |
| `test_task_proc` | 260 | 21 |
| `test_task_exec` | 89 | 9 |
| `test_task_bigwin` | 4 | 1 |
| total task | 699 | 57 |

- The four pass in the common build (all modules linked, including the real sig module) and in the private core + task build (the exec cases that need the vfs are reported `SKIP` there); three to five consecutive runs without failure, no process left, temporary directory removed.
- `test_core_contract` (89) passes against the task module.
- `test_core_dispatch` passed (155) in the private core + task build; in the common build it no longer links, because of its own non-static handlers that conflict with fs (request `task-1.md` point 5).
- The only other failure of the common suite is `test_fs_pipe` (pending bits of SIGPIPE and EINTR with the real sig module): unrelated to task.
- The fork tests really fork (at most three processes at the same time), each child has `alarm(20)`. The thread tests use the fake engine.

## Hang reported under the real engine (2026-10-02, after milestone 3)

`/bin/sh -c '/bin/true; echo status=$?'` stayed stuck in `wait4` (or after the final `write` with `LKD_TRACE=1`). MEASURED (`sample`): no lost wakeup in task; the vfork child, moved into its own host process at exec, was spinning in `lkd_mm_reset_for_exec -> mm_host_cleanup`, which walks the whole 2^46-byte window host page by host page; the shell then did the same in its `exit_group` (`lkd_mm_put`). With the loop fixed in a copy of the tree, the command finishes in 1 s with `status=0` and the expected syscall order. Request `docs/requests/task-4.md` to the mm module. Test added: `tests/test_task_bigwin.c` (production window, 5 s bound): FAILS on the current tree (20,114 ms), PASSES with the fix (524 ms). It will pass when mm has fixed its loop.

## Behaviors (READ in Linux v6.12, MEASURED by the tests)

1. Identity. The main thread has `tid == pid`. The first process has the pid `first_pid` (2 by default), never 1 unless configured, and `ppid` 1 by default (`lkd_main_spec.ppid` changes it). No process 1 is invented: `/proc/1` only exists if the runtime creates a process with pid 1. The first process is session and group leader (pgid = sid = pid). The first process registered without a parent in the family is its init (`fam_child_reaper`, registry header, version 2): it plays the role of the `child_reaper` of a Linux pid namespace, without taking pid 1 (reasons: `docs/journal/01-integration-status.md`, orphans section of 2026-10-10).
2. Shared registry. Each process of the family reads the name, ppid, pgid, sid, state, exe, cmdline (4096 bytes at most), number of threads, exit status, limits of another one. `wait4` publishes the `S` state of the waiting thread (case `waiting_parent_shows_sleeping`).
3. clone. Three forms (`clone.c`, header): thread (`spawn_thread`), process sharing the memory (vfork, posix_spawn: host thread of the parent's host process until the exec, the parent really waits, the child's write to memory is seen by the parent, so glibc's `posix_spawn` error report works), host fork. The flags follow `copy_process()` and `clone3_args_valid()`: SETTLS, PARENT_SETTID, CHILD_SETTID (in the child's memory after fork), CHILD_CLEARTID, PARENT, VFORK, THREAD, SIGHAND, VM, FILES, FS. `CLONE_FILES` or `CLONE_FS` without `CLONE_VM` answers EINVAL (impossible between two host processes, logged). Namespaces: EPERM. `CLONE_PIDFD`: EINVAL (no pidfd). clone3's `set_tid`: EPERM.
4. exec. Resolution through the vfs, x permission according to fsuid/fsgid/groups, `#!` (single argument, truncated name ENOEXEC, 5 levels then ELOOP like `exec_binprm`), ELF64 x86-64 `ET_EXEC`/`ET_DYN` otherwise ENOEXEC (no i386 emulation), empty argv replaced by `""`, E2BIG for a string of more than 128 KiB or a total beyond min(RLIMIT_STACK/4, 6 MiB, 2 MiB), `AT_SYMLINK_NOFOLLOW` on a link ELOOP, `execveat` with `/dev/fd/N[/path]`. Point of no return: `de_thread` (the non-leader thread takes the leader's tid), move of a vfork child to a host fork, `lkd_exec_commit`, name = base of the file name, exe = loaded file, cmdline, environ, dumpable 0 if the file is not readable, robust list and `clear_child_tid` handled like `exec_mm_release`, the parent's setpgid then EACCES. Failure of `exec_image` after this point: SIGSEGV.
5. Exit. `exit` code `(c & 0xff) << 8`, `exit_group` kills the other threads (FATAL + kick), the first group status wins. Per thread: robust list (sig), `clear_child_tid` set to 0 and futex woken if the memory has other users, `task_exit` hooks, parent death signal for the children created by this thread. Per process: zombie published, children reparented (`forget_original_parent`, `find_new_reaper`) to the closest live subreaper among the ancestors, otherwise to the family's init, otherwise 1 when the init itself dies. Each reparented child receives its parent death signal (SI_USER, pid of the dead parent) and takes SIGCHLD as its exit signal (`reparent_leader`); a reparented zombie is announced to the new parent through SIGCHLD, or released if no live process takes it. A host process that died without going through its exit leaves its children orphaned the same way (`poll_host_children`, case `orphans_of_a_killed_host_process`). Then exit signal to the parent (SIGCHLD through `lkd_sig_child_event`, another signal with `CLD_*` siginfo), immediate release if sig asks for it.
6. wait4/waitid. WNOHANG, WUNTRACED/WSTOPPED, WCONTINUED, WNOWAIT, WEXITED, `__WALL`, `__WCLONE`, `__WNOTHREAD`, selection by pid, group (0 = its own) or all, ECHILD, EINVAL for unknown options, `INT_MIN` ESRCH, `P_PIDFD` EBADF, waitid's siginfo zeroed if there is nothing, cumulative rusage of the children. A host child that died without going through its exit (host SIGKILL, engine crash) is reported with the host's real status (case `host_killed_child_is_reported`). The wait is interruptible (ERESTARTSYS) and rereads the registry every 10 ms at most.
7. Time. REALTIME, REALTIME_COARSE, TAI, REALTIME_ALARM: Darwin's `CLOCK_REALTIME`. MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE: `CLOCK_UPTIME_RAW` (stops during sleep like Linux MONOTONIC). BOOTTIME: Darwin's `CLOCK_MONOTONIC_RAW` (counts sleep). CPU clocks `~pid << 3 | which`: process (self or any process of the family through `proc_pidinfo`), thread (self only). Sleep: long part interruptible through the wakeup pipe, last millisecond through host `nanosleep` (measured precision under the millisecond); relative: remainder written and restart through `restart_syscall` at the same deadline; absolute: ERESTARTNOHAND. itimers REAL, VIRTUAL, PROF through an auxiliary host thread per process (SIGALRM/SIGVTALRM/SIGPROF with `SI_KERNEL`), not inherited by fork.
8. Limits and resources. Linux defaults (stack 8 MiB, core 0, NOFILE 1024, MEMLOCK 8 MiB, MSGQUEUE 819200, NICE and RTPRIO 0), NOFILE hard limit = the host's `kern.maxfilesperproc`, NPROC = host limit. getrusage SELF, THREAD (time of the Mach thread), CHILDREN. sysinfo: real memory, load, swap, uptime rounded up like `do_sysinfo`.
9. prctl: SET/GET_NAME, DUMPABLE (0 or 1), PDEATHSIG, CHILD_SUBREAPER, NO_NEW_PRIVS, TID_ADDRESS, KEEPCAPS, TIMERSLACK, CAPBSET_READ/DROP, CAP_AMBIENT, SECUREBITS, THP_DISABLE, MCE_KILL, TSC, SPECULATION_CTRL (not affected), SET_VMA (through mm), GET_AUXV (through the layout reported by the engine); SECCOMP, SYSCALL_USER_DISPATCH, MDWE, PTRACER: EINVAL (logged for the two in the middle); unknown options EINVAL.
10. arch_prctl: SET/GET_FS and GS through the engine's `get_regs`/`set_regs`, EPERM beyond `TASK_SIZE_MAX`, `ARCH_CET_STATUS` EINVAL, CPUID faulting ENODEV, LAM and shadow stack like a CPU without these features.

## Syscalls left unregistered (ENOSYS counted, deliberate choice)

`rseq` (glibc does without it), `seccomp` (kernel modeled without `CONFIG_SECCOMP`), `pidfd_open`, `modify_ldt` (no LDT in the engine), `timer_create`, `timer_settime`, `timer_gettime`, `timer_getoverrun`, `timer_delete` (POSIX timers not written yet), `ioprio_get`, `ioprio_set`.

## Visible ENOSYS or EPERM answers inside registered syscalls

- `prlimit64` that modifies another process, `sched_setscheduler`, `sched_setattr`, `setpriority` on a thread of another host process: logged ENOSYS. `setpriority` by group or user: ENOSYS.
- `sched_setaffinity` on a thread of another host process goes through the inter-process service (`famsvc.h`, opcode `TASK_OP_SET_AFFINITY`, 97): the process that owns the thread sets the mask and publishes it. This is how wineserver applies a game's `SetThreadAffinityMask`. Before, ENOSYS made this call fail, and Wwise abandons a thread whose mask it cannot set (ELDEN RING, 2026-10-10). MEASURED: `affinity_of_a_thread_in_another_host_process` in `test_task_proc`.
- `clock_nanosleep` on a CPU time clock: logged EOPNOTSUPP. CPU clock of another thread: logged EINVAL.
- `arch_prctl(ARCH_*_XCOMP_*)`: logged EINVAL (the XSAVE set belongs to the engine).

## Known gaps (INFERRED unless stated)

1. Affinity and nice are recorded and reported, not applied to the host (Darwin has no thread placement API nor per-thread nice).
2. `pdeathsig` is stored per process in the registry (Linux: per thread of the child). Sent through `lkd_sig_send_pid`.
3. A process sharing its parent's memory (vfork before exec) dies with the parent's host process if the latter exits: it is then reported killed by SIGKILL.
4. A host process of the family that dies abruptly and whose guest parent is not its host parent (`CLONE_PARENT` case) stays `R` in the registry: no real status is available.
5. `cmdline` of the other processes truncated to 4096 bytes, `exe` to 512.
6. The CPU clocks of a process sharing the host of another one count the whole host process.
7. The published state of another host process is `S` only during wait4; for the other waits, request `task-1.md` point 4 is needed.
8. The exit of the main thread alone (other threads alive) currently ends the process on the engine side (`task-3.md` point 2).
9. The engine's `exec_image` answers ENOSYS: every exec reaches the point of no return then kills the process with SIGSEGV (`task-3.md` point 1).
10. `lkd_tid_lookup` publishes the current state of a local thread before answering; `syscall_nr` of a thread of another host process is the one of the last publication.
11. The family's init does not have the rules of Linux's pid 1: `kill(-1)` does not spare it, it does not have the `SIGNAL_UNKILLABLE` protection, and its death does not kill the rest of the family (`zap_pid_ns_processes`). It is the engine's supervisor (`FEXDARWIN_SUPERVISOR_PID`) that then ends the host processes. Until then, the remaining orphans have parent 1, which does not exist.
12. The first process keeps `ppid` 1 while `/proc/1` does not exist. Linux would give 0 for a parent outside the pid namespace (case of a process that entered through `setns`). Not changed for lack of a measured need.

## Open requests

`docs/requests/task-1.md` (core), `task-2.md` (sig, ptrace, procfs, fs), `task-3.md` (engine), `task-4.md` (mm, reset of the address space).

## Possible next steps

POSIX timers (`timer_*`), famsvc opcodes for operations on another host process, cleanup of a zombie whose host process vanished without a status.

## getrusage (2026-10-03, profile of the game menu)

`getrusage(RUSAGE_THREAD)` no longer does a host `getrusage(RUSAGE_SELF)`, which walks all the threads of the host process, only to fill ru_maxrss.
- ru_maxrss comes from `MACH_TASK_BASIC_INFO.resident_size_max`, sampled at most every 10 ms.
- The times come from `thread_info` of the calling thread.

MEASURED (`w20_statbench`, a single thread): from 0.93 us to 0.47 us per call. The gain grows with the number of threads of the host process. RUSAGE_SELF stays the exact sum from the host (0.5 us here).

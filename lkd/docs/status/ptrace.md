# Status of the ptrace module (LKD)

Date: 2026-10-02, 20:40 (after the restart at 20:21). Milestone 3: the EAC sequence passes end to end between two real host processes, the requests, the options, the events, the death of the tracer and of the tracee are tested. 4 test programs, 852 assertions (MEASURED, private core + mm + fs + task + sig + ptrace build in `build-ptrace-agent/` (project `proj/`, `-j 4`, `nice`), each test under `src/fexdarwin/scripts/run-with-timeout.sh`, no process left after the runs; 5 runs in a row without failure before the restart, one after).

## What exists

`src/ptrace/` (NOTICE: blocks imported from HyperLinux `proc_ptrace.c`, Apache-2.0):

- `transport.c`: table of links shared by the whole family. An unlinked temporary file is mapped `MAP_SHARED` (512 slots of 3.4 KiB), created by the first process, inherited by `fork`, named by `LKD_PTRACE_FD` for a host re-exec. Mutual exclusion: local mutex (rank 61) plus `fcntl` byte lock on the file (the host kernel releases it when the holder dies: MEASURED, case `lock_is_released_when_the_holder_dies`). Wakeup of the tracee and notification of the tracer through `famsvc` (opcodes 64 and 65 of the ptrace range). No other transport.
- `stop.c`: tracee side. A stop is published in the slot (Linux code, message, siginfo, register view, FXSAVE and XSAVE images, signal mask) by the tracee's host thread, which parks on its wakeup pipe. The tracer's writes (SETREGS, SETFPREGS, SETSIGINFO, SETSIGMASK, resume signal, mode, step) are commands that the tracee applies itself on resume, in a single critical section; a write after the resume fails (ESRCH). Hooks: `lkd_ptrace_syscall_enter/exit`, `lkd_ptrace_signal_stop`, `lkd_ptrace_event`, `lkd_ptrace_stop`, `lkd_ptrace_trap_stop`.
- `request.c`: the `ptrace(2)` call in the kernel's error order.
- `life.c`: `wait4`/`waitid` (`lkd_ptrace_wait_event`, `lkd_ptrace_has_tracees`, `lkd_ptrace_wait_has`), `lkd_ptrace_check_access`, `lkd_ptrace_tracer_of`, death of a tracer (detach, EXITKILL), death of a tracee (`PTRACE_EVENT_EXIT` stop, report to the tracer, vanished host process), fork/vfork/clone auto-attach, module hooks (init, fork, exec, exit).
- `regs.c`: `getreg/putreg`, selectors, `FLAG_MASK`, FS/GS bases, debug registers, MXCSR, XSAVE header.

## Implemented requests

TRACEME, ATTACH, SEIZE, INTERRUPT, DETACH, KILL, CONT, SYSCALL, SYSEMU, SINGLESTEP and SYSEMU_SINGLESTEP (EIO as long as the engine has no `set_single_step`), SINGLEBLOCK (EIO), PEEKTEXT/PEEKDATA, POKETEXT/POKEDATA (through `lkd_mm_remote_*`, which invalidates the target's translated code), PEEKUSER/POKEUSER (registers, debug registers at offset 848, empty area as zero), GETREGS/SETREGS, GETFPREGS/SETFPREGS, GETREGSET/SETREGSET (NT_PRSTATUS 216, NT_PRFPREG 512, NT_X86_XSTATE of the engine's size, NT_386_IOPERM: ENXIO on read and EOPNOTSUPP on write), GETSIGINFO/SETSIGINFO, GETEVENTMSG, SETOPTIONS (mask 0x3000ff; SUSPEND_SECCOMP = EINVAL), GETSIGMASK/SETSIGMASK, GET_SYSCALL_INFO (24, 80 or 33 bytes), ARCH_PRCTL. Logged EIO: LISTEN, PEEKSIGINFO, SECCOMP_GET_*, GET_RSEQ_CONFIGURATION, user dispatch, GET/SET_THREAD_AREA, GET/SETFPXREGS, any unknown request.

Wait statuses: `(exit_code << 8) | 0x7f`: 0x137f (SIGSTOP), 0x57f, 0x857f (TRACESYSGOOD), 0x4057f (EXEC), 0x1057f (FORK), 0x6057f (EXIT), 0x80057f (PTRACE_EVENT_STOP), 0x80137f (group stop of a SEIZE tracee). `waitid` returns CLD_TRAPPED for any ptrace stop (`wait_task_stopped`, line 1345 of kernel/exit.c v6.12).

## What passes (MEASURED)

| Program | Assertions | Content |
| --- | --- | --- |
| `test_ptrace_requests` | 368 | 17 cases, tracee in a thread: ATTACH and access rules, SEIZE, ESRCH depending on the tracer and the state, regset formats, SETREGSET and putreg, USER area and debug registers, FP and XSTATE, syscall stops and GET_SYSCALL_INFO, change of number, arguments and result, `orig_rax` = -1, SYSEMU, signals (CONT(0), CONT(sig), replaced signal, SETSIGINFO, signal given to a syscall stop requeued as SI_KERNEL), single-step refused, INTERRUPT, mask, ARCH_PRCTL, unmodeled requests, PEEK/POKE and invalidation |
| `test_ptrace_eac` | 134 | the EAC sequence between two host processes (see below) |
| `test_ptrace_life` | 302 | 16 cases: death of the tracer (detach, signal suppressed, EXITKILL), death of the tracee (status, TRACEEXIT), KILL, DETACH with signal, wait4/waitid (WNOWAIT, selection, WNOTHREAD), group stop (non-SEIZE: EINVAL for GETSIGINFO; SEIZE: event 128), exec with and without TRACEEXEC, TRACEME and creating thread, fork with TRACEFORK and auto-attach (3 processes), event options, auto-attach matrix, selectors, tracee killed from outside (vanished host process), tracer killed from outside (the parked tracee resumes 594 ms after, MEASURED), copy of the ids at the exec of a non-leader thread |
| `test_ptrace_transport` | 48 | full table (EAGAIN at 513), lock released on death, table shared between processes, wakeup between processes, measurements |

EAC sequence (`test_ptrace_eac`, real fork, the child plays the engine): TRACEME, tgkill(SIGSTOP) -> 0x137f with `waitid(WNOWAIT)` then `wait4`, SETOPTIONS 0x11, PTRACE_SYSCALL x4 with a 216-byte GETREGSET (entry: rax = -38, orig_rax, rcx = rip, r11 = rflags; exit: rax = result; message 1 and 2), CONT, exec event 0x4057f (message = old tid, siginfo 5/0x405), PTRACE_SYSCALL -> execve exit 0x857f (message 2, orig_rax 59, rax 0, rip = entry, eflags 0x202, fs_base = 0, rcx = 0), PEEK then POKE of the INT3 into a read-only code page of the tracee (the tracee sees 0xcc and `invalidate_code` was called once before its resume), CONT, SIGTRAP 0x57f (si_code 128, si_addr 0, rip = int3 + 1, orig_rax = -1, cs 0x33, ss 0x2b, eflags 0x246), SETREGSET (rip - 1), restoring POKE, NT_PRFPREG save, POKE of the path and of the return address 9 on the stack, SETREGSET (rdi, rsi, rdx, rip, rsp), CONT, SIGSEGV 0xb7f (si_addr 9, rax = result), restore of the registers and of the FP, CONT(0) (the tracee sees the restored values), second SIGSEGV and CONT(11): the tracee dies, `wait4` returns 11.

Cost of a stop and a resume (MEASURED, M5 Max, `test_ptrace_transport`, 1500 rounds, PTRACE_SYSCALL + wait for the next stop):

| Mode | p50 | p90 | p99 |
| --- | --- | --- | --- |
| tracee in another host process, `wait4(WNOHANG)` in a loop | 42 us | 63 us | 120 us |
| tracee in a thread of the same process (the tracee's loop sleeps 300 us) | 367 us | 423 us | 481 us |
| tracee in another process, blocking `wait4` (current task module) | 11,110 us then 36 us (two runs) | 11,203 us then 80 us | 11,253 us then 10,757 us |
| tracee in a thread, blocking `wait4` (current task module) | 11,127 us | 11,529 us | 11,587 us |
| same, with the `wait.c` fix of request ptrace-2 (scratchpad copy) | 43 us | 53 us | 73 us |
| 216-byte GETREGSET (read of the snapshot, without a round trip) | 2 us | 3 us | 3 us |

The blocking `wait4` loses up to 11 ms per stop (depending on whether the wakeup falls before or during the 10 ms slice) because of task's wait loop: see `docs/requests/ptrace-2.md` (four-line fix, measured).

## Known differences

1. `PTRACE_INTERRUPT` only stops at the next syscall, signal or event boundary as long as `lkd_signal_next` does not honor `LKD_TF_TRACE_STOP` (request ptrace-1). Setting this flag without that would make the tracee's blocking waits spin idle. A tracee in pure translated code or blocked in a syscall is not interrupted.
2. No single-step: the engine has no `set_single_step` (EIO, logged). No stop is fabricated.
3. `orig_rax` of a signal stop: -1 for a fault signal (`si_code > 0` or `SI_KERNEL`), otherwise the engine's value (the last syscall). An asynchronous signal should also give -1 (request ptrace-1, engine).
4. The execve exit stop is emitted in `lkd_ptrace_event(EXEC)`, because `lkd_syscall` does not call `lkd_ptrace_syscall_exit` for `LKD_CONTEXT_REPLACED` (request ptrace-1). Same limit for `rt_sigreturn`: no exit stop.
5. Yama missing (kernel modeled without Yama, like `PR_SET_PTRACER` = EINVAL in task): only the classic rules of `__ptrace_may_access` apply. The registry does not keep the suid and sgid of a target: euid and egid replace them.
6. Tracee that is not a child of the tracer: the real parent learns of the death through the task module right away, not after the collection by the tracer (no ptrace reparenting). If the tracer is the parent, ptrace does not report the death (task does).
7. A tracee whose host process vanishes without going through exit is reported as killed by SIGKILL (real status unknown).
8. `PTRACE_LISTEN`, `PEEKSIGINFO`, seccomp, rseq, user dispatch, `GET/SET_THREAD_AREA`: logged EIO. `SETREGSET` reads the whole user buffer before applying (Linux can apply a prefix then fault).
9. Blocking `wait4`: 11 ms latency (ptrace-2). `wait4(pid)` of a pid that is not a child waits instead of returning ECHILD as long as the tracer has a tracee (`lkd_ptrace_wait_has` exists, task must call it). `waitid`: `si_status` of a ptrace stop is truncated to 8 bits by task.
10. Exec from a non-leader thread: the tracee's tid and the tracer's tid are copied into the table by `lkd_ptrace_exec_commit` (tested by simulating the tid change, not with a real de_thread).

## Open requests

`docs/requests/ptrace-1.md` (sig: TRACE_STOP; core: exit stop for a replaced context; engine: orig_rax, single-step), `docs/requests/ptrace-2.md` (task: wait4 wakeup, selector, si_status).

## Possible next steps

End-to-end tests with the real FEX engine (the project lead runs the x86 witnesses: `probe-x86_64`, `target-x86_64`, `target-flags-x86_64`), asynchronous stop through `kick` in translated code, single-step, ptrace reparenting, `PTRACE_LISTEN`.

## Incident of 2026-10-02 20:05

Four crash reports `test_ptrace_life-2026-10-02-2005*.ips`: all in `traceme_rules`, `test_ptrace_life.c:365`, read of `x->proc` after `lkd_task_release(x)` in a temporary debug print of the test (use after free in the test, not in the module). Fixed at 20:10: the test keeps the tid before the release. MEASURED after the restart: `test_ptrace_life` 302 assertions, 0 failures.

## x86-64 EAC probe campaign (2026-10-02, 20:44 to 20:58, integration maintainer)

Launcher: `witnesses/eac-probes/run-probes.py`, host `build-integration/host/fexdarwin-run` linked to `build-integration/lib/liblkd.a`.

MEASURED:

| Run | Probes | Checks | Failures |
| --- | --- | --- | --- |
| before (20261002T204415, identical to the project lead's 20261002T204027) | 39 | 1824 | 232 |
| after the three LKD fixes (20261002T205456) | 39 | 3257 | 7 |
| after the launcher fix (20261002T205719) | 40 | 4376 | 38 |

The number of checks goes up because the probes that stopped at the first failure now go to the end.

LKD fixes, each with a test that fails before and passes after:

1. Forced POKE into read-only code whose 16 KiB host page also contains data (cold_INT3_POKEDATA: EIO). Cause: `mm_local_rw` (`src/mm/remote.c`) deduced the host protection from the union of the VMAs (RW), while the page watched by the JIT was still read-only after the watch was removed (`mm_unwatch_for_write` leaves the opening to the FOLL_FORCE writer). Fix: when the watch has just been removed, the page is opened explicitly. Test: `test_mm_watch` case `host_page_shared_with_data`.
2. Exec of a tracee born from a fork (GETEVENTMSG and GETSIGINFO cluster after the exec stop, 14 probes). Cause: `lkd_ptrace_exec_commit` (`src/ptrace/life.c`) followed the tid change of `de_thread` from `tid_seen`, which still holds the parent's tid in the child of a fork (ptrace's fork hook runs before the identity change). The `tracer_tid` of the link became the tracee's tid and every following request returned ESRCH. Fix: only rewrite the links of the same thread group. Test: `test_ptrace_life` case `exec_of_a_forked_tracee_keeps_its_tracer`.
3. Warm code after POKE (warm_INT3, "warm repeat execution after write"): invalidation called from the mm service thread, without a guest thread, ignored by the engine. Fixed on the engine side by the project lead (InvalidateCode without a thread). The replay mechanism added for a while in LKD was removed.

Launcher (`run-probes.py`): the probe's command line now takes that of its `run-hl.py` (target, mode or `pre`/`post` phase only if they appear in it; before, `peek`, `poke`, `poke-commit`, `proc-remote-maps*` exited with 87 or 89). The probe runs in a private, writable copy of its directory, because the probes create marker files in their current directory (`/`). Before: EROFS on `owned marker O_EXCL creation`.

Remaining failures (MEASURED, 20261002T205719):

- `ptrace-peek-control`, `ptrace-poke-control` (9 each), `ptrace-poke-commit` pre and post (7 each): the child maps with MAP_FIXED at 0x7ffff7ff0000 and 0x7ffffdffb000. This address goes beyond the guest window of 2^46 bytes (LKD's TASK_SIZE, `docs/requests/mm-2.md` point 3). The child exits with 70 before the stop. READ: a 2^47 window does not fit in the host address space with the 8 TiB base. Design limit, not a ptrace defect.
- `ptrace-setregset-deliver` (2) and `ptrace-setregset-two-trap` (4): `CONT(pid, SIGTRAP)` at the delivery stop of a SIGTRAP, without a handler. LKD delivers SIGTRAP and the tracee dies (status 5). INFERRED: this is Linux's behavior (`get_signal`, default action coredump, RLIMIT_CORE 0). The probe's expected 0 has no native oracle in its directory, and the recorded HyperLinux run failed too. To be confirmed on a real Linux before any change.

## Integration update (2026-10-02, 21:45)

Differences 1 (INTERRUPT of a blocked tracee), 4 (exit stop for a replaced context, rt_sigreturn included) and 9 (wait4: 11 ms latency, ECHILD, si_status) fixed, with the tests `interrupt_reaches_a_blocked_tracee`, `sigreturn_has_an_exit_stop`, `blocking_wait4_wakes_at_once` and `wait_selectors_see_tracees`. See requests ptrace-1 and ptrace-2 ("Handled" sections).

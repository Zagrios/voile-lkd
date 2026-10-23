# Darwin facts on process inspection, Mach exceptions, time and identity (report B)

Living report, updated at each milestone. Last update: 2026-10-02 10:20 (draft 1, resumption after interruption of the session).

Convention: MEASURED = output of a program run on this machine, journal cited. READ = reading of a source or a header. DEDUCED = inference, to be qualified. A section marked "to be completed" has no measurement yet.

Working root: `build/experiments/rocket-wine-fex-darwin-20261002` (written `$EXP`). Witness sources: `$EXP/witnesses/darwin-process/`. Journals: `$EXP/evidence/darwin-process/`.

## Progress state

| Witness | Subject | State |
| --- | --- | --- |
| `h0_sysinfo.sh` | environment facts (SIP, boot-args, version, Rosetta) | run, `evidence/darwin-process/h0_sysinfo.log` |
| `j1_time_ident.c` | clocks, timers, identifiers, limits | source left by the interrupted attempt, partial journal (stopped in J2), to be rerun |
| `h1_*` task port, memory, thread states | H | to be written |
| `h2_*` ptrace | H | to be written |
| `h3_*` proc_pidinfo, sysctl | H | to be written |
| `h4_*` cooperative channel | H | to be written |
| `i1_*` Mach exceptions versus signals | I | to be written |
| `i2_*` `svc` probe | I | to be written |
| `h5_*` guest ptrace transport | K | to be written |

## Environment facts (H0, MEASURED)

Source: `evidence/darwin-process/h0_sysinfo.log`, run on 2026-10-02 10:11.

- `sw_vers`: macOS 27.0.1, build 26A434. `sysctl kern.osversion`: 26A434.
- `uname -a`: `Darwin 27.0.0 ... root:xnu-13432.1.9~1/RELEASE_ARM64_T6050 arm64`.
- Machine: `Mac17,7`, Apple M5 Max, 18 cores (2 performance levels, 6 "Super" cores), 36 GiB, 16384-byte pages, `hw.tbfrequency` 24000000.
- `kern.hv_support`: 1 (HVF available; out of scope, noted for the record).
- SIP: `System Integrity Protection status: enabled.`
- `boot-args`: `sysctl kern.bootargs` empty; `nvram boot-args` and `csr-active-config` absent (no custom setting).
- Developer mode: `DevToolsSecurity -status` replies "Developer mode is currently disabled".
- Authorization rights `system.privilege.taskport` and `system.privilege.taskport.debug`: class `user`, group `_developer`, `authenticate-user` true, `allow-root` false (so a `task_for_pid` outside the `get-task-allow` path would request authentication).
- Rosetta: present (`/Library/Apple/usr/libexec/oah/libRosettaRuntime`, `RosettaLinux`, `translate_tool`, package `com.apple.pkg.RosettaUpdateAuto`). `sysctl.proc_translated` is 0 for our shell (native arm64 process). No `arch -x86_64` was launched.
- User: uid 501, gid 20, member of `_developer` and `admin`. No root.
- System limits: `kern.maxproc` 9000, `kern.maxprocperuid` 6000, `kern.num_taskthreads` 9216, `kern.num_threads` 46080, `kern.maxfilesperproc` 138240, `kern.argmax` 1048576. `ulimit -n` 1048576 in this shell, `ulimit -u` 6000.
- Tools: Apple clang 21.0.0, Xcode 27.0, SDK 27.0.

## H: inspect another process (provisional results, h1 and h2 run)

Witnesses: `witnesses/darwin-process/h1_task_inspect.c`, `h2_ptrace.c`, journals `evidence/darwin-process/h1_task_inspect.log` and `h2_ptrace.log`. The debuggee is the same binary relaunched with the argument `debuggee` (signed `com.apple.security.get-task-allow`, main thread in an assembly loop with known registers, one thread blocked in `read(2)`, one server thread).

### H1, task port of a direct child signed get-task-allow (MEASURED, h1)

- `task_for_pid(mach_task_self(), child_pid)` succeeds without prompt: first call 1116 us, following calls p50 188 us (min 175 us, max 772 us). The cost of 190 us per call is that of a round trip with taskgated: a tracer must keep the port, not request it again. `pid_for_task` returns the right pid.
- `task_name_for_pid`, `task_inspect_for_pid` and `task_read_for_pid` also succeed on this child, and `mach_vm_read` works through the `task_read` port.
- `mach_vm_read_overwrite`: 8 bytes p50 0.62 us, 64 bytes 0.62 us, 4 KiB 0.71 us, 64 KiB 2.33 us (28 GB/s), 1 MiB 28.9 us (36 GB/s). `mach_vm_read` plus `mach_vm_deallocate` of 4 KiB: 1.96 us. `mach_vm_write` of 8 bytes: 0.62 us. Content read back equal to the pattern written by the child over 1 MiB.
- `mach_vm_remap(copy=FALSE)` of an anonymous buffer of the child into the tracer succeeds (current prot 3 = rw, max 7): the mapping is shared (a write by the child is seen by the tracer, a write by the tracer is seen by the child), and an 8-byte read through this mapping costs less than the clock resolution (41 ns). This is the fastest path for a future `/proc/<pid>/mem` or burst PEEK.
- `mach_vm_write` directly on a read-only page fails with `KERN_INVALID_ADDRESS` (kr=1, not `KERN_PROTECTION_FAILURE`). `mach_vm_protect(RW|VM_PROT_COPY)`, write, then back to read-only works (POKE semantics on a non-writable page).
- Write into a `__TEXT` code page of the debuggee: `mach_vm_protect(RW|COPY)`, `mach_vm_write` and the return to R-X succeed, but the child dies of `SIGBUS` (Bus error: 10) as soon as it runs the modified page, with or without a prior `ptrace(PT_ATTACHEXC)`. A POKETEXT on signed Mach-O code is therefore not possible. The exception code remains to be captured (h6).
- `mach_vm_region`: 66 regions walked in 0.054 ms (0.8 us per call). `mach_vm_region_recurse`: 192 leaf regions in 18.4 ms (the sub-mapping of the shared cache is expensive). `task_info(TASK_DYLD_INFO)` and `TASK_VM_INFO` work (virtual size of the debuggee: about 466 GiB, page size 16384).
- `task_threads` returns 3 threads; `THREAD_IDENTIFIER_INFO.thread_id` on the tracer side equals `pthread_threadid_np` on the child side; `THREAD_EXTENDED_INFO.pth_name` returns the thread name (equivalent of `/proc/pid/task/tid/comm`); `THREAD_BASIC_INFO.run_state` gives the state (1 running, 3 waiting).
- `thread_get_state(ARM_THREAD_STATE64)` on the main thread: the 26 constant registers x1..x17, x19..x26, x28 are read back exactly, pc in the loop, x27 consistent with the counter in memory (difference 1), `ARM_NEON_STATE64` v0..v7 exact, `ARM_EXCEPTION_STATE64` (esr 0x56000080 = last `svc #0x80`), `ARM_DEBUG_STATE64` (130 words, mdscr_el1 = 0). `thread_get_state` also works on a running thread, without `thread_suspend` (p50 0.67 us, p90 7.8 us).
- Latencies: `thread_suspend` p50 0.38 us (p90 4.5), `thread_get_state` on a suspended thread p50 0.62 us, `thread_resume` p50 0.38 us, full cycle p50 1.46 us, p90 7.96 us, p99 11.7 us. `task_suspend` plus `task_resume` p50 0.79 us. During `task_suspend` the child no longer responds; after `task_resume` it responds.
- A thread blocked in `read(2)` shows pc = `read`+8 (just after the `svc #0x80`), x16 = 3 (BSD syscall number) and x0 = the descriptor: the state of a thread in a system call is readable without cooperation.
- `thread_set_state(pc = function, x0..x2)` on a running thread redirects it: the target function does receive the three arguments (equivalent of SETREGS plus CONT).

### H2, ptrace between a process and its children (MEASURED, h2)

- `ptrace(PT_ATTACHEXC)` and `ptrace(10 = PT_ATTACH)` from a parent on its child signed get-task-allow succeed without prompt (21 us); `waitpid(WUNTRACED)` reports a `SIGSTOP` stop.
- Requests that do not exist on arm64: `PT_READ_I/D/U` and `PT_WRITE_I/D/U` return `EINVAL`, likewise `PT_THUPDATE` with my arguments and all requests 15 to 29 and 32 to 40; `PT_FORCEQUOTA` returns `EPERM`. There is no GETREGS, no SETREGS, no PEEK, no POKE, no SYSCALL, no SEIZE, no event options: all of this must go through Mach or through a cooperative channel.
- Classic mode (`PT_TRACE_ME` in the child, then a signal): signal stop reported by `waitpid` with the signal number, all the child's threads are stopped, `PT_CONTINUE` with `data=0` suppresses the signal, with `data=signal` it is delivered (the child dies of SIGUSR1). The `exec` produces a `SIGTRAP` stop, without event code.
- `PT_STEP` in classic mode works: the pc advances 4 bytes per step; 5000 consecutive steps, round trip `PT_STEP` plus `waitpid` p50 10.75 us, p90 14.1 us, p99 20.8 us. The step applies to the first thread (main thread).
- Exception mode (`PT_ATTACHEXC`): the initial `SIGSTOP` stop stops only the thread that took the signal (the server thread keeps responding); a later signal is reported to `waitpid` but `PT_CONTINUE` and `PT_DETACH` then return `ESRCH`, and the child stays blocked. This mode requires a Mach exception handler on the tracer side (h6).
- `PT_DENY_ATTACH` in the child: a later `PT_ATTACHEXC` returns `EBUSY` and the kernel sends `SIGSEGV` to the tracer, which dies. `task_for_pid` keeps succeeding on this debuggee.
- A sibling (not the parent) can attach: the tracer's `ppid` becomes the sibling's pid during the trace (BSD style, the kernel reparents the tracee), the original parent gets `ECHILD` on `waitpid`, the `ppid` comes back after `PT_DETACH`.

### H3, proc_pidinfo and sysctl without task port (MEASURED, h3)

Witness `h3_pidinfo.c`, journal `h3_pidinfo.log`. Target: the debugged child (8 threads), the parent process (same uid, not debuggable) and pid 1 (root).

- On the child and on the parent, all the flavors tried succeed without task port: `PROC_PIDLISTFDS`, `TASKALLINFO`, `TBSDINFO`, `TASKINFO`, `LISTTHREADS`, `REGIONINFO`, `REGIONPATHINFO`, `VNODEPATHINFO`, `T_SHORTBSDINFO`, `UNIQIDENTIFIERINFO`, `ARCHINFO`, `LISTTHREADIDS` (28), `PLATFORMINFO` (30), plus `proc_pidpath`. On pid 1 (other uid) only `T_SHORTBSDINFO`, `UNIQIDENTIFIERINFO`, `PLATFORMINFO` and `proc_pidpath` respond, the rest returns `EPERM`.
- `PROC_PIDLISTTHREADIDS` returns the 64-bit identifiers of the threads, equal to the child's `pthread_threadid_np` (3 matches out of 3 known); `PROC_PIDLISTTHREADS` returns thread handles (pointers), not identifiers. `PROC_PIDTHREADID64INFO(tid)` returns name, state (`pth_run_state` 1 running, 3 waiting), priority, user and system time: basis of `/proc/pid/task/tid/{comm,stat}`.
- `PROC_PIDREGIONPATHINFO` walks the 76 regions of the child in 1.2 ms (16 us per region, file paths included for 4 regions) without task port, and `PROC_PIDREGIONINFO` walks the 61 regions of the parent process (not debuggable, same uid): a `/proc/<pid>/maps` for any process of the user is possible without rights, the end of the walk returns `EINVAL`.
- `PROC_PIDVNODEPATHINFO` gives the current directory, `proc_pidpath` the path of the executable, `PROC_PIDLISTFDS` plus `proc_pidfdinfo(PROC_PIDFDVNODEPATHINFO)` the descriptors and their paths, `KERN_PROCARGS2` argc, path and arguments (also on the parent; on pid 1 `EINVAL`). `kinfo_proc` gives comm, state, ppid, pgid, uid, number of groups.
- Latencies (p50): `PROC_PIDTBSDINFO` 0.17 us, `PIDTHREADID64INFO` 0.17 us, list of thread identifiers 0.21 us, `PIDLISTFDS` 0.25 us, `PIDREGIONINFO` 0.42 us, `PIDTASKINFO` 0.46 us, `proc_pidpath` 0.88 us, `KERN_PROCARGS2` 2.67 us, `sysctl KERN_PROC_PID` 7.2 us.
- Limit: there is no notion of namespaced `Tgid` or `PPid`, and `pbi_status` (SRUN=2) does not distinguish the Linux states R, S, D, T; these come from `pth_run_state` per thread.

### H4, cooperative channel versus task port (MEASURED, h4)

Witness `h4_coop_channel.c`. The target is a binary launched by exec that owns 4 MiB of private memory; the tracer wants `len` bytes at an address of this memory. Time from the request to the arrival of the bytes in the tracer's buffer, p50 in us:

| Method | 8 B | 4 KiB | 64 KiB | 1 MiB |
| --- | --- | --- | --- | --- |
| direct read of a shared mapping (shm) | 0.00 | 0.04 | 0.54 | 10.8 |
| `mach_vm_remap` of the target's private memory (read by memcpy) | 0.00 | 0.04 | 0.50 | 10.8 |
| `mach_vm_read_overwrite` (task port) | 0.75 | 0.75 | 22.6 | 68.9 |
| request/reply, busy wait on shared memory | 0.12 | 0.46 | 1.79 | 25.5 |
| pipe, notification only (data in shared memory) | 4.21 | 4.29 | 7.58 | 44.6 |
| pipe, data in the reply | 4.29 | 4.42 | 7.12 | 121.7 |
| named POSIX semaphore, both directions | 4.17 | 4.21 | 7.04 | 44.4 |
| socketpair AF_UNIX stream | 4.79 | 5.58 | 7.54 | 44.3 |
| socketpair AF_UNIX dgram | 5.12 | 5.83 | 7.42 | 44.2 |
| Mach message (inline data in the reply) | 6.29 | 6.79 | 20.25 | not measured |
| futex `os_sync_wait_on_address` with `SHARED`, both directions | 6.08 | 6.25 | 7.29 | 45.6 |
| mutex and condition variable `PTHREAD_PROCESS_SHARED` | 7.79 | 7.58 | 8.08 | 44.9 |

- No measurable effect of the QoS class (default versus `USER_INTERACTIVE`) on these times. p99 of the blocking paths: 9 to 12 us.
- A blocking inter-process wakeup costs about 4 us (pipe, semaphore) to 6 us (shared futex, Mach message). The shared futex is not faster than a pipe. Busy waiting goes down to 0.12 us but occupies a core.
- Named `shm_open` plus `ftruncate` plus `mmap(MAP_SHARED)` and `sem_open` work without any particular entitlement; `PTHREAD_PROCESS_SHARED` mutexes and conditions work in the shared page.
- The task port's `mach_vm_read_overwrite` has a fixed cost of about 0.6 us (kernel round trip) then grows with size (36 GB/s in bulk transfer): it is the best path without mapping cooperation, 5 to 10 times faster than a round trip through socket or pipe.

### H5, passing Mach ports without launchd and without task_for_pid (MEASURED, h6_port_inherit)

Witness `h6_port_inherit.c`.

- The child can give its own `mach_task_self()` to the parent by message (`MACH_MSG_TYPE_COPY_SEND` in a port descriptor): the port is movable, and the parent gets `pid_for_task`, `mach_vm_read`, `task_threads` and `thread_get_state` that succeed. This holds for the child signed get-task-allow and also for the child signed without any entitlement (`.noent` copy, non-hardened ad hoc signature). No prompt. Cooperative attachment therefore provides the inspection right without `task_for_pid`, and without get-task-allow, for our own non-hardened Mach-O binaries. Not to be generalized to a binary with hardened runtime or a platform binary: not tested.
- Means of carrying the first port: the special slot `TASK_BOOTSTRAP_PORT`, inherited by `fork` (the parent temporarily places its port there, then restores it) or set by `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)`. Both work for a forked child and for an exec'd child. `TASK_ACCESS_PORT` makes `posix_spawn` fail with `EINVAL`. `mach_ports_register` plus `mach_ports_lookup` give a port to the child, but the messages sent on it do not reach the parent: unusable.
- `bootstrap_register` from an ad hoc signed tool succeeds (`kr=0`), `bootstrap_look_up` finds the name, `bootstrap_check_in` returns 1103 (service active): a rendezvous by name in the user's session is available without a launchd plist, the name disappears with the port.
- An exception port set at task level is inherited by `fork` and by `fork` plus `exec`. A tracer that set it receives the faults of its descendants (with their control ports in the message), the equivalent of `PTRACE_O_TRACEFORK` for exceptions.

### H6, the debugger model through Mach exceptions on a foreign task (MEASURED, h7_foreign_exc)

Witness `h7_foreign_exc.c`. The tracer sets `task_set_exception_ports` on the task of its get-task-allow child and serves the exceptions in a handler thread.

- `brk #0x77`: `EXC_BREAKPOINT`, codes `[1, pc]`, `pc` pointing at the `brk`; `udf #0x1234`: `EXC_BAD_INSTRUCTION`, codes `[1, instruction word]`; `ldr` at 0x10: `EXC_BAD_ACCESS`, codes `[1 (KERN_INVALID_ADDRESS), address]`. The handler rewrites x0 and `pc+4`, replies `KERN_SUCCESS` and the child resumes with the rewritten state, for the three behaviors (`EXCEPTION_STATE_IDENTITY`, `EXCEPTION_STATE`, `EXCEPTION_DEFAULT`). The `DEFAULT` and `STATE_IDENTITY` messages carry the control port of the faulting task (usable by `mach_vm_read`).
- Cost of an inter-process exception: command through pipe plus exception plus reply, p50 11.7 us (STATE_IDENTITY), 10.7 us (STATE), 12.0 us (DEFAULT), versus 5.9, 4.7 and 6.0 us for the same command without exception: about 6 us per exception.
- Write on a read-only page: `EXC_BAD_ACCESS`, codes `[2 (KERN_PROTECTION_FAILURE), address]`. The tracer makes the page writable through `mach_vm_protect` in the child and replies: the instruction is replayed and succeeds (p50 14.0 us for the whole command, about 8 us per fault). This is the write-protection mechanism at host page granularity (16 KiB).
- Execution of h1's modified code page: `EXC_BAD_ACCESS` with `KERN_PROTECTION_FAILURE` on the instruction fetch (address = pc), then `SIGBUS`: a modified signed page is no longer executable, even after `mach_vm_protect(R-X)`.
- Hardware step: `thread_set_state(ARM_DEBUG_STATE64)` with `mdscr_el1` bit 0 gives one `EXC_BREAKPOINT` (codes `[1, 0]`) per instruction. The kernel clears the bit at each step: the handler must rearm it before replying. 20,000 steps at 9.3 us per step; 3.1% of the exceptions are repeated at the same pc without progress (a step client must tolerate it) and all the others advance by exactly one instruction (exact loop counter).
- Hardware breakpoints and watchpoints: `bvr0`/`bcr0` (value 0x1e5) on `hi_target_fn` gives `EXC_BREAKPOINT` `[1, pc]`; `wvr0`/`wcr0` (value 0x1ff5, write on 8 bytes) gives `EXC_BREAKPOINT` `[0x102, data address]`. `hw.optional.breakpoint` is 6 and `hw.optional.watchpoint` is 4 on this machine: enough to emulate the 4 x86 debug registers DR0 to DR3, subject to the width difference.
- `PT_ATTACHEXC` plus exception port: the attach `SIGSTOP` and the later signals arrive as `EXC_SOFTWARE`, codes `[0x10003 (EXC_SOFT_SIGNAL), signal number]`. Replying only `KERN_SUCCESS` lets the signal be delivered (the child dies of SIGUSR1); `ptrace(PT_THUPDATE, pid, thread_port, 0)` before replying suppresses the signal (the child survives); with `data=SIGUSR1` it delivers it. This is a signal-delivery stop with choice of the injected signal, like `PTRACE_CONT` with signal.

## I: Mach exceptions and traps (to be completed)

To be completed: exception ports for oneself, rewritten state, comparison of signals versus Mach, `svc #0` probe in a child process.

## J: time and identity

Partial results of the interrupted attempt (`evidence/darwin-process/j1_time_ident.log`, 2026-10-02 09:59, stopped during J2). To be redone, the full rerun is in progress.

- MEASURED `mach_timebase_info`: 125/3, that is 41.67 ns per tick (24 MHz).
- MEASURED resolution: `CLOCK_REALTIME` and `CLOCK_MONOTONIC` advance in steps of 1000 ns (microsecond). `CLOCK_MONOTONIC_RAW`, `CLOCK_UPTIME_RAW` and `mach_absolute_time` advance in steps of 41 ns.
- MEASURED cost of a call: `CLOCK_MONOTONIC_RAW_APPROX` 4.8 ns, `mach_absolute_time` 5.0 ns, `CLOCK_REALTIME` 12.8 ns, `CLOCK_MONOTONIC` 18.3 ns, `CLOCK_THREAD_CPUTIME_ID` 133 ns, `CLOCK_PROCESS_CPUTIME_ID` 469 ns.
- MEASURED `CLOCK_MONOTONIC` lags `CLOCK_UPTIME_RAW` by 5.893 s after 177,906 s of activity, and equals the wall-clock time since `kern.boottime` (177,901.0 s). DEDUCED: `CLOCK_MONOTONIC` is disciplined to wall-clock time (drift of about 33 ppm from the raw oscillator), while `CLOCK_MONOTONIC_RAW` equals `mach_absolute_time`. To be confirmed at the rerun.
- MEASURED clock identifiers 1, 2, 3, 7, 10, 11, 13 to 15, 17 to 19: `EINVAL`. There is no `CLOCK_BOOTTIME`, `CLOCK_REALTIME_COARSE`, `CLOCK_MONOTONIC_COARSE` nor Linux-style dynamic identifiers.
- MEASURED timers at default QoS, overshoot of the requested duration: `nanosleep` 20 us: p50 6.7 us; 100 us: p50 28.0 us; 1000 us: p50 255.8 us (p99 264.5 us).
- ANOMALY to be checked: `mrs cntfrq_el0` returns 1,000,000,000 and `mrs cntvct_el0` returns 41 while `mach_absolute_time` is 4.27e12 (24 MHz). The counter register read at EL0 is therefore not the source of `mach_absolute_time` on this machine. Potential consequence for RDTSC emulation: read `mach_absolute_time` and not `cntvct_el0`. To be measured properly at the rerun.

## K: cooperative guest ptrace transport (to be completed)

To be completed: prototype of shared memory + socket + futex-type primitive, stop and resume round trips in microseconds.

## Implications for ptrace, procfs and signals on Darwin (to be completed)

To be completed after the H, I, J and K measurements. No conclusion is drawn before.

## Open questions

- To be completed.

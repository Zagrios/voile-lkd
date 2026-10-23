# Requests from sig to the task module

Requester: sig. Date: 2026-10-02.

## 1. Job-control stops and continuations for wait4/waitid

sig does the group stop (SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU by default) and the continuation (SIGCONT). At the last parked thread it calls `lkd_sig_child_event(ppid, pid, uid, CLD_STOPPED, signo)` then `lkd_task_wake_waiters(ppid)`; at continuation, `CLD_CONTINUED` (status SIGCONT) in the same way (`src/sig/deliver.c` `park`, `src/sig/queue.c` `prepare_signal`, `src/sig/send.c` `sig_notify_parent`). The parent often lives in another host process: its wait4 must read the state in the shared registry.

Request: a task function `void lkd_registry_jobctl(int32_t pid, int code, int signo)` that records in the process's shared entry "stopped by signo, not reported yet" or "continued, not reported yet", consumed by wait4 with WUNTRACED/WSTOPPED (`(signo << 8) | 0x7f`) and WCONTINUED (`0xffff`), WNOWAIT respected. sig will call it at the two places above as soon as it exists. For the 'T' state of `/proc`, `lkd_registry_publish(t)` can ask `lkd_sig_task_stopped(t)` (declaration requested from core, sig-1.md).

## 2. pidfd

`pidfd_send_signal` (sig) recognizes a pidfd by `o->ops->name == "pidfd"` and reads the pid in the first `int32_t` field of `o->priv`. Request: that the object created by `pidfd_open` (task) follow this form, or state another convention. Without pidfd, the call answers `EBADF` like Linux for a descriptor that is not a pidfd.

## 3. Guest clocks

The absolute futex timeouts on CLOCK_MONOTONIC are converted assuming that the guest monotonic clock is `lkd_host_mono_ns()` (the host's CLOCK_MONOTONIC_RAW); CLOCK_REALTIME is the host's `clock_gettime(CLOCK_REALTIME)` (`src/sig/futex.c` `abs_deadline`). Request: confirm that guest `clock_gettime` uses the same bases, or expose `int64_t lkd_guest_clock_ns(int clockid)` that sig will use.

## 4. Signal state across a host re-execution

Linux keeps the blocked mask, the ignored dispositions and the pending signals at exec. A host re-execution loses the memory. sig provides `lkd_sig_exec_export(t, buf, cap)` and `lkd_sig_exec_import(t, buf, len)` (tested in `test_sig_basic`, case `exec_state_survives_host_reexec`). Request: that task's exec path place this blob in the family's shared memory before `ops->exec_image` and import it into the task recreated by the new image.

## 5. What sig expects from the registry (READ in sig)

- `lkd_proc_lookup(pid).host_pid` and `.state` ('Z': a kill to a zombie answers 0 after the permission check, like Linux), `.uid`, `.euid`, `.sid`;
- `lkd_tid_lookup(tid).tgid` for tkill/tgkill;
- `lkd_proc_list` for kill(-1) and kill(-pgrp) (sig reads at most 512 processes).


## Handled on 2026-10-02 (integration maintainer)

Point 1: handled by `lkd_task_notify_stop`/`lkd_task_notify_continue` (see task-2) instead of a new registry function. Point 2: no pidfd (`pidfd_open` not registered, EBADF kept). Point 3: `lkd_guest_clock_ns(clockid)` (task, declared in `task.h`); `abs_deadline` in `src/sig/futex.c` converts with the guest clock (guest MONOTONIC = Darwin's `CLOCK_UPTIME_RAW`, and not `CLOCK_MONOTONIC_RAW`). MEASURED: on this Mac the two clocks do not differ at the moment (0.000 s), the gap appears only after a sleep. Test: `test_sig_futex` `bitset_and_absolute_timeouts` (deadline taken on the guest clock). Point 4: not applicable as long as exec is in the same host process (the engine's `ExecImage`); to revisit if a host re-execution comes back.

# Request ptrace-2 (task, core): wait4 does not wake up on a ptrace stop

Requester: ptrace. Date: 2026-10-02. Main recipient: task (`src/task/wait.c`), secondarily core (`src/core/block.c`).

## Finding (MEASURED, `tests/test_ptrace_transport.c`)

A blocking `wait4` that waits for a ptrace stop takes 11.1 ms at p50 (p99 11.3 ms), while the same round trip (PTRACE_SYSCALL then next stop) with a `wait4(WNOHANG)` in a loop takes 42 us between two host processes. EAC makes about 150 syscall stops: 1.7 s lost on this mechanism alone.

Cause (READ): `do_wait` (`src/task/wait.c`, loop `for (i < 10 && fam_seq() == seq)`) sleeps with `lkd_block_sleep(t, now + 10 ms)`. `lkd_block_poll` (`src/core/block.c:51-78`) drains the wake pipe on an `lkd_task_wake` but, if the task is not interrupted, goes back to sleep until the deadline (this is the semantics written in `lkd_internal.h`, "re-evalue"). The notifier does its job, though: ptrace calls `lkd_task_wake_waiters(tracer_pid)` (which increments the family counter and writes into the pipe of the `wait4` marked `LT_IN_WAIT`) a few tens of microseconds after the stop, in the tracer's process (family service). The wakeup has no effect. Same thing for the death of a child of another host process, `lkd_task_notify_stop` and the SIGCHLDs: they all go through `lkd_task_wake_waiters`.

## Proposed fix (MEASURED on a copy of `wait.c` in the scratchpad, without touching the tree)

In the loop of `do_wait`, replace the call to `lkd_block_sleep` with a wait on the wake pipe itself:

```c
for (int i = 0; i < 10 && fam_seq() == seq; i++) {
    if (lkd_sig_interrupted(t)) { interrupted = 1; break; }
    struct pollfd pw = { .fd = t->wake_rd, .events = POLLIN };
    if (poll(&pw, 1, 10) > 0) { char junk[64]; while (read(t->wake_rd, junk, sizeof junk) > 0) { } }
}
```

Result with this fix: blocking `wait4`, tracee in another host process, p50 43 us, p90 53 us, p99 73 us (n = 1500). Without it: p50 11,110 us. A cleaner variant is a core function `int lkd_block_wait_wake(lkd_task *t, int64_t deadline_ns)` that returns 0 as soon as an `lkd_task_wake` is consumed (or -LX_ERESTARTSYS if interrupted), to be used by `do_wait`. Measured pitfall: `lkd_block_wait_fd(t, t->wake_rd, POLLIN, ...)` does not work, because the same descriptor is passed twice to `poll` and macOS reports only one entry of it.

## Other point for task: `lkd_ptrace_has_tracees` and the selector

`scan()` returns 0 (wait) as soon as `lkd_ptrace_has_tracees(t)` is true, even if `wait4(pid)` targets a pid that is neither a child nor a tracee. Linux returns ECHILD in this case (`do_wait` finds no child that satisfies the selector). So that ptrace can answer exactly, task can call instead `int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options)` (defined in `src/ptrace/life.c`, same selector as `lkd_ptrace_wait_event`, to declare in `ptrace.h`). `lkd_ptrace_wait_event` must also always return `CLD_TRAPPED` for a ptrace stop, including a group stop (`why = ptrace ? CLD_TRAPPED : CLD_STOPPED` in `wait_task_stopped`): this is done.

Also `waitid`: for a ptrace stop the `si_status` of `waitid` must be the full stop code (`exit_code`, for example 0x405 for PTRACE_EVENT_EXEC, 0x80 | 5 for a syscall stop), not `(status >> 8) & 0xff` (`src/task/wait.c`, `scan`, ptrace branch). It is `ev.status >> 8` in full (kernel/exit.c v6.12 line 1358, `wait_task_stopped`: `infop->status = exit_code`).


## Handled on 2026-10-02 (integration maintainer)

Point 1 (wakeup): new core wait `lkd_block_wait_wake` (`src/core/block.c`), which returns as soon as a wake byte is consumed; `do_wait` (`src/task/wait.c`) uses it instead of `lkd_block_sleep`. `lkd_task_wake_waiters` (`src/task/tasks.c`) also reaches the waits of ANOTHER host process through famsvc (task opcode 96), for the death, stop and continuation of a child. MEASURED: `test_ptrace_life` case `blocking_wait4_wakes_at_once` (20 stops under 100 ms; before: hang, the test does not finish in 100 s); `test_task_proc` case `wait4_wakes_at_once_across_host_processes` (10 rounds under 65 ms; before: about 100 ms); `test_ptrace_transport` goes from 18 s to 1 s.
Point 2 (selector): `scan()` calls `lkd_ptrace_wait_has` (declared in `ptrace.h`, weak in core). ECHILD for a pid that is neither a child nor a tracee, and for `__WNOTHREAD` from a thread without a tracee. So that `wait4(new pid)` right after PTRACE_EVENT_FORK does not return ECHILD, `lkd_ptrace_event` waits (bounded to 2 s) for the forked child to have created its link. Tests: `test_ptrace_life` `wait_selectors_see_tracees`, `wait_semantics`.
Point 3 (si_status): full stop code (`status >> 8`). Test: `test_ptrace_life` `group_stop_of_a_traced_task` (0x8013).

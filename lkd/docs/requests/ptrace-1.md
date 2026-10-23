# Request ptrace-1 (sig, core, task, engine)

Requester: ptrace. Date: 2026-10-02. Points 1 to 3 are behavior fixes; the module works without them but with the deviations noted in `docs/status/ptrace.md`.

## sig: `PTRACE_INTERRUPT` and `PTRACE_EVENT_STOP`

`lkd_sig_interrupted` already reads `LKD_TF_TRACE_STOP`, but `lkd_signal_next` does not honor it: the flag wakes blocking waits without consuming anything. At the top of the loop of `lkd_signal_next` (before `LKD_TF_GROUP_STOP`), add:

```c
if (atomic_load(&t->flags) & LKD_TF_TRACE_STOP) {
    atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP);
    if (lkd_ptrace_trap_stop(t) < 0) return die(t);   /* declared in ptrace.h */
    continue;
}
```

and define `int lkd_sig_trace_stop_supported(void) { return 1; }` (the ptrace module has a weak version of it that returns 0). As long as this is not done, `PTRACE_INTERRUPT` stops at the next syscall, signal or event boundary, never for a tracee that is blocked or in translated code.

## core: syscall exit stop for a replaced context

`lkd_syscall` does not call `lkd_ptrace_syscall_exit` when `disp == LKD_CONTEXT_REPLACED` (execve, rt_sigreturn). Linux does the exit stop in both cases (`syscall_exit_work`): the first stop after an `execve` with PTRACE_SYSCALL is the execve exit (`orig_rax` 59, `rax` 0, RIP = entry of the new image), expected by EAC. Current workaround: `lkd_ptrace_event(EXEC)` does this stop itself. Request: for `LKD_CONTEXT_REPLACED`, call `lkd_ptrace_syscall_exit(t, &zero)` when the flag was set; ptrace then knows that the context is replaced (rcx and r11 not touched) through `t->cur_nr`. For rt_sigreturn, same call (RAX stays the one of the restored frame).

## engine: `orig_rax`

`LkdHandleSyscall` sets `T->OrigRax = nr` and never resets it to -1. Linux sets it to -1 on entry to an exception or an interrupt. Request: `OrigRax = -1` when the engine leaves translated code for a fault, an int3 or a kick; ptrace has no other way to know it (it sets it to -1 for a fault signal based on `si_code`, which covers int3, SEGV, FPE).

## engine: single step

`set_single_step` is NULL: `PTRACE_SINGLESTEP` returns EIO (logged). When the engine can execute one instruction, it sets `set_single_step`, and reports the end of the step with `lkd_fault(t, SIGTRAP, TRAP_TRACE, rip, 0, 1)`. ptrace calls `set_single_step(t, 0)` on the tracer's task as an availability probe (idempotent).

## mm

Nothing. `lkd_mm_remote_read/write` (PEEK/POKE) invalidate the target's translated code before answering, as planned.

## core: declarations to add to `src/include/ptrace.h`

Defined and strong in `src/ptrace` but missing from the common kernel's header (the tests declare them by hand):

```c
int lkd_ptrace_trap_stop(lkd_task *t);                                   /* sig, PTRACE_INTERRUPT (see above) */
int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options);   /* task, wait4 (ptrace-2) */
int lkd_sig_trace_stop_supported(void);   /* weak in ptrace (returns 0), strong in sig (returns 1) */
```

## engine: environment variables of a host re-execution

If the engine re-executes the host process for a guest `execve`, it must keep `LKD_PTRACE_FD` in the environment and the descriptor it names open (without FD_CLOEXEC, like `LKD_FAMSVC_FD` and `LKD_FAMILY_FD`): it is the table of tracer/tracee links of the whole family. Without it, a tracee would lose its tracer at exec (the first stop after the exec is exactly what EAC expects). `lkd_ptrace_init` reattaches the table at the start of the re-executed image; if the descriptor is absent it creates a new one and logs (the existing links are then lost).


## Handled on 2026-10-02 (integration maintainer)

sig: `lkd_signal_next` honors `LKD_TF_TRACE_STOP` (call of `lkd_ptrace_trap_stop`) and defines `lkd_sig_trace_stop_supported() = 1`. Test: `test_ptrace_requests` case `interrupt_reaches_a_blocked_tracee` (PTRACE_INTERRUPT of a tracee in nanosleep: stop 0x80057f in less than 300 ms; before: no stop).
core: `lkd_syscall` calls `lkd_ptrace_syscall_exit_replaced` (new, `ptrace.h`) for `LKD_CONTEXT_REPLACED`, rereading `LKD_PT_SYSCALL_TRACE` at exit (PTRACE_SYSCALL given at the exec stop). The workaround in `lkd_ptrace_event(EXEC)` is removed. rt_sigreturn now has an exit stop (rax restored, rcx not modified). Test: `test_ptrace_requests` case `sigreturn_has_an_exit_stop`.
Declarations added to `src/include/ptrace.h`: `lkd_ptrace_trap_stop`, `lkd_ptrace_wait_has`, `lkd_sig_trace_stop_supported` (local redeclarations removed).
Left to the engine: `orig_rax = -1` on entry to a fault or a kick (the lead did it in `fexdarwin_kick_safepoint`), single step, `LKD_PTRACE_FD` at host re-execution.

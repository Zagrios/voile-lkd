# Request task-2 to the sig, ptrace, procfs and fs modules

Date: 2026-10-02. Author: task module.

## sig

1. A guest process created by `vfork` or `posix_spawn` (clone with `CLONE_VM` without `CLONE_THREAD`) lives as a host thread in its parent's host process until its `execve` (`src/task/clone.c`, header). If it ends before, `lkd_hooks_proc_exit` is called for it in the parent's host process. `lkd_sig_proc_exit` must then not call `lkd_famsvc_shutdown`: test `lkd_task_shares_host(t)` (`src/task/tasks.c`, request task-1 point 2). Likewise `lkd_task_release` returns 0 for it: the host process continues.
2. Job-control stops: call `lkd_task_notify_stop(t, signo)` on entering a group stop and `lkd_task_notify_continue(t)` on continuation by SIGCONT. task publishes the state and the single report for `wait4`/`waitid`. The SIGCHLD with `CLD_STOPPED`/`CLD_CONTINUED` stays with sig.
3. task calls `lkd_sig_child_event(ppid, pid, uid, code, status)` only when the child's exit signal is SIGCHLD. For another exit signal (clone with a chosen signal), task sends `lkd_sig_send_pid(NULL, ppid, sig, &info)` with a `CLD_*` siginfo (`src/task/wait.c`, `notify_parent`). The return value 1 of `lkd_sig_child_event` (parent that ignores SIGCHLD or `SA_NOCLDWAIT`) frees the zombie at once.
4. task's `ITIMER_*` timers send `lkd_sig_send_task(target, SIGALRM/SIGVTALRM/SIGPROF, &si, 0)` with `si_code = SI_KERNEL` from an auxiliary host thread without a current task (`src/task/time.c`, `itimer_fire`).

## ptrace

5. After a host fork, task calls `lkd_ptrace_task_init(t, t, flags)` in the child, `t` already having the child's identity and `t->ptrace` containing the copy of the parent's link (see request task-1 point 3).
6. `lkd_exec_finish` calls `lkd_ptrace_event(t, PTRACE_EVENT_EXEC, old_tid)`; if it returns 0 and the task is traced without `SEIZE`, task sends SIGTRAP (`SI_USER`) like `exec_binprm`.
7. `PR_SET_PTRACER` (Yama) answers EINVAL in task (kernel modeled without Yama). If ptrace models Yama, say which behavior task must return.

## procfs

8. `uname`: `release` is `6.12.0-lkd`, `version` `#1 SMP PREEMPT_DYNAMIC LKD`, `nodename` the family's name (`obelisk` by default). Use `lkd_task_uname` for `/proc/sys/kernel/osrelease`, `/proc/sys/kernel/hostname` and `/proc/version`.
9. Kernel modeled without `CONFIG_SECCOMP`: `prctl(PR_GET_SECCOMP)` and `PR_SET_SECCOMP` answer EINVAL, `seccomp(2)` ENOSYS. `/proc/<pid>/status` must therefore have no `Seccomp:` or `Seccomp_filters:` line.
10. `lkd_proc_lookup` refreshes the record of a local process before answering. For a process of another host process, `state` is what was published last: `S` during `wait4`, `T` when stopped, `Z` after death, otherwise `R`. `cmdline` is truncated at 4096 bytes in the registry (the full content is in `proc->cmdline` for a local process).

## fs

11. `lkd_exec_prepare` resolves with `lkd_vfs_stat`, `lkd_vfs_realpath`, `lkd_vfs_to_host` then host `open`; for a virtual mount, `lkd_vfs_open` then the object's `host_fd`. For `execveat(fd, "", AT_EMPTY_PATH)`, task reads `ops->fstat` and the object's `host_fd`. Nothing to change if these functions keep their contract (MEASURED: `tests/test_task_exec.c` passes with the current fs module).


## Handled on 2026-10-02 (integration maintainer)

Point 1: `lkd_sig_proc_exit` stops famsvc only when the host process's guest process counter drops to 0 (`src/sig/state.c`), which already covers the co-hosted vfork child. Point 2: sig calls `lkd_task_notify_stop` at the end of the group stop (`park`) and `lkd_task_notify_continue` at the CLD_CONTINUED report (`src/sig/deliver.c`). Test: `test_task_proc` case `sigstop_and_sigcont_reach_wait4` (real SIGSTOP, `wait4(WUNTRACED)` = 0x137f once, SIGCONT, `waitid(WCONTINUED)`). Before: `wait4` returned nothing and the child was killed by its alarm. Points 3 to 11: information, compliant with the code read (nothing to change). Point 7: kernel without Yama, EINVAL kept.

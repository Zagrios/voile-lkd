# Request task-1 to the common kernel (core)

Date: 2026-10-02. Author: task module.

1. `lkd_config_get()` is declared in `src/core/core_priv.h`, reserved for core. The task module needs it (uid, gid, first_pid, hostname). It declares it locally in `src/task/task_priv.h:22`. Request: declare it in `lkd_internal.h`.

2. Declare in `src/include/task.h` the functions that task already provides (definitions in `src/task/tasks.c` and `src/task/sys.c`):
   - `void lkd_task_notify_stop(lkd_task *t, int signo);` and `void lkd_task_notify_continue(lkd_task *t);`: the sig module calls them when a process enters a job-control stop or leaves it. They publish the `T` state and the event that `wait4(WUNTRACED)` and `waitid(WCONTINUED)` report once (MEASURED: `tests/test_task_proc.c` case `stop_and_continue_reports`).
   - `int lkd_task_uname(struct lx_utsname *out);`: the same `release` string for `uname(2)` and for procfs (`/proc/sys/kernel/osrelease`, `/proc/version`).
   - `int lkd_task_shares_host(const lkd_task *t);`: true for a guest process that lives in the host process of another one (child of vfork or of posix_spawn before its exec). The `proc_exit` hooks must then stop no service that is global to the host process.

3. Order of the fork hooks in the child (`src/core/hooks.c`, `lkd_fork_finish`): the reverse order gives net, ptrace, task, sig, procfs, fs, mm. ptrace and net therefore still see the parent's identity in the child. Request: call `lkd_task_fork_child` first in the child, then the others in the current reverse order. Meanwhile, task calls `lkd_ptrace_task_init(t, t, flags)` after the identity switch (`src/task/clone.c`, `clone_fork`), with the copy of the parent's ptrace link still in `t->ptrace`.

4. `src/core/block.c`: set `LKD_TF_IN_BLOCK` during `lkd_block_poll` and clear it afterwards. The registry then shows `S` for a task blocked in a `read` or a `poll` (EAC reads `/proc/<ppid>/status`). task already does it for wait4, nanosleep and clock_nanosleep.

5. `tests/test_core_dispatch.c` defines non-static handlers (`lkd_sys_getpid`, `lkd_sys_read`, `lkd_sys_write`, `lkd_sys_open`, `lkd_sys_ioctl`...). MEASURED: linking fails in the common build because of a duplicate symbol with the fs module (`./build.sh -k 0`: `FAILED: tests/test_core_dispatch`). task made its handlers `static`. Request: `static` in the test, or an `LKD_SYSCALL_STATIC` macro.


## Handled on 2026-10-02 (integration maintainer)

Point 1: done (lead, 16:20). Point 2: `lkd_task_notify_stop`, `lkd_task_notify_continue`, `lkd_task_uname`, `lkd_task_shares_host` declared in `task.h`, weak replacements in `stubs_task.c`, local redeclarations removed. Point 3: not changed. ptrace's fork hook reads the parent's link with the parent's identity (`lkd_ptrace_fork_child`); putting task first would break self-attachment. The defect this caused (parent's tid taken for an old tid at exec, EAC probe cluster) is fixed in `lkd_ptrace_exec_commit`. Point 4: `lkd_block_poll` and `lkd_block_wait_wake` set `LKD_TF_IN_BLOCK` (nesting kept). Test: `test_task_proc` case `blocked_thread_shows_sleeping`. Point 5: `LKD_SYSCALL` now declares the handler `static` (all handlers of the tree and of the tests are, 269 definitions); `test_core_dispatch` links with all modules and keeps its numbers, because it resets the table (`lkd_dispatch_reset`).

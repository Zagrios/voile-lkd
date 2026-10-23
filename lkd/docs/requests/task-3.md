# Request task-3 to the project lead (fexdarwin engine)

Date: 2026-10-02. Author: task module. READ: `src/fexdarwin/src/LkdGlue.cpp`, `src/fexdarwin/src/main.cpp:168-237`.

1. `ops->exec_image` answers `-ENOSYS` (`LkdGlue.cpp`, `ExecImage`). task calls it after the point of no return (modules reset by `lkd_exec_commit`), like Linux: a failure at this point kills the process with SIGSEGV (`src/task/exec.c`, `do_execve`). As long as the operation is missing, any `execve` that succeeded up to there therefore kills the process. Expected contract:
   - input: `img->host_fd` (read-only, CLOEXEC, closed by LKD after the call), `img->guest_path` (target of `/proc/self/exe`), `img->argv`, `img->envp`, `img->original_path` (for `AT_EXECFN`). The file is an x86-64 ELF64 `ET_EXEC` or `ET_DYN` already checked; `PT_INTERP` is left for the engine to load.
   - load the segments with `lkd_mm_map` into the already emptied mm, build stack, auxv and `AT_RANDOM`, call `lkd_proc_set_layout(t, &layout)` (used by `PR_GET_AUXV` and procfs), set the registers with `ops->set_regs` on the model of `start_thread` (RIP = entry point, RSP, all others at 0, `fs_base` 0), invalidate the translated code of the whole space, return 0.
   - an exec by re-executing the host must call `lkd_family_export()` before, keep `LKD_FAMILY_FD` (and `LKD_FAMSVC_DIR`, `LKD_FAMSVC_FD`), then in the new image call `lkd_task_create_main` with `spec.pid` = the guest pid: task then takes over the existing record (same host pid, ppid, pgid, sid, limits).

2. End of the guest main thread alone (`exit` and not `exit_group`, other threads alive): `lkd_task_release` returns 0, the engine does `pthread_exit`, then `main.cpp:233-236` sees `pthread_join` return and ends the process with 70. Linux lets the group continue. Request: do not leave `RunWithLkd` at that moment (wait indefinitely; the end comes from `lkd_task_release` == 1 in another thread).

3. After a fork, the child keeps the same `GuestThread`: its `Tid` field is still the parent's tid. task has already switched the identity when `ops->fork_child` is called: do `GT(t)->Tid = lkd_task_tid(t)` there.

4. `exit_group`: `lkd_task_release` returns 1 for the thread that calls it (and for any thread that exits afterwards); the engine can call `_exit` without waiting for the other threads. The returned code is the exit code, or 128 + signal number for a death by signal (the exact status is in the registry for the parent).

5. `de_thread` (exec from a process with several threads) waits for each other thread to go through `lkd_signal_next` == -1 then `lkd_task_release`. A thread that loops in translated code without a syscall is not reached as long as the kick does not go through FEX's interrupt page (`LkdGlue.cpp`, comment of `KickHandler`). The exec then waits (log every 2 s).

6. Processes sharing memory (vfork, posix_spawn): `spawn_thread` is called for a task of ANOTHER guest process (different pid) that lives in the same host process. The engine must not assume `lkd_task_pid(child) == lkd_task_pid(parent)`. At its `execve`, task does a host fork: in the old host process the syscall returns `LKD_EXIT_THREAD` and `lkd_task_release` returns 0; in the new one, the exec continues normally.

7. Safety limits applied by task: 300 threads per process, 64 live children per process, 512 live processes per family (`src/task/task_priv.h`, `TASK_MAX_*`); beyond that `-EAGAIN`. To raise only with proof of need.

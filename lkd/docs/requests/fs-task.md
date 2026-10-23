# fs -> task and net: cooperation points

## task

1. References: each thread holds a reference on `t->fdt` and `t->fsctx`. The hook `lkd_fs_task_exit` gives them back (and sets the fields to NULL); `lkd_fs_proc_exit` does the same if they are still there. The task module must not give them back a second time after these hooks.
2. exec: `lkd_fs_exec_commit` closes the CLOEXEC descriptors; task does not have to call `lkd_fd_close_on_exec`.
3. `RLIMIT_NOFILE`: fs reads `proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur` under `proc->lock` at each allocation. `setrlimit`/`prlimit64` must refuse `rlim_max > 1048576` with `-EPERM` (Linux's fs.nr_open, the maximum size of the fs table).
4. Exec by re-executing the host: the guest descriptor table (numbers, CLOEXEC, non-host objects) does not survive on its own. fs can provide an export/import of the table (guest number -> inherited host fd, flags, guest path); to request when the exec path is settled.

## net

1. `F_SETFL` and `FIONBIO`/`FIOASYNC` on a net object: fs updates `o->status_flags` (O_APPEND, O_NONBLOCK, O_DIRECT, O_NOATIME) then calls `o->ops->fcntl(t, o, LX_F_SETFL, new_flags)`; a `-EINVAL` answer is ignored, any other negative error is returned. O_ASYNC is refused (`-ENOSYS`) as long as SIGIO does not exist.
2. `readv`/`writev` on a socket call `ops->read`/`ops->write` segment by segment (no coalescing): net may prefer an internal `sendmsg` path, to discuss.
3. fs objects usable by epoll: `fs_pipe_ops.poll` and `fs_stream_ops.poll` exist, the host fds carry the readiness (`o->host_fd`).


## Handled on 2026-10-02 (integration maintainer)

Points 1 to 3: compliant (task does not give back fdt and fsctx twice; `setrlimit(NOFILE)` above 1048576 returns EPERM, `src/task/sys.c:76`). Point 4: not applicable as long as exec stays in the host process. net: compliant.

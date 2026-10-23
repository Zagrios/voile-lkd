# Request procfs-4 to the task, fs, net, ptrace modules

Date: 2026-10-02. Author: procfs module.

## fs
1. `fs_host_io`, `fs_stat_from_host` and `fs_get_root` (defined non-static in `src/fs`) are used by procfs (object on a descriptor received from another process, link `/proc/<pid>/root`), declared in `src/procfs/procfs_priv.h`. Request: declare them in `src/fs/lkd_fs.h` under the names `lkd_fs_host_io`, `lkd_fs_stat_from_host`, `lkd_fs_get_root`, or guarantee that they keep their name and signature.
2. `lkd_vfs_getcwd` returns the getcwd text (with `(unreachable)` outside the root): used as is for `/proc/<pid>/cwd`. Nothing else.

## net
3. inotify limits. procfs publishes `/proc/sys/fs/inotify/{max_user_instances 128, max_user_watches 65536, max_queued_events 16384}` only if `inotify_init1` has an owner. If net applies other limits, say which ones (or export a function `lkd_net_inotify_limits`).

## ptrace
4. `lkd_ptrace_check_access` returns `-EPERM`; procfs translates any refusal to `-EACCES` for maps, environ, auxv, mem (`mm_access`, `kernel/fork.c`) and for exe, cwd, root, fd/N (`proc_fd_access_allowed`). No Yama: `/proc/sys/kernel/yama` is absent.
5. `TracerPid`: `lkd_ptrace_tracer_of(tid)` (measured in `test_procfs_other`: PTRACE_TRACEME from the child, the parent reads the tracer's tid).

## task
6. For `stat`: utime and stime of a thread other than the caller are not available on the host side (no Mach port of the other guest threads). procfs returns 0 for the per-thread view of another thread. If task keeps the thread's Mach port (`mach_thread_self` taken at spawn) in the task structure, procfs could read it (`thread_info THREAD_BASIC_INFO`).
7. `processes` in `/proc/stat` (creations since boot): task does not expose the family's pid counter; the line is omitted. A function `uint32_t lkd_pid_peek(void)` would be enough.
8. `processor` (field 39 of stat): value of `pthread_cpu_number_np` for the calling thread, 0 for another one.


## Handled on 2026-10-02 (integration maintainer)

Point 1: `fs_get_root`, `fs_host_io` and `fs_stat_from_host` are declared in `lkd_fs.h` under their current names, which stay stable, and removed from `procfs_priv.h`. Point 3: `lkd_net_inotify_limits` (`net.h`); procfs publishes net's real limits (128, 8192, 16384; before, 65536 watches were advertised for 8192 applied). Test: `test_procfs_global` `proc_sys_values`. Point 7: `lkd_pid_peek` (`task.h`); `/proc/stat` prints `processes` (identifiers handed out by the family). Test: `test_procfs_global`. Points 2, 4, 5, 8: information, nothing to change. Point 6 (Mach port of the other threads): not done, gap kept.

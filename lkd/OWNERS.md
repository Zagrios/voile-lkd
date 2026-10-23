# Ownership of the LKD modules

A syscall has only one owner: the module that registers it in `lkd_<module>_init()`. `lkd_init()` fails with `-EEXIST` if two modules register the same number, and `tools/owners_check.py` (run by `check.sh`) refuses a name present in two of the lists below. A module that wants a syscall of another one writes a request in `docs/requests/<module>-<n>.md`.

Each module writes only in `src/<module>/`, `tests/test_<module>_*.c`, `docs/status/<module>.md` and `docs/requests/<module>-*.md`. The common kernel (`core`) owns `include/`, `src/core/`, `src/include/`, `tests/support/`, `tests/ltest.h`, `tests/test_core_*.c`, `CMakeLists.txt`, the scripts and `docs/CONVENTIONS.md`.

## Syscalls per module

<!-- owners:begin -->
core: restart_syscall
mm: brk mmap munmap mprotect mremap madvise mincore msync mlock mlock2 munlock mlockall munlockall shmget shmat shmdt shmctl process_vm_readv process_vm_writev process_madvise membarrier pkey_mprotect pkey_alloc pkey_free mbind set_mempolicy get_mempolicy migrate_pages move_pages set_mempolicy_home_node map_shadow_stack mseal remap_file_pages
fs: open openat openat2 creat close close_range read write readv writev pread64 pwrite64 preadv pwritev preadv2 pwritev2 lseek stat fstat lstat newfstatat statx statfs fstatfs ustat access faccessat faccessat2 readlink readlinkat getcwd chdir fchdir chroot rename renameat renameat2 mkdir mkdirat rmdir unlink unlinkat link linkat symlink symlinkat chmod fchmod fchmodat fchmodat2 chown fchown lchown fchownat umask truncate ftruncate fallocate fsync fdatasync sync syncfs getdents getdents64 dup dup2 dup3 fcntl flock ioctl pipe pipe2 utime utimes utimensat futimesat sendfile copy_file_range splice tee vmsplice memfd_create mknod mknodat setxattr lsetxattr fsetxattr getxattr lgetxattr fgetxattr listxattr llistxattr flistxattr removexattr lremovexattr fremovexattr fadvise64 readahead sync_file_range name_to_handle_at open_by_handle_at
task: getpid gettid getppid getpgid getpgrp setpgid getsid setsid getuid geteuid getgid getegid getresuid getresgid setuid setgid setresuid setresgid setreuid setregid setfsuid setfsgid getgroups setgroups capget capset prctl arch_prctl personality uname sethostname setdomainname getrlimit setrlimit prlimit64 getrusage times sysinfo sched_getaffinity sched_setaffinity sched_yield sched_getscheduler sched_setscheduler sched_getparam sched_setparam sched_get_priority_max sched_get_priority_min sched_rr_get_interval sched_setattr sched_getattr getpriority setpriority getcpu getrandom clock_gettime clock_getres clock_nanosleep nanosleep gettimeofday time settimeofday clock_settime adjtimex clock_adjtime getitimer setitimer alarm timer_create timer_settime timer_gettime timer_getoverrun timer_delete clone clone3 fork vfork execve execveat wait4 waitid exit exit_group set_tid_address set_robust_list get_robust_list rseq seccomp ioprio_get ioprio_set unshare setns pidfd_open modify_ldt iopl ioperm
sig: rt_sigaction rt_sigprocmask rt_sigreturn rt_sigpending rt_sigtimedwait rt_sigqueueinfo rt_sigsuspend rt_tgsigqueueinfo sigaltstack kill tkill tgkill pause signalfd signalfd4 futex futex_waitv futex_wake futex_wait futex_requeue pidfd_send_signal
net: socket socketpair bind listen accept accept4 connect getsockname getpeername sendto recvfrom sendmsg recvmsg sendmmsg recvmmsg shutdown setsockopt getsockopt poll ppoll select pselect6 epoll_create epoll_create1 epoll_ctl epoll_wait epoll_pwait epoll_pwait2 epoll_ctl_old epoll_wait_old eventfd eventfd2 timerfd_create timerfd_settime timerfd_gettime inotify_init inotify_init1 inotify_add_watch inotify_rm_watch
ptrace: ptrace
<!-- owners:end -->

The 84 unassigned syscalls (`python3 tools/owners_check.py --list-unowned`) answer `-ENOSYS` in a visible and counted way (`LKD_LOG_ENOSYS=1` names them at the first occurrence). They are mostly privileged kernel calls (`mount`, `umount2`, `pivot_root`, `swapon`, `reboot`, `init_module`, `bpf`, `perf_event_open`, `fanotify_init`...) for which Linux answers `-EPERM` to a user without the capability, System V message queue and semaphore IPC (`msg*`, `sem*`), `io_uring_*` and `io_*` (AIO), `mq_*`, `userfaultfd`, `landlock_*`. The project lead decides, with evidence in hand (ENOSYS counters from a real launch), which ones get an owner; an honest `-EPERM` for a non-root user is the planned answer for privileged calls.

`procfs` has no syscall: it serves `/proc`, `/dev` and `/sys` through the virtual mount interface of `vfs.h`.

## Shared cases (who implements what)

| Topic | Syscall | Cooperation function | Owner of the function |
| --- | --- | --- | --- |
| clone | task | `lkd_mm_clone`, `lkd_fdtable_clone`, `lkd_fsctx_clone`, `lkd_sighand_clone`, `lkd_ptrace_task_init` (sequence in `task.h`) | mm, fs, fs, sig, ptrace |
| host fork | task (clone without CLONE_VM) | `lkd_fork_host` then hooks `lkd_<m>_fork_*` | core orchestrates, each module its own locks |
| wait4 and waitid | task | `lkd_ptrace_wait_event`, `lkd_ptrace_has_tracees` | ptrace |
| wait4 wakeup | (none) | `lkd_task_wake_waiters(pid)` | task |
| SIGCHLD | (none) | `lkd_sig_child_event` | sig |
| fatal signal | (none) | `lkd_task_do_exit_group` | task |
| futex and clear_child_tid | futex (sig), exit (task) | `lkd_futex_wake_addr`, `lkd_futex_exit_robust_list` | sig |
| shared futex key | futex (sig) | `lkd_mm_futex_key` | mm |
| set_robust_list | task (stores it in the task) | walk at exit: sig | sig |
| signalfd | signalfd (sig) | fd object through `fd.h`, poll side through `lkd_net_make_notify_pipe` | sig, net |
| epoll on objects | epoll (net) | `lkd_fd_ops.poll`, `notify_fd` | fs and sig provide the object |
| mmap of a file | mmap (mm) | `lkd_fd_ops.mmap_fd`, `lkd_fdobj_reopen` | fs |
| memfd seals | fcntl (fs) | `lkd_mm_has_writable_shared_mapping` | mm |
| /proc/pid/maps | (procfs) | `lkd_mm_render_maps`, `lkd_mm_remote_render_maps` | mm |
| /proc/pid/mem, PEEK, POKE | ptrace, procfs | `lkd_mm_remote_read/write` (invalidates the code) | mm |
| ptrace access control | procfs, mm | `lkd_ptrace_check_access` | ptrace |
| execve | task | `lkd_exec_prepare` (task), `lkd_exec_commit` (core: hooks), `ops->exec_image` (engine), `lkd_exec_finish` (task then ptrace) | task |
| procfs of another process | (procfs) | `lkd_proc_lookup`, `lkd_tid_lookup`, `lkd_proc_list` | task |

## Common kernel files that the modules read without modifying them

`include/lkd/lkd.h`, `include/lkd/abi_x64.h`, `include/lkd/xlate.h`, `include/lkd/syscall_nr.h` (generated), `src/include/*.h`, `tests/ltest.h`, `tests/support/fake_engine.{h,c}`.

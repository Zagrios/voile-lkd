# Status of the fs module (LKD)

Date: 2026-10-02. Milestones 1 and 2 reached: vfs, descriptor table, file objects, pipe, memfd and seals, 100 syscalls registered, 6 test programs, Debian tree tested.

## What exists

Files (`src/fs/`, 5500 lines): `vfs.c` (mounts, fsctx, resolution), `fdtable.c`, `file.c` (files, directories, O_PATH, host streams, reopening, mmap checks), `dir.c` (getdents), `pipe.c`, `memfd.c`, `tty.c`, `sys_open.c`, `sys_rw.c`, `sys_stat.c`, `sys_path.c`, `sys_misc.c`, `xattr.c`, `fs_init.c`, `fs_priv.h`, `lkd_fs.h` (services for the engine, mm, procfs), `NOTICE`.

HyperLinux imports: the resolution loop of `vfs.c` (`hl_vfs_resolve_at`) and the termios translation of `syscall_io.c`, marked "imported from HL" and listed in `src/fs/NOTICE`. The rest is written for LKD.

Registered syscalls (100, all those of `OWNERS.md` except `tee`): open openat openat2 creat close close_range read write readv writev pread64 pwrite64 preadv pwritev preadv2 pwritev2 lseek stat fstat lstat newfstatat statx statfs fstatfs ustat access faccessat faccessat2 readlink readlinkat getcwd chdir fchdir chroot rename renameat renameat2 mkdir mkdirat rmdir unlink unlinkat link linkat symlink symlinkat chmod fchmod fchmodat fchmodat2 chown fchown lchown fchownat umask truncate ftruncate fallocate fsync fdatasync sync syncfs getdents getdents64 dup dup2 dup3 fcntl flock ioctl pipe pipe2 utime utimes utimensat futimesat sendfile copy_file_range splice vmsplice memfd_create mknod mknodat the 12 xattr fadvise64 readahead sync_file_range name_to_handle_at open_by_handle_at.

## Tests (MEASURED)

Complete build of the tree (`cmake --build`, all modules): 24 programs out of 24 pass. fs programs:

| Program | Assertions | Cases |
| --- | --- | --- |
| `test_fs_vfs` | 135 | 11 |
| `test_fs_fd` | 101 | 9 |
| `test_fs_io` | 265 | 10 |
| `test_fs_pipe` | 71 | 7 |
| `test_fs_memfd` | 111 | 10 |
| `test_fs_glibc` | 64 | 6 |
| total | 747 | 53 |

The same tests also pass with a private core + fs build (weak stand-ins for sig, mm, task), which checks that fs does not depend on the behavior of a particular module.

Coverage: temporary guest root with relative and absolute links, ".." after a link, loops (40 links accepted, 41 refused), escape impossible, bind mounts, synthetic directories, virtual mount with magic links and independent reopening, chroot, openat2 RESOLVE_*; offset sharing through dup and through fork, RLIMIT_NOFILE limits and fds up to 65535, close_range, CLOEXEC and exec, POSIX locks seen from a child with the right pid, flock; byte-by-byte layout of `struct stat` (144 bytes) and of `statx` (256 bytes); getdents and getdents64 (record size, d_off, d_type, resume after lseek, buffer too small); pipes (capacity 65536, EOF, EPIPE and SIGPIPE, wait and interruption, splice); memfd and seals; glibc 2.41 ld.so sequence on `rocket-hyperlinux-20260928/linux-root/rootfs` mounted read-only (`/etc/ld.so.cache` missing: ENOENT, `/lib/x86_64-linux-gnu/libc.so.6` opened, 832 bytes read, newfstatat AT_EMPTY_PATH size 1995216, pread64 of the program headers, readlink of `/lib64/ld-linux-x86-64.so.2`, getcwd, 730 directory entries). The Debian tree was not modified (MEASURED: `find -newer` empty).

## Measured host facts (MEASURED, macOS 27, this Mac)

- Capacity of a host pipe: 65536 bytes (non-blocking fill), as on Linux.
- APFS of the guest root and of `/tmp`: case-insensitive (`pathconf _PC_CASE_SENSITIVE` = 0), case-preserving.
- Deleted file: `F_GETPATH` still returns the old path, `st_nlink` is 0; repeated `ftruncate` and `mmap MAP_SHARED` work.
- `kern.maxfilesperproc` = 138240; `setrlimit(RLIMIT_NOFILE, 138240)` accepted.
- `F_SETNOSIGPIPE` works on a pipe; a write without a reader returns EPIPE without killing the process.
- `flock` and POSIX `fcntl` locks on the same file conflict under macOS (Linux keeps them independent).

## Known differences (honest, documented in the code)

1. Case: a guest path with a different case finds the file (Linux: ENOENT); two names that differ only by case cannot coexist. Nothing is simulated; `test_fs_vfs` records this behavior.
2. Host paths limited to 1024 bytes (macOS PATH_MAX): a longer valid guest path returns ENAMETOOLONG.
3. Race between the walk and the host call: a link placed between the two makes the call fail (O_NOFOLLOW), without escape.
4. Directory opened then renamed: the relative *at calls use the guest path remembered at open time.
5. Pipes created by LKD: the host fd is always non-blocking, the guest O_NONBLOCK is in the object; an F_SETFL O_NONBLOCK is therefore not seen by a forked process that shares the pipe. PIPE_BUF atomicity: macOS only guarantees 512 bytes for a non-blocking write.
6. Shared host streams (tty, launcher pipe): interruptible wait then read; another reader can take the data between the two and the host read then blocks without being interruptible.
7. FIFO: a blocking reader does not wait for a writer to arrive at open time.
8. flock and POSIX locks on the same file interfere (see measurement). Blocking locks (F_SETLKW, flock): repeated attempts every 5 ms, interruptible, without EDEADLK deadlock detection.
9. pipe2 O_DIRECT (packet mode): EINVAL. F_SETPIPE_SZ beyond 65536: EPERM (fs.pipe-max-size is 65536 here). splice, sendfile and copy_file_range copy through a kernel buffer (no zero copy); tee is not registered (ENOSYS counted).
10. SIGIO: O_ASYNC refused with ENOSYS on pipes, streams and objects of the other modules (accepted on regular files, where Linux does nothing either). F_SETOWN, F_SETSIG remembered; F_SETLEASE (other than F_UNLCK) and F_NOTIFY return EINVAL like a kernel where leases and dnotify are disabled.
11. memfd: seals shared between forked processes through a MAP_SHARED area, lost on a host re-exec. Writes through a writable shared mapping of ANOTHER process of the family are not seen by the EBUSY query (it only knows the caller's address space). MFD_HUGETLB: EINVAL. user.* extended attributes of a memfd: not kept (EOPNOTSUPP).
12. Reopening a deleted file through /proc: duplication with a private offset; the access mode cannot exceed that of the original description (EACCES). Pipe: only the same end can be reopened (ENXIO otherwise).
13. O_TMPFILE: EOPNOTSUPP (glibc falls back to a named file). name_to_handle_at: EOPNOTSUPP; open_by_handle_at: EPERM without capability.
14. Owner: the host account of the runtime is presented as the guest identity (1000 by default), root stays 0, any other owner becomes 65534. chown to another user: EPERM. A root guest (euid 0) stays subject to the user's host permissions.
15. fsync does not force the disk cache to be written (no F_FULLFSYNC); fallocate reserves on a best-effort basis (F_PREALLOCATE), APFS can still return ENOSPC later; PUNCH_HOLE writes zeros when F_PUNCHHOLE refuses the range.
16. Host re-exec (execve): the guest descriptor table is not exported yet (see `docs/requests/fs-task.md` §4).
17. Directory positions and state of objects without a host fd: specific to the process, not shared after fork (rule of `fd.h`).
18. SCM_RIGHTS (2026-10-10): a synthetic or virtual directory and an O_PATH descriptor are sent as a record (`LKD_FDREC_FS`, global guest path and flags) and the receiver reopens that path (`fs_rec_import`, `sys_open.c`). It is a new description, with its own position. A host character descriptor without a guest path that is the twin of an emulated device becomes that device (`fs_wrap_host_fd` calls `lkd_procfs_dev_adopt`). See `net.md`.

## Written requests

- `docs/requests/fs-1.md` (core): expose `lkd_config_get`, make `lkd_fs.h` includable, clarifications of `vfs.h`, comment of `fd.h` on pipes, `lkd_sys_*` symbols of the kernel test, `fake_make_task`.
- `docs/requests/fs-engine.md`: `lkd_fs_open_host_file`, `lkd_fs_open_guest_file`, `lkd_fs_install_stdio`, `lkd_fs_wrap_host_fd`, effects of `lkd_init` on the host, ignore host SIGPIPE.
- `docs/requests/fs-procfs.md`: use of the mount interface, fd/N magic links, fdinfo, mounts.
- `docs/requests/fs-mm.md`: `lkd_fs_mmap_check`, expected semantics of `lkd_mm_has_writable_shared_mapping`.
- `docs/requests/fs-task.md`: fdt/fsctx references, RLIMIT_NOFILE, exec; F_SETFL cooperation with net.

## Possible next steps

Export/import of the table for the host exec; pid of ".." and synthetic inodes; `tee`; performance measurement of the walk (one lstat per component) on a real Wine launch.

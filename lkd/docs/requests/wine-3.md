# wine-3: fchdir on a directory fd received through SCM_RIGHTS -> ENOTDIR

Requester: wine-linux agent. Blocking: wineboot does not create the placeholder files of system32 (kernel32.dll, ...), so every following `wine <program>` fails with `could not load kernel32.dll, status c0000135`.

MEASURED (logs/wine-linux-m2-wineboot-005.trace.log, -006.log): NtQueryDirectoryFile on Z:\opt\wine\lib\wine\x86_64-windows. wineserver does `openat(dir, O_RDONLY|O_NONBLOCK) = 72`, `fstat = 0` (the server classifies it FD_TYPE_DIR), then sends it with sendmsg. The client receives the fd with recvmsg then calls `fchdir(fd) = -20 (ENOTDIR)` (dlls/ntdll/unix/file.c NtQueryDirectoryFile, `fchdir( fd )` before opendir(".")). 124 failures of this kind in a single wineboot. The fchdir calls of child processes at startup (fchdir to the current directory received from the server) fail the same way.

Minimal witness: src/wine-linux/tests/dirfd.c (static musl), launched with the Debian ROOT and /opt/wine linked:
```
open /usr -> 3
local fchdir -> 0 errno 0
child received fd 6, fchdir -> -1 errno 20
```
Same result for /opt/wine/lib/wine (read-only link). In the process that opened the fd, fchdir succeeds; after SCM_RIGHTS to another process, it fails.
INFERRED: the received fd object does not keep the directory's path or its directory nature in the receiving process's VFS table.
Request: that a directory fd received through SCM_RIGHTS accept fchdir, and also getdents64 and relative openat(fd, ...) (Wine uses both).

## Handled on 2026-10-02 (integration maintainer)

Cause (READ): on reception, `net_scm_import` passes the host descriptor to `lkd_fs_wrap_host_fd` without a guest path; `fs_wrap_host_fd` (`src/fs/file.c`) created an `LKD_FD_DIR` object without `gpath` or mount, and `fs_dirobj_gpath` answers ENOTDIR without `gpath` (fchdir, relative openat).
Fix (`src/fs/file.c`, `fs_wrap_host_fd`): for a directory or an ordinary file received without a guest path, the host path (`F_GETPATH`) is brought back to a guest path through the mount table (`lkd_fs_host_to_guest`), the mount is found again (`fs_mount_find`), the `O_DIRECTORY` flag is set for a directory. A descriptor outside any guest view keeps the old behavior.
MEASURED: new case `scm_rights_directory_across_fork` in `tests/test_net_unix.c` (two real processes: fstat S_IFDIR, openat(fd, "inner.txt"), getdents64, fchdir, getcwd = "/sub", `/proc/self/fd/N` = "/sub"): fails without the fix (step 3, relative openat), passes with it. Witness `src/wine-linux/tests/dirfd` under `build-integration/host/fexdarwin-run` (Debian root, `/usr`): `child received fd 6, fchdir -> 0 errno 0`. fs tests (6) and net tests (except `test_net_host`, earlier unrelated failure) pass.
Limit: a directory whose host path is under no host mount (outside the guest view) stays without a guest path (ENOTDIR at fchdir).

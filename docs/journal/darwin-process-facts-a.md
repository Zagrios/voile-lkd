# macOS facts on processes, IPC and synchronization (part A)

Last updated: 2026-10-02 (first draft, resumed after the session was interrupted). All sections marked "to be completed" have no measurement yet.

This document separates what was MEASURED in this session (small C programs run on this machine) from what is READ (SDK headers, man pages) and from what is DEDUCED. The programs are in `witnesses/darwin-process/` (prefixes `f_` and `g_`), the logs in `evidence/darwin-process/`. Each log contains `PASS`, `FAIL`, `INFO` or `SKIP` lines: `FAIL` means that the expectation written in the program was not verified, which is a data point and not necessarily a bug.

## Machine and measured limits

Measured (commands `sw_vers`, `uname`, `sysctl`, `ulimit` of this session):

- macOS 27.0.1 (build 26A434), kernel `xnu-13432.1.9~1/RELEASE_ARM64_T6050`, Apple M5 Max, 18 logical cores, 36 GiB of memory (`hw.memsize` = 38654705664).
- `hw.pagesize` = 16384.
- `kern.maxfiles` = 276480, `kern.maxfilesperproc` = 138240, `kern.maxproc` = 9000, `kern.maxprocperuid` = 6000.
- `ulimit -n` of the session shell = 1048576 (the terminal's value, not that of an application launched from the Finder: to be completed).
- Unprivileged user (uid 501), no root, no kernel extension, no authorization dialog triggered.

## Progress

| Fact | Program | Status |
| --- | --- | --- |
| F fork from a multithreaded process, survival in the child | `f1_fork_mt` | measured, 17 PASS 0 FAIL |
| F cost of fork (64 GiB PROT_NONE + 1 GiB touched) | `f2_fork_cost` | measured |
| F posix_spawn and fd inheritance | `f3_spawn_fd` | measured, 32 PASS 2 FAIL (POSIX deviations) |
| F execve from a multithreaded process | `f4_exec_mt` | measured (compiles without warnings with `-Wall -Wextra`) |
| F SCM_RIGHTS, peer identity, fileport | `f5_scm_rights` | measured, 27 PASS 2 FAIL (Linux deviations) |
| F memfd_create equivalents (shm, file, Mach memory entry) | `f6_memfd` | measured, 15 PASS 0 FAIL |
| F file mmap at offsets not aligned on 16 KiB | `f7_mmap_offset` | to be completed |
| F page aliasing and mremap equivalent | `f8_alias_remap` | to be completed |
| G os_sync / ulock between processes, latency, FUTEX | `g1_futex` | to be completed |
| G kqueue for epoll, eventfd, timerfd, signalfd, inotify | `g2_kqueue` | to be completed |
| G AF_UNIX sockets | `g3_unix_sockets` | to be completed |
| G pipe2, O_CLOEXEC, poll, fd limits, sendfile, splice | `g4_fd_misc` | to be completed |
| G level-triggered epoll prototype | `g5_epoll_proto` | to be completed |

## F. Processes and shared memory

### F1. fork() from a multithreaded process (`f1_fork_mt`, log `evidence/darwin-process/f1_fork_mt.log`)

Measured. The program creates 5 helper threads (busy-wait loop, malloc/free loop, a thread that holds a `pthread_mutex_t` and an `os_unfair_lock` blocked in `read`, a thread that holds the stdio lock of a `FILE`, a thread blocked in `read`) then forks from the main thread and from a secondary thread. Result: 17 PASS, 0 FAIL.

| Question | Measured result |
| --- | --- |
| Threads in the child | Only 1 (`task_threads` returns 1). The helper threads no longer run (counters frozen). |
| Thread that forked | Its `pthread_t`, its `pthread_getspecific` values and its `__thread` variables survive. Its identifier (`pthread_threadid_np`) is new (111808 versus 111798). `pthread_main_np()` is 1 if the fork comes from the main thread and 0 if it comes from a secondary thread: unlike Linux, the thread that forks does not become "the main thread". |
| Mach ports | The receive and send right created by the parent is not inherited (`mach_port_type` returns `KERN_INVALID_NAME`). `mach_task_self()` and `mach_thread_self()` do designate the new process, but the numeric names change (task 0x203 in the parent, 0x103 in the child; secondary thread 0x1a03 in the parent, 0x203 in the child). Any port name cached before the fork is therefore wrong in the child. `bootstrap_look_up` of a system service works in the child. The task exception ports are inherited (2 entries). |
| Locks held by another thread | `pthread_mutex_t`: stays locked, `trylock` returns `EBUSY`, a blocking `lock` stays suspended (hung after 700 ms, child killed). Locked `FILE`: `fputs` stays suspended. `os_unfair_lock`: a `lock` does not hang, it kills the child. The system crash report (copied to `evidence/darwin-process/os-crash-reports/f1_fork_mt-2026-10-02-102331.ips`) says `BUG IN CLIENT OF LIBPLATFORM: os_unfair_lock is corrupt, or owner thread exited without unlocking`, `EXC_BREAKPOINT`, signal `SIGKILL`, with the annotation `crashed on child side of fork pre-exec`. |
| malloc in the child | Works (libmalloc registers its atfork handlers). 300 successive forks while a thread hammers malloc: 300 children finished, 0 stuck, 1.24 ms per fork+child+wait. |
| `pthread_create` in the child | Works. |
| libdispatch in the child | If the parent never used GCD before the fork: `dispatch_async` on a global queue works in the child. If the parent had already used it: the child dies of `SIGSEGV` (log `f1_fork_mt.run2-gcd-warm-crash.log`). |
| Signals | Dispositions and mask of the forking thread inherited; pending signals not inherited; running `alarm` not inherited. |
| `atfork` | `pthread_atfork`: prepare and parent run in the parent, child in the child. |
| `vfork` | Behaves like `fork`: a write by the child to a global variable is not visible to the parent. `vfork` followed by `execl` works. |

Memory after fork (measured, 64 MiB regions):

- `MAP_SHARED|MAP_ANON`: really shared. The child's write is seen by the parent and vice versa. Two processes did 2 x 2,000,000 `atomic_fetch_add` on the same word: exact result 4,000,000.
- `MAP_PRIVATE|MAP_ANON`: copy-on-write, isolation in both directions.
- File mapped `MAP_PRIVATE`: copy-on-write, the file is never modified. File mapped `MAP_SHARED`: the child's write is visible to the parent and in the file.
- `munmap` and `mprotect` done by the child on its view do not affect the parent's.
- `minherit(VM_INHERIT_SHARE)` on a private anonymous region: after fork the region is shared like a `MAP_SHARED` one (writes visible in both directions). `minherit(VM_INHERIT_NONE)`: the region does not exist in the child (`mach_vm_region` does not find it). These two calls succeed and play the role of `MADV_DONTFORK` and of "make shared after the fact".
- Cost of a copy-on-write fault (16 KiB pages, median of 3 series over 256 MiB): first access to a fresh page 683 ns, next access without fault 7 ns, first write by the child after fork 1454 ns per page, first write by the parent with an idle child 2252 ns per page, rewrite by the parent after the child's death 378 ns per page (the copy-on-write state is therefore not cleaned up right away).
- `MAP_JIT` region: code compiled by the parent runs in the child; the child can rewrite it (toggle `pthread_jit_write_protect_np`); the parent's copy stays intact. Forking a `MAP_JIT` region costs much more than an ordinary fork (see F2). The child starts from the write state saved at the last context switch of the forking thread, not from its real state (see F7).

### F2. Cost of fork() (`f2_fork_cost`, log `evidence/darwin-process/f2_fork_cost.log`)

Measured, 30 forks per configuration, child that does `_exit(0)` right away. Values in microseconds (median of the steady state, unless stated). The machine was not idle (load 1 to 4 during the measurements, other teams compiling): the maxima are noisy.

| Configuration | `fork()` | child started | total until the wait |
| --- | --- | --- | --- |
| C0 bare process | 610 | 721 | 920 |
| C1 64 GiB reservation `PROT_NONE|MAP_NORESERVE` | 644 | 690 | 856 |
| C2 64 GiB reservation + 1 GiB touched (private) | 617 (first fork: 2675) | 656 | 821 |
| C3 1 GiB `mprotect(RW)` + touched in the reservation | 692 (first: 3296) | 738 | 910 |
| C4 1 GiB RW not touched | 675 | 708 | 869 |
| C5 1 GiB touched `MAP_SHARED` | 750 (first: 832) | 792 | 987 |
| C7 4 GiB touched (private) | 635 (first: 8755) | 673 | 857 |
| C6 1 GiB fragmented into 32,768 VM entries | 4529 (first: 15472) | 4511 | 9113 |
| 200 sleeping threads | 767 | 735 | 1083 |
| C8b 1 GiB `MAP_JIT` region, 0 MiB touched | 7744 | 7748 | 7953 |
| `MAP_JIT` 1 GiB, 16 / 64 / 256 MiB touched | 8921 / 10948 / 21296 | | 9186 / 11321 / 23013 |
| `MAP_JIT` 1 GiB, 256 MiB touched, `minherit(NONE)` | 678 | 677 | 875 |
| `MAP_JIT` 1 GiB, 256 MiB touched, `minherit(SHARE)` | 593 | 567 | 766 |
| Ordinary anonymous memory 1 GiB, 256 MiB touched | 707 | 747 | 929 |

Other measured figures:

- `PROT_NONE` reservation alone: 256 GiB 851 us, 1 TiB 1398 us, 4 TiB 4285 us, 16 TiB 16,423 us, 64 TiB (2^46) 68,993 us. The cost of `fork()` therefore grows linearly with the reserved size, about 1.07 ms per TiB, and the total time (child destroyed) roughly double (64 TiB: 142 ms). A 64 GiB reservation costs nothing measurable.
- The first fork of a process with touched private memory pays for making the parent's pages read-only (1 GiB: about 2 ms more, 4 GiB: about 8 ms more); the following forks do not pay this cost again as long as the parent does not write.
- After the fork, the parent that rewrites 1 GiB in one go (child alive) takes 127 to 182 ms (1944 to 2777 ns per 16 KiB page, 5.5 to 7.9 GB/s).
- `posix_spawn(/usr/bin/true)`: the call returns in 90 to 106 us and the total until the wait is 740 to 910 us, independently of the size of the parent's address space (it does not copy the address space). `fork` + `exec`: 1.5 to 1.65 ms.

Deduced: the dominant cost of an emulated fork is therefore (a) 0.6 ms base, (b) about 1.1 ms per reserved TiB, (c) about 7.2 ms per GiB of mapped `MAP_JIT` region plus 54 us per touched MiB, unless the region is marked `VM_INHERIT_NONE` or `VM_INHERIT_SHARE`, (d) the deferred cost of copy-on-write copies. The (c) measurements show that the kernel copies or walks `MAP_JIT` regions page by page at fork, which it does not do for ordinary anonymous memory.

### F3. posix_spawn of the same binary and descriptor inheritance (`f3_spawn_fd`, log `evidence/darwin-process/f3_spawn_fd.log`)

Measured. 32 PASS, 2 FAIL (the two FAILs are deviations from POSIX, see below: they are data).

- Default mode: 0, 1, 2 and ordinary descriptors are inherited, an `FD_CLOEXEC` descriptor is closed, an `FD_CLOEXEC|FD_CLOFORK` descriptor is closed. A descriptor that only has `FD_CLOFORK` ("close on fork" flag, new in SDK 27, `O_CLOFORK` 0x08000000, `FD_CLOFORK` 2, `F_DUPFD_CLOFORK` 115, `sys/fcntl.h:158,354,361`) is also closed in the child of a `posix_spawn`.
- `POSIX_SPAWN_CLOEXEC_DEFAULT` without action: everything is closed in the child, 0, 1 and 2 included. `posix_spawn_file_actions_addinherit_np(fd)` keeps the descriptor open, including if it has `FD_CLOEXEC` (the flag is cleared in the child). `adddup2(f, 20)` creates descriptor 20 and closes `f` in the child. A descriptor named by no action is closed. `addinherit_np` followed by `addclose` leaves the descriptor closed.
- Deviation from POSIX (2 FAIL): `adddup2(h, h)` on an `FD_CLOEXEC` descriptor does not keep it open, neither with `CLOEXEC_DEFAULT` nor in default mode (the descriptor is closed in the child). POSIX requires that a `dup2` onto itself clears `FD_CLOEXEC`. On macOS `addinherit_np` is needed.
- Silent limit: with `CLOEXEC_DEFAULT`, descriptors whose number is greater than or equal to 10240 (`OPEN_MAX`) are never inherited, even when named by `addinherit_np`, and `posix_spawn` still returns 0. Probed with 1000, 5000, 10000, 10039, 10040, 10041, 10100, 10240, 12000, 12300 actions on descriptors 200 to 12499: the child always has exactly descriptors 200 to 10239 (first closed: 10240). Outside `CLOEXEC_DEFAULT`, 20,000 open descriptors are all inherited. The workaround is to use `adddup2(fd, small_number)` for descriptors above 10239 (not measured).
- Cost: the `posix_spawn` call returns in 106 to 150 us, the total until the wait of a child that exits right away is 1.14 to 1.42 ms; 100 inheritance actions change nothing measurable; 4000 actions: 682 us for the call; 20,000 actions: 2.1 ms.
- Memory shared with a child launched by `posix_spawn`: a `shm_open` descriptor (then `shm_unlink`) or `mkstemp` descriptor (then `unlink`), inherited with `CLOEXEC_DEFAULT` + `addinherit_np`, is mapped `MAP_SHARED` by the child at another address (parent 0x10322c000, child 0x104788000). The parent's pattern is intact, the child's write is visible to the parent, and 2 x 100,000 atomic increments from both processes give exactly 200,000 for both backings.
- Attributes: an ignored signal stays ignored, a handler goes back to the default action, the signal mask, `umask`, the current directory and the `RLIMIT_NOFILE` limit are inherited, the environment is the one passed. `POSIX_SPAWN_SETSIGMASK` and `SETSIGDEF` work, `SETPGROUP` (pgroup 0) and `SETSID` make the child a group and session leader.
- `POSIX_SPAWN_START_SUSPENDED`: the child starts stopped (`waitpid(WUNTRACED)` sees it stopped, with a signal number 0), `SIGCONT` resumes it.
- Errors returned directly by `posix_spawn`: `ENOEXEC` for a text file without `#!`, for an ELF header and for an empty file; `ENOENT` for a missing `#!` interpreter and for an absent path; `EACCES` for a file without execute permission and for a directory.
- `fork`: `O_CLOFORK` descriptors are closed in the fork child, the others stay. `dup()` does not copy the `FD_CLOFORK` flag, `F_DUPFD_CLOFORK` sets it.

### F4. execve from a multithreaded process (`f4_exec_mt`, log `evidence/darwin-process/f4_exec_mt.log`)

Measured. The program creates a 6-thread process (main, an active thread, a sleeping one, one blocked in `read`, one blocked in `os_sync_wait_on_address`, one blocked in `kevent`) then calls `execve` from a secondary thread (then from the main thread, then with `posix_spawn(POSIX_SPAWN_SETEXEC)`).

- The pid, the parent pid, the process group and the process start date (`p_starttime`) are unchanged. Only one thread remains (the new main thread: `pthread_main_np()` is 1 even if the `execve` came from a secondary thread); this thread's signal mask is that of the thread that called `execve` (SIGTERM blocked stays blocked).
- Descriptors: ordinary kept, `FD_CLOEXEC` closed, `FD_CLOFORK` alone kept (this flag only acts at fork and spawn). Signals: ignored stays ignored, handler reset to the default action. `umask`, `nice` value, `RLIMIT_NOFILE` limit kept. An alarm armed before the exec stays armed (60 s remaining). A Mach receive right created before the exec has disappeared (the name can be recycled by the new image: a first version of the test got caught, the final version compares the port context). The task exception ports are those of the new image (2 entries). A `MAP_SHARED|MAP_ANON` page of the old image no longer exists.
- Latency until the first line of `main` of the new image (median): 6-thread process 2.1 ms; `posix_spawn(SETEXEC)` 1.97 ms; with a 64 GiB reservation and 1 GiB touched 15.1 ms; with a `PROT_NONE` reservation of 2^46 bytes 83.4 ms. The cost of tearing down the old address space is therefore paid in the `execve` (about 13 ms per touched GiB, about 1.3 ms per reserved TiB).
- `execve` error table: text file without `#!` `ENOEXEC`, ELF header `ENOEXEC`, empty file `ENOEXEC`, arm64 MH_DYLIB `ENOEXEC`, 32-bit ARM Mach-O `EBADARCH` (86), missing `#!` interpreter `ENOENT`, absent path `ENOENT`, directory `EACCES`, file without x permission `EACCES`, 2099-byte path `ENAMETOOLONG`, link loop `ELOOP`, arguments beyond `kern.argmax` (1,048,576) `E2BIG`. An arm64 Mach-O with truncated load commands does not return an error: the process is killed (the `execve` passed the point of no return). `execve(path, NULL, environ)` succeeds as on Linux. Executing a file open for writing succeeds (no `ETXTBSY` on macOS).

### F5. SCM_RIGHTS, peer identity and fileport (`f5_scm_rights`, log `evidence/darwin-process/f5_scm_rights.log`)

Measured. 27 PASS, 2 FAIL (two deviations from Linux, detailed below). The log of the first run is kept (`f5_scm_rights.run1.log`): same results, only the label of one test was clarified afterwards.

- Descriptor types that go through `SCM_RIGHTS` without error: regular file, both ends of a pipe, `shm_open` descriptor (already `shm_unlink`ed), `mkstemp` (already `unlink`ed), directory, AF_UNIX datagram socket, `/dev/null`, `O_EVTONLY`, `O_SYMLINK`, listening AF_UNIX socket, unconnected TCP socket, pty master and slave. On arrival one has the same object (same `st_mode`) and the same open file description: the offset set to 7 by the received copy is seen by the sender. A `connect()` on the path of a listening socket received over this channel is accepted by the original socket.
- A `kqueue` descriptor is refused by `sendmsg` with `EINVAL` (22). This is a test FAIL (which hoped it would pass), and the data point to keep: an epoll object emulated by kqueue cannot be transmitted to another process. This result matches `logs/host-test-kq-fork-scm.log` from the HyperLinux study.
- Flags: the received descriptor has no flag (the sender's `FD_CLOEXEC` and `FD_CLOFORK` are not transmitted). The Linux value of `MSG_CMSG_CLOEXEC` (0x40000000) is ignored without error and the received descriptor does not have `FD_CLOEXEC`. The receiver must call `fcntl(F_SETFD)` itself.
- Number of descriptors per message: 254 at most (stream as well as datagram, control length 1028 bytes), 255 gives `EINVAL`. No descriptor leaked during the sweep (23 open before and after). Linux's limit of 253 (`SCM_MAX_FD`) is knowledge external to this session, not a measurement.
- Deviation from Linux (the second FAIL): if the receiver only gives room for 3 descriptors and 10 are in flight, `recvmsg` returns 3 descriptors with `MSG_CTRUNC`, but the kernel installed all 10 in the receiver's table (open: 23 then 33, then 30 after closing the 3 announced). Seven descriptors stay open without the program knowing their numbers. Linux closes those that do not fit. Consequence for the emulation: always provide a control area big enough for 254 descriptors (1028 bytes), never less than what the guest asked for without checking, and sweep the table after an `MSG_CTRUNC`.
- Message with no data byte and one descriptor: accepted in stream as in datagram, `recvmsg` returns 0 bytes and 1 descriptor.
- A descriptor closed by the sender while it is in flight still arrives and maps the same shared memory.
- With `fork`: the child receives a `shm_open` descriptor through `SCM_RIGHTS`, maps it `MAP_SHARED`, and the parent sees its write. The child also sends a pipe end back to the parent.
- Peer identity (Linux's `SO_PEERCRED` does not exist). `getsockopt(SOL_LOCAL, LOCAL_PEERCRED)` returns an `xucred` (version 0, uid 501, 16 groups) without pid. `LOCAL_PEERPID` and `LOCAL_PEEREPID` return the pid, `LOCAL_PEERUUID` the UUID, `LOCAL_PEERTOKEN` a 32-byte audit token, `getpeereid` uid and gid. This holds for a `socketpair` (both ends) and for a socket accepted on a path whose client is a forked child. For a `socketpair` created before a `fork`, the pid returned at both ends is the creator's (43034 on both sides in the log), not that of the process that holds the other end. A host pid will therefore have to be translated into a guest pid, and the peer of an inherited `socketpair` will be wrong if the emulation does not itself record who closed what. `sendmsg` with `SCM_CREDS` on AF_UNIX: `EINVAL` (no `SCM_CREDENTIALS`). `setsockopt(SO_TIMESTAMP)` is accepted on AF_UNIX.
- `fileport` (private libSystem API): `fileport_makeport(fd)` gives a Mach port that carries the open file description, `fileport_makefd(port)` creates a new descriptor on the same object (same offset: set to 5 by one descriptor, read at 5 by the other; a `shm_open` descriptor converted and converted back gives a view that aliases the same pages). In a forked child, `fileport_makefd` of the port inherited "by its name" fails (`EINVAL`), because port names are not inherited by `fork` (see F1). F6 shows that a fileport sent by Mach message to an unrelated process works.

### F6. memfd_create equivalents (`f6_memfd`, log `evidence/darwin-process/f6_memfd.log`)

Measured. 15 PASS, 0 FAIL after fixing two errors in the program (see below). `memfd_create` does not exist (`dlsym` returns NULL). Three candidates were compared: B1 `shm_open`, B2 temporary file opened then detached (`mkstemp` + `unlink`, APFS), B3 Mach memory entry.

| Question | B1 `shm_open` | B2 detached file |
| --- | --- | --- |
| Name length | 31 characters at most, leading slash included; 32 gives `ENAMETOOLONG` | not applicable |
| `ftruncate` | only once: the first call succeeds and the size is rounded to 16 KiB (100 gives `st_size` 16384); any later call, even at the same size or at 0, gives `EINVAL` | as many times as wanted, grow or shrink |
| `read`, `write`, `pread`, `pwrite`, `lseek` | `ENXIO` for `read` and `write`, `ESPIPE` for the others: only `mmap` is possible | everything works |
| `mmap` `MAP_SHARED` read-write | yes | yes |
| `mmap` `MAP_PRIVATE` | `EINVAL` | yes (copy-on-write, the file does not change) |
| `mmap` beyond the size | `EINVAL` | accepted, access past the end gives SIGBUS or SIGSEGV |
| `PROT_EXEC` | `mmap` accepted, `mprotect(R+X)` gives `EACCES` | `mmap(PROT_EXEC)` gives `EPERM`, `mprotect(R+X)` is accepted |
| Offset 4096 then 16384 | 4096 gives `EINVAL`, 16384 accepted | same |
| Two views at two addresses | aliasing within a process; `MAP_FIXED` in a `PROT_NONE` reservation works | same |
| Size from 1 GiB to 1 TiB | `ftruncate` and `mmap` in a few microseconds, sparse object (`st_blocks` 0), first and last bytes written | same, a few microseconds more |
| First access per 16 KiB page (median over 256 MiB) | 570 to 719 ns | 969 to 1497 ns (the private anonymous comparison gives 629 to 683 ns, shared anonymous 587 to 745 ns) |
| Memory accounting (`phys_footprint`) | +257 MiB, internal | +0, 256 MiB "external" |
| Holes | `F_PUNCHHOLE` not applicable | `F_PUNCHHOLE` works: `st_blocks` 2048 becomes 1536, the zone reads back zeros |
| `F_GETPATH` | `EBADF` | returns the old path even after `unlink` (so unusable to emulate `/proc/self/fd/N` of a memfd) |

Other measurements: `mmap(MAP_SHARED)` of `/dev/zero` gives `ENODEV` (Linux returns shared anonymous memory: to be emulated with `MAP_ANON|MAP_SHARED`). On a mapped `shm_open` object, `madvise(MADV_ZERO)` resets the bytes to zero immediately; `MADV_FREE`, `MADV_DONTNEED`, `MADV_FREE_REUSABLE` and `MADV_FREE_REUSE` succeed without changing the content. A fork of a child that inherits the descriptor (shm or detached file) maps the same memory: data parent to child, child to parent and the parent's late write are all seen.

Mach memory entry (B3): `mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE)` returns a send right (type 0x10000), not a descriptor. Size rounded to 16 KiB (5000 gives 16384), 256 GiB accepted. `mach_vm_map` of the entry twice gives two views that alias; a `copy=TRUE` view made after the write receives the content and is not affected by later writes to the shared view. At `fork`, `VM_INHERIT_SHARE` shares and `VM_INHERIT_COPY` isolates the child's write. Rendezvous between two unrelated processes: by substituting the child's bootstrap port with `posix_spawnattr_setspecialport_np(TASK_BOOTSTRAP_PORT)`, the child sends a receive port, the parent sends it the entry (and a fileport of a `shm_open` descriptor) in a message with a port descriptor, the child maps them at another address and the parent sees its writes (both PASS), without going through `launchd`. `MAP_SHARED|MAP_ANON` has no handle: only `fork` children inherit it.

Two program errors fixed during this resumption (first log kept: `f6_memfd.run1-mach-invalid-right-segv.log`): `mach_vm_map` with a maximum protection of `VM_PROT_ALL` on an entry created read-write returns `KERN_INVALID_RIGHT` (17), which made the test crash (`SIGSEGV`, exit code 139); a `copy=TRUE` view made before the write receives an empty snapshot, and the test's expectation was wrong.

### F7. MAP_JIT write state of a fork child (`f7_fork_jitstate`, 2026-10-10, request `wine-5`)

Measured on macOS 27 (Darwin 27.0.0), Apple M5 Max. 5 PASS, 0 FAIL, 40 forks per case. Each child executes an instruction from a MAP_JIT page under a temporary SIGBUS handler, so no child crashes and no crash report is written.

`pthread_jit_write_protect_np()` switches the calling thread between "MAP_JIT writable" and "MAP_JIT executable" without entering the kernel (28 ns per pair, measured). The single thread of a fork child does not receive the state the parent's thread has at the moment of the fork, but the one the kernel saved at this thread's last context switch:

| What the parent does before the fork | Children that cannot execute the MAP_JIT code |
| --- | --- |
| blocks (`usleep`) in execute mode | 0/40 |
| blocks in write mode, then goes back to execute without a system call | 37/40 to 39/40 (SIGBUS, permission fault on execution) |
| same, and the child calls `pthread_jit_write_protect_np(1)` before anything else | 0/40 |
| runs 30 ms in write mode (preempted) | 29/40 |
| makes a system call (`getpid`) in write mode | 0/40 |
| takes a page fault (first access) in write mode | 0/40 |
| a signal handler blocks in write mode | 40/40 |
| 4 other threads keep blocking in write mode, not the one that forks | 0/40 |

A thread created (`pthread_create`) after its creator blocked in write mode executes the MAP_JIT code: 40/40. Only fork is affected.

Deduced: the per-thread state is only saved at a context switch (blocking or preemption), not on entry to a system call or a page fault. Fork copies this saved state, which can be stale. The parent is not affected, its register keeps the real state. Consequence for a JIT engine that forks: the child must set again the state it thinks it has (`pthread_jit_write_protect_np(1)` outside a write) before executing MAP_JIT code.

## G. Synchronization and events

### G1. `pthread_rwlock` and signals (`g1_rwlock_signals`, 2026-10-11, ELDEN RING freeze)

Measured on macOS 27.0.1 (26A434), Apple M5 Max. 5 PASS, 0 FAIL. 16 threads read under the lock in a loop, 4 threads write, and the main thread sends SIGUSR2 to one of them every 500 µs, empty handler (like fexdarwin's kick). Each case runs 6 s in its own child.

| Case | Result |
| --- | --- |
| `pthread_rwlock_t`, no signal | 4,002,861 reads, 352,303 writes in 6 s |
| `pthread_rwlock_t`, signals, handler without `SA_RESTART` | frozen at 4 s (10,742 reads, 1,108 writes): no thread makes progress any more |
| `pthread_rwlock_t`, signals, `SA_RESTART` | frozen at 4 s too |
| `pthread_mutex_t`, same signals | 110,173,189 acquisitions in 6 s, no freeze |
| one-word lock on `os_sync_wait_on_address` (LKD's `mm_rwlock`), same signals | 157,165,201 reads, 458,839 writes in 6 s, no freeze |

libc++'s `std::shared_mutex` (mutex and condition variables, not `pthread_rwlock`) does not freeze either: 63 million shared acquisitions and 274,313 exclusive ones in 10 s under 20,000 signals (one-off test outside the suite).

Deduced: a `pthread_rwlock` whose waiting threads receive signals ends up no longer waking anyone. All the threads stay in `__psynch_rw_rdlock` or `__psynch_rw_wrlock` without any of them holding the lock. The freeze needs neither fork nor any particular system call, only signals that land on waiting threads. Consequence for an engine that interrupts its threads with signals: no `pthread_rwlock` on a path these threads take. An atomic word and `os_sync_wait_on_address`, which rereads the state on each wakeup, does not have this defect, and it is also 39 times faster here.

## Implications for a Linux kernel emulation on Darwin

to be completed (table fork, clone, exec, futex, epoll, memfd, SCM_RIGHTS: Darwin primitive, deviation from Linux, first qualification experiment).

## Open questions and things that cannot be tested

to be completed

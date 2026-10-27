# Status of the net module (LKD)

Date: 2026-10-02. Milestones reached: AF_UNIX and AF_INET/AF_INET6 sockets, SCM_RIGHTS and SCM_CREDENTIALS, poll/select, epoll on kqueue, shared eventfd, timerfd, inotify (subset), exchange with a native host process (graphics needs G4).

## What exists (src/net/, 5400 lines, NOTICE listing the HL import)

- `socket.c`: socket object (non-blocking host descriptor, guest O_NONBLOCK in the object, SO_NOSIGPIPE), socket, socketpair, bind, listen, accept, accept4, connect, getsockname, getpeername, shutdown, ioctl FIONREAD/SIOCOUTQ/SIOCATMARK, `sock_poll` (Linux states rebuilt from the host).
- `sockmsg.c`: sendto, recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg; MSG_PEEK, MSG_WAITALL, MSG_TRUNC, MSG_DONTWAIT, MSG_NOSIGNAL, MSG_CMSG_CLOEXEC, MSG_WAITFORONE; SCM_RIGHTS in both directions; SCM_CREDENTIALS (see below).
- `sockopt.c`: getsockopt, setsockopt (SOL_SOCKET, TCP, IP, IPV6); SO_RCVTIMEO/SNDTIMEO held in the object, SO_RCVBUF/SNDBUF doubled, SO_LINGER (seconds), SO_PEERCRED, SO_PASSCRED, booleans read back as 0 or 1.
- `unixaddr.c`: Linux (u16 family) and Darwin addresses, AF_UNIX paths through the vfs, abstract namespace through files.
- `scm.c`: objects across SCM_RIGHTS (file, pipe, socket, host shm object, eventfd as three descriptors, ntsync object through its anchor, and what `ops->export_fds` returns: host twin of an emulated device, record for objects without a host form, see "SCM_RIGHTS of objects without a host descriptor").
- `eventfd.c`: eventfd, eventfd2 (counter in MAP_SHARED shared memory, lock-free updates, readiness pipe).
- `timerfd.c`: timerfd_create, timerfd_settime, timerfd_gettime (expirations computed from the clock, kqueue EVFILT_TIMER as the wait source).
- `epoll.c`: epoll_create, epoll_create1, epoll_ctl, epoll_wait, epoll_pwait, epoll_pwait2; epoll_ctl_old and epoll_wait_old return ENOSYS like Linux.
- `inotify.c`: inotify_init, inotify_init1, inotify_add_watch, inotify_rm_watch on EVFILT_VNODE.
- `wait.c`: poll, ppoll, select, pselect6, `lkd_net_wait_object`, `lkd_net_make_notify_pipe`, poll restart.
- `net_init.c`: init, module lock (rank 71), fork hooks that rebuild the kqueues (timerfd and inotify first, epoll after), `lkd_net_object_released`.

Registered syscalls (45): socket socketpair bind listen accept accept4 connect getsockname getpeername sendto recvfrom sendmsg recvmsg sendmmsg recvmmsg shutdown setsockopt getsockopt poll ppoll select pselect6 epoll_create epoll_create1 epoll_ctl epoll_wait epoll_pwait epoll_pwait2 epoll_ctl_old epoll_wait_old eventfd eventfd2 timerfd_create timerfd_settime timerfd_gettime inotify_init inotify_init1 inotify_add_watch inotify_rm_watch (`tools/owners_check.py`: 0 problems). All handlers are `static`.

## Tests (MEASURED, private core + fs + mm + sig + task + net build, each program run 3 times without failure)

| Program | Assertions | Cases | Content |
| --- | --- | --- | --- |
| `test_net_unix` | 233 | 15 | AF_UNIX stream and datagram, bind/listen/accept/connect, long paths, abstract namespace and autobind, SCM_RIGHTS (pipe, file, socket, eventfd, MSG_CTRUNC truncation, between two processes), SCM_CREDENTIALS, options, poll, blocking read and interruption, SIGPIPE |
| `test_net_epoll` | 352 | 15 | level, edge, oneshot, removal on close, sockets, listening, eventfd, blocking and interrupted wait, epoll_pwait, nesting and ELOOP, rebuild after fork, 100 descriptors |
| `test_net_inet` | 147 | 10 | TCP and UDP on loopback, non-blocking connect, refusal, SO_ERROR, TCP options, IPv6 ::1, sendmmsg/recvmmsg, DNS-like exchange, poll of TCP |
| `test_net_events` | 199 | 16 | poll/ppoll/select/pselect6, eventfd (counter, semaphore, blocking, shared by fork), timerfd (one-shot, periodic, absolute, epoll, fork), inotify |
| `test_net_scm` | 38 | 5 | SCM_RIGHTS between two host processes (fork) of objects without a host descriptor: /dev/null, /dev/zero, /dev/urandom, /dev/random, /dev/full (round trip), host /dev/null as standard input, synthetic directory, /dev, /proc/<pid>, O_PATH of a file and of a synthetic directory, /proc/<pid>/status with its position, signalfd. The five cases fail before the change (EOPNOTSUPP on send) (2026-10-10) |
| `test_net_host` | 48 | 2 | a native host thread plays wsd/vtest: guest connection to a path bound to a host directory, `getpeereid` sees the runtime's uid, host shm object received then `mmap(MAP_SHARED)` by mm with consistent writes in both directions, fence pipe received and waited on by poll and epoll, guest pipe sent to the host |
| total | 979 | 58 | |

## Measured host facts (macOS 27.0.1, this Mac)

- AF_UNIX SOCK_SEQPACKET: `EPROTONOSUPPORT` on socket and socketpair. LKD returns a visible `ESOCKTNOSUPPORT`.
- `sun_path` is 104 bytes (Linux: 108). A longer host path is reached through a directory descriptor and the leaf, with `pthread_fchdir_np` for the thread only.
- A fresh stream socket, a socket after `shutdown(SHUT_WR)` on one side, a closed peer: the host reports POLLHUP where Linux reports OUT, RDHUP or nothing (`tcp_poll`, `unix_poll`). `sock_poll` rebuilds the Linux bits from the host probe (0-byte send: EPIPE, ENOTCONN; recv MSG_PEEK: 0 for EOF).
- A refused TCP connect: `SO_ERROR` is `ECONNREFUSED` and the socket is HUP|ERR; a second connect gives `EINVAL` on the host.
- `connect(AF_UNSPEC)` on UDP gives `EAFNOSUPPORT` but dissolves the association; on TCP it is refused.
- A kqueue descriptor cannot go through SCM_RIGHTS (EINVAL): epoll, timerfd and inotify return `EOPNOTSUPP` on send.
- A received `shm_open` object has no file type bits in `st_mode`: this is how `scm.c` recognizes it (and recognizes the eventfd and the record by their 8-byte signature).
- `/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom` of the host: `F_SETFL` O_NONBLOCK and O_APPEND accepted and shared through SCM_RIGHTS; `poll` returns POLLNVAL, `kevent` EVFILT_READ and EVFILT_WRITE return EINVAL; `write` on `/dev/urandom` returns EPERM; `mmap` returns ENODEV. Numbers: null 3,2, zero 3,3, random 17,0, urandom 17,1 on this Mac (read back at run time). macOS has no `/dev/full`.
- kqueue on a kqueue: readable when an event is ready (used as the wait source for timerfd, inotify and nested epoll).
- A received socket descriptor shares the sender's O_NONBLOCK flag: LKD then passes `MSG_DONTWAIT` on each call instead of changing the shared description.

## Contract with Wine and glibc

- Wine sockets: the server and the clients use AF_UNIX socketpair, SCM_RIGHTS (file descriptors, pipes), epoll, eventfd and poll; all of this is covered by the tests. The named AF_UNIX path and the abstract one work.
- AF_NETLINK: `socket(AF_NETLINK)` returns a visible `EAFNOSUPPORT` (logged once). glibc 2.41: `getaddrinfo` with `AI_ADDRCONFIG` calls `__check_pf`, which opens a NETLINK_ROUTE socket; on failure it assumes IPv4 and IPv6 are present (behavior of `check_pf.c`, READ, not measured here); `getifaddrs` fails (`ENOSYS`/`EAFNOSUPPORT`). A minimal NETLINK_ROUTE is still to be written if a real witness asks for it.
- DNS: glibc reads `/etc/resolv.conf`, `/etc/hosts`, `nsswitch.conf` in the guest root (fs serves them) and talks UDP/TCP AF_INET to the server. Tested: non-blocking UDP socket, connect, send, poll, recv, SO_ERROR. Strategy: the runtime image provides `resolv.conf` with a server address reachable from the host (for example the one from `scutil --dns` or 1.1.1.1); there is no emulated resolver. ICMP error replies on UDP (closed port) are not always reported by the host: the caller falls back to the timeout.
- SCM_CREDENTIALS: on receive with SO_PASSCRED, LKD adds the credentials built from the peer's `LOCAL_PEERPID` and `LOCAL_PEERCRED` (real state, host pid translated into a guest pid through the registry). On send, LKD checks that the sender does not claim to be someone else (like `scm_check_creds`) then sends nothing: the receiver gets the same information from the host. No credentials for a peer outside the family (pid 0, not reported).

## Graphics needs (G4): state

1. Guest AF_UNIX path to a host socket: REACHED (bound through `lkd_vfs_mount_bind`, test `test_net_host`). `getpeereid` on the host side sees the uid of the runtime's account.
2. SCM_RIGHTS in both directions: REACHED. A host shm object becomes a `host-shm` guest object (host size and `fstat`, `read`/`write` refused with ENXIO like the host, `mmap_fd` for mm). A host pipe becomes an fs object; a guest descriptor goes out as a duplicated host descriptor.
3. poll and epoll on these sockets and on the received pipes: REACHED.
4. MAP_SHARED test consistent on both sides: REACHED (writes by the host process seen by the guest, and the reverse, two mappings of the same object that alias).

## SCM_RIGHTS of objects without a host descriptor (2026-10-10)

Linux passes any open file through SCM_RIGHTS. Before, LKD refused (`EOPNOTSUPP`, "SCM_RIGHTS of a dev object cannot cross host processes") any object without a host descriptor. Wine died from it: a process created without standard handles has `/dev/null` as 0 and 1 (`ntdll/unix/process.c` `set_stdio_fd`), and a Wine process without a Wine parent sends its fds 0, 1 and 2 to wineserver (`env.c` `get_initial_console`). Same refusal for a synthetic directory (`/games`, sailover report 06).

General mechanism (`fd.h`): an optional operation `export_fds` gives the host descriptors that carry the description. The receiver rebuilds the object (`net_scm_import`).

| Kind | Before | Now |
| --- | --- | --- |
| file, host directory, pipe, socket, memfd, host shm, tty | host fd | unchanged |
| eventfd | 3 host fds (shared region, pipe) | unchanged |
| ntsync (device and objects) | anchor (pipe) | unchanged |
| /dev/null, /dev/zero, /dev/random, /dev/urandom | refused | host twin (`procfs/dev.c`): the macOS device with the same behavior, opened with the guest access mode on the first send, or the one that arrived. An LKD process that receives it gets the Linux device back (1:3, reads and writes by LKD); a host process receives a device that works |
| /dev/full, a device opened O_PATH | refused | `LKD_FDREC_DEV` record |
| synthetic directory, virtual directory (/proc, /proc/<pid>, /dev, /sys...), O_PATH | refused | `LKD_FDREC_FS` record: the global guest path and the flags, reopened by the receiver |
| generated file of /proc and /sys, /proc/<pid>/mem | refused | `LKD_FDREC_PROCFS` record: canonical path and position |
| signalfd | refused | `LKD_FDREC_SIGNALFD` record: the mask; the receiver reads its own signals, like Linux (`signalfd_dequeue` on `current`) |
| epoll, timerfd, inotify | refused | refused (`EOPNOTSUPP`): their state is a kqueue and a process table, macOS does not pass a kqueue (EINVAL) |

A record (`src/core/fdrec.c`) is a 16 KiB POSIX shm object, unlinked as soon as it is created, that starts with `LKDFREC1`, the type, the Linux status flags and the data of the owner module. It only contains guest names, never a host path or a pointer. The receiver reads it then closes it; the module reopens the thing with the receiver's permissions. A host descriptor of `/dev/null` (or zero, random, urandom) that arrives without a guest path, through SCM_RIGHTS or as the runtime's standard input, becomes the Linux device (`lkd_procfs_dev_adopt`, called by `fs_wrap_host_fd`): before, the guest saw the macOS numbers and `poll` returned POLLNVAL.

Differences: the receiver of a record has a new description of the same thing. The position (directories, /proc files) and flags changed afterwards are not shared, as for objects without a host fd after a fork (`fd.h`). The twin of a device does not share the flags either: each process keeps its own, copied at send time. A /proc directory of a process that died between the send and the receive does not reopen (the descriptor is lost, MSG_CTRUNC not set), where Linux keeps a descriptor whose lookups fail. An O_PATH of a file renamed or deleted in the meantime reopens through its old path.

## Known differences (honest)

1. AF_NETLINK, AF_PACKET, SOCK_RAW: refused (EAFNOSUPPORT, EPERM).
2. AF_UNIX SOCK_SEQPACKET: ESOCKTNOSUPPORT.
3. The AF_UNIX socket names returned by a client's `getpeername` are those given to connect (remembered); `accept` and `recvfrom` translate the host path back into a guest path through the mount table (a path outside the guest view is returned as is).
4. SO_PEERSEC, SO_PEERGROUPS, SO_BINDTODEVICE, SO_TIMESTAMP*, IP_PKTINFO, MSG_ERRQUEUE: ENOPROTOOPT or not supported (logged once).
5. Exhaustion of the socket queue: the buffers are the host's.
6. epoll: EPOLLEXCLUSIVE is validated like Linux then handled as a normal registration (all sets wake up); EPOLLWAKEUP ignored; regular files, directories and memfd refused with EPERM like Linux. An epoll set is not shared after fork: the child rebuilds its copy (Linux shares the set), the two sets diverge afterwards. An epoll shared between processes through SCM_RIGHTS is impossible (EOPNOTSUPP). Edge (EPOLLET): the objects based on a readiness pipe (eventfd, signalfd) signal an edge on every write.
7. timerfd: TFD_TIMER_CANCEL_ON_SET accepted but ECANCELED never arrives (LKD does not see a clock jump); `_ALARM`: EPERM; the state is copied (not shared) after fork.
8. inotify: IN_ACCESS, IN_OPEN, IN_CLOSE_* never delivered; events for the children of a directory need one descriptor per regular file (1024 at most); a burst of creations and modifications can regroup the order; a rename out of the directory is seen as DELETE and one into it as CREATE; no recursion; the instance is not shared by fork (rebuilt copy).
9. Pipe: the write end whose reader is closed is reported HUP by `fs_pipe_ops.poll` (Linux: EPOLLERR); request in `docs/requests/net-1.md`.
10. The fork hooks are those of lkd_fork_host: an engine that forks by itself must call `lkd_fork_prepare` and `lkd_fork_finish` so that the kqueues are rebuilt.
11. An `epoll_wait` call whose items have a wait source that cannot signal all the requested events (EPOLLOUT on an object with a readiness pipe) only sees them at the start of the call.
12. SCM_RIGHTS, flags: a pipe created by LKD always has a non-blocking host fd (fs, difference 5). The process that receives it therefore sees O_NONBLOCK (MEASURED 2026-10-10, `F_GETFL` of a received blocking pipe), and a read without data there returns EAGAIN instead of waiting (INFERRED, `stream_rw`). A received eventfd is blocking even if the sender created it EFD_NONBLOCK (the flag is in the object). Not fixed: it would need a description state shared between processes for these objects.
13. SCM_RIGHTS of a memfd: it goes out as a deleted host file. The receiver sees a regular file without the `/memfd:` name or seals.

## Possible next steps

Minimal NETLINK_ROUTE (RTM_GETADDR, RTM_GETLINK) if a glibc witness asks for it; IP_PKTINFO; sharing an epoll between processes via famsvc; real test of `wineserver` on LKD.

## Interfaces and NETLINK_ROUTE (2026-10-03, request wine-8)

`src/net/ifaces.c`: SIOCGIF* ioctls and NETLINK_ROUTE socket (dumps of links, addresses and routes) from the real state of the host, read-only. Difference 1 "AF_NETLINK refused" no longer applies to NETLINK_ROUTE. Details and limits: `docs/requests/wine-8.md`.

`/proc/net/route` and `/proc/net/ipv6_route` (2026-10-27): the same routes, in the kernel's text format (`lkd_net_render_routes`, published by procfs). Wine reads its routing table there; without them every `GetAdaptersAddresses` failed with `ERROR_NOT_SUPPORTED` (PlayFab Party and the EOS SDK in HELLDIVERS 2).

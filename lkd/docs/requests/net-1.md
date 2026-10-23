# Requests from the net module

Date: 2026-10-02.

## fs

1. `src/fs/pipe.c`, `pipe_poll` (line 116): for the write end of a pipe whose readers are all closed, Linux returns `EPOLLERR` (`pipe_poll`, `fs/pipe.c`). The host reports `POLLHUP` and `fs` translates it to `LX_POLLHUP`. Request: on an object open for writing, convert `POLLHUP` to `LX_POLLERR` (and do not report HUP). The net tests (`test_net_epoll`, case `level_triggered_pipe`) accept either.
2. The net objects use `ops->fcntl(LX_F_SETFL)` as agreed in `fs-task.md` section net: fs needs to keep calling the operation for non-fs objects.

## core

1. `lkd_config_get()` is still declared in `src/core/core_priv.h`: net redeclares it in `src/net/net_priv.h` (see `task-1.md`, point 1).
2. `lkd_internal.h` does not give a close hook for net objects before `release`: `lkd_net_object_released` is enough.

## engine

1. An engine that calls `fork()` itself must go through `lkd_fork_prepare` / `lkd_fork_finish`: without that the kqueues (epoll, timerfd, inotify) are not rebuilt in the child and calls on these objects fail with EBADF.
2. Ignore SIGPIPE on the host side (already requested by fs): net sends SIGPIPE to the guest thread itself and sets SO_NOSIGPIPE on its sockets.


## Handled on 2026-10-02 (integration maintainer)

fs point 1: `pipe_poll` (`src/fs/pipe.c`). The write end without a reader returns EPOLLOUT|EPOLLERR, without HUP or IN (MEASURED: macOS reports IN|HUP). Gap: a full pipe without a reader is also reported OUT (Linux: ERR only). Test: `test_net_epoll` `level_triggered_pipe` (exact value, epoll and poll). fs point 2: unchanged, compliant. core: done (lead). engine: see the final status (engine).

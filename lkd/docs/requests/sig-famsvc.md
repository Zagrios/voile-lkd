# Family IPC service (famsvc): announcement and how to use it

Requester: sig. Recipients: mm, procfs, fs, ptrace, task, lead. Date: 2026-10-02.

Decision of the lead: a single service for the four inter-process needs. It is delivered by the sig module in `src/sig/famsvc.c`, contract in `src/include/famsvc.h` (new internal file, owned by sig, created with the lead's agreement).

## What the service does (READ in the code, MEASURED in the tests)

- Each host process of the family has a host service thread (not a guest thread, `lkd_current()` is NULL, all host signals blocked) that listens on an AF_UNIX stream socket `<dir>/<host pid>.s`, in a family directory with mode 0700 (`LKD_FAMSVC_DIR`, created by `mkdtemp` at the first process). The peer is checked with `getpeereid` (same uid) and identified with `LOCAL_PEERPID`.
- Synchronous call with timeout: `lkd_famsvc_call(host_pid, &req, &resp, timeout_ms)` or `lkd_famsvc_call_pid(guest_pid, ...)` (host pid taken from the registry by `lkd_proc_lookup`). Payload up to 8 MiB in each direction, up to 16 descriptors through `SCM_RIGHTS` (received with `FD_CLOEXEC`).
- Transport errors: `-LX_ESRCH` (no service: process gone), `-LX_ETIMEDOUT`, `-LX_EPROTO`, `-LX_E2BIG`, `-LX_EINVAL` (call from the service thread, opcode out of range). An opcode without a handler answers `-LX_ENOSYS`.
- fork: `lkd_sig_fork_child` closes the inherited socket and the connections, and starts a service for the new host pid. host exec: the listening socket is not close-on-exec and `LKD_FAMSVC_FD="<fd>:<pid>"` designates it; the new image adopts it (MEASURED: `test_sig_famsvc` case `exec_keeps_the_service`, ping served by the re-executed image, counter `adopted == 1`).
- Shutdown: `lkd_sig_proc_exit` (last guest process of the host process) calls `lkd_famsvc_shutdown`, which deletes the socket and, if no live member is left, the directory. The `lkdfam.*` directories older than one minute without a live member (crashed runs) are deleted when a new family is created.
- MEASURED (`test_sig_famsvc`, 2000 PING calls of 4 bytes, M5 Max, macOS 27): round trip to another process p50 10.3 us, p90 13.9 us, p99 22 us; to itself p50 11.6 us.

## Reserved opcodes (fixed ABI between processes of a family)

| Range | Module | Already defined |
| --- | --- | --- |
| 1 | famsvc | PING (echo, status = host pid) |
| 16..31 | sig | QUEUE_SIGNAL (16), CHILD_EVENT (17), SIG_STATUS (18) |
| 32..47 | mm | READ_MEM (32), WRITE_MEM (33), RENDER_MAPS (34): to be registered by mm |
| 48..63 | procfs, fs | LIST_FDS (48), TASK_STATUS (49): to be registered by procfs or fs |
| 64..95 | ptrace | free for the tracer/tracee transport |
| 96..111 | task | free |
| 120..127 | tests | reserved for unit tests |

## Rules for a handler

1. Register in `lkd_<module>_init`: `lkd_famsvc_register(LKD_FAMSVC_OP_READ_MEM, handler)`.
2. The handler runs on the service thread, one request at a time per process, with no LKD lock held. It answers quickly, does no blocking wait on a guest thread, does not take a lock that a guest thread keeps during a wait, and never calls `lkd_famsvc_call` (two services that call each other would end up at the timeout; the call is refused with `-LX_EINVAL`).
3. It reads `req->data/len`, keeps a received descriptor by setting `req->fds[i] = -1` (the others are closed on return), fills `resp->status`, `resp->data` (malloc, freed by famsvc) and `resp->fds` (closed by famsvc after sending).
4. On the caller side, free the response with `lkd_famsvc_msg_free(&resp)`.

Example for mm (INFERRED, to adapt): `READ_MEM` receives `{uint64 gva; uint64 len}`, reads with the fault recovery of the local image, answers with the bytes read (short answer at the first unmapped byte); `WRITE_MEM` calls `ops->invalidate_code` in the target process before answering.

## What the engine must keep

`LKD_FAMSVC_DIR` and `LKD_FAMSVC_FD` in the environment of the host re-execution, and the descriptor named by `LKD_FAMSVC_FD` open (no `closefrom` before the exec). The service thread has no `GuestThread`: the engine's kick handler must ignore it (`LkdGlue.cpp` `KickHandler` already does so by returning if `CurrentGuestThread` is NULL; in any case SIGUSR2 is blocked there).


## Handled on 2026-10-02 (integration maintainer)

Announcement. task now uses opcode 96 (`TASK_OP_WAKE_WAITERS`, `src/task/tasks.c`).

# ntsync (sig module, `src/sig/ntsync.c`)

`/dev/ntsync` with the ioctls of `include/uapi/linux/ntsync.h` (Linux 6.14): `CREATE_SEM`, `CREATE_MUTEX`,
`CREATE_EVENT`, `WAIT_ANY`, `WAIT_ALL` (timeout absolute on `CLOCK_MONOTONIC`, or `CLOCK_REALTIME` with
`NTSYNC_WAIT_REALTIME`; alert event), `SEM_RELEASE`, `MUTEX_UNLOCK`, `MUTEX_KILL`, `EVENT_SET`, `EVENT_RESET`,
`EVENT_PULSE`, `SEM_READ`, `MUTEX_READ`, `EVENT_READ`. Wine 11 uses it when `/dev/ntsync` opens: wineserver creates
the objects, and waits and signals stay in the calling process.

`LKD_NTSYNC=0` removes `/dev/ntsync` (Wine then keeps every object in wineserver).

## Design

- **State.** `ntsync.shm` in the family directory, mapped by every process that uses the device. It holds slots
  (65,536), queued waits (2,048), and an inode hash. One spin lock guards it all (holder's host pid, taken over from
  a dead holder, as `futex.shm`).
- **Identity across processes.** Each device and object has an anchor: the read end of a host pipe whose write
  end is closed. Its inode names the slot. SCM_RIGHTS carries the anchor (`net/scm.c`), and the receiver gets the
  same slot (`lkd_ntsync_import`). A received pipe counts as an anchor only when it has no writer and is empty.
- **Waits.** A wait acquires at once when it can. Otherwise it queues and sleeps on a shared sleeper of `futex.c`,
  so signals and `lkd_task_wake` end it. A change to an object hands it to the queued waits it satisfies, WAIT_ALL
  first, oldest first, as `try_wake_all_obj` and `try_wake_any_obj` do. A wait whose process died is dropped instead.
- **Lifetime.** A slot counts the fd objects that name it in every process, plus objects of a device and queued
  waits. The last guest process of a host process releases what that process still holds (`sig_ntsync_host_exit`).

## Measured (2026-10-03)

- Unit test `tests/test_sig_ntsync.c`, following the kernel selftests: state and errors of each object, WAIT_ANY,
  WAIT_ALL, alerts, wakes across threads, a signal ending a wait, sharing across a host fork and through anchors,
  release at host exit and at a group exit, slot reuse.
  - `EVENT_SET` 0.02 us ; create + close 1.4 us ; a signal ends a wait 3 to 4 ms after it is sent.
- Wine through fexdarwin (`tests/bench/winbench.c`):
  - `WaitForSingleObject(0)` 11.8 → 0.35 us ;
  - `SetEvent` 10.7 → 0.16 us ;
  - an event ping-pong between two threads 34.7 → 8.8 us ;
  - `CreateEvent` + `CloseHandle` 20.9 → 23.3 us (still a wineserver round trip, plus the anchor).

## Limits

- **A fork child's copies are not counted.** The child execs or exits, as Wine's do. A child that keeps using an
  object after its parent closed it finds it gone (`-EINVAL`).
- **A process killed outright leaks its references.** Its slots stay until the family ends.
- **The table is fixed** at 65,536 objects alive at once, and 2,048 waits queued at once (`-ENOMEM` beyond).
- **No `poll`/`read`** on the descriptors, as in Linux.

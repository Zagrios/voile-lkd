# Request procfs-2 to the mm module

Date: 2026-10-02. Author: procfs module.

1. `/dev/zero` (`drivers/char/mem.c` `mmap_zero`): an mmap of the file is anonymous memory (private: zero pages copied on write; shared: anonymous shared object). procfs provides `int lkd_procfs_dev_is_zero(lkd_fdobj *o)` (`src/procfs/lkd_procfs.h`). Today `lkd_fs_mmap_check` answers `-ENODEV` for this object (no host fd), so `mmap(/dev/zero)` fails. Request: in the file mmap path, if `lkd_procfs_dev_is_zero(file)` then treat it as `MAP_ANONYMOUS` with the object named `/dev/zero (deleted)` in maps (Linux: `/dev/zero (deleted)`). No procfs test requires it (it is an mm test).
2. Peak counters. `/proc/<pid>/status` shows `VmPeak` and `VmHWM`. `lkd_mm_stats` does not give them: procfs shows the current value (lower bound of the peak, stated in the status). Request: add `hiwater_vm` and `hiwater_rss` to `struct lkd_mm_stats` (updated at each map and at each `munmap` that reduces `total_vm`).
3. `lkd_mm_remote_*` in identity window mode (tests): the service is not started by `lkd_mm_create` (`mm.c:128`). `tests/test_procfs_other.c` calls `mm_remote_start()` itself. Nothing to change if this is intended.
4. `THP_enabled` in status is fixed at 1 (INFERRED: THP is only a flag in mm). If mm keeps `PR_SET_THP_DISABLE`, say where to read it.


## Handled on 2026-10-02 (integration maintainer)

Point 1: `do_mmap` (`src/mm/sys.c`) treats a `/dev/zero` object (`lkd_procfs_dev_is_zero`, weak in core) as anonymous memory: private "/dev/zero" in maps, shared anonymous shared object "/dev/zero (deleted)"; EACCES for a writable shared mapping on a read-only fd. Test: `test_procfs_global` case `dev_nodes`. Before: ENODEV.
Point 2: `hiwater_vm` and `hiwater_rss` in `struct lkd_mm_stats`. `hiwater_vm` is exact (before each unmapping, like `update_hiwater_vm`). `hiwater_rss` is sampled at each read and before unmappings of 2 MiB or more: it is a lower bound. Reset at exec. procfs shows them in VmPeak and VmHWM. Test: `test_mm_misc` case `high_water_marks`.
Point 3: intended (remote service started only for a production window). Point 4: mm does not keep `PR_SET_THP_DISABLE`; `THP_enabled` stays 1.

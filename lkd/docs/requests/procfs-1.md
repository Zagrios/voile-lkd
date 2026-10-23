# Request procfs-1 to the common kernel (core)

Date: 2026-10-02. Author: procfs module.

1. Include path. `src/procfs/lkd_procfs.h` (API for the engine and the tests: CPUID provider, test backend, `lkd_procfs_dev_is_zero`) is included by relative path (`"../src/procfs/lkd_procfs.h"`). Request: add `src/procfs` to the include path of `liblkd` and of the tests, or create `src/include/lkd_procfs.h` that does `#include "../procfs/lkd_procfs.h"`. Same thing as `docs/requests/fs-1.md` point 2 for `lkd_fs.h`.
2. `lkd_task_uname` is defined by task (`src/task/sys.c:25`) but declared nowhere (request `task-1.md` point 2). procfs redeclares it in `src/procfs/procfs_priv.h`. Request: declare it in `task.h`.
3. `lkd_config_get`: same request as `fs-1.md` point 1 (procfs redeclares it).
4. `fake_make_task` creates neither a descriptor table, nor an fsctx, nor an mm. The procfs tests set them up (`tests/test_procfs_util.h`, `procfs_fixture`). If core adds them, the tests stay valid.
5. Hook order: procfs has no hook (no lock, no per-process state). Nothing to change.


## Handled on 2026-10-02 (integration maintainer)

Point 1: `src/include/lkd_procfs.h` points to `src/procfs/lkd_procfs.h`; tests and net now include `"lkd_fs.h"` and `"lkd_procfs.h"`. Point 2: `lkd_task_uname` declared in `task.h`, redeclaration in `procfs_priv.h` removed. Point 3: done (lead). Point 4: not done (same reason as fs-1 point 6). Point 5: nothing.

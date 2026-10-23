# fs -> core: requests about the shared files

## 1. Expose `lkd_config_get`

`lkd_fs_init` needs `lkd_config.guest_root`, and `file.c` needs the guest uid/gid to present the owner of host files. `lkd_config_get` is declared only in `src/core/core_priv.h`. Local workaround: `src/fs/fs_priv.h` redeclares it. Request: declare it in `src/include/lkd_internal.h`.

## 2. Header `lkd_fs.h` visible to the other modules

The fs services for the engine, procfs, mm and task are in `src/fs/lkd_fs.h` (I can only write under `src/fs`). Request: either add `src/fs` to the include path of `liblkd` and of the tests, or create `src/include/lkd_fs.h` that does `#include "../fs/lkd_fs.h"`. Meanwhile, the tests include `"../src/fs/lkd_fs.h"`.

## 3. Clarifications of `vfs.h` (virtual mount interface, no signature change)

What the fs module implements and what procfs can treat as stable:

- `readdir` returns neither "." nor "..": the vfs adds them (it ignores these names if they are returned). The vfs also adds the mount points located directly under the listed directory (for example `shm` under `/dev`).
- `getattr` is called with `nofollow = 1` during the walk. An ordinary link is read by `readlink` and resolved in the guest space (absolute target from the task's root). A final `magic_link` node that is followed is opened by `open(rel)`; a `stat` that follows a magic link does `open(rel, O_PATH)` then `fstat` of the object. An inner magic link is followed only if its text is an absolute path, otherwise `-ENOTDIR`.
- `open` is called for a non-directory node and for a final magic link; a virtual directory is opened by the vfs itself (`LKD_FD_DIR` object without a host fd, `getdents` goes through `readdir`).
- A mount is never unmounted; mounting a host mount again at the same point replaces it, a second virtual mount at the same point returns `-EBUSY`.

## 4. Comment in `fd.h` on reopening pipes

`fd.h` says that a pipe cannot be reopened through procfs. Linux allows it. The fs module does it for the same end (duplication of the host fd) and returns `-ENXIO` for the other end, which the object does not hold. Request: fix the comment.

## 5. Handler symbols in the kernel tests

`tests/test_core_dispatch.c` defines its own `lkd_sys_read`, `lkd_sys_write`, `lkd_sys_open`, `lkd_sys_ioctl` with `LKD_SYSCALL`, which CONVENTIONS §10.6 forbids as soon as a module defines these names: linking failed (duplicate symbols, MEASURED). Workaround on the fs side: all fs handlers are `static LKD_SYSCALL(...)`. Request: either declare `static` in `LKD_SYSCALL` itself (handlers are referenced only by `LKD_REGISTER` in the same file), or rename the test's handlers.

Full build (MEASURED, 2026-10-02 end of session): `cmake --build` without error, 24 test programs out of 24 pass.

## 6. Fake engine

`fake_make_task` creates neither a descriptor table nor an fsctx; the fs tests set them up themselves (`tests/test_fs_util.h`). Suggestion: create them in `fake_make_task` like `lkd_task_create_main`.


## Handled on 2026-10-02 (integration maintainer)

Points 1 and 2: done (lead). Point 3: contract copied at the top of `src/include/vfs.h`. Point 4: comment of `lkd_fdobj_reopen` fixed in `fd.h`. Point 5: `LKD_SYSCALL` is `static` (see task-1 point 5). Point 6: not done; the tests set up fdt and fsctx themselves, and creating them in `fake_make_task` would duplicate theirs.

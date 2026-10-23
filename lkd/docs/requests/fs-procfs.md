# fs -> procfs: how to serve /proc, /dev and /sys through the vfs

The interface is the one in `src/include/vfs.h`; the behavior clarifications are in `docs/requests/fs-1.md` §3. The test `tests/test_fs_vfs.c` (mount `/vproc`) serves as a complete example: `fd/N` nodes as magic links, ordinary link, loop, listing.

## Mounting

- `lkd_vfs_mount_virtual("/proc", &ops, priv)` from `lkd_procfs_init` (fs is initialized before procfs, the root already exists). Same for `/sys` and `/dev`. The fs module mounts `/dev/shm` (host directory): it is found by the longest prefix and listed under `/dev` without procfs having to know about it.
- `fstype`: "proc", "sysfs", "devtmpfs"; `statfs` derives the Linux f_type from it (0x9fa0, 0x62656572, 0x01021994).

## /proc/<pid>/fd and fdinfo

- List: `lkd_fd_list(t, fds, max)`.
- Node `fd/N`: `getattr` with `magic_link = 1`, `mode = S_IFLNK | 0700`; `readlink` returns `lkd_fs_describe(o, buf, cap)` ("/path", "/path (deleted)", "pipe:[ino]", "/memfd:name (deleted)", or the `describe` of the owner module); `open` returns `lkd_fdobj_reopen(t, o, flags, out)` (new independent description, own offset).
- `fdinfo/N`: `lkd_fs_fdinfo_basic(t, o, &pos, &flags, &mnt_id)` for the lines `pos`, `flags` (octal), `mnt_id`, then `o->ops->fdinfo` if it exists.
- `/proc/<pid>/cwd` and `root`: canonical guest path of the task; `lkd_vfs_getcwd` returns the getcwd text (with "(unreachable)" outside the root). For the root, a function does not exist in vfs.h yet; ask if needed.

## /proc/mounts and mountinfo

`lkd_vfs_render_mounts(t, mountinfo, &len)` (malloc, to free). Source of host mounts: "hostfs", type: the name of the host file system (apfs).


## Handled on 2026-10-02 (integration maintainer)

Information used by procfs (procfs tests green). Nothing to change.

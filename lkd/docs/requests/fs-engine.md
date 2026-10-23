# fs -> engine: services for the ELF loader and startup

Declared in `src/fs/lkd_fs.h` (to include with `"../fs/lkd_fs.h"` as long as `src/fs` is not in the include path, see `fs-1.md`). All tested by `tests/test_fs_glibc.c` against the Debian root.

## File object for `lkd_mm_map`

- `int lkd_fs_open_host_file(const char *guest_path, int host_fd, lkd_fdobj **out)`: from `img->host_fd` returned by `lkd_exec_prepare`. The host descriptor is duplicated (CLOEXEC), the caller's one stays the caller's. The object is a read-only ordinary file whose guest path (for `/proc/<pid>/maps`) is `guest_path`. Errors: `-EBADF` (invalid fd), `-EACCES` (not an ordinary file, like execve). Release with `lkd_fdobj_put`. Callable from any thread.
- `int lkd_fs_open_guest_file(lkd_task *t, const char *guest_path, lkd_fdobj **out)`: same thing through a guest path resolved by the vfs of `t` (symbolic links, guest root).
- `o->ops->mmap_fd(o)` gives the host descriptor to map; `lkd_fs_mmap_check(o, prot, flags)` does the `do_mmap` checks that depend on the file.

## Standard descriptors

- `int lkd_fs_install_stdio(lkd_task *t)`: duplicates host fds 0, 1, 2 and installs them as guest fds 0, 1, 2. A tty becomes a "stream" object (TCGETS, TIOCGWINSZ translated), an inherited pipe or socket gets `F_SETNOSIGPIPE`.
- `int lkd_fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *guest_path, lkd_fdobj **out)` for any other host descriptor (takes ownership).

## What `lkd_init` changes in the host process (through `lkd_fs_init`)

- Host `RLIMIT_NOFILE` raised to `min(rlim_max, kern.maxfilesperproc)` (MEASURED: 138240 on this Mac).
- Host `umask(0)`: LKD applies the guest umask itself at each creation.
- Host directories for guest `/tmp` and `/dev/shm`: `$LKD_HOST_TMP`, `$LKD_HOST_SHM`, otherwise `<DARWIN_USER_TEMP_DIR>/lkd-<uid>/{tmp,shm}` (mode 01777). They are mounted only if `lkd_config.guest_root` is provided.

## Request to the engine

- Ignore `SIGPIPE` in the host process (or at least never let it kill the runtime). LKD sets `F_SETNOSIGPIPE` on its pipes and on the pipes and sockets it wraps, but a host descriptor that the engine keeps for itself is not covered.
- The guest path passed to `lkd_fs_open_host_file` must be the canonical path (`lkd_exec_image.guest_path`), it is the text of `/proc/<pid>/maps`.

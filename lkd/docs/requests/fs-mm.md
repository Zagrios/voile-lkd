# fs -> mm: file mappings and memfd seals

## What fs provides

- `o->ops->mmap_fd(o)`: mappable host fd (ordinary files, memfd), absent for pipes, directories, O_PATH and virtual files.
- `lkd_fs_mmap_check(o, prot, flags)` (in `src/fs/lkd_fs.h`): `do_mmap` checks specific to the file, to call before mapping: `-EBADF` (O_PATH), `-ENODEV` (nothing to map), `-EACCES` (not open for reading; MAP_SHARED|PROT_WRITE without O_RDWR or with O_APPEND), `-EPERM` (MAP_SHARED|PROT_WRITE of a memfd sealed with F_SEAL_WRITE or F_SEAL_FUTURE_WRITE). Tested in `tests/test_fs_memfd.c`.
- `lkd_fs_memfd_seals(o)`: seals of a memfd, -1 otherwise.

## What fs expects from mm

1. `lkd_mm_has_writable_shared_mapping(mm, file)`: return 1 if there is in `mm` a MAP_SHARED mapping of this file that can become writable, that is, like Linux's `i_mmap_writable`, any shared mapping made from an O_RDWR description (VM_SHARED with VM_MAYWRITE), even if it is read-only at that moment. Compare the object or, better, the inode (`o->dev`, `o->ino`), because a reopen through /proc creates another object for the same memfd. fs calls the function through the pointer `lkd_fs_wsm_query` (the tests replace it).
2. After F_SEAL_WRITE or F_SEAL_FUTURE_WRITE: refuse `mprotect(PROT_WRITE)` of a shared mapping of this memfd (`-EACCES`, VM_MAYWRITE removed by `seal_check_write`).
3. Limit to document on the mm side: the query sees only the caller's address space. A shared writable mapping made by another process of the family (after fork) is not seen; Linux would see it. It is the only write that an F_SEAL_WRITE seal cannot block here.


## Handled on 2026-10-02 (integration maintainer)

Point 1: already implemented by mm (`lkd_mm_has_writable_shared_mapping`, tests `test_mm_file`). Point 2: `mprotect(PROT_WRITE)` of a shared mapping of a sealed memfd (F_SEAL_WRITE or F_SEAL_FUTURE_WRITE) returns EACCES (`src/mm/mm.c`, weak `lkd_fs_memfd_seals` in core). Test: `test_fs_memfd` case `sealed_memfd_mprotect`. Point 3: limit kept.

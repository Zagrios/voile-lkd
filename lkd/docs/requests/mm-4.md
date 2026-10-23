# Request mm-4 (task, ptrace, procfs, engine): mm contracts to know

1. References: `lkd_mm_task_exit(t)` (exit hook) gives back the reference of `t->mm` and sets `t->mm` to NULL. task must not do `lkd_mm_put(t->mm)` itself. A single live address space per host process: a second `lkd_mm_create()` returns NULL with errno EBUSY as long as the first one exists.
2. fork: without CLONE_VM, `lkd_mm_clone(parent, flags)` returns `parent` with a reference (the child's copy), `lkd_mm_fork_child` re-identifies it. The locks taken by `lkd_mm_fork_prepare` are released in the child by the thread that forked (reinitializing a held rwlock fails with EBUSY on macOS: MEASURED, child blocked in `test_mm_fork` before the fix).
3. ptrace and procfs: `lkd_mm_remote_read/write` follow the FOLL_FORCE rule of PEEK/POKE and of /proc/<pid>/mem (write into read-only code, read of PROT_NONE) and invalidate the target's translated code before answering. `process_vm_readv/writev` do not use FOLL_FORCE. The transport is one service thread per process (AF_UNIX socket `$TMPDIR/lkdmm.<host pid>.sock`), started for every production address space; the guest pid is translated by `lkd_proc_lookup().host_pid`.
4. Engine: guest memory is never executable on the host side (mm sets only r and w). If the engine protects pages to detect self-modifying code, it must not call `mprotect` itself on the window: mm recomputes the host protection of a 16 KiB page at each mmap, mprotect, munmap and will overwrite the engine's. Open question for the lead: add a "page watched by the JIT" counter in mm, which mm would respect (remove w as long as it is set).
5. The engine classifies SEGV_MAPERR / SEGV_ACCERR with `lkd_mm_find(mm, addr, 0, &vi)`.


## Handled on 2026-10-02 (integration maintainer)

Information. Point 4: settled by mm's watched pages (`src/mm/watch.c`).

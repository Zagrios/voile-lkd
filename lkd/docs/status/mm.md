# Status of the mm module (LKD)

Date: 2026-10-02. Milestone 4 (the last of this session): the module covers its whole priority list. 8 test programs pass, for 563 assertions.

## What exists

`src/mm/` (3705 lines):

- `vma.c`: VMA table as an AVL tree sorted by start address. Lookup, insertion and removal in O(log n), about 100 bytes per entry, maximum gap per subtree for the hole search (like Linux's augmented tree). Replaces HyperLinux's sorted array (linear memmove, 4144 bytes per entry).
- `window.c`: host window. `mm_window_reserve` (`window.c:36`) first tests the wanted base with `mach_vm_allocate(VM_FLAGS_FIXED)`, which fails instead of overwriting. Then it tries 8 TiB, 16 TiB, 32 TiB, 0x7d80000000, then anywhere aligned on 1 GiB, and sets `lkd_guest_base`.
- `lkd_mm_find_nowait` (`mm.c`): `lkd_mm_find` without waiting, -EBUSY if the VMA table is being modified. For fexdarwin's fatal fault report, written from a signal handler whose thread may hold that lock. `find_nowait` case of `test_mm_map`.
- `mm.c`: address spaces; `mm_map_locked` (`mm.c:440`), `mm_unmap_locked` (`mm.c:344`), `mm_host_cleanup` (`mm.c:380`); Linux's top-down hole search with hint, `MAP_32BIT`, `MAP_ABOVE4G`, 2 MiB THP alignment for anonymous memory (`mm.c:612`); strong `lkd_mm_check_range` (`mm.c:681`); exact `/proc/<pid>/maps` (`mm.c:848`, `mm.c:878`); smaps, stats, futex keys; fork hooks (`mm.c:1092`), exec hooks and thread-exit hooks.
- `sys.c`: mmap (error order of `do_mmap`, `sys.c:101`), munmap, mprotect, pkey_mprotect, pkey_alloc, pkey_free, map_shadow_stack, brk (`sys.c:318`), madvise, mincore (`sys.c:524`), msync, mlock, mlock2, munlock, mlockall, munlockall.
- `remap.c`: mremap (MAYMOVE, FIXED, DONTUNMAP, growth in place, move through `mach_vm_remap`, `remap.c:133`).
- `shm.c`: shmget, shmat, shmdt, shmctl (IPC_STAT, IPC_SET, IPC_RMID, IPC_INFO, SHM_INFO, SHM_STAT, SHM_STAT_ANY, SHM_LOCK, SHM_UNLOCK). The segments are `shm_open` objects; the table is shared by fork.
- `remote.c`: memory service between processes (one thread per process, AF_UNIX socket); `lkd_mm_remote_read/write/render_maps` with the FOLL_FORCE rule and invalidation of the target's code; process_vm_readv/writev (`remote.c:320`).
- `lkd_mm_ext.h`: entries not in `mm.h` yet (see `docs/requests/mm-2.md`). `NOTICE`: one block imported from HyperLinux (rendering of a maps line).

Memory model (READ, `mm_priv.h`):

- I1: a 16 KiB host page without a VMA is a fresh reservation (PROT_NONE, zero).
- I2: its protection is the union of its subpages. It is readable if one subpage is accessible, writable if one subpage is, never executable.
- I3: a host page of a shared mapping (anonymous MAP_SHARED, SysV, shared file) belongs only to that mapping.

Private files: a whole host page is mapped directly (host copy-on-write) if its file offset is aligned on 16 KiB and if it starts before the end of the file. The other pages are copied through `pread`. The hole search prefers an address congruent to the offset modulo 16 KiB, to allow the direct mapping.

## MAP_SHARED smaller than a host page (request from the project lead, first Wine launch)

Triggering case: the Wine client maps wineserver's 4 KiB memfd (KUSER_SHARED_DATA) as `MAP_FIXED|MAP_SHARED|PROT_READ` over its private 4 KiB reservation at 0x7ffe0000; the server updates the page every few ms, it must be really shared. Before: EINVAL. Now: succeeds.

What is handled (MEASURED, `test_mm_shpart` 85 assertions, `test_mm_window` case `wine_kuser_shared_data_production_window`):

- A MAP_SHARED VMA (file, memfd, anonymous, SysV) can cover 4, 8, 12 KiB or any 4 KiB size. The 16 KiB host page or pages it touches are mapped from the shared object (`MAP_SHARED`, `MAP_FIXED`, file offset = VMA offset minus its position in the host page). The guest pages next to it in the host page (the "siblings") alias the bytes of the object at that place.
- Address condition: the address and the file offset must have the same position within a 16 KiB page (`(address - offset) % 16 KiB == 0`). A non-fixed mapping picks such an address itself (4 KiB at offset 4 KiB lands on an address ≡ 0x1000 mod 16 KiB). A FIXED mapping at an address aligned on 16 KiB with offset 4 KiB, or with an offset smaller than the position in the page, returns EINVAL with a visible message. Shared anonymous memory has no offset: any 4 KiB address works.
- Safety rule at creation: the siblings must be unmapped, or private PROT_NONE, or other pieces of the SAME object at the same relative position. Otherwise EINVAL with a message ("siblings must be unmapped or PROT_NONE"). Another object in the same host page: EINVAL.
- Rule afterwards: an accessible private `mmap` in a sibling returns EINVAL; an `mprotect` that would make a sibling accessible returns ENOMEM, with a visible message. Nothing is ever written into the object's bytes from the siblings (filling and zeroing of private pages skip the shared host pages).
- The 4 KiB view of the VMA stays exact: protections are per VMA, the host page takes the union (a read-only shared page with PROT_NONE siblings is PROT_READ on the host side). The kernel copies (`lkd_user_*`) and the engine's classification through `lkd_mm_find` see the exact view (EFAULT on a PROT_NONE sibling).
- munmap / mprotect of a piece: the VMA is split; a host page whose last shared VMA is gone but which keeps PROT_NONE siblings becomes fresh private memory again (zeros): the object does not stay visible behind them, and a later `mprotect` of those siblings succeeds and reads zeros. The reservations around stay private PROT_NONE.
- fork: the child keeps the mapping as a real shared host mapping (test: the child writes through a second mapping, the parent sees it; the sibling rules stay valid in the child). mremap of a shared piece: a move is possible if the position in the host page is kept.

Limit to know (cannot be fixed by mm): the exact view is not enforced by the hardware. An access by GUEST CODE to a PROT_NONE sibling will not fault, because the host page is readable (host protection = union): it reads the object's bytes. The engine only raises SEGV_ACCERR if the host faults. Accesses by the LKD kernel, on the other hand, are refused exactly. No accessible private access can alias the object, so nothing is written by mistake.

## Code pages watched by the JIT (`src/mm/watch.c`, `src/mm/lkd_mm_watch.h`)

API (standalone, usable from C++ by the engine):

- `int lkd_mm_watch_code(uint64_t gva, uint64_t len)`: every 16 KiB host page touching the range loses PROT_WRITE, even if the guest VMA is writable. Idempotent, lock-free when everything is already watched. One bit per host page, in a `MAP_NORESERVE` bitmap (a window of 2^46 bytes costs 512 MiB of address space, only the touched words are allocated).
- `int lkd_mm_code_write_fault(uint64_t gva)`: called by the engine's SIGSEGV/SIGBUS handler for a faulting store. If the page is watched and the VMA allows writing: `ops->invalidate_code(page, 16 KiB)` once, the watch is lifted, the page becomes writable again, return 1 (replay the store). Otherwise 0 (real guest fault). A return of 1 on a page already made writable again by another thread is bounded to 4 consecutive answers per thread and per page, then 0: a store that fails for another reason does not loop.
- `int lkd_mm_code_watched(uint64_t gva)` for the tests.

Important MEASURED fact for the engine: a store to a NON-RESIDENT read-only page gives a TRANSLATION fault (ESR 0x92000046, DFSC 0x06), not a permission fault (0x0f). The engine's handler must not filter on DFSC: the criterion is "data abort (EC 0x24) with WnR = 1". A test case covers it (`store_to_nonresident_watched_page`).

Watch lock: recursive mutex, taken before the VMA read lock, never held during guest code, during `ops->invalidate_code` nor during an `lkd_user_*` copy. It protects the consistency between the bits and the host protections (`mm_host_refresh`, mm's temporary write windows, the two API functions). The signal handler therefore takes a mutex: acceptable because the interrupted code is guest code or `lkd_uc_copy`, never mm code. `ops->invalidate_code` is called from that handler and must not take any lock that guest code could hold.

Consistency:

- `mprotect` recomputes the protections without giving write access back to watched pages. `mremap`: the moved range is not watched at its new address (the code translated at the old one is invalidated). `munmap` and the exec reset lift the watch on the freed host pages; a partial `munmap` keeps the watch on a page still used by another VMA. Fork: the child clears all the bits and gives the pages back their protection (it inherits no translation), the parent keeps its own.
- Kernel copies: `lkd_mm_check_range` (so `lkd_user_write` and all the copies) lifts the watch and invalidates before the copy. `lkd_mm_remote_write` and `process_vm_writev` do the same. MADV_POPULATE_WRITE does not rewrite a watched page. MADV_DONTNEED on a watched page keeps the protection.
- MAP_SHARED and SysV: same rule within this process. Limit: a write by ANOTHER process into a shared page translated here is not seen (no fault in this process).

Fault recovery of the copies (core): without a fix, a copy that faults on a watched page after the check (another thread watches it again between the check and the copy) returns -EFAULT. MEASURED in the scratchpad: `lkd_user_write` returns -14 without the fix, 0 with one invalidation with the fix. Request and exact diff: `docs/requests/mm-5.md`.

Tests: `test_mm_watch` 104 assertions (store that faults then is replayed, single invalidation with the right range, `lkd_user_write` and `process_vm_writev` and forced POKE, host page shared with a data VMA, mprotect that keeps the watch, munmap/mremap/exec that lift it, MAP_SHARED, fork, non-resident page, bounded retries).

## Watch lock in the child of a fork (reported by the sig agent)

A hang in `lkd_mm_fork_child -> mm_host_refresh -> mm_watch_lock` came from an intermediate version of `watch.c`: the child did `mm_watch_unlock()` on the recursive mutex taken by `fork_prepare`, and macOS refuses that unlock from the child's thread (MEASURED: `test_mm_fork` hung). The lock is now rebuilt by `mm_watch_fork_child()`, and this call is the very first operation of `lkd_mm_fork_child`. The new case `fork_watch_lock_in_child` of `test_mm_fork` (35 assertions in total) forks three times with a watched page, uses the lock in the child (mmap, watch, mprotect, madvise, munmap), then forks from a secondary thread; each child has a 10 s timeout and the test a 90 s timeout. `test_task_bigwin`: 525 ms, passes. If an older liblkd.a binary still hangs, it must be rebuilt.

## task-4 fix: walking the window in O(VMA) (2026-10-02, evening)

- Cause (READ, request `docs/requests/task-4.md`): `mm_host_cleanup` advanced one 16 KiB host page at a time. Over the whole production window (2^46 bytes), that makes 2^32 lookups in the tree, at every execve (`lkd_mm_reset_for_exec`) and at every process end (`mm_destroy`).
- Fix: `mm_host_cleanup` jumps to the page of the next VMA and does a single zeroing `mmap` per empty interval. The same defect existed elsewhere and was fixed:
  - `mm_host_refresh` now walks per VMA and per range, with a single `mprotect` for a gap or for the inside of a VMA;
  - the fill loop of `mm_map_locked` now only handles the edge host pages and those that carry file data;
  - MADV_DONTNEED, WIPEONFORK and the old interval of mremap DONTUNMAP go through `mm_discard_range`, which does a single zeroing for the inside;
  - the residency for smaps and stats is read per host region (`mach_vm_region`) for a VMA of more than 256 MiB;
  - the watch bits of an empty interval are cleared by whole bitmap pages.
  The other remaining loops do not go beyond the size requested by the call (mincore, populate, copies).
- `lkd_mm_window_size()` is defined.
- MEASURED: `test_task_bigwin` (task module) passes in 521 ms in a private build without net, against 20,114 ms according to task-4. The new case `whole_window_operations_are_fast` of `test_mm_window` runs under 2 s: reservation of 1 TiB PROT_NONE, smaps, stats, DONTNEED of 1 TiB, munmap of the whole window, exec reset, destruction. The whole program runs in 0.85 s.

## What passes (MEASURED)

Same results in the private core + mm build (scratchpad script) and in the common build (`./build.sh -k 0`, complete liblkd.a with the other modules).

| Program | Assertions | Content |
| --- | --- | --- |
| `test_mm_vma` | 32 | tree: lookups, gaps, 20,000 random operations compared against a reference array |
| `test_mm_map` | 144 | mmap/munmap/mprotect on real memory, host accesses probed by write/read on a pipe, real SIGBUS/SIGSEGV fault in a child, MAP_FIXED, NOREPLACE, hint, MAP_32BIT, THP, anonymous MAP_SHARED shared with a child, JIT invalidation |
| `test_mm_file` | 70 | real host file, through `lkd_mm_map` (ELF loader path) and through the syscall: direct mapping, 4 KiB offsets, ld.so segment pattern, copy-on-write, MAP_SHARED write visible in the file (msync), EACCES/ENODEV/EOPNOTSUPP rules, "(deleted)" |
| `test_mm_misc` | 220 | brk, mremap, madvise, mincore, msync, mlock*, high-water marks, RSS and its cache, Vm* counters against a walk, pkeys, exact maps format (column 73, trailing space), futex keys |
| `test_mm_fork` | 23 | real fork through `lkd_fork_host`: private copy, MAP_SHARED sharing, DONTFORK, WIPEONFORK, memory locks not inherited; reset at exec |
| `test_mm_shm` | 71 | SysV: creation, attach by another process, nattch, deferred IPC_RMID, permissions, IPC_INFO, SHM_STAT |
| `test_mm_remote` | 45 | process_vm_readv/writev (partial counts, errors), forced POKE into read-only code with invalidation, service between two real host processes |
| `test_mm_watch` | 104 | watched code pages (see above) |
| `test_mm_window` | 37 | production window of 2^46 at 8 TiB, operations on the whole window in less than 2 s: guest addresses 0x10000 and 0x7ffe0000, EPERM below 0x10000, search below 2^46 - 128 MiB, service started then removed, fallback to 16 TiB when 8 TiB is taken |

Defects found by the tests and fixed:

- `discard` zeroed the whole host page for a MADV_DONTNEED of a single subpage.
- The child of a fork stayed stuck, because macOS refuses to reinitialize a held rwlock (EBUSY). The child now releases the locks.

Complete suite (MEASURED, `tools/run_tests.sh`): the remaining failures are `test_fs_pipe`, `test_sig_basic` and `test_task_time` (signal delivery, other modules), as well as the link of `test_core_dispatch` (duplicate symbols between core and fs). None of them touches mm.

## Syscalls registered by mm

brk mmap munmap mprotect pkey_mprotect pkey_alloc pkey_free map_shadow_stack mremap madvise mincore msync mlock mlock2 munlock mlockall munlockall shmget shmat shmdt shmctl process_vm_readv process_vm_writev.

Not registered (ENOSYS visible and counted): process_madvise, membarrier, mbind, set_mempolicy, get_mempolicy, migrate_pages, move_pages, set_mempolicy_home_node, mseal, remap_file_pages.

## Known limits (documented)

1. 4 KiB subpages in a 16 KiB host page: a PROT_NONE or read-only subpage next to an accessible subpage is not protected against guest code, because the host applies the union. The kernel copies (`lkd_user_*`) respect the exact view. Consequence (INFERRED): a 4 KiB guard page only faults if it covers a whole host page. Affected: glibc's thread stack (AT_PAGESZ 4096) and Wine's guards.
2. MAP_SHARED (file, SysV) requires the address and the offset to have the same position within a 16 KiB page, otherwise EINVAL; an accessible private mapping never shares a host page with a shared mapping (see the previous section). An anonymous or SysV shared mapping does not grow through mremap beyond its object (ENOMEM).
3. Copied private files: the copied pages do not see writes made to the file afterwards, while Linux shows them as long as the page has not been written. Pages entirely beyond the end of the file read zeros instead of raising SIGBUS.
4. MAP_GROWSDOWN returns EINVAL (no stack extension). No ASLR: mmap_base is fixed (top of the window minus 128 MiB). RLIMIT_AS does not apply to mmap, RLIMIT_DATA only applies to brk.
5. MAP_POPULATE does not prefault. MADV_WILLNEED, COLD, PAGEOUT and FREE do nothing, which their semantics allow. MADV_REMOVE on a shared file returns EOPNOTSUPP, MADV_COLLAPSE returns EINVAL. KSM and THP are only flags. In smaps, Pss equals Rss and the residency is known per host page. SHM_LOCK is only a flag, shm_rss is 0.
6. mlock wires the host pages (`mlock`). A refusal by the host returns EAGAIN or ENOMEM, never a fake success.
7. The SysV table does not survive a host re-exec (`docs/requests/mm-3.md`). nattch leaks if a process dies without going through the exit hooks.
8. With a window of 2^46, the guest TASK_SIZE is smaller than Linux's (`docs/requests/mm-2.md` point 3).
9. Error paths without a test: host `mmap` failure during a direct mapping, `mach_vm_remap` failure in mremap.
10. Cost of the service between processes (one connection per request) not measured.

## Next

Measure the cost of mmap/munmap and of the remote service. Apply RLIMIT_AS. Add membarrier (Wine uses it, with a fallback) and a one-node NUMA policy. Handle the pages watched by the JIT (question of `docs/requests/mm-4.md` point 4).

## memfd phase and aliased code (2026-10-02, 22:55, request wine-7)

`mm_backing.hbias`: a memfd can have a host phase (host offset = file offset + phase), set by its first mmap, so that a MAP_SHARED at an address not congruent to the offset modulo 16 KiB is possible. The JIT code watches cover all the views of the same shared object in the process (`mm_alias_pages`). Details, measurements and limits: `docs/requests/wine-7.md`.

## Cost of VmRSS and of the Vm* counters (2026-10-03, rewritten 2026-10-23)

`lkd_mm_stats` walks nothing:
- VmSize, VmData, VmExe and VmStk come from counters the VMA tree keeps at every insertion, removal and bound change, and around a change of prot or label in place (`vt_account`), as Linux keeps them (`vm_stat_account`). The VMA holding `start_stack` moves to VmStk at read time. VmLck is `locked_bytes`. `vt_check` compares the counters with a walk.
- RSS is the host process's resident memory, `task_info(TASK_VM_INFO)`: `internal` as RssAnon, the rest as RssFile, RssShmem 0. It is cached for 100 ms (`MM_RSS_TTL_NS`) as long as no mapping is added or removed, so `stat` and `status` read back to back agree.

Accuracy: RSS includes the engine's own memory (FEX's code cache, LKD), a few hundred MiB for a big game, and shared anonymous memory counts as anonymous or file by its host backing. It can lag 100 ms behind pages touched or freed without a mapping change. VmHWM is sampled at these reads and before every unmap. smaps still gives the guest residency of each VMA from the host regions (`mach_vm_region`, mincore for the edge pages).

Why: the RSS of 2026-10-03 was a walk of the VMAs, one `mach_vm_region` per host region and mincore for the edge pages, cached the same way. Dead by Daylight reads `/proc/self/status` every frame (`GetProcessMemoryInfo`) and maps or unmaps memory all the time, so the cache never served and every frame paid the walk: 35 to 40 fps instead of 60 (`docs/journal/02-performance.md`, October 23). Every munmap also walked the VMAs for VmPeak.

MEASURED (witness `w20_statbench`: 1000 small VMAs and 256 MiB touched, real engine, 2026-10-03, with the walk):

| Operation | Before | After |
| --- | --- | --- |
| read of `/proc/self/status` | 4.45 ms | 24 to 26 us |
| full walk, without cache | 4.5 ms | 1.26 ms |

MEASURED (native test, 40,000 host regions, 2026-10-23): a `mach_vm_region` walk 19.5 ms, one `task_info(TASK_VM_INFO)` 0.48 us.

Tests: `test_mm_misc` cases `high_water_marks`, `rss_counting_and_cache` and `vm_counters_follow_the_tree`.

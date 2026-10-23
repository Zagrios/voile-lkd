# wine-7: MAP_SHARED of a 4 KiB memfd at an address not congruent modulo 16 KiB -> EINVAL; the game's EAC module loops forever

Requester: wine-linux agent. Blocking: RocketLeague.exe stops before kernel32 initialization (no window, no D3D device).

MEASURED (evidence/eac-runs/20261002T223742, LKD_TRACE=1, guest.trimmed.log; the loop produced 49 million lines, 3.4 GB, truncated):
- Game process (guest pid [71], right after ntdll was loaded at 0x6fffffd20000, while kernel32 is being mapped): loop of
  `memfd_create(name, MFD_CLOEXEC)`; `ftruncate(fd, 0x1000)`; `mmap(addr, 0x1000, PROT_READ|PROT_EXEC, MAP_SHARED|MAP_FIXED_NOREPLACE, fd, 0)` for addr = 0x6fffffd17000, 0x6fffffd27000, ... 0x6fffffdb7000 (step of 64 KiB, ntdll - 0x9000 + k*64 KiB); `close(fd)`.
  All addresses return EEXIST (occupied by ntdll and its neighbors) except 0x6fffffd17000, the only free one, which returns EINVAL: `lkd: mm: MAP_SHARED at 0x6fffffd17000 offset 0x0: address and offset are not congruent modulo the 16 KiB host page`. 6970 loop rounds in 100 s, without end.
- This code is not Wine (Wine never calls memfd_create with a fixed executable mmap near ntdll): INFERRED it is the Linux code of the EAC module in the game process ("Starting Wine module mapping" from the bootstrapper, then 301). Under Linux with 4 KiB pages this mmap succeeds; we must not change what EAC does, so it is up to mm to accept it.
- The memfd is new and is 4 KiB; the host page 0x6fffffd14000..0x6fffffd18000 has no other occupant according to the EEXISTs (to check in the VMA table).
Request: accept MAP_SHARED of an object whose (address - offset) is not a multiple of 16 KiB, at least for a memfd created by the guest. INFERRED lead: LKD creates the memfds itself; it can place the content of the host object with an internal offset chosen at the first mmap (or reallocate the host object when it is not mapped anywhere yet and its size is < 16 KiB), so that the host page contains the object at the right position; the other pages of the host page stay inaccessible (PROT_NONE) according to the current neighbor rule.

## Handled on 2026-10-02, 22:55 (integration maintainer)

### 1. Phase of a memfd (`src/fs/memfd.c`, `src/mm/sys.c`, `src/mm/mm.c`, `src/mm/remap.c`)

The host file of a memfd can store byte x of the guest file at host offset x + B, B being 0, 4, 8 or 12 KiB (the "phase").
- When the phase is set: at the first mmap of the memfd (any process of the family, any description). This mmap passes all its checks first, then the phase is set, and the content is moved if B is not 0. It no longer changes after that.
- Value chosen: for a fixed MAP_SHARED, B = (address - offset) mod 16 KiB. For any other view, the phase already set, or 0.
- Storage: bits 16 to 18 of the seals word, which is shared by fork and by reopens. F_GET_SEALS does not show them.
- All fs operations add B: read and write (own offset or host offset), pread and pwrite, readv and writev (RWF_APPEND included), sendfile, splice, copy_file_range, ftruncate, fallocate, lseek (SEEK_END, DATA, HOLE), fstat, and the F_SEAL_GROW limit.
- On the mm side: `mm_backing.hbias` applies to the congruence check, to the shared host mapping, to the direct mapping and to the `pread` copies of private views, to mremap, and to the address search (a non-fixed view gets an address congruent to offset + B).
- `/proc/<pid>/maps` shows the guest offset.
- process_vm goes through guest memory, it is not touched.

What stays refused, visibly (message once in the log):
- A fixed MAP_SHARED view that asks for another phase returns EINVAL.
- An ordinary host file cannot be shifted: the existing congruence rule applies, with EINVAL.

Raw uses of the host descriptor: `lkd_fs_pin_raw` sets the phase to 0, or refuses with EOPNOTSUPP if it is already something else. It is called in three cases:
- SCM_RIGHTS (`src/net/scm.c`);
- exec by descriptor (`src/task/exec.c`);
- `/proc/<pid>/fd/N` reopen served to another process (`src/procfs/remote.c`).

Considered but not done:
- Shared anonymous memory: it has no offset, any 4 KiB address already works.
- SysV segments: shmat at a non-congruent 4 KiB address still returns EINVAL. A `shm_open` object can be neither read nor written under macOS, so moving the content would require mapping it; not done for lack of a known user.

Gap: another process that reads or writes the memfd during the first mmap that sets a non-zero phase can see the content while it is being moved.

MEASURED: new case `phased_shared_mapping` of `test_fs_memfd`. It covers:
- EAC's sequence (RX, MAP_SHARED|MAP_FIXED_NOREPLACE, 0x3000 into the host page, offset 0) on a memfd that already had content;
- fstat, lseek and pread unchanged for the guest;
- an RW view chosen by mm at an address congruent to 0x3000, with shared writes in both directions and with pwrite;
- EINVAL for another phase, and the same phase accepted at another offset;
- a private view that reads the right bytes;
- EOPNOTSUPP from `lkd_fs_pin_raw`.
The first mmap of this case returned EINVAL before (message quoted in the request).

### 2. Neighbors

READ in `evidence/eac-runs/20261002T223742/guest.trimmed.log`:
- The addresses tried in the loop (0x6fffe4b57000, 0x700017f77000, 0x6fffed4b7000…, step of 64 KiB, always 0x3000 into the host page) are all free: MAP_FIXED_NOREPLACE did not return EEXIST.
- The EINVAL came only from the congruence.
- In the first phase of the loop (0x7f8275fb8000…, congruent positions), the addresses were occupied: EEXIST, as under Linux.

INFERRED: in the 16 KiB pages of these candidates, the three other 4 KiB pages are unmapped. The neighbor rule is therefore satisfied and the first try now succeeds. This is not checked on a real launch: the process's VMA table is not in the truncated log.

Accessible private neighbor: refused, and there is no exact solution. A 16 KiB host page has a single host mapping: either the shared object, or private memory.
- The neighbors of a shared view therefore necessarily read bytes of the shared object.
- With a phase B, these are the padding bytes [0, B) of the host file, themselves shared with any other view of this host page (other process, fork child, other view of the same process).
- Putting private data there would make it seen and modified by these views: this is the forbidden aliasing.

The visible EINVAL stays the answer.

### 3. Aliased code (`src/mm/watch.c`)

A JIT code watch now covers all the host pages of the process that map the same bytes of a shared object (`mm_alias_pages`).
- They lose PROT_WRITE together.
- A write fault on one of them releases them all and invalidates the range of each.
- A view created afterwards inherits the watch (`mm_watch_alias_sync`).
- A write through the descriptor (write, pwrite, kio) also invalidates the watched views (`lkd_mm_file_written`).

Gap: the views of ANOTHER process of the family are not covered (as before for MAP_SHARED).

MEASURED:
- `test_mm_watch` case `aliased_views_share_the_watch`: fails without the alias code (view A was not watched), passes with it.
- Guest witness `witnesses/elf-x86_64/src/w19_memfd_alias.c` (`EXEC=1 ./build.sh src/w19_memfd_alias.c`), under `build-integration/host/fexdarwin-run`, default SMC mtrack mode. It does: RX view at 0x3000 into its host page, offset 0; RW view chosen by the kernel; code written by A, executed by B, rewritten by A (2 times, then 100 rounds), then pwrite through the descriptor.
  - With the fixed library: PASS 8/8.
  - With the alias code disabled: FAILED 5/8 (B still executes the old code).

Non-regression:
- 41/41 programs, 6154 assertions.
- Witnesses w01, w05, w06_smc, w10, w17, w18: exit 0.
- `wine cmd /c echo hi`: prints hi, code 0.

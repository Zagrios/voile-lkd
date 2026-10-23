# Reuse of the HyperLinux core by an in-process FEXCore engine

Date: 2026-10-02. Status: complete version of the first half of the study. It covers guest memory, VMAs, the vfs, procfs, fd objects, process identity and ABI conversion. Threads, fork/exec, signals, futex, ptrace and networking are in `reports/hl-reuse-process.md`.

Conventions. READ: read in the code, with file:line. MEASURED: obtained by running a program or a test on this machine, with the command or the log cited. INFERRED: deduction not checked by execution. Path abbreviations:

- HL: `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/hyper-linux/src`, working copy, HEAD 8048a96.
- FEX: `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/probes/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038/Source/Tools/LinuxEmulation/LinuxSyscalls`, intact tree.
- EXP: `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-wine-fex-darwin-20261002`.
- XP: `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`, previous experiment folder, read-only.

Machine: Apple M5 Max, macOS 27.0.1, 16 KiB host pages (`getpagesize()` = 16384, MEASURED, appendix A).

## Summary

1. READ. HL is an ARM64 Linux kernel written around HVF guest memory. A single host buffer `host_base` of 64 GiB or 1 TiB, guest virtual address = guest physical address = offset in this buffer, level 1 page tables built by the host, 4 KiB guest pages on a 16 KiB host (`guest.h:6-15`, `guest.c:65-66`).
2. MEASURED (`wc -l`, `nm -u`). 84 files, 40,617 lines. 25 of the 39 compiled objects have no unresolved HVF symbol; the HVF API used fits in 20 functions; the file, I/O, time and procfs handlers only need five guest memory access functions. Reused almost as is: 3,835 lines (9%). To adapt, ABI and memory access: 9,082 (22%). To rewrite: 6,197 (15%). To drop: 7,563 (19%). Process layer, covered in the other report: 13,940 (34%).
3. MEASURED (appendix C). HL's real objects, linked with a fifteen-line shim that replaces the five guest memory accessors with the identity, run in an ordinary macOS process: `openat` through the rooted vfs, `read`, `write`, `fstat`, `getdents64`, symbolic links resolved inside the guest root, `/proc/self/maps` rendered from the VMA table with 4 KiB splits, reopening `/proc/self/fd/N` of a deleted file with an independent cursor. 23 checks out of 23. No HVF VM is created.
4. The three things that the guest MMU gives for free do not exist in a process: per-page protection at 4 KiB, low guest addresses, fault on a removed page. MEASURED (appendix A): on this machine `mmap`, `munmap` and `mprotect` round to 16 KiB, refuse an address not aligned on 16 KiB (EINVAL) and refuse a file offset of 0x1000 (EINVAL). The FEXCore team additionally measures that a fixed `mmap` at 0x400000 fails (errno 12) and that `__PAGEZERO` cannot be shrunk.
5. The `guest_vma_t` table and the rendering of `/proc/self/maps` are pure C and can be reused as a 4 KiB guest view. The structure must be redone: 4,144 bytes per entry, linear `memmove`, 16,384 entries at most. The 2 MiB backing blocks, the VA aliases, the overflow segments and the kernel buffer are to be thrown away.
6. ABI. HL speaks the generic ARM64 ABI. Moving to x86-64 changes the numbers, `stat`, the `O_*` flags, `epoll_event`, `clone`, the signals and about thirty legacy calls. `dirent64`, `statfs`, `rlimit`, `flock`, `sockaddr`, `msghdr`, `timespec` have the same layout. FEX sends 241 x86-64 calls as is to the host kernel, HL implements only 128 of these 241 (MEASURED by comparing names).
7. Recommendation: a new x86-64 syscall layer written for FEXCore, with selective import from HL (vfs, fd, procfs, task names, flag and errno translations) and from FEX (`x64` structures, VMA and self-modifying code tracking, protected memory access). Do not compile FEX's `LinuxEmulation` on top of an "ARM64 kernel" library.
8. Gaps relative to Wine and EAC: memfd seals missing, `/proc/<pid>/maps` of other processes, `/proc/self/task` and `/proc/self/fd` as directories, `/proc/self/mem`, `/proc/self/environ`, `mremap`, `process_vm_readv`, `userfaultfd` missing. `madvise`, `mincore`, `msync`, `mlock` are stubs. See section 5.
9. INFERRED estimate: 8,000 to 11,000 lines written or modified, for about 10,500 lines reused almost without change.
10. Five risks: the 4 KiB granularity on 16 KiB, the low addresses, the real semantics of `munmap`/`mprotect`/`mremap` in a process, the fidelity of `/proc` and memfd for EAC, memory sharing between processes without a Linux kernel.

## 0. Tree read, private fixes, test bench

### 0.1 State of the tree

READ (`git status --short`, `git diff --stat`, read-only): 33 tracked files modified (4,475 insertions, 1,560 deletions) and 11 untracked source files, that is 44 useful paths. Untracked: `fault_dump.c/h`, `proc_names.c/h`, `proc_ptrace.c/h`, `signal_notify.h`, `syscall_filemap.c/h`, `syscall_reopen.c/h`. HEAD: 8048a96 "docs: link the public examples sibling".

`README.md` and `CLAUDE.md` are not modified and lag behind the code. The README announces "~26,000 lines" and "MAP_SHARED treated as MAP_PRIVATE"; `src/` counts 40,617 lines and handles file MAP_SHARED with a real host mapping (`syscall.c:837-856`). The code is authoritative.

### 0.2 What the private fixes changed, and why

The lines touched are insertions plus deletions from `git diff --stat`. The reasons come from the code comments, the names of the added tests and `XP/README.md`; I did not replay them.

| Subsystem | Files (lines touched) | Reason |
| --- | --- | --- |
| Table of exact guest mappings at 4 KiB, separate from the backing blocks | `guest.c` (562), `guest.h` (73), `syscall.c` (608), `proc_emulation.c` (716), test `test-page-table-pool.c` (240) | EAC's resolver looks for a `/libc-` or `libc.so` line in `/proc/self/maps`. The old tracking showed anonymous 2 MiB blocks (`XP/README.md`, section "EAC reading the memory map"). |
| Shared file mappings on a 16 KiB aligned IPA | `syscall_filemap.c/h` (new, 466), `syscall_shm.c` (17), `guest.c` | `syscall_filemap.c:1-3`: the L3 entries choose 4 KiB pages to leave the neighboring private pages intact on a 16 KiB host. |
| Reopening `/proc/self/fd/N` with an independent file description | `syscall_reopen.c/h` (new, 361), `proc_emulation.c`, `syscall_fs.c` (45), `syscall_io.c` (415) | Reopening by `dup` shared the position left at end of file; reading the 832-byte ELF header of the memfd returned zero (`XP/README.md`, same section). |
| Task registry shared within the fork family | `proc_names.c/h` (new, 539), `syscall_proc.c` (991), `proc_emulation.c` | `/proc/<tid>/comm`, `status`, `cmdline`, `exe` of another process; common PID allocator (`syscall_proc.c:76-113`). |
| Path resolution in the guest namespace | `vfs.c` (334), `syscall_fs.c` (45), test `test-vfs-unit.c` (136) | A Wine prefix contains absolute links that must restart from the guest root. Tests: "absolute interior link crosses guest root", "dotdot is evaluated after following an interior link", "forty-one interior links return ELOOP". |
| Bottom of the guest space given back to the guest | `hl.c` (60), `shim.S` (22), `stack.c` (8), `vdso.c` (7), `guest.c`, test `test-guard-page.c` (160) | EL1 shim and page table pool moved to the top of the buffer (`guest.c:83-89`, `guest.c:171-176`). Diff comment: guest MAP_FIXED can use the bottom of the space "as Wine does for its DOS area". |
| x86 ptrace bridge, signals, threads, fork | `proc_ptrace.c/h` (new, 922), `fork_ipc.c` (457), `syscall_signal.c` (444), `thread.c` (392), `fault_dump.c/h`, `signal_notify.h` | Out of scope here (`XP/STATUS.md`, `hl-reuse-process.md`). |
| Small ABI fixes | `stack.c` (real UID/GID in the auxv), `syscall_fs.c` (`fcntl` lock types converted to the macOS values) | Read in `git diff`. |

### 0.3 Test bench reused

MEASURED. The previous attempt copied HL into `EXP/scratch-hl/hyper-linux-copy` (sha256 manifest identical before and after, `EXP/evidence/hl-source-manifest-before.sha256` and `-after.sha256`). The script `EXP/tools/build-hl-scratch.sh` compiles the 39 C sources with the upstream Makefile's flags: `EXP/logs/hl-compile.log` is empty (no warning, no error), 39 objects, `_build/hl` linked (488,944 bytes, unsigned, not launched). I created no HVF VM and I launched no guest.

The eight pure host tests of `EXP/tools/build-run-host-tests.sh` were rerun during this session from the scratch binaries, output sent to the session's temporary directory: `test-fd-object-lite`, `test-vfs-unit`, `test-app-open`, `test-oss-abi`, `test-fd-digest`, `test-aot-format`, `test-thread-vcpu-lifetime`, `test-page-table-pool`, all code 0 and final line `PASS`. The original logs are in `EXP/logs/host-test-*.log`, their contents agree. The test `host-build-test-thread-vcpu-lifetime-asis.log` fails at link time (`_proc_trace_tracer_exit` undefined) when built like the upstream Makefile does: the working copy is not consistent with its own host test, worked around in the scratch by `scratch-hl/stub-trace.c`.

## 1. Architecture of the core as implemented

### 1.1 Guest memory

READ. A single `mmap(MAP_ANON)` of 64 GiB (36-bit IPA) or 1 TiB (40-bit IPA) serves as guest physical memory, mapped in one block as RWX in stage 2 (`guest.c:293-439`, `hv_vm_map` at 392-393). `GUEST_IPA_BASE` is 0 (`guest.h:95`): guest address = guest physical address = offset in the buffer. For COW fork, the buffer is replaced by a `mkstemp` file mapped `MAP_SHARED|MAP_FIXED` (`guest.c:361-380`) and the child maps it `MAP_PRIVATE` (`guest.c:441-501`).

The guest MMU is the vCPU's: level 1 page tables written by the host in a 16 MiB pool at the end of the buffer (`guest.h:36`, `guest.c:171-176`), 2 MiB block descriptors by default (`guest_build_page_tables`, `guest.c:1588-1689`), split into 512 L3 descriptors of 4 KiB when the permissions differ (`split_l2_block`, `guest.c:2007`, `guest_update_perms`, `guest.c:2314`, `guest_map_pages`, `guest.c:2071`). HVF enforces W^X, hence two arenas: RX at 0x10000000 and RW at 0x200000000 (`guest.h:41-61`, `syscall.c:997-1048`). The EL1 shim and the EL1 stack pages are 32 MiB below the end of the buffer (`guest.c:83-89`).

4 KiB guest on 16 KiB host. `PAGE_SIZE` is redefined to 4096 in `guest.c:65-66` and the auxv announces `AT_PAGESZ` = 4096 (`stack.c:257`). The permissions the guest sees are those of the 4 KiB PTEs; the host kernel sees only a single large RWX mapping. The host page size only comes into play for external memory plugged in by `hv_vm_map`: the IPA and the size must be multiples of 16 KiB (`syscall_filemap.c:220-224`).

Translation of a guest pointer (`gva_resolve`, `guest.c:821-878`), in order: external resolvers (SysV SHM and file mappings, 8 at most, `guest.c:795-819`); fast path `host_base + gva`; Rosetta's kernel buffer window (`KBUF_VA_BASE`, `guest.h:76-88`); rejection of non-canonical space; page table walk (`guest_walk_pt`, `guest.c:748-787`); overflow segments (`guest_overflow_resolve`, `guest.c:590-597`). `guest_read`, `guest_write` and `guest_read_str` split the copy into page segments (`guest.c:888-948`). MEASURED (`grep -o`, function calls only): 269 calls to `guest_ptr`, `guest_ptr_avail`, `guest_read`, `guest_write`, `guest_read_str` or `guest_gpa_ptr` in 20 files outside `guest.c`; 57 in `syscall_fs.c`, 40 in `syscall_net.c`, 26 in `syscall_io.c`, 25 in `audio_oss.c`, 18 in `syscall_proc.c`, 17 in `syscall.c`. The handlers access guest memory only through these functions, which makes a replacement by protected copies possible without touching the call sites. The direct references to `host_base` or `gva_resolve` (grep lines) are in the loading and memory code: `hl.c` 9, `syscall.c` 8 (mmap, brk, munmap), `syscall_exec.c` 6, `vdso.c` 5, and 3 each in `fork_ipc.c`, `rosetta.c`, `syscall_filemap.c`, `syscall_shm.c`. No file, network or I/O handler contains any.

Zeroing and release. An anonymous `mmap` does a `memset` of the buffer (`syscall.c:1103-1105`, `syscall.c:972-973`) because `munmap` gives nothing back to the host: it zeroes the non-PROT_NONE regions, removes the region and leaves the PTEs of the bottom of the space valid (`syscall.c:1385-1515`, author's comment `syscall.c:1458-1470`). An access after `munmap` therefore reads zeros instead of raising SIGSEGV in the bottom of the space. `mprotect(PROT_NONE)` invalidates the PTEs (`guest_invalidate_ptes`, `syscall.c:1593-1596`).

Files. A `MAP_PRIVATE` file `mmap` is a `pread` into the buffer (`syscall.c:1112-1117`, `syscall.c:974-982`). Only file `MAP_SHARED` does a real host `mmap`, plugged in by `hv_vm_map` in an IPA window between `mmap_limit + 2 Gio` and `interp_base`, then selected page by page by PTEs (`syscall_filemap.c:210-298`). A `MAP_SHARED|MAP_ANONYMOUS` is treated as private (`syscall.c:837`, `syscall.c:1120-1121`).

### 1.2 VMAs and host-side tracking

Three independent structures in `guest_t` (`guest.h:259-264`):

1. `regions[]` (`guest_region_t`, 4096 sorted entries, 112 bytes). Allocator of backing blocks in physical space: `find_free_gap` (`syscall.c:519-570`), merging of anonymous neighbors (`regions_mergeable`, `guest.c:1247`). Also feeds `VmSize` of `/proc/self/status` and the zero-fill of `guest_reset`. Specific to the physical model.
2. `va_ranges[]` (4096 entries). Exact reservations of high VAs, created for Rosetta's mappings at 128 TiB (`guest_track_high_mapping`, `guest.c:130-146`).
3. `vmas` (`guest_vma_t`, dynamic array, 16,384 entries at most). Exact mappings visible to the guest: start, end, offset, inode, dev, permissions, flags, 4096-byte path (`guest.h:203-211`, 4,144 bytes per entry). Operations `guest_vma_replace`, `guest_vma_remove`, `guest_vma_protect` with splitting, and `vma_merge` of compatible neighbors (`guest.c:1107-1205`). Each successful `mmap` calls `record_mmap_vma` (`syscall.c:1163-1179`), `munmap` calls `guest_vma_remove` (`syscall.c:1511`), `mprotect` calls `guest_vma_protect` (`syscall.c:1635`).

Other tables: `va_aliases[16]`, `overflow[4]` of 1 GiB, `preannounced[16]`, `pt_free_list[256]`, 256 backings and 2,048 file views in `syscall_filemap.h:7-8`. These fixed caps come from the physical model.

### 1.3 vfs and guest root

READ, `vfs.c` (751 lines) and `vfs.h`:

- 64-entry mount table `guest_prefix` to `host_path`, flags RO, NOEXEC, NOSUID, VIRTUAL (`vfs.h:17-39`). The longest prefix wins (`find_mount`, `vfs.c:264-282`). Two modes: `legacy` (no rewriting) and `rooted` (`vfs.h:21-24`).
- Virtual CWD, never a host `chdir` in rooted mode (`vfs.c:646-670`). Reverse map host to guest (`hl_vfs_host_to_guest`, `vfs.c:610-644`). Synthetic root and mount ancestors (`vfs.c:293-302`, `vfs.c:362-415`).
- The resolver advances component by component. An interior symbolic link is resolved in the guest namespace (an absolute link restarts from guest `/`), with 40 links at most and ELOOP (`vfs.c:511-608`). Names that are too long give ENAMETOOLONG instead of being truncated (`vfs.c:284-291`). Confinement canonicalizes the parent with `realpath` (`vfs.c:315-353`) and `sys_openat` then opens under the mount root with `O_RESOLVE_BENEATH` (`syscall_fs.c:485-594`, flag defined in the macOS SDK, `sys/fcntl.h:128`).
- The sysroot is a separate field (`vfs.h:47`) with a basename fallback for Nix paths (`resolve_sysroot_path`, `syscall_fs.c:107-134`). The experiment's launcher mounts the Debian root read-only on `/`, then `prefix`, `tmp`, `home`, a host `proc` directory on `/proc` and `/run/user/UID` (`XP/run-linux.py`).
- `/dev` and `/proc`: `sys_openat` first tries the device registry, the procfd reopen and `proc_intercept_open` on the path string, before any resolution (`syscall_fs.c:451-473`). `hl_vfs_apply_app_profile` additionally declares `/dev` and `/proc` as VIRTUAL mounts (`vfs.c:732-733`).
- No HVF call, no guest pointer. Only dependency: `syscall.h` for the `LINUX_*` constants and `fd_table` (`vfs.c:438-446`), and `syscall.h` includes `Hypervisor.h` (`syscall.h:9`), a header dependency.
- Not checked: APFS is case-insensitive by default and the resolver does not fold case. Wine can do its own case-insensitive lookup, but the sysroot is on the user's volume; to measure.

### 1.4 fd objects

READ. Three levels:

- `fd_table[1024]` (`syscall.h:601`, `syscall.c:82`), entry `{type, host_fd, linux_flags, dir, of, desc}` (`syscall.h:623-630`). Allocation of the lowest free number by bitmap (`syscall.c:88-96`, `syscall.c:149-171`). Procfd files are allocated from 128 (`syscall_fs.c:466`), which departs from the Linux lowest-free-descriptor rule.
- `hl_descriptor_t` per descriptor number: counter, `host_fd`, CLOEXEC flags, `removed` mark (`fd_object.h:111-119`).
- `hl_open_file_t` per open description: counter, kind, operations table, atomic `status_flags`, state, `mount_id`, `guest_path_hint` (`fd_object.h:98-107`). `hl_fd_ops_t` table: `read, write, readv, writev, ioctl, lseek, fstat, fcntl_getfl/setfl, poll_host_fd, destroy, fork_export, fork_import` (`fd_object.h:72-96`). `hl_fd_get` pins the description for the duration of a call (`fd_object.h:121-128`). `dup`, `dup3` and `F_DUPFD` share the description (`fd_object.c:339-440`).

Kinds: stdio, file, directory, pipe, socket, epoll, timerfd, eventfd, signalfd, inotify, virtual directory, OSS dsp and mixer, device (`syscall.h:603-614`). Pipes and sockets are host descriptors. Emulations: eventfd by pipe and counter, timerfd by `kqueue` EVFILT_TIMER, signalfd by synthetic reads (`syscall_fd.c:1-8`), epoll by `kqueue` (`syscall_poll.c:1-8`), inotify by `kqueue` EVFILT_VNODE (`syscall_inotify.c:1-20`). Their state is indexed by the guest number, hence `fd_special_subsystem_close` for every path that frees a number (`syscall_fs.c:649-657`).

memfd: `mkstemp`, `unlink`, `fd_alloc(FD_REGULAR)`, only `MFD_CLOEXEC` is read, the name is ignored (`syscall.c:2385-2398`). No sealing: `grep -i seal` finds nothing in `src/`. Procfd reopen: `syscall_reopen.c` creates an independent description whose cursor lives in a shared, locked state file (`reopen_shared_t`, `syscall_reopen.c:20-30`), exportable at fork through `hl_reopen_export_state_fd` (`syscall_reopen.h:16-20`).

Locks: `fd_lock` of rank 3 after `mmap_lock` and `pt_lock` (`syscall_internal.h:10-38`).

### 1.5 Process identity

READ.

- The root process has PID 1 and PPID 0 (`proc_init`, `syscall_proc.c:355-361`). The TID of the main thread is the PID. `gettid` returns `current_thread->guest_tid` (`syscall.c:1362-1364`), `getpgid` returns the PID (`syscall.c:1823-1825`), `getuid` and `getgid` return the host's (`syscall.c:1665-1676`).
- PID and TID come from the same counter. It lives in a `MAP_SHARED` page of a deleted `mkstemp` file, initialized to 2, incremented by `compare_exchange`, cap `GUEST_PID_MAX` = 0x3fffffff (below the futex owner bit), without wraparound (`syscall_proc.c:76-113`, `syscall_proc.c:516-529`). The descriptor is exported to the child at fork (`fork_ipc.c:1847`) and imported before any guest thread (`fork_ipc.c:539`, `syscall_proc.c:538-562`). The three forms of clone call `proc_alloc_pid` (`fork_ipc.c:1295`, `1573`, `2614`).
- A fork child receives `proc_set_identity(child_pid, parent_pid)` (`fork_ipc.c:530`, `1219`). The PID does not change at exec; the allocator and the registry survive (`proc_names.h:15-17`).
- The `proc_names` registry is a shared file with POSIX locks, header version 4, one record per task `{tid, tgid, ppid, comm[16]}`, an exec snapshot (absolute path and `cmdline` bytes), the tracing relation and the zombie state (`proc_names.h:8-60`, `proc_names.c:15-60`). It serves `/proc/<tid>/{comm,status,cmdline,exe}` of another process (`proc_emulation.c:84-183`, `402-446`).
- Mapping between guest PID and host PID: `proc_table[64]` `{host_pid, guest_pid, exited, exit_status}` (`syscall_proc.h:99-106`), so 64 live children at most per process.
- `prctl(PR_SET_NAME)` writes into the registry (`syscall.c:1834-1848`). `uname` announces `Linux`, node `hl`, version `6.8.0-101-generic`, machine `aarch64` (`syscall_sys.c:28-45`).

### 1.6 Coupling measured by unresolved symbols

MEASURED. `nm -u` on the 39 objects of `EXP/scratch-hl/hyper-linux-copy/_build/obj` (compiled without execution, see 0.3). This measurement replaces the impression given by mentions in the text.

- 25 objects out of 39 have no unresolved `_hv_*` symbol: `app_open`, `audio`, `audio_coreaudio`, `audio_oss`, `device`, `elf`, `fd_object`, `futex`, `proc_emulation`, `proc_names`, `proc_ptrace`, `stack`, `syscall_fd`, `syscall_fs`, `syscall_inotify`, `syscall_io`, `syscall_net`, `syscall_poll`, `syscall_reopen`, `syscall_stats`, `syscall_sys`, `syscall_time`, `trace`, `vdso`, `vfs`.
- The 14 others call HVF: `guest` (9 functions), `thread` (8), `gdb_stub` (7), `syscall_proc` (7), `syscall_signal` (6), `fork_ipc` (5), `hl` (4), `syscall_exec` (4), `syscall` (3), `crash_report`, `fault_dump`, `syscall_filemap`, `syscall_shm` (2 each), `rosetta` (1). The HVF API used fits in 20 distinct functions: `hv_vcpu_get_sys_reg`, `hv_vcpu_get_reg`, `hv_vcpu_set_sys_reg`, `hv_vcpu_set_reg`, `hv_vm_map`, `hv_vm_unmap`, `hv_vcpu_get/set_simd_fp_reg`, `hv_vcpus_exit`, `hv_vcpu_create`, `hv_vcpu_destroy`, `hv_vcpu_run`, `hv_vm_create`, `hv_vm_destroy`, `hv_vm_config_*` (4), `hv_vcpu_set_vtimer_mask`, `hv_vcpu_set_trap_debug_exceptions`.
- Seven objects have no unresolved symbol from HVF, guest memory (`guest_*`), `proc_*`, `thread_*` or `signal_*`: `vfs`, `fd_object`, `device`, `proc_names`, `syscall_reopen`, `trace`, `syscall_stats`. They are the safest to reuse. `syscall_time` stays classed as "reuse": it only needs two guest memory accesses and `signal_get_itimer` with `signal_set_itimer`.
- The objects of the "adapt" class (`syscall_fs`, `syscall_io`, `syscall_fd`, `syscall_sys`, `syscall_time`, `proc_emulation`, `stack`, `syscall_net`, `syscall_poll`, `syscall_inotify`) need, on the guest memory side, only five functions: `guest_write` (10 objects), `guest_read` (9), `guest_read_str` (2), `guest_ptr_avail` (2), `guest_ptr` (2). This is the minimal interface of the in-process shim.
- Their dependencies on the process layer are short. `syscall_fs`: `proc_get_sysroot`, `proc_set_sysroot` and the five entries of `proc_emulation`. `syscall_io`: `proc_get_hl_path`, `proc_get_pid`, `signal_pending`, `signal_queue`, `thread_host_io_begin`, `thread_host_io_end`. `proc_emulation`: `current_thread`, `proc_get_cmdline`, `proc_get_elf_path`, `proc_get_pid`, `proc_get_ppid`, `proc_names_*`, `thread_active_count`. `syscall_fd`: `proc_get_pid`, `signal_consume`, `signal_get_state`.
- The rest of `guest.c` is used by 56 symbols in total outside of it: 5 for data access, then the regions (`guest_region_add` in 6 objects, `guest_region_remove` 3, `guest_region_set_prot`), the PTEs (`guest_invalidate_ptes` 4, `guest_update_perms` 3, `guest_map_pages` 2, `guest_extend_page_tables`, `guest_clear_pages`), the high mappings (`guest_map_va_range` 2, `guest_track_high_mapping`), the shim (`guest_shim_base` 2, `guest_shim_data_base` 3), `kbuf` (4), the VMA (`guest_vma_reserve` 2, `guest_vma_replace`, `guest_vma_remove`, `guest_vma_protect`) and the entry points `guest_init`, `guest_init_from_shm`, `guest_destroy`, `guest_reset`, `guest_get_used_regions`. Among these groups, only the VMA is to be kept; everything else is the physical memory model.

### 1.7 In-process witness: HL's handlers run without HVF (MEASURED)

To check the "five functions are enough" thesis by execution and not by reading, I linked the real objects of the scratch (all except `hl.o` and `guest.o`) with a fifteen-line shim that replaces `guest_ptr`, `guest_ptr_avail`, `guest_read`, `guest_write` and `guest_read_str` with the identity and `memcpy` (appendix C). The unresolved HVF symbols are never called (`-Wl,-undefined,dynamic_lookup`). Three C drivers, compiled in the session's temporary directory, call the `sys_*` functions directly with real host pointers as "guest addresses":

- Driver 1, 12 checks passed: mount of the temporary directory on `/data` in rooted mode, `sys_openat(O_CREAT|O_RDWR)`, `sys_write`, `sys_lseek`, `sys_read`, `sys_fstat` (128-byte `linux_stat_t` structure), `sys_close`, `sys_getdents64`, host symbolic link with absolute target `/data/hello.txt` resolved inside the guest root, `ENOENT` returned as `-2`.
- Driver 2, 5 checks passed: the real functions `guest_vma_replace`, `guest_vma_protect` and `guest_vma_remove` (extracted from `guest.c:1107-1205` and compiled alone), then `sys_openat("/proc/self/maps")` and `sys_read`. The rendering shows the exact 4 KiB splits and the host path converted back through the bind:
  `7f0000001000-7f0000002000 r-xp 00000000 00:00 0   /usr/lib/x86_64-linux-gnu/libc.so.6` then `7f0000002000-7f0000003000 r--p 00001000 …`.
- Driver 3, 6 checks passed: the EAC bootstrapper path. Write to a file, `unlink`, then `sys_openat("/proc/self/fd/A")`: the reopened description reads the contents from offset 0 while the original descriptor keeps its cursor at end of file, and a second reopen with `O_RDWR` has its own cursor at 0.

Constraints found during this witness: `sys_read` and `sys_write` require a registered `current_thread` (`thread_host_io_begin` returns EAGAIN without it, `thread.c:100-106`); driver 1 first returned `-11` for this reason. This is a dependency on the process layer: interruption of blocking I/O relies on a private per-thread signal. Limits: the ABI tested is ARM64 (128-byte `linux_stat_t`), the three drivers cover neither `mmap`, nor dispatch, nor sockets, and no guest code runs.

## 2. Per-file table

LOC: `wc -l` on the working copy. Coupling column: concrete symbols. Verdict: reuse (as is or almost), adapt, rewrite, drop.

| File | LOC | Subsystem | HVF, EL1, Rosetta, physical memory coupling | Assumed guest ABI | Verdict |
| --- | --- | --- | --- | --- | --- |
| `guest.c` | 2402 | guest memory, stage 1, regions, VMA | `hv_vm_create`, `hv_vm_config_*`, `hv_vm_map`, `hv_vm_unmap`, `hv_vm_destroy`, `hv_vcpu_destroy`, `guest_walk_pt`, `ttbr0`, `ttbr1`, `PT_*`, `MEM_PERM_EL1`, `need_tlbi`, `kbuf_*`, `va_aliases`, `overflow`, `is_rosetta` | neutral for the VMA (l.1107-1235), ARM64 stage 1 elsewhere | rewrite; extract 130 lines of VMA (l.1107-1205) |
| `guest.h` | 516 | memory structures | `Hypervisor/Hypervisor.h`, `hv_vcpu_t`, `hv_vcpu_exit_t` in `guest_t`, constants `ROSETTA_*`, `KBUF_*` | `guest_vma_t` neutral | rewrite; keep `guest_vma_t` after shrinking `name[4096]` |
| `syscall.c` | 2512 | fd table, errno, flags, mmap and brk, dispatch | 14 `hv_*`, `hv_vcpu_get_reg(HV_REG_X0-X8)` (l.1249-1255), `hv_vcpu_set_reg(X8)` for the TLBI (l.2502-2507), `g->regions`, `kbuf`, `ttbr` | generic ARM64, X8 and X0-X5 | split: l.70-494 adapt, l.495-1180 memory to rewrite, l.1181-2512 dispatch to reclassify as x86-64 |
| `syscall.h` | 645 | numbers, constants, `linux_*` structures | `Hypervisor.h`, `hv_vcpu_t` in `syscall_dispatch` | ARM64: numbers l.17-200, `O_*` l.352-356, `linux_stat_t` l.402 | adapt (numbers and `stat`) |
| `syscall_internal.h` | 132 | lock ordering, externs | none | neutral | reuse |
| `syscall_fs.c` | 2397 | stat, open, close, directories, fcntl, xattr, dirfd | 58 `guest_*` calls, `F_GETPATH`, `O_RESOLVE_BENEATH`, `fdopendir` | ARM64: `linux_stat_t` (l.59-77), `O_*` | adapt |
| `syscall_fs.h` | 108 | declarations | none | neutral | reuse |
| `syscall_io.c` | 1836 | read, write, tty ioctl, splice, sendfile | 29 `guest_*`; l.638-1224: `rosettad` server, AOT cache, `posix_spawn` | ARM64 (termios and ioctl identical to x86-64) | adapt 1,249 lines, drop 587 |
| `syscall_io.h` | 63 | declarations | `rosetta.h` | neutral | adapt |
| `syscall_fd.c` | 814 | eventfd, timerfd, signalfd | `kqueue`, `signal_*` | `signalfd_siginfo` | adapt |
| `syscall_fd.h` | 55 | declarations | none | neutral | reuse |
| `syscall_sys.c` | 301 | uname, getrandom, sysinfo, prlimit64, rusage | `mach/mach.h`, `sysctl` | `machine` = `aarch64` (l.37) | adapt |
| `syscall_time.c` | 299 | clocks, nanosleep, itimer | `translate_clockid` | `timespec` identical | reuse |
| `syscall_stats.c` | 439 | call profiling | none | ARM64 numbers | reuse, optional |
| `vfs.c` | 751 | mounts, virtual CWD, resolver | `fd_table` (through `syscall.h`) | neutral | reuse |
| `vfs.h` | 124 | vfs API | none | neutral | reuse |
| `fd_object.c` | 492 | descriptions and descriptors | none | neutral | reuse |
| `fd_object.h` | 195 | fd model | `guest_t` as a forward declaration | neutral | reuse |
| `device.c` | 226 | /dev registry | `audio_oss.h` | neutral | reuse; remove OSS |
| `proc_emulation.c` | 1078 | synthetic /proc, /dev, /etc | `guest_t` (`vmas`, `regions`), `thread_active_count`, `sysctl`, ARM cpuinfo (l.501-518) | ARM cpuinfo, rest neutral | adapt |
| `proc_names.c` | 479 | shared task registry | POSIX locks | neutral | reuse |
| `syscall_reopen.c` | 324 | independent procfd description | `mmap` of a state file | neutral | reuse |
| `syscall_filemap.c` | 426 | file `MAP_SHARED` | `hv_vm_map`, `hv_vm_unmap`, `guest_map_pages`, `guest_set_extra_resolve`, `g->host_base`, `vm_page_size` | 4 KiB on 16 KiB | rewrite (about 250 lines) |
| `syscall_shm.c` | 915 | SysV SHM | `hv_vm_map`, `guest_map_va_range`, IPA stolen in stage 2 | same | rewrite (about 300 lines) |
| `syscall_shm.h`, `syscall_filemap.h`, `syscall_reopen.h`, `proc_emulation.h`, `proc_names.h` | 50, 40, 37, 35, 60 | declarations | `guest.h` | neutral | follow the source file |
| `elf.c` | 331 | ELF loader | `elf_map_segments(guest_base, guest_size, load_base)` copies by `fread` into the buffer | x86-64 and ARM64 | rewrite; use FEX's loader |
| `elf.h` | 145 | ELF structures | none | neutral | reuse |
| `stack.c` | 296 | initial stack and auxv | `guest_write`; `AT_PLATFORM` `aarch64` (l.173-176), ARM HWCAP (l.56-130), `AT_PAGESZ` 4096 (l.257) | ARM64 | adapt or use FEX's loader |
| `stack.h` | 53 | stack API | none | neutral | reuse |
| `vdso.c` | 507 | ARM64 vDSO and [vvar] page | `host_base`, fixed pages 0xE000 and 0xF000, publication every 1 ms | ARM64 | drop; FEX's `VDSO_Emulation.cpp` |
| `vdso.h`, `vdso_guest.S` | 69, 110 | same | same | ARM64 | drop |
| `hl.c` | 1463 | CLI, VM, default profile | 25 `hv_*`, 55 Rosetta mentions, `shim_blob` | ARM64 and x86-64 through Rosetta | drop |
| `shim.S` | 656 | EL1 shim | exception vectors, HVC, MMU, DC ZVA | ARM64 EL1 | drop |
| `hv_util.h` | 121 | HVF macros | `HV_CHECK`, `SCTLR_*` | ARM64 EL1 | drop |
| `rosetta.c`, `rosetta.h`, `aot_format.h`, `fd_digest.h` | 301, 136, 49, 50 | Rosetta and AOT cache | `ROSETTA_PATH`, `guest_add_va_alias` | ARM64 | drop |
| `gdb_stub.c`, `gdb_stub.h` | 1732, 78 | GDB stub | `hv_vcpu_get_reg` | ARM64 | drop |
| `crash_report.c`, `crash_report.h`, `fault_dump.c`, `fault_dump.h` | 195, 37, 162, 8 | diagnostics | `hv_vcpu_get_reg` | ARM64 | drop; redo in 100 lines |
| `trace.c`, `trace.h` | 209, 68 | per-category traces | none | neutral | reuse |
| `audio.c`, `audio.h`, `audio_oss.c`, `audio_oss.h`, `audio_coreaudio.c`, `linux_oss_abi.h`, `app_open.c`, `app_open.h` | 639, 129, 701, 27, 255, 150, 60, 27 | OSS, Core Audio, application opening | Core Audio, `guest_*` (25 calls in `audio_oss.c`) | OSS | drop for Wine and EAC |
| `thread.h` (detail, because read for this study) | 226 | guest thread table | `hv_vcpu_t vcpu`, `hv_vcpu_exit_t *vexit`, `sp_el1`, `linux_user_pt_regs_t`, `linux_user_fpsimd_state_t`, `gdb_reg_snapshot[788]`, `_Thread_local current_thread` | ARM64 registers | rewrite; keep only `guest_tid`, `clear_child_tid`, signal mask and alternate stack; `proc_emulation.c` reads `current_thread` and `thread_active_count`, `syscall_io.c` requires `thread_host_io_begin` (see 1.7) |
| Process layer: `thread.*`, `futex.*`, `syscall_proc.*`, `syscall_signal.*`, `syscall_exec.*`, `fork_ipc.*`, `proc_ptrace.*`, `signal_notify.h`, `syscall_net.*`, `syscall_poll.*`, `syscall_inotify.*` | 13,940 in total | threads, fork, exec, signals, ptrace, networking, polling | 72 `hv_*` in `fork_ipc.c`, 87 in `syscall_proc.c`, 34 in `syscall_signal.c`, 30 in `thread.c` | ARM64 | see `hl-reuse-process.md`; only the PID allocator (`syscall_proc.c:76-113`, `516-562`, about 90 lines) is reused here |

Totals per class, MEASURED by summing `wc -l`: reuse 3,835 lines (`vfs`, `fd_object`, `syscall_reopen`, `proc_names`, `device`, `trace`, `syscall_stats`, `syscall_time` and their headers); adapt 9,082 (`syscall_fs`, `syscall_io` except `rosettad`, `syscall_fd`, `syscall_sys`, `proc_emulation`, `syscall.c` except memory, `syscall.h`, `syscall_internal.h`, `stack`); rewrite 6,197 (`guest`, memory part of `syscall.c`, `syscall_filemap`, `syscall_shm`, `elf`, `vdso`); drop 7,563; process 13,940. Sum 40,617.

## 3. VMA logic independent of HVF, reusable with guest address = host address on 16 KiB

### 3.1 What can be reused

READ, without any Mach or HVF call:

- `guest_vma_t` and its operations: `guest_vma_replace` (removal of overlaps, sorted insertion, merging), `guest_vma_remove` (split in two), `guest_vma_protect` (split at the bounds), merge rules of `vma_merge` (same permissions, flags, inode, dev, name, contiguous offset for a file), offset adjustment at the split of `vma_trim_start` (`guest.c:1107-1205`). The rules are exact at 4 KiB: `& ~4095` everywhere.
- `record_mmap_vma` (`syscall.c:1163-1179`): name through `fcntl(F_GETPATH)`, inode and dev through `fstat`, dev split into major and minor.
- `proc_maps_fd` (`proc_emulation.c:222-288`): sorting, name column at 73, guest path through `hl_vfs_host_to_guest`, escaping of the newline. It only reads `nvmas`, `vmas`, `preannounced` and `verbose` from `guest_t`.
- The test `test-page-table-pool.c` contains useful oracles for the splits ("exact shared/private pages, protection restore, replacement and clear").

Structural defects not to reuse. Each entry carries 4,096 bytes of path (4,144 bytes), the array grows to 16,384 entries, that is about 65 MiB, and each insertion or removal does a `memmove` of the whole suffix (`guest.c:1127-1131`). Opening `/proc/self/maps` copies the whole array before sorting it (`proc_emulation.c:224-228`). An ordered tree and an interned path are needed.

FEX already has 4 KiB tracking: `VMATracking` (`FEX/SyscallsVMATracking.h:102`, 594 + 171 lines), a `std::map` indexed by address, with permissions, sharing, offset and `MRID` resource (dev, inode); it serves self-modifying code and the code cache. Two differences from HL. The file name is kept only for an executable regular file (`FEX/SyscallsSMCTracking.cpp:595-609`, `S_IXUSR` test), whereas `/proc/self/maps` must name every DLL or library, including Wine's non-executable DLLs. Shared anonymous mappings have an identifier, no name (`SyscallsSMCTracking.cpp:699`). Recommendation: a single tracker, FEX's, extended with an interned path and the full dev/inode, with the `/proc/self/maps` rendering taken from HL. Keeping two trackers in parallel creates divergences at the first `MAP_FIXED` over an existing mapping.

### 3.2 What must change on a 16 KiB host with GVA = HVA

MEASURED (appendix A, M5 Max, 16 KiB pages):

| Host operation | Result |
| --- | --- |
| `mmap(MAP_FIXED)` at `base+0x1000` (length 0x1000) and at `base+0x2000` (length 0x4000) | EINVAL failure in both cases |
| `mmap(MAP_FIXED)` at `base+0x4000`, length 0x1000 | succeeds and replaces 16 KiB: the bytes from `+0x4000` to `+0x7fff` are zeroed |
| `mprotect(base+0x9000, 0x1000)` | EINVAL |
| `mprotect(base+0x8000, 0x1000, PROT_READ)` | succeeds and applies to 16 KiB (region of 0x4000 bytes, permissions 1) |
| `munmap(base+0x1000, 0x1000)` | EINVAL |
| `munmap(base+0x10000, 0x1000)` | succeeds and removes 16 KiB |
| file `mmap`, offset 0x1000, `MAP_PRIVATE` or `MAP_SHARED` | EINVAL |
| file `mmap`, offset 0x4000, length 0x4000, `MAP_SHARED` | succeeds |
| `mach_vm_protect(a, 0x1000)` with `a` aligned on 16 KiB | succeeds and protects 16 KiB (region of 0x4000 bytes). `mach_vm_protect(a+0x1000, 0x1000)` also succeeds, the address is rounded, its exact effect was not isolated |

Consequences, point by point.

- Permission granularity. The host cannot protect 4 KiB. The VMA table remains the truth of what the guest expects (error codes, `/proc/self/maps`); the host permissions are an envelope over 16 KiB. A 4 KiB guard page inside a readable and writable region cannot fault. Three paths, to decide: announce 16 KiB to the guest (`AT_PAGESZ`, `sysconf`) and run Wine with 16 KiB pages (PE sections are aligned on 4 KiB, Wine must handle it); emulate 4 KiB permissions through faults and a shadow table (cost of each fault); keep HL's stage 1, which is ruled out here. None has been tested. FEX assumes 4,096 (`FEXCore/include/FEXCore/Utils/TypeDefines.h:6-7`).
- Backing blocks. `regions[]`, `va_ranges[]`, the overflow, the aliases and `kbuf` disappear. `find_free_gap` is replaced by the host's address choice (`mmap(NULL, …)` in the reserved window) or by a lookup in the VMA table. The FEXCore team reserves the guest window as `PROT_NONE` (`reports/fexcore-darwin-port.md`, decision 3).
- File mappings at an offset not aligned on 16 KiB. HL absorbs them: copy by `pread` for `MAP_PRIVATE`, `delta` in an aligned IPA for `MAP_SHARED` (`syscall_filemap.c:220-224`). In a process, a `MAP_PRIVATE` becomes anonymous plus `pread` (as in `syscall.c:1112-1117`), with the cost in resident memory and the loss of cache sharing; the VMA keeps the name and the offset. A `MAP_SHARED` is only possible if the address and the offset are both aligned on 16 KiB and congruent; otherwise refuse (EINVAL) or accept an incoherent copy, to decide. Wine's views follow the 64 KiB allocation granularity (INFERRED from Wine's design, not checked here).
- Mappings shared between processes and fork. HL has no shared anonymous memory (`MAP_SHARED|MAP_ANONYMOUS` becomes private, `syscall.c:837`). For files, it exports through SCM_RIGHTS the backing descriptors and the (va, length, offset) triplets and reimports them in the child (`syscall_filemap.c:334-426`): this 90-line scheme can be reused as is for a fork that is not a real `fork()`. With a real host `fork()` the sharing is native, but the safety of `fork()` in a multithreaded process is the process team's concern.
- `MAP_FIXED` over an existing mapping. HL removes the regions, then rewrites the PTEs (`syscall.c:938-993`). In a process, `mmap(MAP_FIXED)` replaces atomically on the kernel side, but the VMA table must be updated and the JIT translations invalidated under the same lock (model: `FEX/SyscallsSMCTracking.cpp:302-350`, `GuestMmap`). `MAP_FIXED_NOREPLACE` does not exist in the macOS SDK (grep of `sys/mman.h`): the overlap test goes through the VMA table, like `guest_mapping_overlaps` (`guest.c:103-119`).
- `mremap`. Absent from HL (ENOSYS through the default case, `syscall.c:2451-2462`) and from the macOS SDK. FEX calls `::mremap` then tracks (`FEX/SyscallsSMCTracking.cpp:390-412`). To emulate: shrinking by `munmap` of the tail; growing in place if the VMA table shows a hole, by `mmap(MAP_FIXED)`; moving by `mach_vm_remap` at 16 KiB then updating the table and invalidating the JIT. glibc falls back to malloc, copy, free when `mremap` fails (INFERRED from memory of `realloc`'s code).
- `munmap` and `mprotect`. In a process they must be exact: real `munmap`, real `PROT_NONE`. The guest will at last observe a SIGSEGV after `munmap`, which HL does not produce in the bottom of the space.
- Zeroing. HL does a `memset` at each anonymous `mmap`; a fresh host `mmap` is already zeroed. But `madvise(MADV_DONTNEED)` does not zero the page on Darwin (measurement by the FEXCore team, `fexcore-darwin-port.md`), whereas it does under Linux for private anonymous memory.
- Self-modifying code. FEX's tracking protects guest code by `mprotect(PROT_READ)` in 4 KiB units (`SyscallsSMCTracking.cpp:136-196`, `FEX_PAGE_SIZE`). At 16 KiB it will over-invalidate, which is safe (decision 2 of `fexcore-darwin-port.md`).

## 4. ABI question: generic ARM64 Linux versus x86-64 Linux

### 4.1 How HL represents arguments and structures

READ. `syscall_dispatch` reads X8 and X0-X5 from the vCPU through `hv_vcpu_get_reg` (`syscall.c:1249-1255`), a `switch` on the ARM64 number calls `sys_*(guest_t *g, …)` functions that receive guest addresses as `uint64_t` and read them through `guest_read` and `guest_write`, and the result goes back in X0 (`syscall.c:2495`). The structures are copies of the ARM64 kernel structures: 128-byte `linux_stat_t` (`syscall.h:402-423`), `linux_statfs_t` (`syscall.h:450-463`), unpacked 16-byte `linux_epoll_event_t` (`syscall_poll.c:583-588`), 32-byte `linux_sigaction_t` (`syscall_signal.h:80-85`). Each handler translates between the Linux kernel structure and the Darwin API: `translate_stat` (`syscall_fs.c:59-77`) with re-encoding of `dev_t` (`syscall_fs.c:52-56`), errno (`linux_errno`, `syscall.c:330-414`), `AT_*` (`syscall.c:421-444`), `O_*` (`syscall.c:453-473`), clock identifiers, `RLIMIT_*` (`syscall_sys.c:228-242`), `sockaddr` with BSD `sa_len`, `cmsghdr` (16 versus 12 bytes of header, `SOL_SOCKET` 1 versus 0xFFFF).

### 4.2 Differences between generic ARM64 and x86-64

| Object | Generic ARM64 (HL) | x86-64 | What FEX does | State in HL |
| --- | --- | --- | --- | --- |
| Numbers | asm-generic, `read`=63, `openat`=56, `mmap`=222 (`syscall.h:17-200`) | `read`=0, `write`=1, `open`=2, `mmap`=9 | about 467 x86-64 entries (`x64/SyscallsEnum.h`) to about 469 ARM64 entries (`Arm64/SyscallsEnum.h`) through `SYSCALL_DEF` (`Syscalls.h:54`, 400 uses) | to redo |
| Registers | X8, X0-X5 | rax; rdi, rsi, rdx, r10, r8, r9 | `CpuStateFrame`, typed handlers | to redo |
| Legacy calls absent from ARM64 | not applicable | `open`, `stat`, `lstat`, `access`, `pipe`, `dup2`, `poll`, `select`, `rename`, `mkdir`, `rmdir`, `link`, `unlink`, `symlink`, `readlink`, `chmod`, `chown`, `creat`, `getdents`, `utime`, `time`, `fork`, `vfork`, `arch_prctl` | calls the glibc functions (`Syscalls/FS.cpp:23-48`, `x64/FD.cpp:122-128`), emulation of `getdents` through `getdents64` (`GetDentsEmulation`) | to write (about 30 calls) |
| `struct stat` | 128 bytes (`syscall.h:402`) | 144 bytes: `st_dev`, `st_ino`, `st_nlink` (8), `st_mode`, `st_uid`, `st_gid`, `__pad0`, `st_rdev`, `st_size`, `st_blksize` (8), `st_blocks`, 3 timestamps, 3 spare words | `guest_stat`, `static_assert` 144 (`x64/Types.h:112-215`), used in `x64/FD.cpp:65-103` | new 25-line function; `mac_to_linux_dev` can be reused |
| `dirent64` | 19-byte header, `d_ino`, `d_off`, `d_reclen`, `d_type` (`syscall_fs.c:97-103`) | identical | `linux_dirent_64`, `static_assert` 19 (`x64/Types.h:213-222`) | reuse |
| `statfs` | `linux_statfs_t` (`syscall.h:450`) | same 64-bit layout | passes `FM.Statfs` directly (`x64/FD.cpp:135-139`) | reuse |
| `rlimit` | 16 bytes, `RLIM_INFINITY` = 2^64-1 (`syscall_sys.c:244-299`) | identical | passthrough | reuse |
| `flock` (`fcntl`) | 32 bytes | identical | passthrough | reuse; the conversion of F_RDLCK 0, F_WRLCK 1, F_UNLCK 2 to macOS's 1, 3, 2 is a private fix in `syscall_fs.c` |
| `sockaddr`, `msghdr`, `cmsghdr` | LP64 Linux | identical | passthrough | reuse (`syscall_net.c`, other report) |
| `timespec`, `timeval`, `iovec`, `pollfd`, `sysinfo`, `rusage` | `linux_timespec_t` etc. (`syscall.h:437-545`) | identical | passthrough | reuse |
| `epoll_event` | 16 bytes, unpacked (`syscall_poll.c:583`) | 12 bytes packed | `epoll_event_x86`, `static_assert` 12 (`Types.h:56-72`) and copy by vector (`x64/EPoll.cpp:24-94`) | to adapt (20 lines) |
| `O_*` | `O_DIRECTORY` 0x4000, `O_NOFOLLOW` 0x8000, `O_DIRECT` 0x10000, `O_LARGEFILE` 0x20000 (`syscall.h:352-356`) | `O_DIRECT` 040000, `O_LARGEFILE` 0100000, `O_DIRECTORY` 0200000, `O_NOFOLLOW` 0400000 | `RemapFromX86Flags` and `RemapToX86Flags` (`Syscalls.h:490-562`) | to adapt; `translate_open_flags` (`syscall.c:453-473`) ignores `O_DIRECT`, `O_SYNC`, `O_DSYNC`, `O_NOATIME`, `O_PATH`, `O_TMPFILE` |
| kernel `sigaction` | 32 bytes: handler, flags, restorer, mask | 32 bytes, same order | `GuestSigAction` (`Types.h:97-106`) | same size; the real ARM64 structure has no `restorer` as far as I remember the kernel headers, not rechecked here (signals team) |
| Signal frame, `ucontext`, `rt_sigreturn` | ARM64 frame built on the guest stack, restorer in the vDSO (`vdso.h:34-38`) | x86-64 frame with xsave `fpstate`, restorer provided by the libc (`SA_RESTORER`) | `SignalDelegator` (1,352 + 856 lines) | out of scope; do not reuse |
| `clone` | ARM64 argument order | `flags, stack, parent_tid, child_tid, tls` (the last two are swapped compared to ARM64) | `x64/Thread.cpp` | process team |
| `uname` | `machine` `aarch64` (`syscall_sys.c:37`) | `x86_64` | not applicable | one line |
| `mmap` | same flags, no `MAP_32BIT` | `MAP_32BIT` 0x40 | `Get32BitAllocator()` for addresses below 4 GiB (`SyscallsSMCTracking.cpp:316-325`) | useless on Darwin, the space below 4 GiB does not exist |
| `getrandom`, `futex`, `ppoll`, `pselect6`, `wait4`, `ioctl` (except tty) | same arguments | identical | passthrough | reuse |

### 4.3 How FEX converts x86-64 calls to the ARM64 host ABI

READ in FEX. Each x86-64 call is registered by `REGISTER_SYSCALL_IMPL_X64(name, lambda)` or `REGISTER_SYSCALL_IMPL(name, lambda)` (both architectures). Four mechanisms:

1. Passthrough. 168 common calls (`RegisterCommon`, `Syscalls/Passthrough.cpp:212-414`) plus 73 specific to x86-64 (`Passthrough.cpp:416-501`), that is 241 calls sent as is. On an ARM64 Linux host, an inline assembly `svc #0` with the ARM64 number (`Passthrough.cpp:17-150`); on other hosts, `::syscall` (l.152-210). The list includes `read`, `write`, `ioctl`, `readv`, `writev`, `sendmsg`, `recvmsg`, `setsockopt`, `getsockopt`, `futex`, `wait4`, `ppoll`, `pselect6`, `clock_*`, `timerfd_*`, `preadv`, `pwritev`, `process_vm_readv`, `process_vm_writev`, `memfd_create`, `getrandom`, `futex_waitv`.
2. Structure or flag conversion, about 100 handlers: `stat` to `guest_stat` (`x64/FD.cpp:65-103`), `epoll_*` (`x64/EPoll.cpp`), `fcntl` with `O_*` remapping (`x64/FD.cpp:45-55`), `rt_sigaction` to the signal delegate (`x64/Signals.cpp:21-28`), legacy `getdents`, `clone`.
3. Legacy calls implemented by the glibc functions (`::rename`, `::mkdir`, `::unlink`…), which take the guest path as is.
4. Memory: `GuestMmap`, `GuestMunmap`, `GuestMremap`, `GuestMprotect` call the host's `::mmap`, `::munmap`, `::mremap`, `::mprotect`, then update `VMATracking` and invalidate the JIT code (`SyscallsSMCTracking.cpp:302-514`). Paths go through `FileManager`, which overlays a root (`RootFSFD`) and emulates a few files: `/proc/cpuinfo`, `/proc/version`, `/proc/sys/kernel/osrelease`, `/proc/self/auxv`, `/sys/devices/system/cpu/online`, `/proc/self/exe` (`FEX/EmulatedFiles/EmulatedFiles.cpp:522-592`, `FEX/FileManagement.h`). All the rest of `/proc` is the real Linux kernel's, which does not exist on Darwin.

MEASURED (counting). The `LinuxEmulation` directory excluding `x32` is 21,705 lines in 75 files, `x32` 10,793. Excluding `x32`, `grep` counts 137 `::syscall(` lines, 400 uses of `SYSCALL_DEF`, 19 `asm volatile` lines in 5 files, 39 inclusions of `<linux/…>` or `<asm/…>` headers (the macOS SDK has no `usr/include/linux` directory) and 25 inclusions of 14 `<sys/…>` headers absent from the SDK: `auxv`, `epoll`, `eventfd`, `fsuid`, `inotify`, `klog`, `personality`, `prctl`, `sendfile`, `signalfd`, `statfs`, `swap`, `sysinfo`, `timerfd`.

MEASURED (name comparison, throwaway script). Of the 241 passthrough calls, HL implements 128 and lacks 113. Among the missing: `process_vm_readv`, `process_vm_writev`, `futex_waitv`, `timer_create` and five other `timer_*`, `capget`, `capset`, `getsid`, `times`, `settimeofday`, `rt_sigqueueinfo`, `sched_*` (nine calls), `semget`, `semop`, `semtimedop`, `msgget`, `msgsnd`, `msgrcv`, `msgctl`, `mq_*`, `io_*` (seven calls), `pidfd_*`, `readahead`, `sync_file_range`, `syncfs`, `mlockall`, `mlock2`, `getcpu`, `setgroups`, `setfsuid`, `setfsgid`, `syslog`, `unshare`, `setns`, `kcmp`.

### 4.4 Comparison of the two compositions

Composition A: FEX's `LinuxEmulation` compiled for Darwin on top of an "ARM64 kernel on Darwin" library derived from HL's handlers. Composition B: a new x86-64 layer written directly for FEXCore.

For A, the following is needed:

- Replace the 7 passthrough templates (`Passthrough.cpp:17-210`) and the 137 `::syscall` sites with a `hl_syscall(nr, a0..a5)` call. Mechanical, about 150 sites.
- Provide a "Linux" libc: FEX directly calls `::open`, `::stat`, `::fcntl`, `::select`, `::rename`, `::mkdir`, `::epoll_*`… with Linux flag values (`RemapToX86Flags` assumes ARM64 Linux `O_*`, `Syscalls.h:528-562`). On Darwin these calls would return Darwin values. About 100 wrappers, plus header isolation: 64 inclusions target headers absent from the macOS SDK (39 Linux UAPI and 25 `<sys/…>`) and, INFERRED, the imported UAPI headers risk redefining types that the SDK also defines (`struct stat`, `timespec`).
- Extend the library from 128 to about 240 calls to cover the passthrough, or mark as ENOSYS the calls that Wine and EAC do not use.
- Convert twice: Darwin to ARM64 in the library, ARM64 to x86-64 in FEX (`stat`, `epoll`, flags). `st_dev` would have two encodings to chain.
- Port what remains Linux in FEX anyway: thread creation through raw system calls in assembly (`Utils/Threads.cpp:128-173`), signal delegate on ARM64 Linux `ucontext` (`ArchHelpers/MContext.h`, 409 lines, `UContext.h`, 439), `execve` done through `execveat(…, "/proc/self/exe", …)` (`Syscalls.cpp:443`), FEXServer client (`SyscallsSMCTracking.cpp:434-466`), 32-bit allocator (`LinuxAllocator.cpp`), `GdbServer`.
- Keep two conflicting path managers: FEX's rooted `FileManager` and HL's mount-based vfs.

For B, the following is needed:

- An x86-64 dispatch table on `CpuStateFrame`: about 150 to 200 calls, with the 103 ARM64 numbers seen in the Wine plus EAC traces (appendix B) as the base.
- The x86-64 converters: `stat`, `epoll_event`, `O_*` flags, `getdents`, `uname`, about 30 legacy calls.
- HL's "Linux on Darwin" services as is: vfs, fd, procfs, task names, errno and flag translations, `kqueue` for epoll, timerfd, inotify.

Reasons to choose B:

1. MEASURED: 241 of FEX's calls are passthrough and HL covers only 128 of them. A forces HL to become an almost complete ARM64 kernel, B requires serving what Wine and EAC call. The traces of the previous experiment (appendix B) give 103 distinct calls, 101 present in HL's dispatch.
2. The conversion happens once, from Darwin to x86-64.
3. HL channels the handlers' accesses to guest memory through five functions (269 sites outside `guest.c`). In a process, `guest_ptr(g, gva)` becomes the identity and `guest_read`/`guest_write` protected copies (model: `FEX/FaultSafeUserMemAccess.cpp`, 183 lines). The 269 sites do not change.
4. The parts of FEX worth importing are coupled to the CPU, not to the kernel, and can be imported into B without A: `SyscallsVMATracking.*` (765 lines), `SyscallsSMCTracking.cpp` (816), `x64/Types.h` (222), `Types.h` (108), `FaultSafeUserMemAccess.cpp` (183), the `/proc/cpuinfo` generator (`EmulatedFiles.cpp:522-534`), `VDSO_Emulation.cpp` (906). Licenses: FEX is MIT and HL Apache-2.0, both permissive. The FEXCore team points out the rule in FEX's `CLAUDE.md` on AI-generated contributions (`fexcore-darwin-port.md`, last section); it targets upstream contributions, not this private test bench.
5. The FEXCore team is already writing a `fexdarwin-run` host with its ELF loader, its initial stack and its signal delegate (`fexcore-darwin-port.md`, next steps).

Amount of glue, INFERRED from the table in section 2 and the counts above: B needs about 2,600 to 3,400 lines for the table and the converters, plus 1,800 to 2,400 lines of touch-ups to the reused handlers. A needs roughly B plus 3,000 to 4,500 lines: 2,000 to 3,000 of touch-ups in FEX, 800 to 1,200 of Linux libc, 1,500 to 2,500 to get HL out of `guest_t`. Caveat: A's advantage is to track FEX upstream; it is small here because the FEX repository is already forked for Darwin.

## 5. Gaps of the core relative to the needs of Wine and EAC

State in the working copy. The needs come from the traces of the previous experiment (`XP/README.md`, `XP/STATUS.md`): the EAC bootstrapper writes 6.6 to 9.7 MB into a memfd depending on the runs, reads the memory map to find `libc`, reopens the memfd through `/proc/self/fd/N`, tries `F_ADD_SEALS`, calls `ptrace`, `readlinkat(/proc/self/exe)`.

| Item | State | Detail |
| --- | --- | --- |
| `/proc/self/maps` | present | Rendered from the VMA table, names `[heap]`, `[stack]`, `[sysvshm]` (`hl.c:891-903`, `syscall_shm.c:584`); `[vdso]` and `[vvar]` only in Rosetta mode (`rosetta.c:238-241`). The guest path is recomputed by `hl_vfs_host_to_guest`. |
| `/proc/<pid>/maps`, another process | absent | Only the strict alias of the current PID is rewritten to `/proc/self` (`proc_emulation.c:60-68`). This alias fixed the opening of `/proc/1/maps` by the EAC bootstrapper when its own PID is 1 (`XP/README.md`, line 109). No read of another PID's map was seen in the cited traces. |
| `/proc/self/status` | partial | Name, State, Tgid, Pid, PPid, TracerPid, Uid, Gid, VmPeak, VmSize, VmRSS, Threads (`proc_emulation.c:521-576`). VmSize and VmRSS come from the backing blocks. Missing: VmData, VmStk, VmExe, VmLib, Seccomp, NoNewPrivs, Cap*, Sig*, Cpus_allowed. |
| `/proc/<tid>/status` of another process | partial | Name, State (Z or t), Tgid, Pid, PPid, TracerPid, Uid, Gid (`proc_emulation.c:133-166`). |
| `/proc/self/stat` | partial | 52 fields, only pid, comm, state `R`, ppid, pgrp, session, utime, stime, threads, vsize, rss are filled (`proc_emulation.c:804-855`). |
| `/proc/self/task` | partial | Opening of `/proc/<tgid>/task/<tid>/comm` and `status` (`proc_emulation.c:99-111`). No listing of `/proc/self/task` nor of `/proc/self/task/<tid>/`: opening a directory under a virtual mount returns ENOENT (`syscall_fs.c:549`). |
| `/proc/self/fd` | partial | Opening, `readlink` and `stat` of `/proc/self/fd/N` (`proc_emulation.c:293-395`, `1053-1075`, `997-1007`); no directory listing. No `/proc/self/fdinfo`. |
| `/proc/self/exe` | present | `readlink` as a guest path through `realpath` and `hl_vfs_host_to_guest` (`proc_emulation.c:1026-1031`), opening of the binary (`493-498`). Other tasks: exec snapshot. |
| `/proc/self/cmdline` | present | NUL-separated bytes, exec snapshot for the other tasks (`proc_emulation.c:579-584`). |
| `/proc/self/environ` | absent | No occurrence. |
| `/proc/self/mem` | absent | No occurrence. |
| `/proc/cpuinfo` | partial | ARM fields only: "CPU implementer 0x61" (`proc_emulation.c:501-518`). Wrong for an x86-64 guest; reuse FEX's generator. |
| `/proc/meminfo` | partial | Fixed ratios: 50% free, 75% available (`proc_emulation.c:730-782`). |
| `/proc/self/smaps`, `pagemap`, `limits`, `auxv` | absent | No occurrence in `src/`. FEX provides `auxv` (`FEX/EmulatedFiles/EmulatedFiles.cpp:589-592`). |
| `/proc/self/io` | partial | File of zeros (`proc_emulation.c:787-797`). |
| `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random`, `/dev/tty` | present | Opening of the host node (`device.c:47-105`, `proc_emulation.c:449-471`). |
| `/dev/stdin`, `/dev/stdout`, `/dev/stderr`, `/dev/fd/N` | present | `proc_emulation.c:473-487`. |
| `/dev/shm` | absent | No node or mount. To work around with a mount of a host directory, to test with the file MAP_SHARED path. |
| `/dev/full`, `/dev/ptmx`, `/dev/pts` | absent | No occurrence. |
| `memfd_create` | partial | Deleted temporary file, `MFD_CLOEXEC` only, name ignored (`syscall.c:2385-2398`). `readlink /proc/self/fd/N` does not give `/memfd:name (deleted)`. |
| Seals (`F_ADD_SEALS`, `F_GET_SEALS`) | absent | No occurrence of "seal". `F_ADD_SEALS(0xf)` returns EINVAL (`XP/README.md`, line 209). Probe `XP/probes/memfd-seals`: 8 checks out of 29 pass under HL, 29 out of 29 under Linux. The allow versus deny trial on a real Linux kernel keeps 509/210 in both cases (`XP/linux-reference/seal-comparison/README.md`), so the link with EAC's rejection is not established. On Darwin, no check blocks writes through an existing mapping: exact sealing is impossible, an approximate sealing will have to be decided and documented. |
| `mremap` | absent | ENOSYS (`syscall.c:2451-2462`). |
| `madvise` | partial (stub) | Returns 0 without doing anything (`syscall.c:1692-1694`); `MADV_DONTNEED` does not clear the page. |
| `mincore` | absent (explicit ENOSYS) | ENOSYS (`syscall.c:2409-2411`). |
| `mlock`, `munlock`, `msync` | partial (stub) | Return 0 (`syscall.c:2399-2408`). |
| `process_vm_readv`, `process_vm_writev` | absent | ENOSYS. `XP/STATUS.md` notes seven `process_vm_readv` reads returning ENOSYS in the complex witness. On Darwin, reading another process goes through `mach_vm_read_overwrite` on the peer's task port; obtaining it is the process team's concern. |
| `userfaultfd` | absent | ENOSYS. No Darwin equivalent; I found no use in the traces. |
| Descriptors beyond 1,024 | absent | `FD_TABLE_SIZE` = 1024 (`syscall.h:601`). |
| xattr family, numbers | defect | `SYS_fgetxattr` is 16 and `SYS_fremovexattr` 18 in `syscall.h:166` and `169`; the real ARM64 numbers are 10 and 16. An `fgetxattr` call (number 10) falls into "unimplemented syscall 10" (19, 36 and 22 times in the three EAC traces) and an `fremovexattr` (16) would go to the `fgetxattr` handler. Defect also present at HEAD (`git show HEAD:src/syscall.h`). Systematic check: of the 177 `#define SYS_*` in `syscall.h`, 175 match the ARM64 table that I reconstructed from memory (throwaway script) and only these two are wrong. |
| `O_TMPFILE`, `O_PATH`, `O_SYNC`, `O_DSYNC`, `O_NOATIME` | absent | Ignored without error by `translate_open_flags` (`syscall.c:453-473`). |

Defects found while reading the code and not to reproduce: anonymous `MAP_SHARED` treated as private (`syscall.c:837`, `1120`); `munmap` frees nothing and keeps the PTEs of the bottom of the space (`syscall.c:1458-1470`); procfs files allocated from descriptor 128 (`syscall_fs.c:466`); 65 MiB VMA table in the worst case.

## 6. Line estimate and five risks

### 6.1 INFERRED estimate for a FEXCore host

Basis: sections 2 and 4. Margin: plus or minus 35%.

| Item | Lines | Origin |
| --- | --- | --- |
| Reused without notable change | about 10,500 | 3,835 of the reuse class minus 150 to 300 touched up, plus 6,700 to 7,300 of the 9,082 of the adapt class that do not change |
| Touch-ups of the adapt class (numbers, `stat`, `O_*`, `epoll_event`, `machine`, removal of the `hv_` types, `guest_ptr` shim) | 1,800 to 2,400 | 20 to 25% of 9,082 |
| Touch-ups of the reuse class (separate `syscall.h` from the constants, remove audio) | 150 to 300 | `vfs.c`, `fd_object.c`, `device.c` include `syscall.h`, which includes `Hypervisor.h` |
| Address space manager: mmap, munmap, mprotect, MAP_FIXED, 4 KiB on 16 KiB handling, VMA, JIT invalidation hooks | 1,300 to 1,900 | replaces 2,918 lines of `guest.*` and 686 of `syscall.c` |
| x86-64 dispatch table, legacy calls, converters | 2,600 to 3,400 | section 4.4 |
| Shared mappings, SysV SHM, memfd and approximate sealing | 700 to 1,100 | `syscall_filemap`, `syscall_shm` rewritten (550) plus seals (250 to 500) |
| procfs additions: `task`, `fd`, `environ`, `mem`, `/proc/<pid>/maps`, `smaps`, x86 cpuinfo, full status | 700 to 1,100 | section 5 |
| `mremap`, `madvise`, `mincore`, `process_vm_readv` and `writev` | 600 to 900 | section 5 |
| Total written or modified | 8,000 to 11,000 | |

### 6.2 The five riskiest items

1. 4 KiB guest granularity on 16 KiB host pages with guest address = host address. MEASURED: host `mmap` and `mprotect` round to 16 KiB, a 4 KiB `MAP_FIXED` destroys 12 neighboring KiB, a file offset at 0x1000 is refused. HL solves this with its guest MMU, which no longer exists. A choice to make early, because it shapes Wine (announced page size, stack guards, `VirtualProtect`, PE sections): announce 16 KiB, or emulate through faults. Neither has been tested.
2. Low guest addresses. MEASURED by the FEXCore team: no fixed `mmap` below 4 GiB, `__PAGEZERO` cannot be shrunk (`reports/fexcore-darwin-port.md`, measurement table). HL avoids it (guest.h:95, `GUEST_IPA_BASE` = 0) and its private fixes free the bottom of the space for Wine's DOS area. An x86-64 ELF linked at 0x400000 or a Wine preloader at a low fixed address does not load in a process; only address translation in the JIT or guests linked higher get out of the dead end.
3. Exact semantics of `munmap`, `mprotect`, `mremap` and `madvise` and consistency with the JIT. HL cheats: `munmap` frees nothing, `madvise` and `mincore` are stubs. In a process the guest observes the real faults, `mremap` does not exist on Darwin and `MADV_DONTNEED` does not zero. Every write to the VMA table must be atomic with the invalidation of the translations (model: FEX's `GuestMmap`). A race here produces stale code being executed, hard to diagnose.
4. Fidelity of `/proc` and memfd for EAC. The bootstrapper opens `/proc/<its pid>/maps` to locate `libc` (`/libc-` or `libc.so` line), reopens the memfd through `/proc/self/fd/N` with an independent cursor and tries `F_ADD_SEALS` (`XP/README.md`, section "EAC reading the memory map"). Seals cannot be imitated identically on Darwin. Each `/proc` difference is a possible silent failure, with no reference other than the real Linux used for comparison.
5. Memory sharing and identity between processes without a Linux kernel. Shared anonymous memory does not exist in HL, file views only survive a fork through descriptor export, the guest PID is decoupled from the host PID, `kill` and `wait4` go through `proc_table[64]`, `/proc/<pid>` relies on a shared file locked with POSIX locks. A separate wineserver that reads a client's memory needs a task port (process team). The risk is inconsistency between these three tables (VMA, host descriptors, name registry) after `exec` or a crash.

## 7. Host tests already run

MEASURED, rerun during this session (see 0.3):

- `test-fd-object-lite`: 12 `OK` lines then `PASS` (creation, pipe, installation, `dup` shares the description, close).
- `test-vfs-unit`: 15 `OK` lines then `PASS` (relative resolution, internal links, ELOOP at 41 links, longest bind, refusal of absolute host targets).
- `test-page-table-pool`: 6 `OK` lines then `PASS` (pool outside the arena, 512 split blocks, Wine's low reservations, exact fork holes, high VA, permissions). It tests stage 1 code that will be dropped; its oracles on VMA splits remain useful.
- `test-aot-format`, `test-app-open`, `test-fd-digest`, `test-oss-abi`, `test-thread-vcpu-lifetime`: `PASS`, not directly related to the FEX core.

## 8. Points not established

- No test has run guest code. The verdicts of section 2 and the estimates of section 6 are judgments based on reading code.
- The traces of appendix B stop at EAC error 509: the call set of a full Rocket League session is wider (audio, networking, GPU).
- Page size announced to the guest, real fork versus mapping export, sharing of task ports between processes: open decisions, no measurement.
- The behavior of EAC's own `fcntl(F_ADD_SEALS)` was not recorded under Linux (`XP/linux-reference/seal-comparison/README.md`, "Limits and decision").

## Appendix A. Page granularity probe (MEASURED)

C program compiled with `clang -O0` and run with `perl -e 'alarm 20; exec @ARGV' ./pg2`, in the session's temporary directory (not kept outside this report). It creates 256 KiB of anonymous memory aligned on 16 KiB, tries the operations then reads the region through `mach_vm_region_recurse`.

```c
#define TRY(label, expr) do { errno = 0; long _r = (long)(expr); int _e = errno; \
    printf("%-64s -> %s errno=%d (%s)\n", label, (_r == -1 || (void*)_r == MAP_FAILED) ? "FAIL" : "OK  ", \
           _e, _e ? strerror(_e) : "-"); } while (0)
char *base = mmap(NULL, 0x40000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
TRY("mmap(MAP_FIXED|ANON, base+0x1000, 0x1000)", mmap(base+0x1000, 0x1000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
TRY("mmap(MAP_FIXED|ANON, base+0x2000, 0x4000)", mmap(base+0x2000, 0x4000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
memset(base, 0xAA, 0x40000);
TRY("mmap(MAP_FIXED|ANON, base+0x4000, 0x1000)", mmap(base+0x4000, 0x1000, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0));
TRY("mprotect(base+0x9000, 0x1000, PROT_NONE)", mprotect(base+0x9000, 0x1000, PROT_NONE));
TRY("mprotect(base+0x8000, 0x1000, PROT_READ)", mprotect(base+0x8000, 0x1000, PROT_READ));
TRY("munmap(base+0x1000, 0x1000)", munmap(base+0x1000, 0x1000));
TRY("munmap(base+0x10000, 0x1000)", munmap(base+0x10000, 0x1000));
/* 64 KiB file: mmap offset 0x1000 MAP_PRIVATE and MAP_SHARED, offset 0x4000 length 0x4000 MAP_SHARED */
```

Output (excerpts, M5 Max, macOS 27.0.1):

```
getpagesize=16384 sysconf=16384 vm_page_size=16384
mmap(MAP_FIXED|ANON, addr=base+0x1000, len=0x1000)               -> FAIL errno=22 (Invalid argument)
mmap(MAP_FIXED|ANON, addr=base+0x2000, len=0x4000)               -> FAIL errno=22 (Invalid argument)
mmap(MAP_FIXED|ANON, addr=base+0x4000, len=0x1000)               -> OK   errno=0 (-)
   after: +0x3fff=aa +0x4000=00 +0x5000=00 +0x7fff=00 +0x8000=aa  (00 where the 16K page was replaced)
mprotect(base+0x9000, 0x1000, PROT_NONE)                         -> FAIL errno=22 (Invalid argument)
mprotect(base+0x8000, 0x1000, PROT_READ) (16K aligned, 4K len)   -> OK   errno=0 (-)
   region(after mprotect 4K len @0x102948000): kr=0 start=0x102948000 size=0x4000 prot=1 maxprot=7
munmap(base+0x1000, 0x1000)                                      -> FAIL errno=22 (Invalid argument)
munmap(base+0x10000, 0x1000) (16K aligned, 4K len)               -> OK   errno=0 (-)
   region(after munmap of 4K len at +0x10000 @0x102950000): kr=0 start=0x102954000 size=0x2c000 prot=3 maxprot=7
mmap(file, off=0x1000, len=0x1000, MAP_PRIVATE)                  -> FAIL errno=22 (Invalid argument)
mmap(file, off=0x1000, len=0x1000, MAP_SHARED)                   -> FAIL errno=22 (Invalid argument)
mmap(file, off=0x4000, len=0x4000, MAP_SHARED)                   -> OK   errno=0 (-)
mmap(file, off=0x4000, len=0x1000, MAP_SHARED|MAP_FIXED at base+0x1000) -> FAIL errno=22 (Invalid argument)
mach_vm_protect(a+0x1000, 0x1000)                                -> kr=0 ((os/kern) successful)
mach_vm_protect(a, 0x1000) 16K aligned, 4K len                   -> kr=0 ((os/kern) successful)
   region(after mach_vm_protect 4K len @0x102960000): kr=0 start=0x102960000 size=0x4000 prot=1 maxprot=7
```

A first version of the probe crashed on a write into the 16 KiB page that `mprotect` had just made read-only: the crash confirms the rounding.

SDK checks (`grep` in `usr/include/sys/mman.h` and `fcntl.h` of the macOS SDK): `MAP_FIXED_NOREPLACE`, `mremap`, `memfd_create`, `epoll_create`, `eventfd` absent; `MAP_32BIT` (0x8000), `MAP_JIT` (0x0800), `MADV_ZERO` (11), `O_RESOLVE_BENEATH` (0x1000) present.

## Appendix B. Call set seen in the Wine and EAC traces of the previous experiment (MEASURED)

Command: `grep -a -o '^hl: syscall [0-9]*(' <trace> | sed … | sort -n | uniq -c`, on `XP/run/logs/eac-syscall-trace.log` (282,379 calls, 102 distinct numbers) and `XP/run/logs/eac-parent-ptrace-regset.log` (251,693 calls, 103 numbers). These are ARM64 calls issued by Rosetta on behalf of Wine and EAC, plus Rosetta's own, up to EAC error 509. Union of the two traces, 103 numbers (ARM64 number and name):

8 getxattr, 10 fgetxattr, 17 getcwd, 19 eventfd2, 20 epoll_create1, 21 epoll_ctl, 22 epoll_pwait, 23 dup, 24 dup3, 25 fcntl, 29 ioctl, 32 flock, 34 mkdirat, 35 unlinkat, 38 renameat, 43 statfs, 44 fstatfs, 46 ftruncate, 48 faccessat, 49 chdir, 50 fchdir, 53 fchmodat, 56 openat, 57 close, 59 pipe2, 61 getdents64, 62 lseek, 63 read, 64 write, 66 writev, 67 pread64, 68 pwrite64, 72 pselect6, 73 ppoll, 78 readlinkat, 79 newfstatat, 80 fstat, 85 timerfd_create, 93 exit, 94 exit_group, 96 set_tid_address, 98 futex, 99 set_robust_list, 113 clock_gettime, 114 clock_getres, 115 clock_nanosleep, 117 ptrace, 122 sched_setaffinity, 123 sched_getaffinity, 124 sched_yield, 129 kill, 131 tgkill, 134 rt_sigaction, 135 rt_sigprocmask, 139 rt_sigreturn, 148 getresuid, 150 getresgid, 157 setsid, 160 uname, 163 getrlimit, 165 getrusage, 166 umask, 167 prctl, 169 gettimeofday, 172 getpid, 173 getppid, 174 getuid, 175 geteuid, 176 getgid, 177 getegid, 178 gettid, 179 sysinfo, 194 shmget, 195 shmctl, 196 shmat, 198 socket, 199 socketpair, 200 bind, 201 listen, 202 accept, 203 connect, 204 getsockname, 205 getpeername, 207 recvfrom, 208 setsockopt, 209 getsockopt, 210 shutdown, 211 sendmsg, 212 recvmsg, 215 munmap, 220 clone, 221 execve, 222 mmap, 223 fadvise64, 226 mprotect, 240 rt_tgsigqueueinfo, 260 wait4, 261 prlimit64, 264 name_to_handle_at, 269 sendmmsg, 278 getrandom, 279 memfd_create, 291 statx.

The most frequent in the second trace: `read` 56,887, `rt_sigprocmask` 50,532, `write` 24,806, `clock_gettime` 22,401, `epoll_pwait` 20,658, `writev` 17,010, `mmap` 10,358, `mprotect` 6,001, `recvmsg` 4,798, `openat` 4,497, `readlinkat` 4,351, `newfstatat` 4,066.

Among the 103, only two have no `case` by number in HL's dispatch: 10 (`fgetxattr`, see the numbering defect in section 5) and 264 (`name_to_handle_at`, 2 calls). The "unimplemented syscall" lines of the traces contain only these two numbers. Neither `mremap`, nor `madvise`, nor `mincore`, nor `process_vm_readv`, nor `userfaultfd` appear in these traces, which does not prove that the rest of the session does not use them.

## Appendix C. In-process witness (MEASURED)

The three drivers and the shim are in the session's temporary directory (`scratchpad/inproc`), not kept outside this report. SHA-256 fingerprints of the sources, first 16 characters: `drv.c` b5ed6aa5886ee9d8, `drv2.c` fc8c3d444ea55dee, `drv3.c` f47ff861c37ea3e3, `shim.c` 27182628a6de137f, `vma_only.c` 232ba06c0cb12564.

Shim, the whole guest memory interface required by the file, I/O, time and procfs handlers:

```c
void *guest_ptr(const guest_t *g, uint64_t gva) { (void)g; return (void *)(uintptr_t)gva; }
void *guest_ptr_avail(const guest_t *g, uint64_t gva, uint64_t *avail) { (void)g; if (avail) *avail = 1ULL << 30; return (void *)(uintptr_t)gva; }
int guest_read(const guest_t *g, uint64_t gva, void *dst, size_t len) { (void)g; memcpy(dst, (void *)(uintptr_t)gva, len); return 0; }
int guest_write(guest_t *g, uint64_t gva, const void *src, size_t len) { (void)g; memcpy((void *)(uintptr_t)gva, src, len); return 0; }
int guest_read_str(const guest_t *g, uint64_t gva, char *dst, size_t max) {
    (void)g; if (!max) return -1; const char *s = (const char *)(uintptr_t)gva; size_t i = 0;
    for (; i + 1 < max && s[i]; i++) dst[i] = s[i];
    dst[i] = 0; return s[i] ? -1 : (int)i;
}
```

This shim is not safe against an invalid pointer: it does a raw `memcpy`. In production it must be replaced by protected copies that return `-EFAULT` (model: `FEX/FaultSafeUserMemAccess.cpp`).

Linking (bash, without running HVF): all the `EXP/scratch-hl/hyper-linux-copy/_build/obj/*.o` except `hl.o` and `guest.o`, plus `drv.o` (or `drv2.o` with `vma_only.o`, or `drv3.o`) and `shim.o`:

```
clang -arch arm64 -o drv drv.o shim.o $OBJS -framework AudioToolbox -framework CoreAudio \
      -framework Hypervisor -Wl,-undefined,dynamic_lookup
```

Outputs, final lines: driver 1 `PASSED (0 failures)` with 12 `OK` lines, driver 2 `PASSED (0 failures)` with 5 `OK`, driver 3 `PASSED (0 failures)` with 6 `OK`. First try of driver 1: `write` returned `-11` for lack of `current_thread` (see 1.7), fixed by `thread_init(0x100000)` and `thread_register_main(0, NULL, 1, 0x100000)`. First try of driver 2: the path line showed `/tmp/…` instead of `/usr/…`, because my VMA name was not canonical while `hl_vfs_add_bind` canonicalizes the root of the bind (`/tmp` is a link to `/private/tmp` on macOS, `vfs.c:139-172`); in production `fcntl(F_GETPATH)` returns the canonical path, which the driver simulates through `realpath`.

# Porting FEXCore to macOS arm64: living report

This document is updated at each milestone. It separates what was measured in this session from what is inferred from reading the code. The `file:line` citations point to the intact tree `src/fex-pristine` (FEX commit 0df84d3844bcdb87bb7d3f5b8fb0959cd009c038), unless stated otherwise.

Last updated: 2026-10-02 10:57 (milestone 3: witnesses a, b, d, e, h pass).

## CPU performance measurement (2026-10-02 14:57)

MEASURED, as an indication only (a single run per configuration, two for native and Rosetta, M5 Max). Portable C program `w12_bench.c` compiled as an x86-64 ELF (`-O2`, zig cc) for FEXCore, and as x86_64 and arm64 Mach-O (`clang -O2`) for Rosetta and native. Durations in ms:

| Kernel | native arm64 | x86_64 under Rosetta | FEXCore, base 0 | FEXCore, guest base 8 TiB |
| --- | --- | --- | --- | --- |
| integers (LCG, 400 M iterations) | 411 / 400 | 379 / 386 | 378 | 394 |
| memory (8 MiB, 200 passes, 64-byte stride) | 29 / 29 | 30 / 31 | 35 | 37 |
| double floats (200 M) | 430 / 427 | 351 / 352 | 367 | 361 |
| calls (fib 32) | 3 / 3 | 3 / 3 | 5 | 5 |
| shell sort 100 k x 20 (branches) | 128 / 128 | 141 / 142 | 174 | 175 |

The x86 checksums (Rosetta and FEXCore) are identical; the native float checksum differs (FMA contraction by the arm64 compiler), which is not a defect. On these kernels FEXCore is at Rosetta's level, 0 to 23% slower, and the guest base costs at most 4% (one addition per memory access; the `[x24, xaddr]` addressing without an extra instruction is not used yet). This is not a game measurement: here the JIT sees neither Wine, nor frequent system calls, nor self-modifying code. Logs: `evidence/fexcore-darwin/bench/`.

## Milestone 7: Linux fidelity of selectors and execution faults (2026-10-02 14:54)

MEASURED. (1) CS=0x33 and SS=0x2b as under Linux x86-64 (`arch/x86/include/asm/segment.h`), instead of 0x30 and 0: FEX picks the GDT entry by `Selector >> 3`, the RPL bits change nothing at execution (`w04_tls` prints `cs=0x0033 ss=0x002b`). (2) New witness `w11_badjump` 15/15: `call *%rax` to 9, `push 9; ret` (the EAC injection pattern), `jmp` to an unmapped high address. Linux delivers SIGSEGV, `si_code` SEGV_MAPERR, `si_addr` = target, RIP = target, `trapno` 14, error code 0x14. FEX classified every instruction fetch fault as SEGV_ACCERR with the "present" bit (0x15): the host now reclassifies from the mapping table (unmapped: MAPERR and 0x14; mapped without execute: ACCERR and 0x15).

Open problem: `w07_fault` passes its 99 checks then loops at the end of the program (jump to a stack address after the stack overflow test on the alternate stack, then the witness's handler always returns to the same label). Cause not established; lead: FEX's shadow call stack (`REG_CALLRET_SP`) after the reset on a guard fault.

## Milestone 6: guest threads (2026-10-02 14:50)

MEASURED. The prototype host implements `clone` for threads (CLONE_VM|FS|FILES|SIGHAND|THREAD with SETTLS, PARENT_SETTID, CHILD_CLEARTID), `futex` WAIT/WAKE and their BITSET variants on top of `os_sync_wait_on_address`, and thread exit (`exit` without `exit_group`, with clearing and waking of `clear_child_tid`). Witness `w10_threads` 13/13 with and without guest base: 8 guest threads run in parallel by the JIT, 1,600,000 consistent `lock add`, FS base specific to each thread, 2000 baton passes by futex between two threads, end waits through `CHILD_CLEARTID`. Fork-type `clone` calls return ENOSYS: they belong to the LKD layer. Logs `evidence/fexcore-darwin/runs/w10_threads-14494*.log`.

## Milestone 5: SIMD and x87 identical to the oracle (2026-10-02 14:47)

MEASURED. New witness `w03_simd`: 109 operations (SSE, SSE2, SSSE3, SSE4.1/4.2, AES, PCLMUL, AVX, AVX2, FMA, F16C, AVX2 gather, `vmaskmovps`, x87 including `fistp` under two rounding modes, `fcompp`, `fldpi`, `fsin`, `f2xm1`, `fyl2x`, MXCSR round down and FTZ/DAZ). The same source compiled as an x86_64 Mach-O runs under Rosetta as an independent oracle (`witnesses/elf-x86_64/oracle.sh`). The FEX and Rosetta transcripts are identical line for line, 109/109, with and without guest base (`evidence/fexcore-darwin/runs/w03_simd-144706.log`, `oracle/w03_simd-rosetta-144228.log`).

Defect found and fixed along the way: the `x87StackOptimizationPass` pass emits `LoadMem`/`StoreMem` on the host address of the CPU state (`FormContextAddress`) in its slow path (after `fldcw`). With a guest base these accesses were offset and faulted (host SIGBUS, address = CPU state + base). The register allocator inserts copies that hide the origin of the address, so detection in the JIT (`IsHostAddress`) is not enough: under Apple the pass now uses `LoadContextIndexed`/`StoreContextIndexed`, which carry no guest address semantics. Patch SHA-256 `53a6dcb0...`.

## Milestone 4: guest base offset (2026-10-02 14:40)

MEASURED. FEXCore now runs x86-64 code with a guest base: host address = `FEXCore::GuestBase` + guest address. Register x24 is reserved (`GUESTBASE`, `Arm64Emitter.h`), removed from the dynamic pool and reloaded by `FillStaticRegs` at each dispatcher entry. The JIT's memory operations go through `GuestPtr` or through the `[x24, address]` form: loads and stores, TSO variants, pairs, per-element, masked and broadcast vectors, push/pop without writeback (`Push`, `PushTwo`, `Pop`, `PopTwo`), `MemSet`, `MemCpy`, atomics (including `CASPair`), cache lines, emulated gather, `ValidateCode`. The decoder reads guest bytes through `G2H` (`Frontend.cpp`, `AdjustAddrForSpecialRegion`). The prototype host translates all its guest pointers (ELF loader, initial stack, syscalls, signal frames, fault address returned as a guest address).

Results with `FEXDARWIN_GUEST_BASE=0x80000000000` (8 TiB): new witness `w09_lowmem` 12/12 (fixed mmap and accesses at 0x7ffe0000 and 0x10000, locked xadd and cmpxchg, `rep stos`/`rep movs`, call of code placed at 0x14000, push, pop, call and ret on a stack at 0x1c000); without a guest base the same binary dies with a host SIGSEGV (139), which confirms that the witness does exercise the low area. Earlier witnesses with the base: `w02_int` 9524/9524, `w04_tls` 25/25, `w05_mem` 39/39, `w06_smc` 20/20, `w08_sys` 50/50, `w01_hello` OK. Logs `evidence/fexcore-darwin/runs/*-1440*.log`. Patch `src/patches/fexcore-darwin.patch` SHA-256 `edc42aab...`, host `fexdarwin-run` SHA-256 `6998374b...`.

Limits: the cost of the addition per access is not measured; `LoadMem`/`StoreMem` could use the `[x24, xaddr]` addressing without an extra instruction (optimization to do after the performance witnesses). The SVE paths are not covered (no SVE on this CPU). The on-disk code cache and the debug disassembler still read guest code without `G2H`: disabled here.

## State at restart (2026-10-02 10:55, after the second interruption)

Reread on disk: the FEXCore patch (`src/patches/fexcore-darwin.patch`) is up to date as of the last export; the host `src/fexdarwin` (11 source and header files, `scripts/build.sh`, `scripts/run-with-timeout.sh`, `scripts/export-patch.sh`) compiles; the witnesses are in `witnesses/elf-x86_64` (`build.sh`, `run.sh`, `oracle.sh`, `src/`, `bin/`, `oracle/`, `gen/`). Witnesses that pass on FEX in this session, logs in `evidence/fexcore-darwin/runs/`: (a) `w01_hello`, (b) `w02_int` 9524 out of 9524, (d) `w04_tls` 25 out of 25, (e) `w05_mem` 39 out of 39, (h) `w08_sys` 50 out of 50. Witness (f) `w06_smc` first hung after its first run (timeout exceeded, code 142, log `w06_smc-105508.log`). Cause established: macOS delivers a SIGBUS with `si_code` 1 for permission faults and for alignment faults, and my handler took every SIGBUS fault in the JIT for an unaligned atomic access (`HandleUnalignedAccess`); the write fault on a protected page therefore restarted forever. Fix: tell alignment apart by the ESR syndrome (`DFSC` = 0x21). Witness (f) then passes. Witnesses (c), (g), (i) are not written yet. Rosetta 2 serves as an independent oracle for the tables of witness (b) (log `evidence/fexcore-darwin/oracle/w02_int-rosetta-104410.log`, 9460 out of 9460). The analysis of the guest base offset is written further down and is still to be implemented.

## Current state in one sentence

Milestone 2 reached: witness (a) passes. `fexdarwin-run` loads a static x86-64 ELF, FEXCore compiles its code to ARM64 with the JIT in a native macOS arm64 process (ad hoc linker signature, no special entitlement), the `write` syscall is emulated and prints `hello from x86-64 under FEXCore on macOS arm64`, `exit_group` ends with code 0. Raw log: `evidence/fexcore-darwin/runs/w01_hello-103618.log`. The other witnesses are not written or run yet.

## What exists on disk

Working root: `build/experiments/rocket-wine-fex-darwin-20261002` (written `$EXP`).

| Path | Contents | State |
| --- | --- | --- |
| `$EXP/src/fex-pristine` | intact copy of the FEX sources, made read-only (`chmod -R a-w`) | done |
| `$EXP/src/fex-darwin` | patched working copy | 13 files modified or added |
| `$EXP/src/patches/fexcore-darwin.patch` | `diff -ruN fex-pristine fex-darwin`, 997 lines | exported at each milestone (last export 10:25) |
| `$EXP/src/fexdarwin/build/fexcore` | FEXCore Ninja build directory (outside the patched tree) | compiled, libraries produced |
| `$EXP/src/fexdarwin/` (host sources) | `CMakeLists.txt`, `include/`, `src/`, `scripts/build.sh`, binary `build/host/fexdarwin-run` | first draft that runs witness (a) |
| `$EXP/logs/fexcore-configure-001.log`, `fexcore-configure-002.log` | CMake configurations (the second fixes the displayed FEX version) | kept |
| `$EXP/logs/fexcore-build-001.log` to `fexcore-build-006.log` | six successive builds: 57, 125, 6, 0 errors, then side libraries | kept |
| `$EXP/witnesses/elf-x86_64` | `src/rt.h`, `src/w01_hello.c`, `build.sh`, `run.sh`, `bin/w01_hello` | witness (a) built and run |
| `$EXP/evidence/fexcore-darwin` | macOS probes (sources and logs, being reconstructed) | partial |

The files `logs/hl-*`, `logs/host-*`, `tools/build-*.sh` and the directories `*darwin-memory`, `*darwin-process`, `scratch-hl` belong to the other team and are not touched.

Files of the current patch (taken from the header of `fexcore-darwin.patch`):

- `CMakeLists.txt`: accepts `Darwin`, sets `FEX_DARWIN_CORE_ONLY`, skips the `aarch64_fit_native.py` script that reads `/proc/cpuinfo` (`CMakeLists.txt:514` in the intact tree), skips `Source/`, `Data/binfmts`, `Data/AppConfig` and glibc jemalloc.
- `Data/CMake/LinkerGC.cmake`: `-dead_strip` instead of `--gc-sections --strip-all --as-needed` under Apple.
- `FEXCore/Source/CMakeLists.txt`: does not include `Utils/Allocator/64BitAllocator.cpp` (line 15 of the intact tree) nor the shared library under Apple.
- `FEXCore/include/FEXCore/Utils/TypeDefines.h`: adds `FEX_HOST_PAGE_SIZE` (16384 under Apple).
- `FEXCore/include/FEXCore/Utils/JITWriteScope.h` (new): RAII guard around `pthread_jit_write_protect_np`, with a per-thread nesting counter.
- `FEXCore/include/FEXCore/Utils/AllocatorHooks.h`: `malloc/malloc.h`, `MAP_JIT` for executable allocations, `MADV_ZERO` instead of `MADV_DONTNEED`, no THP.
- `FEXCore/include/FEXCore/Utils/PrctlUtils.h`, `SharedCodeBufferManager.cpp/.h`, `ThreadPoolAllocator.h`, `JIT.cpp`, `InternalThreadState.h`, `Dispatcher.cpp`: 16 KiB host pages for the guard pages, the interrupt page and the temporary buffers.

Not done yet (list of the next changes): `Allocator.cpp`, `AllocatorHooks.cpp`, `FEXHeaderUtils/Syscalls.h`, `Profiler.cpp`, `MiscOps.cpp` (`SYS_getcpu`), futexes of `WritePriorityMutex.h` and `InterruptableConditionVariable.h`, `SignalScopeGuards.h`, `Threads.cpp`, removal of `x18` from the JIT register allocator's registers, JIT write toggle at the places that modify code.

## Results measured on this machine

Machine: Apple M5 Max, macOS 27.0.1 (26A434), 16 KiB pages (`sysctl hw.pagesize` = 16384), Xcode 27.0, Apple clang 21.0.0. These measurements come from small C programs run in this session. Their sources and logs are being archived in `$EXP/evidence/fexcore-darwin/probes/` (the session's temporary directory was emptied by the interruption, the programs are rewritten identically).

| Question | Measured result | Consequence |
| --- | --- | --- |
| `mmap` with `MAP_JIT` without special entitlement | succeeds, binary ad hoc signed by the linker (`flags=0x20002(adhoc,linker-signed)`), with no entitlement and no hardened runtime | minimal signature sufficient for now |
| RWX `mmap` without `MAP_JIT` | fails, `errno` 13 | code buffers must be `MAP_JIT` |
| `mprotect` RW to RX on ordinary memory | succeeds and the code runs | possible fallback, slower |
| Memory below 4 GiB (for example 0x400000) | `mmap MAP_FIXED` fails, `errno` 12 | a classic x86-64 ELF linked at 0x400000 cannot be loaded |
| Shrink `__PAGEZERO` (`-Wl,-pagezero_size,0x4000` or `0`) | the process is killed at launch (exit code 137) | no workaround through linking |
| `mach_vm_deallocate(0, 4 Gio)` then `mach_vm_allocate` at 0x400000 | the release succeeds, the allocation fails (`KERN_INVALID_ADDRESS`) | the limit is in the VM map, not in an occupied region |
| Highest user address | page 0x7ffffdffc000, end at 0x7ffffe000000 | guest addresses are less than 47 bits minus 32 MiB |
| Free map (through fixed `mach_vm_allocate` without overwrite) | free: 0x110000000 to 0x180000000, 0x7000000000 to 0x7780000000, 0x7d80000000 to 0x7ffffe000000; occupied in between | most of the free space is near the top |
| `PROT_NONE` `MAP_NORESERVE` reservation of 2^46 bytes | succeeds; 2^47 fails | a very wide guest window can be reserved |
| Anonymous RW mapping of 2^40 bytes | succeeds | the page table of FEX's lookup cache can be allocated |
| `MADV_DONTNEED` | does not zero the page (remaining byte 0xcd) | FEX assumes zero fill, `AllocatorHooks.h:128` must change |
| `MADV_ZERO` | zeroes on RW, `EPERM` on `PROT_READ` | used as the equivalent, with a remap fallback |
| `MAP_32BIT` | fails, `errno` 12 | no help for low memory |
| `x18` under load and signals | 6826 values lost over 334453 iterations of 90 µs, value read 0 | `x18` cannot hold JIT state |

Memory faults and exceptions, measured by the probe `evidence/fexcore-darwin/probes/sig1.c` (log `probe-sig1-001.log`):

| Case | Signal | `si_code` | ESR syndrome |
| --- | --- | --- | --- |
| write to a `PROT_READ` page | SIGBUS | 1 | 0x92000047 |
| read or write to a `PROT_NONE` page | SIGBUS | 1 | 0x92000007 or 0x92000047 |
| access to an unmapped page, at address 0, non-canonical or above the high limit | SIGSEGV | 2 | 0x92000004 to 0x92000007 |
| execution of a non-executable page | SIGBUS | 1 | 0x8200000f |
| `ldxr`, `casal`, `ldadd`, `ldapr`, `stlr` straddling 16 bytes or a cache line | SIGBUS | 1 | 0x92000021 or 0x92000061 (alignment fault) |
| unaligned `ldaxr` within a 16-byte granule, unaligned `ldapr` within the granule, ordinary unaligned `ldr` or `ldp` | no fault | | |
| `hlt #0`; `udf #0`; `mrs ctr_el0` | SIGILL | 2 | |
| `brk #0` | SIGTRAP | 0 | |

Consequences: `si_code` does not separate permission and alignment, only the ESR does. For the Linux guest, a host SIGBUS (permission) must become SIGSEGV with `SEGV_ACCERR`, and a host SIGSEGV (unmapped) must become SIGSEGV with `SEGV_MAPERR`, which the host's VMA table makes it possible to decide.

Processor capabilities (`sysctl hw.optional.arm.*`): AES, PMULL, SHA1, SHA256, SHA512, SHA3, CRC32, LSE, LSE2, LRCPC, LRCPC2, FlagM, FlagM2, FCMA, JSCVT, DotProd, I8MM, BF16, RPRES, AFP, CSSC, FRINTTS, ECV, HBC, WFxT, SME and SME2. Absent from the list: ordinary SVE, RNG (RNDR), MOPS. `hw.cachelinesize` = 128.

## Reconnaissance: Linux dependencies of FEXCore

The FEXCore library itself has few kernel dependencies. The Linux layer is in `Source/Tools/LinuxEmulation`, outside the scope of this library. Table of the points found:

| Category | Location | Problem under macOS | Handling |
| --- | --- | --- | --- |
| Missing headers | `AllocatorHooks.h:9` (`malloc.h`), `MiscOps.cpp:9` (`syscall.h`), `Profiler.cpp:5` (`linux/magic.h`) | do not exist in the SDK | `malloc/malloc.h` (done), rest still to handle |
| Raw system calls | `MiscOps.cpp:270` (`SYS_getcpu`) | no `getcpu` call | still to handle |
| Futex | `WritePriorityMutex.h:276-294`, `InterruptableConditionVariable.h:42,85` | no futex | `os_sync_wait_on_address` (to do) |
| Signal mask | `SignalScopeGuards.h:161,174` (`rt_sigprocmask`) | no raw call | `pthread_sigmask` (to do) |
| `prctl` | `SharedCodeBufferManager.cpp:72` (MDWE), `Allocator.cpp:63`, `AllocatorHooks.cpp:43,128` | no `prctl` | removed or no effect |
| `/proc/self/maps` | `Allocator.cpp:220` | does not exist | `mach_vm_region` (to do) |
| `MAP_FIXED_NOREPLACE` | `Allocator.cpp:123,258` | flag missing | fixed `mach_vm_allocate` without overwrite |
| `madvise` | `Allocator.cpp:263` (`MADV_DONTDUMP`), `AllocatorHooks.h:128,146` | semantics or flag missing | `MADV_ZERO`, no effect for THP |
| `memalign`, `malloc_usable_size` | `AllocatorHooks.cpp:70` and following | missing | `posix_memalign`, `malloc_size` (to do) |
| Executable memory | `SharedCodeBufferManager.cpp:21` | RWX refused | `MAP_JIT` and `JITWriteScope` (done for the allocation) |
| Page size | `TypeDefines.h:9` (4096), `SharedCodeBufferManager.cpp:25`, `InternalThreadState.h:90,125`, `Dispatcher.cpp:37`, `JIT.cpp:917,923` | 16 KiB host pages | `FEX_HOST_PAGE_SIZE` (done) |
| Hard-coded 12-bit shift | `LookupCache.h:131,132,150,196,345,346,400,411`, `Dispatcher.cpp:158,195,204` | 4 KiB guest page granularity, consistent with `FEX_PAGE_SIZE` (guest) | left at 4 KiB, see the page analysis |
| Platform register | `Arm64Emitter.cpp:79,86` (`r18` in the dynamic pool) | macOS clobbers `x18` | remove `r18` (to do) |
| System registers | `CPUID.cpp:147` (`CNTFRQ_EL0`), `MiscOps.cpp` (`TPIDRRO_EL0`), `ALUOps.cpp:96,102` (`CNTVCT_EL0`) | `CNTVCT_EL0` readable at EL0, `TPIDRRO_EL0` holds something else under macOS | `SupportsCPUIndexInTPIDRRO=false` (to confirm) |
| RDTSCP and RDPID | `OpcodeDispatcher.cpp:5076,5090` | `HostType != Linux` without TPIDRRO: unsupported instruction | accepted, to document in witness (h) |
| CPU feature detection | `Source/Common/HostFeatures.cpp` (outside FEXCore) reads `ID_AA64*` through `mrs` | `mrs` of the ID registers triggers SIGILL at EL0 under macOS | replace with `sysctlbyname("hw.optional.arm.*")` in the host |
| In-place code rewriting | `JIT.cpp:535-646` (block links and unlinkers), `JIT.cpp:1163` (block copy), `Arm64.cpp:2106,2119-2160` (SIGBUS handler fixups) | write refused on `MAP_JIT` without toggle | `JITWriteScope` (to put in place) |
| `getrandom` | `Core.cpp:109-111`, `JIT.cpp:78` via `FHU::Syscalls::getrandom` | no call | `getentropy` (to do) |

## Wine's constraints on the address space (measured on the real preloader)

File read: `rocket-hyperlinux-20260928/linux-root/rootfs/usr/lib/wine/wine64-preloader.static` (sha256 `fafbb248c5214f116191f8c9298e38067873b55e8795b9de88f7b4775e7a6aab`, 17640 bytes, identical to `wine64-preloader`). Scripts and outputs: `evidence/fexcore-darwin/wine-preloader/` (pyelftools installed in the venv).

- The preloader of this Debian package is an x86-64 `ET_DYN` ELF linked at address 0 (entry 0x1000, four `PT_LOAD`, no `PT_INTERP`, one `.dynamic` section). It is therefore not linked near 0x7d400000 like the old `ET_EXEC` preloaders: in this version, the load address is chosen by the loader at launch time.
- Its `wine_preload_info` table (`.data` section, four 16-byte entries followed by two empty entries filled at run time for the range of the PE executable) requests these reservations:

| Entry | Start | End | Size |
| --- | --- | --- | --- |
| 0 | 0x10000 | 0x110000 | 1 MiB |
| 1 | 0x110000 | 0x68000000 | 1662.9 MiB |
| 2 | 0x7f000000 | 0x7fff0000 | 15.9 MiB (contains 0x7ffe0000, USER_SHARED_DATA) |
| 3 | 0x7ffffe000000 | 0x7fffffff0000 | 31.9 MiB |

- Measured consequence: entries 0, 1 and 2 are below 4 GiB, so impossible to reserve in a macOS arm64 process (see the measurement table). Entry 3 starts at 0x7ffffe000000, exactly the first address above the measured high limit of a macOS process (the last mappable page ends at 0x7ffffe000000): it would fail too. The preloader prints a `failed to reserve range` warning and continues on reservation failures, but Wine needs address 0x7ffe0000 for USER_SHARED_DATA (inferred from knowledge of Wine's code, not checked here on ntdll).
- So the guest = host identity cannot host Wine. A guest base offset is needed (analysis further down, dedicated section). The identity is still used for the first witness because it reduces the risk of porting the JIT.

## "Guest base offset" mode: analysis of the sites and of the cost (not implemented)

Goal: host address = GUEST_BASE + guest address, so that the guest has the low addresses (0x10000, 0x110000 to 0x68000000, 0x7ffe0000) that macOS refuses to an arm64 process. This analysis was done by reading the code (read-only agent, then spot checks on my side of `MemoryOps.cpp:587`, of the GDT read `OpcodeDispatcher.cpp:4210-4215`, of `FormContextAddress` and of the SMC CRC `Core.cpp:645-650`). The cost figures are estimates, no measurement. Paths relative to `FEXCore/Source/Interface/Core/` of the intact tree, `MO` = `JIT/MemoryOps.cpp`, `AO` = `JIT/AtomicOps.cpp`.

### Findings that change the design

1. Some IR `LoadMem` and `StoreMem` carry host addresses: the x87 stack slots (`IR/Passes/x87StackOptimizationPass.cpp:439,475,622`, `MO:352`) and the GDT read (`OpcodeDispatcher.cpp:4215`). A blind rewrite in the backend would break them. An IR flag is needed, or they must be converted to `LoadContextIndexed`.
2. With the defaults (`TSOEnabled=true`, `Config.json.in:577`), integer accesses use `ldapur`, `ldapr`, `ldar`, `stlur`, `stlr`, which have no register plus register form. `VectorTSOEnabled` and `MemcpySetTSOEnabled` are false. `SMCChecks=mtrack` disables `ValidateCode` and `MonoBackpatcherWrite`.
3. Apple cores have no SVE: about 34 lines of SVE emission are dead on this target.
4. There is no MemBase register despite the comment `JIT.cpp:957` (stale).

### JIT sites (45 functions or sites, 228 lines of memory emission)

| Group | Emitters | Current form | With the base | Lines |
| --- | --- | --- | --- | --- |
| G1 LoadMem, StoreMem, Prefetch, TSO vector halves | MO 704, 1639, 2466, 767, 1778 | `[Xn]`, `[Xn,#imm]`, `[Xn,Xm,ext]` | bare `[Xn]` becomes `[B,Xn]` (free), the others +1 ADD | 29 |
| G2 integer TSO | MO 790-825, 1800-1826 | `ldapur/stlur [Xn,#imm9]`, `ldapr/ldar/stlr [Xn]` | impossible to fold, +1 ADD | 20 |
| G3 pairs | MO 741, 1752 | `ldp/stp [Xn,#imm7]` | +1 | 10 |
| G4 Push, Pop | MO 1468, 1553, 1576, 1608 | pre or post-index on the guest RSP | no rewrite of the pointer possible, +1 or +2, affects the post-RA fusion | 16 |
| G5 vector elements | MO 1349, 1387, 1421 | `ld1/st1` lane, `ld1r`, `ldr q` | +1 | 20 |
| G6 masked and gather | MO 855, 959, 1057, 1170, 1268 | per-element ASIMD fallback | base folded into the existing `add`, at most +1 | 28 |
| G7 contiguous SVE | MO 733, 843, 1682, 1843, 1690, 1721 | `[Xn,#imm,MUL VL]` | dead on Apple | 12 |
| G8 atomics | AO 13 to 298 (CASPair, CAS, Swap, Add, Sub, And, CLR, Or, Xor, Neg) | `caspal/casal/swpal/ldadd... [Xn]`, `ldaxr/stlxr` fallback | impossible, +1 ADD before the LL/SC loop | 32 |
| G9 MemSet, MemCpy | MO 1851, 2086 | everything through TMP2 and TMP3 | the `mov` becomes `add`, +0 | 43 |
| G10 cache lines | MO 2391, 2421, 2445 | `dc civac/cvac/zva` | +1 | 6 |
| G11 non-temporal | MO 2504, 2530, 2546 | `stnp`, `stnt1b` | +1 | 7 |
| G12 ValidateCode | `BranchOps.cpp:323` | post-index `ldr` | the `mov` becomes `add`, +0 | 4 |
| G13 Thunk, MonoBackpatcherWrite | `BranchOps.cpp:293`, `MiscOps.cpp:351` | guest pointer passed to host code | ADD at the `mov`, the work is in C++ | 0 |
| G14 JIT callback entry | `Dispatcher/Dispatcher.cpp:504` | `str x0,[x2]` | `[B,x2]` free | 1 |

The other families (ALUOps, ConversionOps, EncryptionOps, MoveOps, VectorOps except masked) do not access guest memory: they only touch STATE, constant tables and the host stack. `ExitFunction` and the dispatcher's L1/L2 lookups use the guest RIP only as a key.

### Memory operand

`GenerateMemOperand` (`MO:587-613`) returns `[Xn]`, `[Xn,#imm]` or `[Xn,Xm,SXTX|UXTW|SXTW #s]`. On the IR side, `Addressing.cpp:54-151` picks scaled imm9 or imm12, or register plus register only in 64 bits without TSO and without shift, otherwise a chain of `add`. With a base B: bare `[Xn]` becomes `[B,Xn]`; the imm and register forms emit `add T,B,Xn` then use T (TMP1 or TMP2, free at the five callers). Estimated overhead per guest access: 0 for `[reg]` outside TSO or vector, +1 for `[reg]` in integer TSO (default case), +1 for `[reg+imm]`, +1 for `[reg+reg<<s]`. The fs and gs bases are added in the IR (`OpcodeDispatcher.cpp:4101,4115`), so TEB accesses cost +1 unless `fs_cached` is pre-biased.

### C++ dereferences of guest pointers (18 sites, 4 mandatory by default)

Decoder byte reads (`Frontend.cpp:131-133`, `143-147`), `AdjustAddrForSpecialRegion` (`Frontend.cpp:1364,1370-1371`), entry conversions to `DecodeLoop` (`Core.cpp:518,546`, `DiskCache.cpp:561`), full SMC CRC (`Core.cpp:645-650`), disk cache (5 sites, disabled by default), code cache (2), `MonoBackpatcherWrite` (`Core.cpp:1178,1180`), SHA read of `ThunkOp` (`OpcodeDispatcher.cpp:66-73`), Zydis (debugging). No change for the lookup cache (indexed by guest address only), the call-ret stack (host memory) or the interpreter's fallback functions. The unaligned access handler (`Utils/ArchHelpers/Arm64.cpp`) reads host addresses already in the registers, but it recognizes exact JIT sequences (for example `HandleCASPAL_ARMv8` walks 9 instructions): the base ADD must be placed before each sequence.

### Register budget

Current use (x86-64, excluding ARM64EC): x0 to x3 temporaries, x4 to x17 plus x19 and x29 for the static registers, x26 and x27 for PF and AF, x25 call-ret stack, x28 STATE, x20 to x24 and x30 dynamic pool (x18 removed on Apple). Best candidate for GUEST_BASE: x24, callee-saved, so preserved by the AAPCS64 ABI and by `preserve_all`, to load once after `PushCalleeSavedRegisters` and at the callback entry. It is neither in SRA nor in `SignalDelegatorConfig`. The dynamic pool would go from 7 to 6 (without x18) then 5 (without x24). Alternative without a reserved register: `orr T,Xn,#2^44` if the guest fits under 2^44, same overhead (+1) but without the free `[B,Xn]` cases.

### Cost and risks

About 70 sites: 45 JIT, 18 C++, 3 IR, 5 configuration or dispatcher. Easy: 13, medium: 46, hard: 5 (Push and Pop family with the post-RA fusion, 5th temporary for CASPair, host or guest distinction of `LoadMem`, h2g conversions of the fault handlers, SMC and system calls, thunk ABI). Estimate (not measured): +1 ALU operation on 75 to 90% of memory accesses, that is +10 to +15% dynamic instructions and +3 to +8% time on memory-heavy code. Risks: Push and Pop writebacks, `[Xn]`-only forms (LDP, LSE, exclusives, DC ZVA), rigidity of the LL/SC sequences, exhaustion of the CASPair temporaries, negative guest pointers that wrap onto B plus x (plan guard pages), B aligned on 4 GiB.
Alternatives studied: TBI can only ignore tag bits, masking does not move 0x10000 to a mappable address, a range check per access costs at least 3 instructions, a trap costs microseconds (acceptable only for isolated accesses such as 0x7ffe0000, not for 0x110000 to 0x68000000), `[B,Wn,UXTW]` is free but only for a guest under 4 GiB.

## Design decisions already made

1. FEX uses guest addresses as host addresses (guest code is read through `reinterpret_cast<const uint8_t*>(GuestRIP)` in `Core.cpp`). The segments of the guest ELF must therefore be mapped at their real virtual address in the macOS process. The test programs will be linked at a high address, in the measured free window.
2. `FEX_PAGE_SIZE` stays 4096: it is the guest page granularity (lookup cache, code page tracking). `FEX_HOST_PAGE_SIZE` (16384) covers what FEX protects itself on the host. Self-modifying code tracking will work at 16 KiB grain and over-invalidate (safe).
3. No port of FEX's 64-bit address space allocator. The host reserves the guest window (`PROT_NONE`) and manages the guest mappings itself.
4. JIT code memory: `MAP_JIT`, writes under `JITWriteScope`. The JIT output is first written to an ordinary temporary buffer (`JIT.cpp:917-1163`), then copied into the shared buffer: a single major write site for compilation.
5. Memory model: x86 memory ordering emulated by software barriers (default values `TSOEnabled=true`, `HalfBarrierTSOEnabled=true`, `Config.json.in:577`), because Apple's hardware TSO mode is not accessible to an ordinary process.

## Witness status

| Witness | Status | Evidence (raw log) |
| --- | --- | --- |
| (a) hello through write and exit_group | PASS | `runs/w01_hello-103618.log` |
| (b) integers and flags (9524 checks: 8/16/32/64-bit ALU, mul, div, shifts, rotations, bsf, bsr, popcnt, lzcnt, tzcnt, bt, strings, cmpxchg, xchg, lock) | PASS 9524/9524 | `runs/w02_int-104412.log`; Rosetta oracle `oracle/w02_int-rosetta-104410.log` PASS 9460/9460 |
| (c) SSE, SSE2, AVX, AVX2, x87 | NOT WRITTEN | |
| (d) TLS through arch_prctl and %fs, %gs | PASS 25/25 | `runs/w04_tls-104605.log` |
| (e) mmap, mprotect, munmap, brk, madvise (multiples of 16 KiB) | PASS 39/39 | `runs/w05_mem-104605.log` |
| (f) self-modifying code (20 checks: rewrite between calls, instruction of the current block, mprotect RW/RX, munmap then remap, MAP_FIXED over translated code, hot loop) | PASS 20/20 with `--smc=mtrack` (default, 210 write faults handled) and with `--smc=full`; with `--smc=none` the same binary FAILS (12/20), which proves that the witness detects the lack of invalidation | `runs/w06_smc-105705.log`, `w06_smc-smc=mtrack-105723.log`, `w06_smc-smc=full-105723.log`, `w06_smc-smc=none-105714.log` |
| (g) guest fault with SIGSEGV handler | NOT WRITTEN (guest signal delivery to do) | |
| (h) syscall clobbers, cpuid, rdtsc, xgetbv | PASS 50/50 (cpuid leaf 0xB returns zeros in FEX, recorded as an upstream limit) | `runs/w08_sys-104648.log` |
| (i) clone and futex | NOT WRITTEN | |
| Performance measurements | NOT RUN | |

## Next steps

1. Finish the FEXCore build fixes, produce `libFEXCore.a`, archive the logs.
2. Write the host `fexdarwin-run` (ELF loader, initial stack, system calls, signal delegate, capability detection).
3. Compile the witnesses with a private cross toolchain and get witness (a) first.
4. Extend to the next witnesses, update this report at each milestone.

## Note on the usage license of the FEX code

The `CLAUDE.md` file at the root of the FEX tree contains the sentence: "AI must not be used to generate code for contributions to this project." The patch produced here is a private, local fix of a research copy. It must not be submitted as is to the upstream FEX repository.

## Milestone log

- 2026-10-02 14:50: milestone 6, guest threads through clone and futex (`w10_threads` 13/13).
- 2026-10-02 14:47: milestone 5, `w03_simd` 109/109 identical to the Rosetta oracle; x87 pass fixed for the guest base.

- 2026-10-02 14:40: milestone 4, guest base offset in the JIT and the host; `w09_lowmem` 12/12 and all the earlier witnesses pass with an 8 TiB base (taken over by the main agent after the FEX agent stopped).

- 2026-10-02 09:43: copy of the FEX tree into `fex-pristine` (read-only) and `fex-darwin`.
- 2026-10-02 09:56: CMake configuration succeeded; first build: 57 errors of 3 types.
- 2026-10-02 10:08: first export of the patch (398 lines).
- 2026-10-02 10:44: milestone 3, witness (b) integers PASS 9524/9524 on FEX, tables validated by Rosetta (9460/9460). First trap found in my own tests: in extended asm clang numbers the tied input of a `"+r"` operand after the explicit inputs, a misplaced `%2` read the destination (bsr).
- 2026-10-02 10:46: (d), (e), (h) PASS. Measurements on the output of (h): cpuid vendor GenuineIntel, maximum leaf 22, brand "Apple Silicon", AVX, AVX2, FMA, F16C, AES, PCLMUL, BMI1, BMI2, ADX, SHA, FSGSBASE advertised, AVX-512 and RDRAND not advertised, XCR0 = 7.
- 2026-10-02 10:36: milestone 2, first witness run (hello). Measurement in passing: `mprotect` on a `MAP_JIT` region always fails with `EACCES` (probe `probe-jit2-001.log`), so the final guard page of the shared code buffer is not armed (fix in `SharedCodeBufferManager.cpp`).
- 2026-10-02 10:25: milestone 1, FEXCore and its dependencies compile (build-006). Library fingerprints in `evidence/fexcore-darwin/libs/sha256-libs-001.txt`.

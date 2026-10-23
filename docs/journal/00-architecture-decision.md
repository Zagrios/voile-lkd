# Architecture decision, October 2, 2026

Working document of the Wine and FEX line on Darwin. It sets down what is established, what is chosen, and how it is verified. The detailed reports are in the same folder. Each claim points to a report or a log; what is not measured is said.

## 1. Established facts

1. The installed runtime is entirely x86_64 under Rosetta. No arm64 PE, no arm64 slice of D3DMetal (`wine-macos-bridge.md` §0 points 1 and 3).
2. Under Rosetta, a raw `syscall` with a Linux number raises SIGSYS. Wine macOS redirects it to its NT dispatcher, and the Linux numbers overlap the NT identifiers (`wine-macos-bridge.md` §0 point 5). The EAC PE bootstrapper itself issues Linux `syscall`s (319, 1) from its image. The EAC module must therefore live in a process that offers a real Linux personality.
3. A macOS arm64 process maps nothing below 4 GiB. Shrinking `__PAGEZERO` makes the kernel refuse the execution. Wine needs 0x7ffe0000 (`USER_SHARED_DATA`) and its preloader needs 0x10000-0x68000000 and 0x7f000000-0x7fff0000 (`fexcore-darwin-port.md`, `wine-macos-bridge.md` §3). A guest at an address identical to the host cannot host Wine.
4. The arm64 user space goes up to 0x7ffffe000000; a `PROT_NONE` reservation of 2^46 bytes succeeds, 2^47 fails; the free space is mostly at the top (`fexcore-darwin-port.md`).
5. 16 KiB host pages; `mmap`, `munmap` and `mprotect` round to 16 KiB and refuse an unaligned address; a file offset of 0x1000 gives EINVAL (`hl-reuse-core.md` appendix A). Only an x86_64 target under Rosetta gets 4 KiB pages (`posix_spawnattr_set_4k_page_size_np` answers EBADMACHO for arm64, `darwin-memory-facts-a.md`).
6. `x18` is reset to zero by the kernel: the JIT cannot use it (`fexcore-darwin-port.md`).
7. FEXCore 0df84d3 compiles as a static library on macOS arm64 with 13 modified files (`fexcore-darwin-port.md`, milestone 1). No witness had been run yet at the time of this note.
8. HyperLinux has 40,617 lines of C. 25 objects out of 39 have no HVF symbol. A bench ran its real vfs, fd and procfs objects in an ordinary macOS process: 23 checks out of 23, without a VM (`hl-reuse-core.md` summary 3). Its user-space ptrace tracer/tracee relationship (`proc_ptrace.c`) passes 79 checks out of 79 on the host (`hl-reuse-process.md` summary 4).
9. The native `fork()` of macOS 27 holds in a multithreaded process with `MAP_JIT` (17 PASS), with orphaned locks to handle (`hl-reuse-process.md` summary 7).
10. EAC verdicts: the only non-physical environment accepted (code 301) is QEMU system x86 TCG, without authentication, in 3.5 s; all ARM stacks give 509/210 or 502 "Cannot run under Virtual Machine". The lack of authentication is a closed cause. A launch of the bootstrapper without credentials is therefore a valid discriminator (`eac-controls-digest.md` §0).
11. Observed contract of the module: fork from a worker thread, `PTRACE_TRACEME`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, `GETREGSET NT_PRSTATUS` of 216 bytes, `GETSIGINFO`, exec of the Wine loader by the child, `INT3` at the image entry, `POKE` and `SETREGSET` to inject a `dlopen("/proc/self/fd/N")`, `SIGSEGV` expected at address 9 (`eac-probes-digest.md` §0 points 2 and 5, §6).
12. The Wine 11 of voile-wine already contains the ARM64EC plumbing and a simulation of 4 KiB pages on a 16 KiB host; Wine's `xtajit64` interface is identical, name for name, to that of FEX's `libarm64ecfex.dll` (`wine-macos-bridge.md` §4).

## 2. Options compared

| Option | Principle | Verdict |
| --- | --- | --- |
| O1 | Wine macOS under Rosetta, ELF module in a separate FEX process | Rejected. The bootstrapper loads the module in its own process through raw `syscall`s that no external bridge sees, and the module must trace the real game process. |
| O2 | Native ARM64EC Wine on macOS, FEX as ARM64EC emulator, pocket Linux in the process for EAC | Kept as a later optimization. Best graphics and performance path, but requires porting arm64 Wine to macOS (`x18`, low zone, 16 KiB), housing glibc in the process, and routing between two personalities. |
| O3 | Complete x86-64 Linux userland (Linux Wine, glibc, EAC module) on FEXCore in a macOS arm64 process, with a Linux-on-Darwin kernel layer | Chosen. A single personality, Linux fidelity by construction, proof of existence in the QEMU path. |
| O4 | HyperLinux (Hypervisor.framework per process) | Excluded as long as the user has not accepted per-process virtualization. Read only for its contracts. |

## 3. Decision

O3, with these choices:

1. Engine: FEXCore ported to Darwin. Memory accesses from guest code add a base register: host address = `GUEST_BASE` + guest address. The host reserves the guest window once (PROT_NONE, MAP_NORESERVE, at most 2^46 bytes) and commits it in pieces. All dereferences of guest pointers by FEXCore and by the kernel layer go through a single function.
2. Pages: the guest sees 4 KiB pages. A VMA table keeps the exact view at 4 KiB; the host protection of a 16 KiB page is the OR of the protections of its subpages; file mappings at an unaligned offset are copied into anonymous memory for the private case. FEXCore ignores the guest execute bit, which removes the W^X problem of RWX unions.
3. Kernel layer (working name LKD): a new x86-64 syscall layer, called by the engine on each guest `syscall`. Modules taken from HyperLinux: rooted vfs, fd objects, procfs, task names, flag and errno translations, user-space ptrace tracer/tracee relationship. Elements taken from FEX: x64 structures, x86-64 signal frames, vDSO, VMA tracking. FEX's `LinuxEmulation` layer is not compiled on top of an "ARM64 kernel" library (recommendation of `hl-reuse-core.md`, against the opinion of mode A of `hl-reuse-process.md`; mode A remains the fallback if writing the x86-64 conversions turns out more expensive than planned).
4. Processes: a guest process is a host process. Guest `fork` = host `fork` with FEX's lock protocol. Guest `exec` = re-execution of the runtime. PID and TID come from a counter shared by the family, the TID of the main thread equals the PID.
5. Signals: per-task software queues and an inter-process mailbox; a single reserved host signal interrupts the JIT code; synchronous faults are translated into guest signals with the exact address.
6. ptrace: cooperative user-space emulation between two runtime processes. The real x86 state is in FEXCore's `CPUState`, so the 216-byte view is exact by construction. Code writes invalidate the tracee's translation cache before it resumes.
7. Graphics: Neptune client in Linux Wine, vtest transport, rendering in a separate macOS process (D3DMetal-native under Rosetta or DXMT-native arm64). The two are compared with witnesses before the game.
8. Display, input, sound: a Wine driver to write that talks to the macOS host over a socket, or a minimal X server; to be decided by the witnesses of `graphics-display-audio.md`.
9. No fabricated answer: each answer from `/proc`, from ptrace or from a syscall comes from a real state of the runtime. Unimplemented functions answer ENOSYS visibly.

## 4. Interfaces

Narrow boundary between engine and kernel, taken from `hl-reuse-process.md` §3.2: guest memory access (`user_read`, `user_write`, `user_strn`), syscall entry, signal delivery at safe points, fault reporting, engine services (registers, FP, code invalidation, thread spawn, frame construction and restoration), fork and exec points, ptrace stops. The reference header file is `src/lkd/include/lkd/lkd.h`.

## 5. Qualification ladder

Each rung has owned programs and recorded oracles (QEMU never rerun). A rung is passed only if the output matches the oracle.

1. Static x86-64 ELF without libc: loading, direct syscalls, TLS through `arch_prctl`, self-modifying code, guest fault.
2. Static ELF with libc (musl then glibc): malloc, threads, signals, files.
3. Dynamic glibc from the Debian sysroot: `ld.so`, `/bin/sh`, basic tools.
4. Processes: fork, exec, wait, pipes, Unix sockets, shared futex, signals between processes.
5. Wine `--version`, then `wineboot --init`, then `cmd /c echo`, then an owned PE: first witness of crossing PE, ELF, macOS.
6. x86 ptrace: the existing witnesses (76 checks of the Linux x86 oracle) then the EAC contract.
7. EAC bootstrapper without credentials: code 301 versus 509/210.
8. Graphics, input, sound: separate witnesses before the game.
9. Protected game: authenticated session, full match, reconnection, FPS and frame time.

## 6. Risks

1. Base offset in the JIT: modification of many FEXCore memory sites (atomics, vectors, `rep movs`). Mitigation: estimate by the FEX agent, then implementation behind an option.
2. 4 KiB on 16 KiB: Windows guard pages and file mappings at 4 KiB offsets. Mitigation: page witnesses on real Wine.
3. Asynchronous signals and inter-process mailbox (Wine server sends SIGUSR1 to clients).
4. Futex shared between processes, eventfd and epoll on kqueue (kqueue is not inherited by fork).
5. Cooperative ptrace and translation cache coherence after POKE.
6. EAC verdict: ARM has never produced 301; the VM refusal criteria are unknown. Mitigation: measure CPUID, `cpuinfo`, DMI and the auxiliary vector of the environment, without masking FEX's identity.
7. Performance: FEX without hardware TSO. Measure early on microprograms then on Wine.

## 7. Questions for the user

1. If the 4 KiB or 16 KiB wall blocked Wine irreparably, would they accept per-process Hypervisor.framework as a fallback? Not asked as long as no witness establishes this blocking.
2. Epic token: the permissions classifier refused to read the keychain. It is only required at the end (protected session). The user can write the file themselves with the `!` command given in the conversation, or launch the game from Obelisk with a lab engine.

# EAC probes digest: bootstrapper and genuine Linux module under Wine

Written on 2026-10-02. Read-only review of the existing probes. No launch of the game, of EAC, of QEMU or of UTM. A single write: this file.

## Conventions

Abbreviated paths:

- `P/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/probes`
- `R/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `D/` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/diagnostics` (returns from the Bazzite PC, from WSL2 and from the Windows PC)
- `GR` = `P/eac-general-regset-20261001/run-20261001T102102834594Z/eac.log`: the most advanced trace of the module (HyperLinux runtime `f3f7ca30`, 2026-10-01 10:21Z, 63,463 lines). The line numbers `GR:n` refer to it.
- `LOG-REGSET` and `LOG-STATUS` = `R/run/logs/eac-parent-ptrace-regset.log` and `R/run/logs/eac-parent-proc-status.log` (complete HyperLinux syscall traces, 35 MB each, ARM64 syscall numbering).

Status labels (the four imposed by the task):

- `observed on real Linux x86`: two distinct sources, always qualified. (a) Physical PC: Dell XPS 15 9500 under Bazzite, kernel 6.18.52, `systemd-detect-virt` = none (`D/rocket-bazzite-return-20260929/ANALYSE.md:31`). (b) Linux x86 kernel 6.12.107+deb13-amd64 under QEMU TCG: safe for kernel semantics, this is not x86 hardware (`P/ptrace-x86-full-context-control-20260929/README.md:7`).
- `observed under HyperLinux/Rosetta`: HyperLinux (HL) or Rosetta trace on OrbStack's real Linux ARM64 kernel (RLX). In this second case the kernel is real but the ptrace view is that of an ARM64.
- `observed under FEX`: no observation of this type exists in these probes. FEX is only read statically there (`P/fex-eac-feasibility.md`).
- `inferred`: deduction by the author of this digest, with the elements it rests on.

Structuring fact: no trace of EAC's syscalls was taken on a real Linux x86. On real Linux x86 we have (1) the bootstrapper and game logs (physical PC), (2) the memory maps and the process list of the 91.54 s protected session under QEMU TCG, (3) owned witnesses whose ptrace semantics were measured under QEMU TCG and, for two probes, on the physical PC. The exact order of EAC's requests is only known from the HyperLinux/Rosetta traces, up to the point where EAC gives up. Everything that follows this abandonment is inferred.

## 0. Quick read for the architecture decision

1. The PE bootstrapper (`RocketLeague_EAC.exe` 1.9.3) picks `linux64` under Wine, downloads an encoded module, decodes it into a memfd, finds libc by parsing `/proc/<pid>/maps`, calls `dlopen("/proc/self/fd/N")` in its own process, then lets the x86-64 ELF module (glibc, 230 libc imports) do the rest (`R/linux-reference/eac-static-210/analysis.md:15-19`, `maps-resolver.md:5-9`).
2. The module forks from a worker thread. The child traces itself (`PTRACE_TRACEME`), creates a memfd of 7,757,608 bytes without `CLOEXEC`, rereads `RocketLeague.exe`, then re-executes the Wine loader (target of `readlink("/proc/self/exe")`). The parent thread traces the exec, follows the preloader's syscalls until `prctl(PR_SET_NAME)`, sets an `INT3` on the main program's entry point, reads the tracee's memory to resolve a libc function, writes the string `/proc/self/fd/34`, rewrites the registers (rdi, rsi=1, rdx=0, rsp, rip) and resumes with a return address equal to 9. Interpretation: injection of a `dlopen` of the second ELF into the future game process (`inferred`, see §3.4).
3. On the physical PC the complete flow lasts about 0.5 s: `Starting Wine module mapping` at 11:51:52.026, launcher 301 at 11:51:52.546, `Terminating with: 0` at 11:51:52.570. The bootstrapper then exits; the game is reparented to `pv-adverb` (`D/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log:22-27`, `received/chronologie/process-tree-during-match.json`, `received/chronologie/lifecycle.jsonl:1`). Under QEMU TCG the game process contains the 7.76 MB memfd mapped as ELF (`P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1096`). The ptrace relationship is therefore short-lived, limited to the launch (`inferred` for the exact duration).
4. EAC requests `NT_PRSTATUS` with `iov_len` = 216, i.e. 27 words of 8 bytes: the size of the x86-64 `struct user_regs_struct`. Under Rosetta on an ARM kernel, the kernel returns the 216-byte prefix of a 272-byte ARM64 context (`P/regset-architecture.md:3,9`). An exact x86-64 view is therefore needed, with correct RIP, RSP, FS_BASE, RFLAGS, `orig_rax` and RCX/R11 (`P/ptrace-x86-feasibility-20260929.md:80-89`).
5. Register writes required: EAC does `SETREGSET` (RIP minus 1 after `INT3`, then call injection) and also reads `NT_PRFPREG` (512 bytes). Memory writes required: `POKEDATA` of executable code (`INT3`, then restore) into the tracee, with invalidation of the translation cache before resuming. Rosetta fails here (stale translation), this is HyperLinux's most recent blocker (`P/ptrace-remote-setregset-runtime-20261001/README.md:32-45`).
6. EAC reads `/proc/<tracee>/maps` of another process (stat, open, read) and, before that, the `/proc` files of its own parent. A Wine without a parent (PID 1, PPid 0) produces `502 / Launch Error: 256` in 39 ms even on a real kernel (`R/linux-reference/pid1-control/README.md`).
7. The module explicitly refuses a VM: WSL2 returns `502 / Cannot run under Virtual Machine.` 19 ms after the start of the mapping (`D/rocket-wsl-eac-return-20260929/ANALYSE.md:3,21-27`). QEMU TCG (`-cpu max`, machine `pc`, TCG) is accepted. The detection criteria are unknown.
8. No run got past 509/210 outside the physical PC. The last HL state: the injection is prepared and launched, the second stop arrives as `SIGTRAP` instead of an expected `SIGSEGV` at address 9 (inferred), EAC kills the tracee (`P/ptrace-general-regset-runtime-20261001/README.md:9`).
9. Reusable without QEMU: about thirty freestanding x86-64 witnesses (static ELFs already compiled, C or assembly source) and the recorded Linux x86 oracle outputs (§5).
10. FEX Linux refuses `TRACEME`, `SETOPTIONS`, `SYSCALL`, `GETREGSET` (EPERM) and only forwards PEEK/POKE/ATTACH/DETACH (`P/fex-eac-feasibility.md:7,13`, revision `0df84d3`). A Darwin port will have to provide the whole x86 tracing relationship (§6).

## 1. Launch flow of the genuine bootstrapper under Wine on Linux

### 1.1 Who does what

| Component | Observed or read role | Evidence | Status |
| --- | --- | --- | --- |
| PE bootstrapper `RocketLeague_EAC.exe` 1.9.3 (3,975,920 bytes, SHA-256 `903e7aba…14d1`) | Reads `EasyAntiCheat\Settings.json`, logs `System name: 'linux64'`, downloads `https://modules-cdn.eac-prod.on.epicgames.com/modules/<ProductId>/<DeploymentId>/linux64`, decodes, loads the module, relays the callbacks. | `R/linux-reference/eac-static-208/anticheatlauncher.log:2-8,30-33,94-100` | observed under HyperLinux/Rosetta (RLX), identical on PC (`D/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log:17-27`): observed on real Linux x86 (PC) |
| PE SysV bridge | Adapts the SysV ABI to Windows for the callback (`0x14004ba20`); `0x14001cc10` writes the bytes into a Linux descriptor, formats `/proc/self/fd/%i` and calls the native loader through `__libc_dlopen_mode`/`dlopen` then `__libc_dlsym`/`dlsym`. The PE code therefore executes native SysV calls in the same process. | `R/linux-reference/eac-static-210/analysis.md:15-18` | inferred (static reading of the PE) |
| PE libc resolver | `getpid`, reads `/proc/%i/maps` in 4096-byte blocks up to 5 MiB, splits lines and fields, keeps the first line containing `/libc-` or `libc.so` (case-insensitive), computes `start + file offset`, requires the ELF header (`e_machine=62`, `e_type=3`, `e_phentsize=56`) and a `PT_DYNAMIC`. Cache initialized once. | `R/linux-reference/eac-static-210/maps-resolver.md:5-9` | inferred (static); failure observed under HL (507/206) as long as libc was missing from `/proc/self/maps` (`docs/research/rocket-league-macos.md:142`) |
| Downloaded module | Non-ELF CDN file. Relation `encoded[i] = (plain[i] + plain[i+1] + 3*i) mod 256` (next byte zero after the end). Old: 9,681,573 bytes, SHA `ce28ef66…`, decoded ELF SHA `c6bf6c7d…`. Current: 13,825,695 bytes, SHA `74b6cab9…`, decoded ELF SHA `1b9faa54…` (ET_DYN x86-64, 11 PT_LOAD, 33 sections, exports `a b c d e`). | `P/eac-current-module-static-20260929/README.md:7-11`; `P/eac-native-module-capture-plan.md:7-9` | observed on real Linux x86 (PC: 13,825,695 bytes received); decoding verified outside execution |
| ELF module in the bootstrapper's Wine process | Worker thread (TID 76 in `GR`, 71/72 in the 2026-09-28/29 runs). Environment probes, fork, tracer role, network. | `P/eac-parent-status-transition/README.md:49-56`; `GR` | observed under HyperLinux/Rosetta (HL) |
| Child forked before exec | `prlimit64`, `prctl(PR_SET_DUMPABLE,0)`, `PTRACE_TRACEME`, `tgkill(self,self,SIGSTOP)`, creates and fills a memfd, rereads the game, `readlink`, `execve` of the Wine loader. | §2.4, §2.5 | observed under HyperLinux/Rosetta (HL) |
| Process after exec | New Wine process (`wine-preloader` then `wine64`), traced, which will become `RocketLeague.ex`. | `GR:61377-61403`; `D/rocket-bazzite-return-20260929/received/rapport.md:72` (name `wine64` then `RocketLeague.ex`) | observed under HyperLinux/Rosetta (HL) for the exec; observed on real Linux x86 (PC) for the final name |
| Second ELF (memfd 7,757,608 bytes) | Mapped as ELF in the game: segments r--p, r-xp, r--p, guard, r--p, rw-p, then r-xp at offset 0x21f000 (end 0x764000). | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1096` | observed on real Linux x86 (QEMU TCG) for the mapping; inferred for the origin (same size as the child's write: 0x765f28 ≈ 0x764000 + file tail) |
| Proton/Heroic ELF bridge (`easyanticheat_x64.so`, `PROTON_EAC_RUNTIME`) | Absent from the EOS 1.9.3 bootstrapper (strings not found in ASCII and UTF-16). The A/B runs with and without the Heroic component both give 509/210. No loading observed. | `P/eac-proton-runtime.md:7`; `P/eac-heroic-bridge-ab-20260929/RESULTATS-HEROIC.md:3-12,28` | observed under HyperLinux/Rosetta (RLX) |

### 1.2 Observed timeline

The timestamps come from the bootstrapper log; the ptrace sequences come from `GR`.

1. The PE loads `Settings.json`, prints `Loader initializing with bootstrapper version 1.9.3, architecture: x86-64`, then connects to the CDN (`eac-static-208/anticheatlauncher.log:30-33`). PC: HTTP 200, 13,825,695 bytes in 534 ms (`essai-01/eac-bootstrapper.log:26`). `observed on real Linux x86 (PC)`.
2. Wine loads `secur32`, `kerberos`, `netapi32`, `dnsapi`, `iphlpapi`, `nsi`, `rsaenh`, `schannel` on the Unix side (`GR:59092-61336`) for the download's TLS stack. `observed under HyperLinux/Rosetta (HL)`.
3. The PE executes in this order (complete HL trace): `memfd_create(name, 2)` (line 1011659), `write(fd, buf, 9 681 573)` of the old module (1011712), `getpid` (1011722), opening of `/proc/<pid>/maps` and reading until EOF (1011726 and following; `R/linux-reference/eac-static-210/maps-resolver.md:5`), all in `LOG-REGSET`. `observed under HyperLinux/Rosetta (HL)`.
4. `dlopen("/proc/self/fd/N")`: `link map` with base 0x7fffef6c2000, size 0x93d650, 12 program headers, `calling init`, successful open (`R/linux-reference/loader-trace/ge-eac.14:965-973`, described in `eac-ge-loader-analysis.md:9-14`). The memfd's 64-byte header starts with `7f454c46 02 01 01`, type 3, machine 62. `observed under HyperLinux/Rosetta (RLX)`.
5. The PE resolves the exports `a`, `b`, `c`, `d`, passes a callback pointer (`0x140034530`) and calls the native export (`0x1400347b8`) (`eac-static-210/analysis.md:18`). `inferred` (static).
6. Environment probes of the module in the process (§2.3), then `fork` (`clone` 0x1200011).
7. The tracer waits for the initial `SIGSTOP` stop, installs options 0x11 and follows the child's syscalls (§3.2). Window measured between the start of the mapping and the callback under HL: 0.7 to 0.9 s (`P/eac-ptrace-syscall-transition/report.md:26`, `P/eac-ptrace-regset-transition/report.md:43`).
8. PC: `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'`, 520 ms after the start of the mapping, then `waiting for game window`, `Terminating with: 0` 24 ms later (`essai-01/eac-bootstrapper.log:22-27`). `observed on real Linux x86 (PC)`.
9. Failures observed elsewhere: 507/206 (libc not found), 502/208 `Launch Error: 256` (parent unreadable), 502/208 `Cannot run under Virtual Machine.` (WSL2), 509/210 `Unexpected error. (#1)` (all Mac runs). Event table: the switch `0x140030c10` subtracts 501, 507 to 206, 509 to 210 (`eac-static-210/analysis.md:15-16`).

### 1.3 Process tree by environment

| Environment | Tree | Evidence | Status |
| --- | --- | --- | --- |
| HL, run `GR` | PID 1 = shell `dash` (parent of Wine), PID 2 = the bootstrapper's Wine (`wine-preloader`), TID 76 = tracer thread, PID 80 = tracee (leaf, leader), wineserver and services in PID 3 to 70. Parent = worker, child = leader. | `GR:61379` (stat `/usr/bin/dash`), `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` (section "Current sequence") | observed under HyperLinux/Rosetta (HL) |
| RLX Cachy, 509/210 | bootstrapper PID 2863, `RocketLeague.ex` PID 2881 with parent 2863; this process runs the game's startup code (thread names `PoolThread`). | `P/eac-cachyos-process-trace-20260929/RESULTATS.md:9-21` | observed under HyperLinux/Rosetta (RLX) |
| PC Bazzite | `pv-adverb` (9065) is the parent of both `wineserver` (9161) and `RocketLeague.ex` (9268). The bootstrapper (8539) had already exited. | `D/rocket-bazzite-return-20260929/received/chronologie/process-tree-during-match.json`, `lifecycle.jsonl:1` | observed on real Linux x86 (PC) |
| QEMU TCG, protected session | A single `RocketLeague.ex` (PID 347) at 684 s with `wineserver`, `services.exe`, `winedevice.exe`, `explorer.exe`. `launcher_code: 0`. | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/process-status.json` | observed on real Linux x86 (QEMU TCG) |

Consequence (inferred): the tracee becomes independent when the bootstrapper exits. No trace shows a persistent ptrace link during the match; no read of the game's `TracerPid` exists.

### 1.4 What is not established

- The order of EAC's ptrace requests on real Linux x86 (never traced).
- The content of the second ELF before it is written into the memfd; the argv and the environment passed to the `execve` (not logged).
- The VM detection mechanism, the reason for 509/210, the exact function called by the injected call (identified only by address).
- The hypothesis that the loader's name decides what follows: see §3.7.

## 2. Observed operations, by component

Convention: column `Oracle` = file that contains the Linux x86 kernel semantics for this operation, when it exists (otherwise `none`). The syscall numbers are those of Linux x86-64. The HL traces use ARM64 numbering; the correspondence was established here.

### 2.1 PE bootstrapper, module loading step

| # | Operation | Argument pattern | Expected or observed result | Component | Status | Evidence and oracle |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | `newfstatat`/path opens | `/game/Binaries/Win64/RocketLeague.exe.eac` (then the variant `…exe.eac?`) and the directory `/game/Binaries/Win64` | result not logged; no `.eac` file is kept in the prefix (`payloads: []`) | PE | observed under HyperLinux/Rosetta (HL) | `GR:59071-59089`; `P/eac-current-payload-20260929/README.md:20`. Nature (module cache): inferred |
| 1 | `getpid` (39) | none | pid of the Wine process | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011722`; `maps-resolver.md:5`. Oracle: none |
| 2 | `openat` (257) | `AT_FDCWD`, `/proc/<pid>/maps`, `O_RDONLY` | fd; Linux content (≥6 fields per line, field 0 `start-end`, field 2 hexadecimal offset, path from field 5 on) | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011726-1011727` (63,226 bytes returned by HL); `maps-resolver.md:5-9`. Oracle: remote maps semantics in `P/ptrace-remote-maps-control-20261001/qemu-20261001T000419470178Z` |
| 3 | `read` (0) then `close` (3) | 4096-byte blocks, 5 MiB cap | reads until EOF; libc in the first segment (offset 0) | PE | inferred (static) | `maps-resolver.md:5,7` |
| 4 | `memfd_create` (319) | random name, flags `2` (`MFD_ALLOW_SEALING`) | fd ≥ 3 (32 in this trace) | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011659` (`syscall 279(…, 0x2, …)`) |
| 5 | `write` (1) | the memfd's fd, decoded buffer, 9,681,573 bytes (old) or 13,825,695 (current) | bytes written in one go | PE | observed under HyperLinux/Rosetta (HL) | `LOG-REGSET:1011712` (`syscall 64(0x20, …, 0x93baa5)`); `GR:61343` (`mapped_elf_sha256`, `file_size` 13,825,695) |
| 6 | `dlopen("/proc/self/fd/N")` | `RTLD_*` not logged | libc resolves the magic link, `openat` + `fstat` + private PROT_EXEC `mmap` of the memfd, `mprotect`, init | PE via libc | observed under HyperLinux/Rosetta (RLX) | `R/linux-reference/loader-trace/ge-eac.14:965-973`. `GR:61343`: guest fd 34, host 56, offset 0, `nlink` 0 |
| 7 | `dlsym` (no syscall) | `a`, `b`, `c`, `d` | export addresses | PE | inferred (static) | `eac-static-210/analysis.md:18` |

### 2.2 Module, first environment probes (main process of the bootstrapper)

All the rows of this table are `observed under HyperLinux/Rosetta (HL)`. HL's procfs files are synthesized by the emulator; their bytes are not those of a kernel.

| # | Operation | Argument pattern | Observed result | Evidence |
| --- | --- | --- | --- | --- |
| 8 | `getppid` (110) | none | 1 | `P/eac-parent-status-transition/after-status-context.txt:1014804-1014805` |
| 9 | `openat` + `read` + `close` | `/proc/<own pid>/status`, read of 4096 bytes | 166 bytes returned by HL | `P/eac-parent-proc-comparison-after-status-final/windows.txt` (original line 1062313-1062322) |
| 10 | same | `/proc/<ppid>/comm`, read of 31 bytes | 3 bytes | same, 1062383-1062387 |
| 11 | `readlinkat` (267) | `/proc/self/exe`, 4096 buffer | 37 bytes (Debian: `/usr/lib/wine/wine64-preloader.static`) or 56 bytes (GE: `…/x86_64-unix/wine-preloader`) | same, 1062446-1062448; `P/eac-readlink-boundary-20260929/README.md:3` |
| 12 | `readlinkat` | `/proc/<ppid>/exe` then `/proc/<pid>/exe`, then `newfstatat` (262) on each target | 13 bytes (`/usr/bin/dash`); 37 or 56 bytes; stat 0 | same, 1062453-1062467; `GR:61379` |
| 13 | `openat` + `read` + `close` | `/proc/<ppid>/cmdline`, 31 bytes | 31 bytes (ENOENT before the bb2f fix) | same, 1062476-1062480; `P/eac-parent-proc-comparison.md:11-17` |
| 14 | `openat` + `read` + `close` | `/proc/<ppid>/status`, 31 bytes | 31 bytes (ENOENT before the 'status' fix) | `after-status-context.txt:1014815-1014819`; `windows.txt` 1062511 |
| 15 | `openat` | `/tmp/eac.flag` | ENOENT, no known effect | `windows.txt` 1062538; `P/eac-parent-proc-comparison.md:18,22` |
| 16 | `openat` + `close` | `/proc/self/maps` | fd 131 then closed; 67,510 bytes generated, no read seen on this open | `after-status-context.txt:1015491-1015495` |
| 17 | `newfstatat` | path not logged | ENOENT | `after-status-context.txt:1015549-1015550` |
| 18 | `openat` | `/sys/devices/virtual/dmi/id/board_name` | ENOENT | `after-status-context.txt:1015616` |
| 19 | `openat` | `/sys/devices/virtual/dmi/id/product_name` | ENOENT | `after-status-context.txt:1015646` |
| 20 | `openat` | `/proc/bus/pci/devices` | ENOENT | `after-status-context.txt:1015664` |
| 21 | path walk | `stat` of `/opt`, `/opt/ge-proton`, …, `…/wine-preloader`, `readlink` of the components (EINVAL expected for a non-link) | success | `GR:61387-61399`; `P/eac-parent-status-transition/README.md:25-26` (`LOG-STATUS:1015725-1015756`) |
| 22 | `openat` | `/proc/mounts` (attribution to the module not established: Wine also reads this file) | opened | `GR:61385` |
| 23 | `openat` | `/usr/lib/x86_64-linux-gnu/gconv/UTF-32.so` (`iconv` conversion) | opened | `GR:61401` |
| 24 | `openat` | `<dirname of /proc/self/exe>/EasyAntiCheat\Certificates\runtime.conf` (literal backslash separators) | ENOENT under HL; opened with an overlay of the game's file, without changing 509/210 | `LOG-STATUS:1016121`; `GR:61403`; `P/eac-runtime-conf-availability-20260929/README.md:15-24` |
| 25 | `newfstatat` | `/game/Binaries/Win64/RocketLeague.exe` | success | `GR:61345` |

The strings `hidepid`, `debugger` and `VM` exist in the module without evidence that these branches are taken (`R/linux-reference/umu/eac-native-elf-capture/README.md:43`).

### 2.3 Fork and child before exec

| # | Operation | Argument pattern | Observed result | Component | Status | Evidence and oracle |
| --- | --- | --- | --- | --- | --- | --- |
| 26 | `clone` (56) = `fork` | flags `0x1200011` (`CHILD_SETTID` + `CHILD_CLEARTID` + `SIGCHLD`) | child pid (75, 80, depending on the run) | MOD, worker thread | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-target-role-20260929.md:9`; `P/eac-ptrace-transition/report.md:9` |
| 27 | `wait4` (61) | `(child, &status, 0, NULL)` called before the child has started | blocks until the `0x137f` stop | MOD | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-transition/report.md:9` line 1014380; oracle: `P/ptrace-signal-two-int3-control-20260930/qemu-20260930T142310221768Z/manifest.json` (`first real SIGSTOP` = 4991) |
| 28 | `prlimit64` (302) | not decoded | 0 | child | observed under HyperLinux/Rosetta (HL) | `P/eac-parent-status-transition/README.md:61-62` |
| 29 | `prctl` (157) | `PR_SET_DUMPABLE` (4), 0 | 0 | child | observed under HyperLinux/Rosetta (HL) | `P/eac-parent-status-transition/README.md:62-63`; dumpable semantics: `P/ptrace-peek-control-20260930/README.md` (QEMU 375/375) |
| 30 | `ptrace` (101) `PTRACE_TRACEME` | `(0,0,0,0)` | 0 | child | observed under HyperLinux/Rosetta (HL) | `GR:61407`; oracle: all the `ptrace-*-control-*` |
| 31 | `gettid` (186), `getpid` (39) | none | tid, pid of the child | child | observed under HyperLinux/Rosetta (HL) | `P/eac-x86-ptrace-exec-diagnostic-20260929/README.md:7`; `GR` (clobber seq 6-7) |
| 32 | `tgkill` (234) | `(pid, tid, SIGSTOP=19)` on itself | 0; the child stops, signal-delivery stop, status `0x137f` | child | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-options-transition/report.md:11`; oracle: `P/ptrace-x86-tgkill-context-control-20260929/results-20260929T165852960827Z` (272/272, five stops, no tgkill exit stop before the first syscall stop) |
| 33 | `mmap` (9) | `NULL, 0x766000, PROT_READ\|PROT_WRITE, MAP_PRIVATE\|MAP_ANONYMOUS, -1, 0` | buffer for 7,757,608 bytes rounded up | child | observed under HyperLinux/Rosetta (HL) | `GR` regset stop_seq 2 (rsi 0x766000, rdx 3, r10 0x22) |
| 34 | `memfd_create` | name, flags `2` | fd 33, 34 or 35 depending on the run | child | observed under HyperLinux/Rosetta (HL) | `GR` stop_seq 4/5 (rax 34); `P/eac-ptrace-regset-transition/report.md:21` |
| 35 | `write` | fd, buffer, 7,757,608 (0x765f28) | complete write | child | observed under HyperLinux/Rosetta (HL) and (RLX) | `P/eac-ptrace-target-role-20260929.md:7` (RLX, `write(fd=3, 0x765f28)`); `P/eac-x86-ptrace-tgkill-20260929/README.md:15` (HL) |
| 36 | `fcntl` (72) | `F_ADD_SEALS` (1033), `0xf` (SEAL \| SHRINK \| GROW \| WRITE) | EINVAL under HL. On a real kernel EAC's call is not traced; the kernel implements seals and the A/B control that allows or refuses the command through seccomp gives 509/210 in both cases | child | observed under HyperLinux/Rosetta (HL) for EINVAL; result on a real kernel: not observed | `P/eac-ptrace-regset-transition/report.md:23,35-37`; `docs/research/rocket-league-macos.md:270-272` |
| 37 | `fcntl` | `F_GETFD` (1) on the memfd | 0 (no `FD_CLOEXEC`, so inherited across exec) | child | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:24` |
| 38 | `openat` | `/game/Binaries/Win64/RocketLeague.exe`, `O_RDONLY` | fd | child | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:25`; `GR` (fs openat) |
| 39 | `fstat`, `lseek`, `read`, `close` | `fstat` ×2; `lseek(fd, 0x2512000)` then `read` of 2488 bytes; `lseek(fd, 0)` then `read` of 4096 bytes | successful reads. 0x2512000 + 2488 = 0x25129b8 = file size (38,873,528 bytes): read of the end of the file | child | observed under HyperLinux/Rosetta (HL) | `P/eac-ptrace-regset-transition/report.md:26-27`; size measured in `R/game/Binaries/Win64/RocketLeague.exe`; the PE's Authenticode table starts at 0x2510000 (10,680 bytes). Interpretation (signature check): inferred |
| 40 | `readlinkat` (267) | `AT_FDCWD`, `/proc/self/exe`, 256 buffer | Wine loader path, 56 bytes (GE) or 37 bytes (Debian), not truncated | child | observed under HyperLinux/Rosetta (HL) | `GR` (`readlink_boundary`); `P/eac-readlink-boundary-20260929/README.md:3` |
| 41 | `newfstatat` (262) | `<dirname of the loader>/wine64` | 0 (GE) | child | observed under HyperLinux/Rosetta (HL) | `GR` fs pathop `…/x86_64-unix/wine64` (just before the exec) |
| 42 | `execve` (59) | Wine loader path; argv/envp not captured | ptrace exec event, status `0x4057f` | child | observed under HyperLinux/Rosetta (HL) | `GR` (clobber seq 24, fs resolve `wine-preloader`); oracle: `P/ptrace-postexec-exit-control-20260930/qemu-20260930T084834023780Z/manifest.json` |
| 42b | Debian path: `close(memfd)`, `munmap`, `exit_group` (231) code 1 | after the 37-byte `readlink` | the child exits without exec | child | observed under HyperLinux/Rosetta (HL) | `P/eac-readlink-boundary-20260929/README.md:9`; `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` (table, row "Child continuation") |

### 2.4 Tracee after exec (Wine loader then ld.so)

These calls are those of an ordinary Wine; they are listed because EAC observes them one by one with `PTRACE_SYSCALL`.

| # | x86-64 sequence | Evidence | Status |
| --- | --- | --- | --- |
| 43 | Preloader: `arch_prctl` (158), `mmap` ×4, `open` (2), `read`, `mmap`, `mprotect` (10), `mmap` ×3, `close`, `open`, `read`, `mmap`, `mprotect`, `mmap` ×4, `close`, `prctl(PR_SET_NAME)` (157): 23 syscalls | `P/eac-postexec-exit-comparison-20260930/README.md:19`; `GR` (clobber generation 3, seq 1-23) | observed under HyperLinux/Rosetta (HL) |
| 44 | ld.so: `brk` (12), `access`, 12 `openat` interleaved with 11 `newfstatat` (library search), `read`, `pread64`, `fstat`, `mmap`, `arch_prctl`, `set_tid_address`, `set_robust_list`, `rseq`, `mprotect`, `prlimit64`, `getrandom`, `brk` ×2, `openat`/`read`/`mmap` of the libraries, `futex` | `GR` (clobber generation 3, seq 24-111) | observed under HyperLinux/Rosetta (HL) |

### 2.5 Tracer thread (module), ptrace requests and waits

The ordered detail is in §3.2. The table gives the shapes.

| # | Operation | Argument pattern | Result | Status | Evidence and oracle |
| --- | --- | --- | --- | --- | --- |
| 45 | `ptrace(SETOPTIONS 0x4200)` | `(pid, 0, 0x11)` = `TRACESYSGOOD` \| `TRACEEXEC` | 0 | observed under HyperLinux/Rosetta (HL) | `GR:61417`; `P/ptrace-options-contract.md:3`; oracle `P/ptrace-options/results/` (14/14) |
| 46 | `ptrace(SYSCALL 24)` | `(pid, 0, 0)`; signal 0 | 0; stops `0x857f` | observed under HyperLinux/Rosetta (HL) | `GR:61838`; `P/ptrace-syscall-contract.md:7-13` |
| 47 | `ptrace(GETREGSET 0x4204)` | `(pid, NT_PRSTATUS=1, &iovec{base, len=216})` | 216 bytes copied; 27 x86-64 words | observed under HyperLinux/Rosetta (HL) | `GR:62027`; oracle: `P/ptrace-x86-full-context-control-20260929/results-20260929T161941227190Z/manifest.json` (272/272) and PC probe `D/rocket-bazzite-return-20260929/received/sondes/results-host/results.json` (13/13, request of 4096, return 216) |
| 48 | `ptrace(GETREGSET)` | `NT_PRFPREG` (2), 512-byte iovec | 512-byte FXSAVE (EINVAL before candidate `77450112`) | observed under HyperLinux/Rosetta (HL) | `GR:62868`; `P/ptrace-fpregset-runtime-20261001/README.md:5-7`; oracle: `P/ptrace-fpregset-control-20261001/qemu-*` (43/43) |
| 49 | `ptrace(SETREGSET 0x4205)` | `NT_PRSTATUS`, 216 bytes | 0 under the candidate; EIO or EINVAL otherwise | observed under HyperLinux/Rosetta (HL) | `GR:62033,62878`; oracle: `P/ptrace-setregset-control-20261001/qemu-20261001T063637525656Z/manifest.json` (76/76 per mode) |
| 50 | `ptrace(GETSIGINFO 0x4202)` | `(pid, 0, &siginfo)` after the exec event | 128 bytes; EAC calls it once | observed under HyperLinux/Rosetta (HL) | `GR:61492`; PC oracle: `D/rocket-bazzite-return-20260929/ANALYSE.md:49` (30/30); QEMU: `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (signo 5, code 128) |
| 51 | `ptrace(PEEKDATA 2)` | `(pid, addr, &word)`; 164 calls in `GR` | one 8-byte word copied to `data`, raw return 0 | observed under HyperLinux/Rosetta (HL) | `GR:61858` and following; oracle: `P/ptrace-peek-control-20260930/qemu-20260930T110720674039Z/manifest.json` (375/375) |
| 52 | `ptrace(POKEDATA 5)` | `(pid, addr, 8-byte word)`; 7 calls | 0; write into an executable page of the tracee | observed under HyperLinux/Rosetta (HL) | `GR:61908`; oracle: `P/ptrace-poke-int3-control-20260930/qualification.json` (37/37); `P/ptrace-remote-poke-control-20261001/qemu-20261001T061756029742Z` |
| 53 | `ptrace(CONT 7)` | `(pid, 0, sig)`; sig 0 except once 5 (SIGTRAP forwarded) | 0 | observed under HyperLinux/Rosetta (HL) | `GR:61913,62884,63027` |
| 54 | `kill` (62) | `(pid tracee, SIGKILL)` twice | 0 | observed under HyperLinux/Rosetta (HL) | `GR:63046,63053`; `P/eac-remote-maps-finding-20260930/README.md:3` |
| 55 | `wait4` (61) | `(pid, &status, 0, NULL)` | statuses: `0x137f` (4991), `0x857f` (34175), `0x4057f` (263551), `0x57f` | observed under HyperLinux/Rosetta (HL) | `GR` (`wait4`, `status` events); oracle: `P/ptrace-tracer-window-control-20260930/README.md:5` |
| 56 | `newfstatat`, `openat`, `read` ×2, `close` | `/proc/<tracee>/maps`, read in 4096-byte blocks, two rereads after the `INT3` | 13 bytes of path (`/proc/80/maps`), contents of the tracee's VMAs | observed under HyperLinux/Rosetta (HL) | `GR:61848,62050,62061`; `P/eac-remote-maps-finding-20260930/README.md:3-7`; oracle: `P/ptrace-remote-maps-control-20261001/qemu-20261001T000419470178Z` (268/268) |

### 2.6 Module thread after the failure: network, files, threads

These operations come from the 783 `tracer_syscall_window` events of `GR` (entries and exits; odd `record` field = entry; lines 61838 to 63461) and are `observed under HyperLinux/Rosetta (HL)`. They follow the `kill` of the tracee (records 405 and 407). It is not known whether the success path executes them too.

| # | Operation | Pattern | Remark |
| --- | --- | --- | --- |
| 57 | `clock_gettime` (228), `rt_sigprocmask` (14) | blocks framing each internal queue message | The internal messages go through a descriptor pair 26/27: `write(27, buf, 64)` and `read(26, buf, 64)` between `SIG_BLOCK` and `SIG_SETMASK`. `GR:63057-63461` (records 409 and following) |
| 58 | `uname` (63) ×3 | user buffer | values not logged. `GR` (records 447, 449, 451) |
| 59 | `openat`, `fstat`, `lseek`, `read` (435 bytes), `close` | path not logged | small text file (unknown nature). records 421-433 |
| 60 | `newfstatat` | `/sys/class/dmi/id/sys_vendor`, `/sys/class/dmi/id/product_name`, `/run/host/container-manager`, `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/etc/resolv.conf`, `/etc/nsswitch.conf` | container and platform detection. `GR:63084-63123` (`tracer_stat_path`); `GR:63095-63127` |
| 61 | `openat` | `/etc/hosts` | name resolution. `GR:63127` |
| 62 | `socket` (41) | non-blocking `AF_INET` datagram, then raw `AF_NETLINK` (16), then `AF_INET` stream | UDP DNS, netlink for `getaddrinfo`, TCP. records 471-507 |
| 63 | `connect` (42), `poll` (7), `sendto` (44), `recvfrom` (45), `ioctl FIONREAD` (0x541b), `setsockopt` (54), `getsockopt` (55), `fcntl` | 35-byte DNS query, 182-byte ClientHello, TLS exchanges | the module's network stack, through libc. records 473-627 |
| 64 | `epoll_create` (213), `epoll_ctl` (233), `epoll_wait` (232) | 50 descriptors max, 2500 ms timeout | records 517-523 |
| 65 | `getrandom` (318), `time` (201) | | TLS |
| 66 | `munmap` | frees buffers of 13,828,096 and 14,221,312 bytes | records 631, 701 |
| 67 | `exit` (60) | end of the thread | record 783 |

### 2.7 Signals

| Signal | Where | Evidence | Status |
| --- | --- | --- | --- |
| `SIGSTOP` (19) self-sent by `tgkill` | child, before any `PTRACE_SYSCALL`; status 4991 | §2.3 #32; `GR:61416` | observed under HyperLinux/Rosetta (HL) |
| `SIGTRAP` (5), `si_code` 128 (`SI_KERNEL`), `si_addr` 0, status `0x57f` | `INT3` set by the tracer | oracle `P/ptrace-signal-two-int3-control-20260930/qemu-20260930T142310221768Z/manifest.json`; PC `D/rocket-bazzite-return-20260929/ANALYSE.md:49` (si_code 128) | observed on real Linux x86 (QEMU TCG and PC) for the semantics |
| `SIGKILL` (9) | tracer against the tracee, after failure, twice | §2.5 #54; `GR:63046,63053` | observed under HyperLinux/Rosetta (HL) |
| `SIGTRAP` forwarded by `CONT(…, 5)` | tracer, after the second stop | `GR:63027` | observed under HyperLinux/Rosetta (HL) |
| `SIGSEGV`, status 2943 (`0xb7f`) | tracee, after the forwarded `SIGTRAP`; under HL only | `GR:63033` | observed under HyperLinux/Rosetta (HL) |
| `SIGSEGV` at address 9 expected on return from the injected call | tracee | return address written by `POKEDATA` (value 9) at `GR:62873` | inferred |
| `SIGCHLD` | implicit, stops of the child; the module only does `wait4` | no measurement | inferred |

## 3. ptrace: roles, sequence and requirements

### 3.1 Roles and reasons

- Tracee: child forked by the module (PID 80 in `GR`, leader). It traces itself with `PTRACE_TRACEME`. Linux then attaches the real parent, that is the thread that created the child.
- Tracer: the module's worker thread (TID 76 of PID 2). Only this thread can issue the requests. HyperLinux had to implement this rule: `TRACEME` binds the child to the creating TID, another thread of the same process cannot take it over (`P/ptrace-runtime.md:21`).
- Reason (inferred): launch the game as a traced child of the bootstrapper, to inject the second ELF into a fresh Wine process before its user code, without `LD_PRELOAD`. Grounds: memfd inherited across the exec (§2.3 #37), `POKEDATA` of the string `/proc/self/fd/34` (§3.2 step 21), mapping of the memfd as ELF in the game under QEMU TCG (§1.1).
- Options: exactly `0x11`. Neither `PTRACE_O_EXITKILL` nor `TRACECLONE` is requested (`GR:61417`, `P/ptrace-options-contract.md:3`).
- `GETEVENTMSG` is never called by EAC in these traces (0 occurrences out of 265 `ptrace` calls). `GETSIGINFO` is called once, `SEIZE`/`INTERRUPT`/`ATTACH`/`DETACH`/`SINGLESTEP`/`GETREGS`/`PEEKUSER`/`POKEUSER` never. Count per request in `GR`: `PEEKDATA` 164, `SYSCALL` 52, `GETREGSET` 32, `POKEDATA` 7, `CONT` 4, `SETREGSET` 3, `TRACEME` 1, `SETOPTIONS` 1, `GETSIGINFO` 1. The other runs of 2026-09-30 and 10-01 add no request outside this list.

### 3.2 Measured sequence (run `GR`, tracee PID 80)

Each row cites the line of `GR` (or the `stop_seq` field of the `ptrace_regset` event). Status of the whole table: `observed under HyperLinux/Rosetta (HL)`, unless stated. The register values come from the 27 words returned to EAC (`ptrace_regset` events), the request arguments from `ptrace_provenance` (`x86_args`) and from `tracer_syscall_window`.

| Step | Request and context | Measured detail | Line |
| --- | --- | --- | --- |
| 1 | `TRACEME` in the child | all arguments zero | 61407 |
| 2 | `wait4` returns the initial stop | status 4991 (`0x137f`, SIGSTOP) | 61416 |
| 3 | `SETOPTIONS` | data = 17 (`0x11`) | 61417 |
| 4 | `SYSCALL` then `GETREGSET` | first syscall stop (status 34175 = `0x857f`): `mmap` ENTRY, `rax` = -38, `orig_rax` = 9, RCX = RIP, `rsi` = `0x766000` | stop_seq 2; wait4 61424 |
| 5 | `SYSCALL` ×2, `GETREGSET` ×2 | `memfd_create` ENTRY (stop_seq 4), then EXIT (stop_seq 5, `rax` = 34) | stop_seq 4-5 |
| 6 | `CONT` (sig 0) | the child executes `write`, `fcntl` ×2, `openat`, …, `readlinkat`, `newfstatat`, `execve` without syscall stops | |
| 7 | `wait4` | status 263551 (`0x4057f`, exec event) | 61491 |
| 8 | `GETSIGINFO` | a single call in the whole run | 61492 |
| 9 | `SYSCALL` then `GETREGSET` | first stop after exec = exec exit: `orig_rax` = 59, `rax` = 0, RIP = `0x7fffffffa000`, `eflags` = `0x202`, `fs_base` = `gs_base` = 0. This is the order of the Linux oracle. | stop_seq 7; `P/eac-postexec-exit-comparison-20260930/README.md:9-17` |
| 10 | `SYSCALL` ×2 + `GETREGSET` loop | one `GETREGSET` per syscall, at the exit (phase 2), until `prctl(PR_SET_NAME)` (`rdi` = 15, `orig_rax` = 157, stop_seq 53) | wait4 61505-61833 |
| 11 | `newfstatat`, `openat`, `read` ×2, `close` | `/proc/80/maps`, read in one go | 61847-61856 (`tracer_stat_path` 61848) |
| 12 | `PEEKDATA` ×8 | 64 bytes at `0x7ffffdffb000`: the ELF header (size 64) of the main program; `e_entry` = `0x11f0` | 61858-61893 |
| 13 | `PEEKDATA` ×2, `POKEDATA` | address `0x7ffffdffc1f0` = base + `e_entry`. Value written `0x89485ed18949edcc`: low byte `0xcc` (INT3); original bytes `31 ed 49 89 d1 5e 48 89` (`xor ebp,ebp; mov r9,rdx; pop rsi; mov rdx,rsp…`: start of a program's `_start`) | 61898-61908 |
| 14 | `CONT` (sig 0), `wait4` | status 1407 (`0x57f`, SIGTRAP) | 61913; wait4 62025 |
| 15 | `GETREGSET` | RIP = INT3 address + 1 (`0x7ffffdffc1f1`), `orig_rax` = -1, `eflags` = `0x246` | 62027 |
| 16 | `SETREGSET` | a single word changes: RIP (index 16) from `0x7ffffdffc1f1` to `0x7ffffdffc1f0` | 62033; event `setregset_requested_difference` |
| 17 | `PEEKDATA`, `POKEDATA` | restores the original word `0x89485ed18949ed31` | 62039, 62044 |
| 18 | maps reread (×2) | two series `newfstatat`, `openat`, `read` ×2, `close` on `/proc/80/maps` | 62049-62069 |
| 19 | `PEEKDATA` ×153 | resolution of a symbol in a library of the tracee: 113 words (904 bytes = 64-byte ELF header + 15 program headers of 56 bytes) at `0x7ffffddcd000`; 16 words at `0x7ffffdfb1940`; 3 words at `…1528`, `…152c`, `…1530` (start of a GNU hash table); 1 word at `…30a4` and at `…58b0`; 3 words at `0x7ffffdde3e40` (a 24-byte `Elf64_Sym`); 16 unaligned words at `0x7ffffddeb14f` (name comparison) | 62071-62831 |
| 20 | `GETREGSET` | state to save | 62836 |
| 21 | `POKEDATA` ×4 | on the tracee's stack at `0x7fffffff8200`, `…8208`, `…8210`, `…8218`: `0x65732f636f72702f`, `0x34332f64662f666c`, 0, 0 = `/proc/self/fd/34` followed by zeros | 62842-62857 |
| 22 | `GETREGSET` `NT_PRSTATUS` then `NT_PRFPREG` | save of the general and floating-point registers | 62862, 62868 |
| 23 | `POKEDATA` | `0x7fffffff81f8` receives the value 9 | 62873 |
| 24 | `SETREGSET` | five words change: `rdx` (12) = 0, `rsi` (13) = 1, `rdi` (14) = `0x7fffffff8200`, RIP (16) = `0x7ffffde5bfc0`, `rsp` (19) = `0x7fffffff81f8` (old value `0x7fffffff82a0`); `rsp` + 8 is 16-aligned | 62878; `setregset_requested_difference` |
| 25 | `CONT` (sig 0) | the injected call is supposed to run | 62884 |
| 26 | `wait4` | status 1407 (`0x57f`): under HL the second stop arrives as SIGTRAP (Rosetta internal detour, X28 = 9, X22 = 0) | 62920; wait4 63025 |
| 27 | `CONT` with sig = 5 | EAC forwards the received signal, without reading registers | 63027 |
| 28 | `wait4`, `GETREGSET`, `SETREGSET` | status 2943 (`0xb7f`, SIGSEGV); then `GETREGSET` and `SETREGSET` return EIO | 63033; 63035, 63040 |
| 29 | `kill(80, SIGKILL)` ×2, then failure path (§2.6) | callback 509, launcher 210 | 63046, 63053 |

Interpretation of steps 19 to 24 (`inferred`): the bytes written, the registers and the return address are measured. Deduced are: (a) the image read at step 19 is libc's (15 program headers, large size, address lower than ld.so's, target RIP at offset `0x8efc0` of this image); (b) the function called is `dlopen` (arguments `(string, 1, 0)`: `RTLD_LAZY`); (c) the loaded object is the 7.76 MB memfd (number 34 is the one returned at step 5). The stack is aligned as at the entry of a System V function, with return to address 9.

What follows on real Linux x86 (`inferred`): the return to address 9 page-faults; the kernel delivers `SIGSEGV` (stop `0xb7f`, `si_addr` = 9) at the first `wait4` of step 26; EAC reads the result there (`rax`), restores the registers saved at step 22 (floating-point included), suppresses the signal with `CONT(0)` and lets the child continue. This reading rests on EAC's observed behavior at step 27: faced with a stop different from the expected one, it forwards the signal and reads no register from it; only at the SIGSEGV stop of step 28 does it request the registers again.

### 3.3 Formats and sizes

| Object | Required content | Evidence |
| --- | --- | --- |
| `NT_PRSTATUS` (1) | 27 x86-64 words in `struct user_regs_struct` order: r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs. 216 bytes. A request of 4096 bytes returns 216 (PC and QEMU TCG). Lengths that are not multiples of 8 (215, 7, 1) give EINVAL on the ARM64 kernel (`P/regset-architecture.md:9`); the `ptrace_regset` rule is generic, so probable for x86 (inferred, not measured on x86) | `P/ptrace-x86-tgkill-context-control-20260929/README.md:12`; `D/rocket-bazzite-return-20260929/ANALYSE.md:48` |
| `NT_PRFPREG` (2) | 512 bytes, `FXSAVE` layout (x87, 16 XMM registers, MXCSR) | `P/ptrace-fpregset-runtime-20261001/README.md:5` |
| iovec | 16 bytes read first; only `iov_len` rewritten (not `iov_base`); the length is truncated to the regset size; partial copy possible before a fault | `P/remote-getregset-contract.md:44-46` (measurements on Linux ARM); x86 oracle: `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (`NT_PRSTATUS buffer canaries`) |
| Values at the `INT3` stop | `orig_rax` = -1, `cs` = 0x33, `ss` = 0x2b, `eflags` = 0x246, RIP = INT3 address + 1 | `P/ptrace-signal-two-int3-control-20260930/…manifest.json` (rip 4198615 for a site at 4198614) |
| Values at syscall stops | ENTRY: `rax` = -38 (ENOSYS), `orig_rax` = number, RCX = RIP after `syscall`, R11 = eflags; EXIT: `rax` = result | `GR` stop_seq 2-5; oracle `P/ptrace-x86-full-context-control-20260929/README.md:3` |
| Values at exec exit | RIP = entry of the new image, `rax` = 0, `orig_rax` = 59 (even for `execveat`, 322), `eflags` = 0x202, other registers zero | `P/ptrace-tracer-window-control-20260930/README.md:5,23` |

### 3.4 INT3 sequence, stop and resume

1. The tracer writes `0xcc` into the low byte of an 8-byte word of an executable page of the tracee (`POKEDATA` with forced write: read-only target allowed on a private mapping, like `FOLL_FORCE`). Oracle: `P/ptrace-poke-int3-control-20260930/README.md` (37/37); permission rules: `P/ptrace-poke-design-20260930/DESIGN.md:9-11,40-44`.
2. `CONT`: the tracee executes the INT3. Signal-delivery stop, status `0x57f`, `siginfo.si_code` = 128 (`SI_KERNEL`), RIP = INT3 + 1.
3. The tracer reads the registers, restores the original byte with `POKEDATA`, then `SETREGSET` with RIP = INT3. Linux never rewinds RIP by itself. The tracer does it explicitly.
4. `CONT(0)`: the original instruction runs. The witness requires the tracee to actually execute the restored instruction (exit 77): the code translation placed in the cache with the INT3 must have disappeared.
5. In EAC, step 4 is not done at this stop: after the restore (§3.2 step 17), EAC reads the tracee's memory, prepares the injected call and issues no `CONT` before step 25.

Major implementation point: any ptrace write into a process's code must invalidate that process's translations before resuming. Rosetta fails here: two translation fragments cover the byte (the original fragment and a one-byte `INT3` fragment) and its per-thread cache reinserts a stale `BRK` translation after erasure (`P/ptrace-remote-setregset-runtime-20261001/README.md:32-45`, `P/ptrace-jit-invalidation-runtime-20261001/README.md:5`). With FEX, the tracee and the tracer are two macOS processes; the invalidation must therefore cross processes (`inferred`, design point, see §6).

### 3.5 Registers that must be exact

- Read: the 27 words at each read stop, in particular RIP, RSP, `orig_rax`, `rax`, RCX, R11, `eflags`, `fs_base`, `gs_base` and the selectors. The values must follow the kernel's behavior: RCX = next RIP and R11 = eflags at `syscall` (`P/ptrace-x86-feasibility-20260929.md:45-47,80-89`), `fs_base` and `gs_base` non-zero in the witness (`ptrace-x86-full-context-control-20260929/README.md:5`).
- Write: every word not modified by EAC must be accepted as it was read (exact round trip). The words modified by EAC are RIP, `rsp`, `rdi`, `rsi`, `rdx`. The callee must preserve `rbx`, `rbp`, `r12` to `r15`, `fs_base` (glibc TLS for `errno` and the stack canary), `eflags` (DF = 0), MXCSR and x87 (through `NT_PRFPREG`). The HL candidate only accepts "safe" writes and refuses (EIO) ambiguous changes of selectors, bases, flags and R11 (`P/ptrace-general-regset-runtime-20261001/README.md:3`).
- `orig_rax` = -1 at signal stops; syscall value at syscall stops. Writing `-1` must not restart a syscall.

### 3.6 Exec and fork events, statuses, siginfo, tgkill

- `wait4` statuses to be decoded with the Linux values, not with the Darwin macros (the `0x137f` stop is read as "continued" by Darwin's `WIFCONTINUED`): `P/proc-ptrace-module.md:26`.
- Initial stop: `0x137f` (SIGSTOP). Syscall stop with `TRACESYSGOOD`: `0x857f`, `GETEVENTMSG` 1 (ENTRY) or 2 (EXIT) (`P/ptrace-syscall-contract.md:9`). Exec event with `TRACEEXEC`: `0x4057f` (`P/ptrace-options-contract.md:5`). `INT3`: `0x57f`. Child exit: `0x4d00` for exit 77 (witness).
- The first stop after the exec is the exec exit, not the entry of the first syscall of the new image: status `0x857f`, `GETEVENTMSG` 2, successful `GETREGSET` (`P/ptrace-tracer-window-control-20260930/README.md:5`). HL missed this stop for a long time (`P/eac-postexec-exit-comparison-20260930/README.md:9`).
- The self-addressed `tgkill` is followed by a signal stop and by no `tgkill` exit stop before the syscall stops (5 stops in the oracle: getpid ENTRY/EXIT, write ENTRY/EXIT, exit ENTRY, phases 1,2,1,2,1). An extra stop `orig_rax` = 234 makes HL diverge (`P/ptrace-x86-tgkill-context-control-20260929/README.md:5-12`).
- After `TRACEEXEC`, an ordinary signal passed to `CONT` at this event is ignored; a signal passed at a syscall stop is requeued as `SI_KERNEL` (`P/ptrace-options-contract.md:7`, `P/ptrace-syscall-contract.md:13`).
- `GETSIGINFO` (128 bytes): for the self-sent `SIGSTOP`, `si_signo` 19 and `si_code` `SI_TKILL` (-6) with the child's pid and uid; for a `TRACESYSGOOD` syscall stop, `si_signo` 5 and `si_code` 0x85; for the exec event, 5 and 0x405; for the INT3, 5 and 128 (`SI_KERNEL`), `si_addr` 0. Values expected by `P/ptrace-exec-siginfo-control-20260930/README.md` and `P/ptrace-siginfo-contract-control-20260930/README.md` (prepared fixtures: `ptrace-exec-siginfo-control` has a red HL baseline in `baseline-evaluation.json`; `ptrace-siginfo-contract-control` had been neither run nor compared with a native Linux when it was prepared); only the INT3 case (128) has an x86 oracle: QEMU TCG and PC (`D/rocket-bazzite-return-20260929/ANALYSE.md:49`). The other codes are `inferred` from the Linux ABI.

### 3.7 Progress ladder under HyperLinux and known divergences

Each row gives the first refusal attributable to a given runtime; the final verdict stays 509/210 at all steps (except 506/207, 502/208 at steps 1 and 2). Times in UTC.

| Step | Runtime (short SHA) | First attributable refusal | Evidence |
| --- | --- | --- | --- |
| 1 | HL, 2026-09-28 | libc missing from `/proc/self/maps`: 507/206 | `docs/research/rocket-league-macos.md:142` |
| 2 | HL up to `bb2f` | `/proc/<ppid>/exe`, `cmdline` ENOENT: 502/208 `Launch Error: 256` | `P/eac-parent-proc-comparison.md:9-24` |
| 3 | HL `3c1babf6` | `/proc/<ppid>/status` provided: 502 becomes 509/210; the child gets EINVAL at `TRACEME` | `P/eac-parent-status-transition/README.md:3-6,58-69` |
| 4 | HL `361882f5` | `SETOPTIONS(0x11)` EINVAL | `P/eac-ptrace-transition/report.md:5` |
| 5 | HL `a4178585` | `PTRACE_SYSCALL` EINVAL | `P/eac-ptrace-options-transition/report.md:5` |
| 6 | HL `0701cfc1` | `GETREGSET` ESRCH (the tracee is another host process) | `P/eac-ptrace-syscall-transition/report.md:5,20` |
| 7 | HL `6601c6ec` | 66 `GETREGSET` succeed with ARM64 registers (216 bytes = x0 to x26); `F_ADD_SEALS` EINVAL; the child does `exit_group(1)` | `P/eac-ptrace-regset-transition/report.md:3` |
| 8 | RLX (real kernel + Rosetta, Debian Wine) | same 66 ARM captures; the child also does `memfd_create`, `write`, `fcntl`, then `exit_group(1)` without exec; the return of `fcntl` is not traced | `P/eac-ptrace-target-role-20260929.md:5-7`; `P/eac-ptrace-audit-regset-linux-20260929/README.md:36-50` |
| 9 | HL `3d37afab` (Rosetta's x86 bridge, Debian Wine) | three correct x86 `GETREGSET` (`mmap`, `memfd_create` ENTRY and EXIT), 37-byte `readlink`, closing of the memfd, `exit_group(1)` | `P/eac-x86-ptrace-tgkill-20260929/README.md:7-15,28`; `P/eac-readlink-boundary-20260929/README.md:3-9` |
| 10 | HL `a8829b17` (09-30 07:43, Cachy 11 Wine) | the child reaches the exec; `GETSIGINFO` ESRCH | `P/eac-cachyos-tgkill-20260930/run-20260930T074315115989Z/README.md`; `next-controls-source-provenance.md` |
| 11 | HL `872a6f2a` (08:00), `44d98bdc` (08:13) | `GETSIGINFO` ok; 26 `GETREGSET`; first stop after exec = entry of `arch_prctl` with `rax` = -38 for EAC (entry of `prctl` for the owned witness), so the exec exit stop is missing; after the exit of `prctl(PR_SET_NAME)` the parent sends two `SIGKILL` | `P/ptrace-tracer-window-control-20260930/README.md:5`; `P/eac-process-lifecycle-20260930/run-20260930T081347056221Z/README.md` |
| 12 | HL `7cadfcc4` (09:14) | correct exec exit; 27 `GETREGSET`; entry of `brk`, then two `SIGKILL` | `P/eac-postexec-exit-comparison-20260930/README.md:5-21` |
| 13 | HL `b797c823` (09:35), `7b7133a4` (09:51) | last tracer call before the `SIGKILL`s: `newfstatat("/proc/80/maps")` = ENOENT (path captured at 09:51) | `P/eac-tracer-window-20260930/run-20260930T093526586977Z/tracer-window-summary.json`; `P/eac-remote-maps-finding-20260930/README.md:3` |
| 14 | HL `1f842509` (10:23) | remote maps provided (stat 0, open, 2580 bytes, EOF); `PEEKDATA(0x7ffffdffb000)` EINVAL | `P/eac-proc-remote-maps-20260930/run-20260930T102349870211Z/progress-finding.json` |
| 15 | HL `48a7ee6a` (11:25) | 10 `PEEKDATA` ok; `POKEDATA(0x7ffffdffc1f0, 0x89485ed18949edcc)` EINVAL | `P/eac-ptrace-peek-20260930/run-20260930T112510372128Z/progress-finding.json`; `P/ptrace-poke-design-20260930/DESIGN.md:3` |
| 16 | HL `173201a1` (13:39) | `POKEDATA` ok; `CONT`; SIGTRAP stop `0x57f`; `GETREGSET` EIO; the original byte is restored; two `SIGKILL` | `P/eac-ptrace-poke-20260930/run-20260930T133949334669Z/progress-finding.json` (field `observed_delta`) |
| 17 | HL `29649335`, `80953d6e`, `fb965df1` (10-01 00:05 to 06:19) | real VMAs, `PEEKDATA` and `POKEDATA` replace the previous approximations | `P/ptrace-remote-maps-runtime-20261001/README.md`; `P/ptrace-remote-peek-runtime-20261001/README.md`; `P/ptrace-remote-poke-runtime-20261001/README.md` |
| 18 | HL `e2f12c45` (06:31), `cf568d78` (06:34) | real x86 registers at the INT3 stop; requested `SETREGSET` observed: a single word, RIP minus 1 | `P/ptrace-signal-regs-runtime-20261001/README.md`; `P/eac-setregset-diff-20261001/run-20261001T063442877259Z/setregset-diff-summary.json` |
| 19 | HL `acc0844f` (08:49) | `SETREGSET` accepted; `GETREGSET NT_PRFPREG` EINVAL; no `CONT` before the `SIGKILL`s | `P/ptrace-jit-invalidation-runtime-20261001/README.md:11` |
| 20 | HL `77450112` (10:09, 10:12) | `NT_PRFPREG` ok; the second `SETREGSET` (general write) returns EIO | `P/eac-fpregset-20261001/validation-summary.json` |
| 21 | HL `f3f7ca30` (10:21, 10:25) | general write accepted, `CONT`, second SIGTRAP stop at Rosetta's detour (X28 = 9, X22 = 0), snapshot unavailable (EIO), signal forwarded, SIGSEGV, `SIGKILL` | `P/ptrace-general-regset-runtime-20261001/README.md:9`; `GR:63025-63053` |

Suspicious divergence (`inferred`, not tested in isolation): at step 9 the child closes the memfd again and exits when `readlink(/proc/self/exe)` returns a path ending in `wine64-preloader.static` (Debian 10, runtime `2cd7375c`/`3d37afab`); at step 10, with `…/x86_64-unix/wine-preloader` (GE-Proton, Cachy 11, runtime `a8829b17`), it does `newfstatat(<dir>/wine64)` then `execve`. The two runs differ in the runtime, the Wine version and the loader name. `P/eac-cachyos-tgkill-20260930/next-controls-source-provenance.md` notes the difference without attributing it and writes that the different names "do not prove an error". No test changes only this name.

Observation (`inferred`): `Launch Error: 256` is 0x100, the raw `wait4` status of an `exit(1)`. This equality is not demonstrated; `R/linux-reference/eac-static-208/analysis.md:28-32` rules out any reading of 256 as errno, exit status or verdict.

## 4. Environment facts checked by the module

Only what is evidenced appears here.

| Fact | Evidence | Status |
| --- | --- | --- |
| A parent with a readable `/proc` is required: PID 1 / PPid 0 gives 502 `Launch Error: 256` in 39 ms, even on a real kernel. No fabricated parent | `R/linux-reference/pid1-control/README.md` | observed under HyperLinux/Rosetta (RLX) |
| Reads `comm`, `exe`, `cmdline`, `status` of the parent and `status`, `exe` of itself; 31 bytes on the parent; ENOENT failure of `exe`/`cmdline` gives 502/256; failure of `status` gives 502/256 | `P/eac-parent-proc-comparison.md:9-24`; `P/eac-parent-status-transition/README.md:3-6` | observed under HyperLinux/Rosetta (HL) |
| Refuses a VM: WSL2 gives 502/208 `Cannot run under Virtual Machine.` at +19 ms, before any ptrace. Criterion unknown | `D/rocket-wsl-eac-return-20260929/ANALYSE.md:3,21-29,45` | observed on real Linux x86 (WSL2 is an x86 VM; it is not a native kernel) |
| Accepted without VM refusal: physical PC (Bazzite); QEMU TCG `-cpu max -machine pc,vmport=off,hpet=off -accel tcg`, 4 vCPU, 8 GiB, no flag added or removed; HyperLinux (no kernel). Rosetta on an ARM64 kernel (OrbStack VM) gives 509, not the VM message | `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/config.plist:170-215`; `D/rocket-bazzite-return-20260929/ANALYSE.md:31` | observed on real Linux x86 (QEMU TCG, PC) |
| The DMI (`board_name`, `product_name`, `sys_vendor`) and PCI (`/proc/bus/pci/devices`) probes are read; their absence does not stop the module | `P/eac-parent-status-transition/after-status-context.txt:1015616-1015664`; `GR:63084-63087` | observed under HyperLinux/Rosetta (HL) |
| Container detection by `stat` of `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/run/host/container-manager` (after the failure in `GR`, exact role unknown). On the PC the game runs under Heroic Flatpak + pressure-vessel (`Seccomp` 2, `NoNewPrivs` 1) and EAC answers 301; the presence of these files in the bootstrapper's namespace is not measured | `GR:63090-63105`; `D/rocket-bazzite-return-20260929/received/chronologie/process-tree-during-match.json`; `D/rocket-bazzite-return-20260929/received/rapport.md:37` | observed under HyperLinux/Rosetta (HL) for the stats; observed on real Linux x86 (PC) for the acceptance under a container |
| Loader path: `readlink(/proc/self/exe)` must name a relaunchable file; `<dir>/wine64` must exist (GE); `runtime.conf` is looked for at `<dir>\EasyAntiCheat\Certificates\runtime.conf` (literal name with backslashes) | §2.2 #24 and §2.3 #40-41 | observed under HyperLinux/Rosetta (HL) |
| Uname: three calls to `uname`, values not logged; `gnu_get_libc_version` imported | `GR` (records 447-451); `P/eac-current-module-static-20260929/dynamic-symbols.txt` | observed under HyperLinux/Rosetta (HL) for the call; inferred for the use |
| `ptrace_scope`: 0 on the PC (SELinux Enforcing); WSL probe at 1 as root; EAC only uses `TRACEME` | `D/rocket-bazzite-return-20260929/ANALYSE.md:31`; `D/rocket-x86-return-20260929/ANALYSE.md:31` | observed on real Linux x86 (PC) |
| Dumpable: the child sets `PR_SET_DUMPABLE` to 0 before `TRACEME`; the exec sets it back to 1, which allows `PEEK` and opening `/proc/<pid>/maps`. With dumpable = 0: `PEEK` gives EIO, opening maps EACCES | `P/ptrace-peek-control-20260930/README.md` (QEMU 375/375) | observed on real Linux x86 (QEMU TCG) for the semantics; EAC: observed under HyperLinux/Rosetta (HL) |
| Seccomp: `prctl(PR_SET_SECCOMP)` EINVAL ×27 appears in the Windows control and the EAC launch (issuer not attributed; no cause of 509 demonstrated) | `P/eac-cachyos-process-trace-20260929/RESULTATS.md:27` | observed under HyperLinux/Rosetta (RLX) |
| `/tmp/eac.flag` is probed | §2.2 #15 | observed under HyperLinux/Rosetta (HL) |
| `process_vm_readv`/`writev`: no named import; ENOSYS under HL with no known effect | `R/linux-reference/umu/eac-native-elf-capture/README.md:35`; `P/ptrace-x86-feasibility-20260929.md:18` | observed under HyperLinux/Rosetta (HL) |
| CPUID, `/proc/cpuinfo`, cgroups, `LD_PRELOAD`, `mmap_min_addr`, `personality`, ASLR, `TracerPid`: no read or test established in these probes. Notes: the return address 9 must stay inaccessible (so `mmap_min_addr` > 9); ASLR is active in the measured map of the game under QEMU (`game-process-maps.txt`) | | inferred, except ASLR (measured) |
| libc imports of the current module (230): `fork`, `execv`, `execvp`, `waitpid`, `ptrace`, `syscall`, `prctl`, `kill`, `getpid`, `uname`, `gnu_get_libc_version`, `sysconf`, `getpwuid`, `popen`/`pclose`, `dlopen`/`dlsym`/`dladdr`/`dl_iterate_phdr`, `shmget`/`shmat`/`shmdt`/`shm_open`/`shm_unlink`, `mmap`/`mprotect`/`madvise`/`mlock`, `getcontext`/`makecontext`/`setcontext`, `epoll_*`, `socket` and the socket stack, `getaddrinfo`, `iconv_*`, `pthread_*` (rwlock, cond), `sigaction`, `signal`, `raise`, `alarm`, `select`, `ioctl`, `tcgetattr`, `glob`, `realpath`, `getentropy` | `P/eac-current-module-static-20260929/dynamic-symbols.txt` | inferred (static import table) |

## 5. Witnesses and oracles reusable without QEMU

### 5.1 Principle

The witnesses are static x86-64 ELFs without libc, compiled with Apple clang (`-target x86_64-linux-gnu -ffreestanding -fno-pie`) and linked with `ld.lld -static --no-pie -e _start --image-base=0x400000 -z separate-code -z max-page-size=4096`. The linker used is the Android NDK one (`/Users/mathieu/Library/Android/sdk/ndk/28.2.13676358/…/ld.lld`). The already linked binaries (`probe-x86_64`, `target-x86_64`, `target-flags-x86_64`) are in each directory and their SHA-256 in `build.json`. To qualify a new runtime it is enough to launch `probe-x86_64` (it forks its targets) and then compare. The outputs are JSON lines: `kernel`, `mount_proc`, `check{name, actual, expected, pass}`, `regset{stop, result, length, rip, hex}`, `siginfo{signo, code, addr}`, `result{passed, total}`. The comparison is done by ordered check names, `pass` result and result/expected relation.

The provided runners (`run-hl.py`) are specific to HyperLinux: they check the SHAs of the inputs, run the runtime with `--bind`, forbid the network through `sandbox-exec`, bound the time (25 s) and the log (1 MiB) and read `--oracle native-oracle-flags.json`. A `run-darwin.py` would have to be written on this model: launch `probe-x86_64` in the new runtime, capture stdout and compare with `qemu-*/manifest.json` (field `events`).

Rebuild: `python3 <dir>/build.py` (checks the expected SHAs). Oracle: `run-qemu.py` (reuses a cached ARM64 Docker image and the cached 6.12.107+deb13-amd64 kernel, SHA `2b2358b3…`, without network or disk). This last step does not need to be replayed: the recorded manifests are enough.

### 5.2 Table of witnesses

| Directory (under `P/`) | What it tests | Recorded Linux x86 output | Reading |
| --- | --- | --- | --- |
| `ptrace-x86-full-context-control-20260929` | 27 fields of `NT_PRSTATUS`, sizes 216 and 272, five exact stops (getpid, write, exit), true RCX/R11, non-zero FS/GS, child exit 42 | `results-20260929T161941227190Z/manifest.json`: 272/272 (QEMU TCG) | observed on real Linux x86 (QEMU TCG) |
| `ptrace-x86-tgkill-context-control-20260929` | same witness with `tgkill(self,self,SIGSTOP)`; ELF SHA `2a3f6544…` | `results-20260929T165852960827Z`: 272/272; `eac-order-readonly.json`, `evaluation.json` | same |
| `ptrace-x86-semantic-control-20260929` | context at an `INT3`; `portable/` package for the physical PC (`run-native.py`) | `semantic-evaluation.json`; Rosetta and HL: 0/36 correct fields (ARM view) | Rosetta: observed under HyperLinux/Rosetta; the native result was not collected in this directory |
| `ptrace-x86-syscall-semantic-control-20260929` | context at `PTRACE_SYSCALL` stops | `semantic-evaluation.json` (Rosetta and HL identical, ARM X8/X24/X28) | observed under HyperLinux/Rosetta (RLX, HL) |
| `ptrace-signal-regset-control-20260930` | `SIGSTOP` then `INT3`, `GETREGSET` 216, `GETSIGINFO` | `qemu-20260930T135510823911Z/manifest.json`: 15/15 | observed on real Linux x86 (QEMU TCG) |
| `ptrace-signal-two-int3-control-20260930` | two INT3 sites, `CONT(0)` after each, exit 77 | `qemu-20260930T142310221768Z/manifest.json`: 22/22 | same |
| `ptrace-signal-regs-control-20261001`, `-qualified-` | x86 registers at the INT3 `SIGTRAP`, five modes | `native-oracle-flags.json`; 60/60 on Linux x86 | same |
| `ptrace-breakpoint-control-20261001`, `ptrace-setregset-control-20261001`, `-two-trap-`, `-deliver-` | INT3 + `SETREGSET` (RIP minus 1) + code restore + resume; modes plain, cont, syscall, pre-syscall, execveat | `ptrace-setregset-control-20261001/qemu-20261001T063637525656Z/manifest.json`: 76/76 per mode; `ptrace-breakpoint-control-20261001/qemu-20261001T062322664795Z` and `…062540698072Z` | same. HL: 74/76 (stale translation). JIT invalidation: 76/76 |
| `ptrace-general-regset-control-20261001` | general register write (new stack, x86 function call, exit 37) | `candidate.json`, `red.json`; Linux x86 76/76 | same |
| `ptrace-fpregset-control-20261001` | `NT_PRFPREG`: x87, 16 XMM, rounding, FTZ, SSE exceptions; 43/43 (edge) | `qemu-20261001T095005943638Z`, `…095620328583Z`, `…100141654400Z`, `…101654707738Z`; `fp-*.json` | same |
| `ptrace-postexec-exit-control-20260930` (+ `-candidate-`, `ptrace-postexec-prctl-control`, `ptrace-mapped-elf-postexec-control`, `ptrace-tracer-window-control`) | order `TRACEME`/SIGSTOP, `SETOPTIONS`, `CONT`, EXEC, `SYSCALL`; exec exit; `GETEVENTMSG`, `GETSIGINFO`; EFAULT errors | `qemu-20260930T084834023780Z/manifest.json`; `native-oracle-flags.json` | same. The 5 modes score 12/12 to 162/162 |
| `ptrace-peek-control-20260930`, `ptrace-peek-observation-control`, `ptrace-peek-extended-diagnostic-control-20261001` | `PEEKTEXT/PEEKDATA`: all-ones word, unaligned, page crossing, `PROT_NONE`, EIO, EFAULT, ESRCH, dumpable | `qemu-20260930T110720674039Z/manifest.json`: 375/375; `native-oracle.json` | same |
| `ptrace-remote-peek-control-20261001`, `ptrace-remote-maps-control-20261001`, `ptrace-remote-poke-control-20261001`, `ptrace-stat-window-control-20260930`, `ptrace-tracer-stat-control-20261001` | memory read/write of another process, remote `/proc/<pid>/maps` (stat, open, read, mprotect, munmap), captured `newfstatat` path | `qemu-20261001T001541805339Z` (289/289), `…T000419470178Z` (268/268), `…T061756029742Z` and `…T061912553710Z` (54/54) | same |
| `ptrace-poke-int3-control-20260930`, `-exec-`, `-commit-`, `-pte-toggle-`, `-translation-`, `-translation-clflush-`, `ptrace-poke-design-20260930` | `POKE` of a byte of executable code, `INT3` set and removed, page permissions, translation | `qualification.json` (37/37 for INT3) | same |
| `ptrace-options`, `ptrace-options-controls` | `SETOPTIONS 0x11`, `GETEVENTMSG`, EXEC event, WNOWAIT | `ptrace-options/results/`, `ptrace-options-controls/results/` (on real Linux ARM64 and Rosetta, not QEMU) | observed under HyperLinux/Rosetta (RLX) |
| `ptrace-syscall*`, `ptrace-traceme`, `ptrace-worker`, `ptrace-lifecycle`, `ptrace-regset*` | `PTRACE_SYSCALL`, signal injection, timers, lifecycle, 272 ARM64 bytes | `results/` of each directory (ARM64 and x64/Rosetta references) | observed under HyperLinux/Rosetta (RLX) |
| `ptrace-exec-siginfo-control-20260930` (+ `-postexec-`), `ptrace-siginfo-contract-control-20260930`, `ptrace-siginfo-control-20260929` | `GETSIGINFO`: `SIGSTOP` by `tgkill` (code -6), exec event (0x405), syscall stop (0x85), EFAULT/ESRCH refusals; ARM64 BRK version | `baseline-evaluation.json` (red HL baseline); the `ptrace-siginfo-control` control has a Linux ARM64 reference 23/23 | no native x86 oracle recorded for these fixtures (except INT3: PC and QEMU) |
| `ptrace-audit-control-20260929`, `ptrace-audit-memory-control-20260929` | x86-64 glibc `LD_AUDIT` observers for `ptrace@plt` calls: `ptrace-audit.so` (SHA `32d2ab71…`, 8 checks) and `ptrace-audit-memory.so` (SHA `cb2ae09e…`, reads the iovec and the `GETREGSET` buffer, 8 cases) | `results-20260929T141302293656Z/manifest.json`, `results-20260929T143930143296Z/manifest.json` | observed under HyperLinux/Rosetta (RLX, HL). Tool reusable on any glibc runtime to trace the module's 21 `ptrace` sites; its presence (`LD_AUDIT` variable) changes the process environment (effect on EAC not established) |
| `ptrace-fpreg-control-20260929`, `ptrace-fpreg-set-control-20260929`, `ptrace-hwdebug-*`, `ptrace-sigchld-control-20260929`, `ptrace-notify-runtime-20260929` | ARM64 `NT_PRFPREG` (528 bytes), `NT_ARM_HW_WATCH`, `SIGCHLD` notification from Rosetta's debugger | `results-*` of each directory | Internal to Rosetta and to the HL runtime; outside the EAC contract, of no interest for a FEX engine |
| Physical PC: `D/rocket-bazzite-return-20260929/received/sondes/` | `linux-regset-probe.c` (SHA `ac04f0be…`, 13/13) and `linux-siginfo-probe.c` (30/30) | `results-host/results.json`, `linux-regset-probe.txt`, `linux-siginfo-probe.txt` | observed on real Linux x86 (PC) |
| WSL2: `D/rocket-x86-return-20260929/received/linux/` | same regset probe (13/13, 216 bytes, RAX of a getpid) | `probe-result.json`, `probe.stdout.txt` | observed on real Linux x86 (VM) |

### 5.3 Other reusable references

- Static: `P/eac-current-module-static-20260929/` (decoded ELF `module-current.elf`, disassembly, `verified-ptrace-call-sites.json`: 21 calls to `ptrace@plt` in the unsectioned executable segment `[0xa7e000, 0xd33cc8)`, 7 `waitpid`), `R/linux-reference/eac-static-208|210/`.
- Dynamic on a real kernel: `R/linux-reference/loader-trace/ge-eac*.` (LD_DEBUG) and `ge-eac-process-maps.txt`.
- Protected session: `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/` (`game-process-maps.txt`, `process-status.json`, `cpu-task-status.json`); the `*.private.log` files were not opened.
- Unauthenticated EAC runners: `P/eac-process-lifecycle-20260930/run-current.py`, `P/eac-general-regset-20261001/run-eac-minimal.py`; they depend on HyperLinux, Rosetta and a prepared Cachy Wine. They are protocol models, not tools for Darwin.

## 6. Minimal Linux ABI for the new runtime, by priority

Each entry says what is required, why, and the evidence. "Required" means: EAC stops or diverges if it is not provided (unless stated).

### P0: without them, no callback beyond 206/208

1. A Linux x86-64 process with real glibc and ld.so in memory, where `/proc/<pid>/maps` lists, in address order, the real mappings with guest file names, exact offsets and the dev/inode column; the first line carrying `libc.so` or `/libc-` must be the offset-0 segment that contains the ELF header and a usable `PT_DYNAMIC`; `dlopen` and `dlsym` (or `__libc_dlopen_mode`) must be exported (`maps-resolver.md:5-9`). Without this point the result is 206 or 507, like Voile.
2. `memfd_create` with `MFD_ALLOW_SEALING`, `write`, `lseek`, `fstat`, `pread`, private executable `mmap` of a memfd, and `openat("/proc/self/fd/N")` that reopens the memfd (magic link to the object) (`§2.1`).
3. Process identity and procfs of self and of the parent: `getpid`, `gettid`, `getppid` ≠ 0; `/proc/<pid>/{status, comm, cmdline, exe, maps}` with Linux semantics: `exe` is a readable link to an executable file; `cmdline` NUL-separated; `comm`; partial read of 31 bytes; no invented name (`P/proc-exe-cmdline-contract.md:5-17`). Wine must not be PID 1.
4. `fork()` of a multithreaded Wine process (clone `0x1200011`): the child keeps only the calling thread; copy of the whole address space (the Wine mappings, the stacks, the translator context). The parent thread must stay a non-leader thread.
5. ptrace, x86-64 view, with a `TRACEME` relationship between the child and the creating thread: `TRACEME`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, `CONT`, `GETREGSET`/`SETREGSET` of `NT_PRSTATUS` (216 bytes), `GETSIGINFO`, and `wait4` with the Linux statuses; `kill(SIGKILL)`; self-addressed `tgkill(SIGSTOP)` with the exact order of stops (§3.6).
6. `execve` of the traced child with a `PTRACE_EVENT_EXEC` event (`TRACEEXEC`), inheritance of an fd without `CLOEXEC`, `dumpable` reset to 1, first trace after exec = exec exit (`0x857f`, `GETEVENTMSG` 2) with RIP equal to the entry of the new image (§3.6).
7. `prctl(PR_SET_DUMPABLE)` with its consequence on PEEK and on the remote maps open (`ptrace-peek-control` README).

### P1: needed for the known continuation of the injection

8. `PEEKDATA` and `POKEDATA` of another process, with forced write on private executable pages; partial EIO; `ESRCH`; order of errors (`P/ptrace-poke-design-20260930/DESIGN.md:12-48`). Invalidation of the tracee's translations before it resumes.
9. `/proc/<tracee>/maps` readable by the tracer (stat, open, read), with the tracee's real VMAs and updates after `mprotect`/`munmap`.
10. Complete `SETREGSET`: exact round trip of the 27 words; modification of RIP, RSP, RDI, RSI, RDX; correct resume. `NT_PRFPREG` for reading (512 bytes) and, by symmetry with the purpose (inferred), for writing.
11. Correct delivery of `SIGSEGV` (stop `0xb7f`, `si_addr` = 9) when the tracee jumps to address 9, readable by `GETSIGINFO`/`GETREGSET`, with `CONT(0)` suppressing the signal and `CONT(sig)` delivering it. Under Rosetta it arrives as `SIGTRAP` through an internal detour (inferred).
12. Real `fcntl(F_ADD_SEALS, 0xf)` and `F_GET_SEALS` on memfd (EINVAL failure under HL not fatal for the exec, but the correct behavior is required: no fake success). The A/B that refuses seals on a real kernel keeps 509 (`docs/research/rocket-league-macos.md:270-272`), so this is not the known cause of 509.
13. Loader path: `readlink(/proc/self/exe)` returns a path that the module can `execve`, with `wine64` next to it; the name `wine-preloader` works (GE/Cachy). The name `wine64-preloader.static` (Debian) leads to exit 1 of the child (step 10 of §3.7).

### P2: required by the module outside the ptrace path

14. Threads (clone with `CLONE_VM`), futex, signals (`rt_sigaction`, `rt_sigprocmask`), `pipe`/`eventfd`, `epoll`, `poll`, `select`, `ioctl FIONREAD`.
15. Network: `socket(AF_INET/AF_INET6/AF_NETLINK)`, `connect`, `sendto`, `recvfrom`, `setsockopt`, `getsockopt`, `getrandom`, `time`, `clock_gettime`, name resolution through `getaddrinfo` (files `/etc/resolv.conf`, `/etc/hosts`, `/etc/nsswitch.conf`).
16. `uname`, `sysconf`, `getpwuid`, `popen`/`pclose`, `iconv` with gconv modules (`UTF-32.so`), `shm_open` and SysV shm (Wine also uses shm for X11).
17. Probed files whose absence is tolerated: `/sys/devices/virtual/dmi/id/{board_name,product_name,sys_vendor}`, `/proc/bus/pci/devices`, `/tmp/eac.flag`, `/.flatpak-info`, `/run/.containerenv`, `/.dockerenv`, `/run/host/container-manager`. ENOENT did not stop the module under HL; do not fabricate content.

### P3: to decide or to measure

18. VM refusal criterion: unknown. The presence of `hypervisor` in CPUID/cpuinfo and the DMI strings are not demonstrated as criteria (QEMU TCG accepted, WSL2 refused). Measure with a witness that reproduces CPUID, `/proc/cpuinfo`, DMI and sysfs of the new runtime.
19. `process_vm_readv`/`process_vm_writev`: not imported by the module, ENOSYS under HL with no known consequence; to provide if trivial.
20. Persistence of the tracee after the bootstrapper exits: detach automatically when the tracer dies (Linux behavior); the game must continue and be adopted by an ancestor.

### Points specific to FEX and Darwin

- FEX Linux forwards `PEEKTEXT`, `PEEKDATA`, `POKETEXT`, `POKEDATA`, `ATTACH`, `DETACH` to the host kernel and answers EPERM to everything else (`P/fex-eac-feasibility.md:13`). Under Darwin there is no Linux kernel: the whole tracer/tracee relationship, the `wait4` statuses, the signals, `/proc`, the copy-on-write fork and the exec must be emulated in the runtime.
- FEX's modification points are listed in `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md:124-130`: `CpuStateFrame`, `HandleSyscall` boundary, `SignalDelegator`, `InvalidateGuestCodeRange`, `Thread.cpp`.
- The POKE by one process into another's code must reach the tracee's translation cache; with two distinct host processes this requires a command channel between the two FEX instances or a cooperative stop of the tracee (the HyperLinux lesson: remote memory, snapshot at the owner thread, never direct access to another process's vCPU; `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md:114`).
- Darwin's 16 KiB pages versus 4 KiB guest pages, the double RW/RX mapping of memory pages (see below) and macOS's W^X policy for the JIT are separate constraints, outside this digest.
- The game in the protected session also contains one-page `memfd` pages in `r-xs` with `rw-s` aliases (executable shared mappings at `6fffffe82000`, `6ffffff00000`, `6ffffff0f000`, `7febb1da8000`, `rw-s` aliases at `7febae934000` and following), close to `kernel32.dll`/`ntdll.dll` (`game-process-maps.txt:571-577,1033-1035,1125`). Origin not established (Wine or module). They require `memfd` + `mmap(MAP_SHARED, PROT_EXEC)` with a second RW mapping of the same object (`inferred` for the necessity).

## 7. Unknowns and interpretation pitfalls

- No run reaches 301 outside the physical PC. The 509/210 are a global verdict: "the ptrace calls succeed" does not prove that EAC accepts the environment (admission repeated in `P/eac-ptrace-regset-transition/report.md:45`).
- The content of the second ELF, the argv of the exec, the function called (`dlopen` only by deduction) and the reason for the `(#1)` code remain unknown. The module is heavily obfuscated (indirect dispatcher, opaque constants): static analysis does not link `509` to the `ptrace` calls (`P/eac-regset-static/README.md:3`, `P/eac-current-module-static-20260929/eio-return-path-20260930.md` "Causality limit").
- The register semantics were measured on a Linux x86 kernel under QEMU TCG and two probes on PC, not on the EAC module.
- The HL logs mix several processes and threads without a PID prefix. Do not pair neighboring lines (`P/eac-parent-status-transition/README.md:90-97`).
- Three recent `GR` runs give almost identical totals of `ptrace` requests (183, 183, 185 depending on the run); the differences of 65 versus 66 `GETREGSET` between old runs are not attributed (`P/eac-runtime-conf-availability-20260929/README.md:22`).
- DMI path: the static analysis of the old module says it does not find `/sys/bus/pci/devices` in clear text, the trace shows `/proc/bus/pci/devices`. The module's strings are obfuscated; only the execution trace is authoritative.

## Appendix A: useful constants and fingerprints

| Object | Value |
| --- | --- |
| Bootstrapper | `RocketLeague_EAC.exe`, 3,975,920 bytes, SHA-256 `903e7aba292448d68976597efe3f0dc6f5cc66ef1889e3c1efcaf3eaf4e614d1` |
| Game | `RocketLeague.exe`, 38,873,528 bytes (0x25129b8), SHA-256 `ca3a9075422f6c79c0cd3ef8c4edd5c9ae1cc48f51b1077d8e18fb495f9738fa`; Authenticode table at 0x2510000 (10,680 bytes) |
| Old module | CDN 9,681,573 bytes `ce28ef66…`, ELF `c6bf6c7d…` |
| Current module | CDN 13,825,695 bytes `74b6cab9…`, ELF `1b9faa54…`, first export `a` at 0x44f00, 21 `ptrace` calls, unsectioned RX segment `[0xa7e000, 0xd33cc8)` |
| Second ELF | 7,757,608 bytes (0x765f28) for the current module, 6,618,885 for the old one |
| ptrace options | 0x11 = `TRACESYSGOOD` (1) \| `TRACEEXEC` (0x10) |
| Requests | `TRACEME` 0, `PEEKDATA` 2, `POKEDATA` 5, `CONT` 7, `SYSCALL` 24, `SETOPTIONS` 0x4200, `GETSIGINFO` 0x4202, `GETREGSET` 0x4204, `SETREGSET` 0x4205 |
| `wait4` statuses | `0x137f` SIGSTOP, `0x57f` SIGTRAP, `0x857f` syscall stop, `0x4057f` exec, `0xb7f` SIGSEGV |
| `clone` | `0x1200011` |
| Kernel oracle | Linux 6.12.107+deb13-amd64 under QEMU 10.0.13 TCG; Docker image `sha256:cefd9cb7…`; kernel SHA `2b2358b3…` |

## Appendix B: `eac-*` directories read

For the run directories, the verdict is the one from the launcher log (`anticheatlauncher-0.log` or `result.json`) and the SHA is that of the HyperLinux runtime. "509/210" = `Unexpected error. (#1)`.

| Directory (under `P/`) | Run and runtime | Verdict | Contribution |
| --- | --- | --- | --- |
| `eac-cachyos-dns-trace-20260929` | `2cd7375c` | no verdict (120 s) | curl error 6; two local `connect` with wrong status |
| `eac-cachyos-hyperlinux-20260929` | `2cd7375c` | stopped at 32 MiB of log | failure to create the Wine 11 desktop |
| `eac-cachyos-process-trace-20260929` | RLX, branch B | 509/210 | `RocketLeague.exe` runs its startup (PID 2881, parent 2863) |
| `eac-cachyos-readonly-section-20260929` | `2cd7375c` + ntdll `e6ad055d` | no verdict | `linux64`, then curl error 6 |
| `eac-cachyos-tgkill-20260930` | `a8829b17` (07:43), `872a6f2a` (08:00) | 509/210 | the child reaches the exec; `GETSIGINFO` ESRCH then successful (26 `GETREGSET`); exec exit stop missing |
| `eac-current-module-static-20260929` | static | n/a | current module decoded; 21 `ptrace` calls; imports; code in an unsectioned segment |
| `eac-current-payload-20260929` | `6601c6ec` | 509/210 | payload 13,825,695 bytes; 66 `GETREGSET`; separate Heroic bridge |
| `eac-fpregset-20261001` | `77450112` | 509/210 | `NT_PRFPREG` accepted; second `SETREGSET` EIO |
| `eac-general-regset-20261001` | `f3f7ca30` | 509/210 | most advanced trace (`GR`) |
| `eac-heroic-bridge-ab-20260929` | Cachy, branches A and B | 509/210 on both sides | the Heroic bridge changes nothing; two earlier attempts discarded (Rosetta `xfeatures` assertion) |
| `eac-jit-invalidation-20261001` | `acc0844f` | 509/210 | `SETREGSET` accepted; `NT_PRFPREG` EINVAL |
| `eac-notify-fpset-20260929` | `02c77f34`, `62ed4284` | 509/210 | 66 captures; role of the child unknown |
| `eac-parent-capture-fault` | n/a | n/a | Rosetta crash from a corrupted context pointer, not EAC |
| `eac-parent-proc-comparison*` (4 directories) | extracts of `LOG-*` | n/a | windows of the parent's `/proc` reads |
| `eac-parent-shell` | script | n/a | launches `wine64 …RocketLeague_EAC.exe --verbose` under a real parent shell |
| `eac-parent-status-transition` | `3c1babf6` | 502 then 509/210 | `/proc/1/status`; the child's TRACEME EINVAL |
| `eac-peek-extended-diagnostic-20261001` | `73272999` | 509/210 | widened windows (8192 records); 10 `PEEKDATA` then 1 `POKEDATA` |
| `eac-postexec-comparison-review-20260930` | doc | n/a | preparation of the post-exec comparison |
| `eac-postexec-exit-20260930` | `7cadfcc4` | 509/210 | correct exec exit (RIP `0x7fffffffa000`, `eflags` `0x202`); entry of `brk` then two `SIGKILL` |
| `eac-postexec-exit-comparison-20260930` | comparison | 509/210 | 27 valid `GETREGSET`; 23 identical syscalls; kill at the entry of `brk` |
| `eac-proc-remote-maps-20260930` | `1f842509` | 509/210 | remote maps read (stat 0, fd 131, 2580 bytes, EOF); `PEEKDATA(0x7ffffdffb000)` EINVAL |
| `eac-process-lifecycle-20260930` | `44d98bdc` | 509/210 | after the exit of `prctl(PR_SET_NAME)`, the parent TID 76 calls `kill(80, SIGKILL)` twice (the run's `README.md`) |
| `eac-proton-runtime.md` | doc | n/a | no evidence that `PROTON_EAC_RUNTIME` is involved |
| `eac-ptrace-audit-linux-20260929` | RLX, LD_AUDIT | 509/210 | 68 calls: TRACEME, SETOPTIONS(150,0x11), 66 GETREGSET, all 0 |
| `eac-ptrace-audit-regset-linux-20260929` | RLX | 509/210 | 66 requests of 216 bytes; ARM64 registers |
| `eac-ptrace-callsite-audit-20260929` | `62ed4284` | Rosetta assertion | no `ptrace` call captured |
| `eac-ptrace-options-transition` | `a4178585` | 509/210 | SETOPTIONS ok; SYSCALL EINVAL |
| `eac-ptrace-peek-20260930` | `48a7ee6a` | 509/210 | 10 successful `PEEKDATA`, then `POKEDATA` EINVAL; 128-record window saturated (`progress-finding.json`) |
| `eac-ptrace-poke-20260930` | `173201a1` | 509/210 | `POKEDATA` ok, `CONT`, SIGTRAP `0x57f`, `GETREGSET` EIO, original byte restored, two `SIGKILL` (`progress-finding.json`, `observed_delta`) |
| `eac-ptrace-regset-transition` | `6601c6ec` | 509/210 | 66 GETREGSET; F_ADD_SEALS |
| `eac-ptrace-syscall-transition` | `0701cfc1` | 509/210 | SYSCALL ok; GETREGSET ESRCH |
| `eac-ptrace-transition` | `361882f5` | 509/210 | TRACEME ok; SETOPTIONS EINVAL |
| `eac-read-regsets-20260929` | `a5453fd7` | 509/210 | rerun of the `eac-current-payload` runner with the "read-regsets" runtime; 66 copies `ptrace regset-copied format=arm64 requested=216` |
| `eac-readlink-boundary-20260929` | `2cd7375c` | 509/210 | `readlinkat` captured at the x86 boundary |
| `eac-readlink-observation-20260929` | `a864312b` | 509/210 | no host event (Rosetta short-circuits) |
| `eac-regset-diagnostic` | `50385ea2` | 509/210 | `iov_len` = 216 |
| `eac-regset-static` | static | n/a | no x64 reader identified |
| `eac-remote-maps-20261001` | `29649335` | 509/210 | real remote maps (stat 0, open 131, 2580 bytes, EOF); next `PEEKDATA` EINVAL |
| `eac-remote-maps-finding-20260930` | analysis | 509/210 | ENOENT on `/proc/80/maps`; SHA of the mapped module measured |
| `eac-remote-peek-20261001` | `80953d6e` | 509/210 | real maps and `PEEKDATA` succeed; window bounded to 16 records |
| `eac-remote-poke-20261001` | `fb965df1` | 509/210 | real `POKEDATA`; 442 window records |
| `eac-runtime-conf-availability-20260929` | `62ed4284` | 509/210 | `runtime.conf` accessible: no effect |
| `eac-runtime-conf-research.md` | doc | n/a | no external source explains the open |
| `eac-setregset-diff-20261001` | `cf568d78` | 509/210 | requested difference: RIP minus 1 |
| `eac-signal-regs-20261001` | `e2f12c45` | 509/210 | real x86 registers at the INT3 SIGTRAP (`stops_after_breakpoint`: seq 58, status 1407) |
| `eac-stat-window-20260930` | `7b7133a4` | 509/210 | path `/proc/80/maps` captured; SHA of the mapped module measured (`1b9faa54…`) |
| `eac-tracer-stat-20261001` | `f3ee91bd` | 509/210 | `newfstatat /proc/80/maps` ENOENT just before the two `SIGKILL` (`stat-window-summary.json`) |
| `eac-tracer-window-20260930` | `b797c823` | 509/210 | `newfstatat` ENOENT just before two `SIGKILL`, path still unknown (`tracer-window-summary.json`) |
| `eac-x86-ptrace-20260929` | `4c719659` | wineserver crash | first attempt at the x86 bridge |
| `eac-x86-ptrace-exec-diagnostic-20260929` | `cf7ad808` | 509/210 | 13 qualified x86 `ptrace` |
| `eac-x86-ptrace-tgkill-20260929` | `3d37afab` | 509/210 | 3 real x86 `GETREGSET` |
| `eac-native-module-capture-plan.md` | doc | n/a | memfd capture plan |

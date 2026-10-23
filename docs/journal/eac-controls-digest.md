# EAC controls digest: why one environment is accepted and the others rejected

Written on: 2026-10-02. Complete report, reread once against the sources.

Conventions. **Measured** or **Read**: the fact appears in a cited file. **Inferred**: deduction of this report, not demonstrated by an experiment. **Unknown**: no data. All durations are in seconds. The "delay" is the time between the `Starting Wine module mapping` line of the bootstrapper log and the callback (or the launcher result), unless stated otherwise. No authentication value was read or copied. The deviations from the rule on files whose name suggests a secret (`.private`, `auth`, `token`, `session`, `credential`) are listed in section 6.

Path abbreviations.

- `B` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928`
- `P` = `B/probes`, `LR` = `B/linux-reference`, `PERF` = `P/rocket-performance-20260930`, `FEXN` = `PERF/fex-native-hyperlinux-20261001`
- `DIAG` = `/Users/mathieu/Documents/Github/Zagrios/sailover/build/diagnostics`
- `CHR` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-macos.md` (chronology, line numbers of the file)
- `TECH` = `/Users/mathieu/Documents/Github/Zagrios/sailover/docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`

## 0. Summary for the architecture

1. Only two environments produced a launcher 301 followed by a protected EAC session: a physical x86 Linux PC (Bazzite, 72.82 s) and x86 system QEMU in TCG with a real x86 Linux kernel (91.54 s, voluntary exit). No stack with an ARM kernel or an ARM translator produced 301.
2. The tightest control pair sets QEMU TCG (301 in 3.50 s, without authentication) against HyperLinux with Rosetta (509/210): same Wine files (ntdll `e6ad055d...`), same original prefix, same Steam Runtime 4, same Heroic EAC component, same EAC payload of 13,825,695 bytes, same four game binaries, same synchronization variables, no authentication. The delta is the kernel and CPU stack.
3. The 509/210 "Unexpected error. (#1)" also appears on a real ARM Linux kernel (OrbStack) with Rosetta (three Wine builds) and with unmodified FEX (two tries). It is therefore not a defect specific to HyperLinux. On this real stack, the 68 EAC ptrace calls visible at the PLT return 0 and the helper child still ends with `exit_group(1)`, with no register write observed by this audit. Under HyperLinux with native FEX, the child also ends with code 1 (raw status 256).
4. Four distinct verdict families must not be merged: 507/206 (the bootstrapper's libc resolver), 502/208 "Launch Error: 256" (Wine started as PID 1), 502/208 "Cannot run under Virtual Machine." (explicit refusal), 509/210 (residual verdict of all the ARM stacks that reach module initialization).
5. Closed causes: libc order in `/proc/self/maps` (507), PID 1 and parent metadata (502/256), loopback (505), Wine version, payload version, product configuration, absence of authentication. Open cause: the 509. The faithful x86 ptrace hypothesis is the best supported by the structure of the module (21 call sites to `ptrace`, 66 register reads before the failure), but no try has tested it with the whole contract met and nothing rules out other factors specific to the x86 architecture (CPUID, `cpuinfo`, auxiliary vector).
6. The last known refusal under native FEX is `PTRACE_SETOPTIONS(0x11)` (TRACESYSGOOD|TRACEEXEC), which returns -95 (`FEXN/README.md:333-335`). No trace of EAC's ptrace calls exists in an accepted environment.
7. The "Cannot run under Virtual Machine." verdict is selective: WSL2 and ARM Linux under HVF with FEX received it, not QEMU TCG, not OrbStack, not HyperLinux. The signals used are unknown.
8. The QEMU protected session only lasted 91.54 s, without reconnection or ptrace trace. The Epic checklist checked online requires more than 10 minutes connected then joining another protected session. No environment of the repository reached this criterion.

## 1. Environment matrix

### 1.1 Verdict families

| Family | Callback / launcher | Text | Reading |
| --- | --- | --- | --- |
| F0 | 301 / termination 0 | `Easy Anti-Cheat successfully loaded in-game` | Module initialized, game started protected |
| F1 | 507 / 206 | `Failed to map the anti-cheat module.` then `Failed to load the anti-cheat module.` | The bootstrapper could not load the module (libc resolver, dlopen) |
| F2 | 502 / 208 | `Launch Error: 256` | Generic launch failure; reproduced with Wine PID 1 |
| F3 | 502 / 208 | `Cannot run under Virtual Machine.` | Explicit VM refusal |
| F4 | 509 / 210 | `Unexpected error. (#1)` | Verdict of the module after loading; cause unknown |
| F5 | 505 | CDN DNS failure, null client | Local network prerequisite missing (loopback) |

The bootstrapper does not build the `(#1)` text nor the value 256: it passes on the message pointer received from the module, and none of these texts exists as a literal in its PE (`LR/eac-static-210/analysis.md:16,19`, `LR/eac-static-208/analysis.md:13-14,20`). The causes of F2 and F4 therefore cannot be read in the bootstrapper's code.

**Measured.** In the `real-parent` run (F4, native FEX), helper child 82 ends with code 1 and the host `wait4` records raw status 256 (`FEXN/real-parent-control/eac-result-summary.json`, `child_exit_1_actual_host_wait_records`). **Inferred, not demonstrated.** The "256" detail of F2 and the "(#1)" of F4 are both compatible with "the child ended with code 1" (raw status 256, code 1). The F2 run under native FEX has no recorded 256 status (`FEXN/topdown-sparse-diagnostic-runtime/README.md`, third paragraph): the hypothesis is therefore not confirmed for F2. If it were, F2 and F4 would be the same event (failure of the child) classified differently depending on the launch context.

### 1.2 Table

The identifiers `S1` to `S16` (with `S6b` and `S6c`) refer to the list of sources that follows the table.

| # | Environment | Kernel and CPU | Wine and runtime | Auth. | Verdict | Protected session | Delay | Sources |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Physical x86 Linux PC (Bazzite) | 6.18.52-ogc1.1.fc44, i7-10750H, `systemd-detect-virt` = none | Proton-CachyOS `1784681208` (Wine 11.0), UMU 1.4.4, steamrt4, Heroic | yes | F0 (tries 01 and 02) | yes, 72.82 s, voluntary stop | mapping to 301: 0.520 (try 01, HTTP 200) and 0.262 (try 02, HTTP 304) | S1 |
| 2 | QEMU TCG x86, bootstrapper only | Debian 6.12.107+deb13-amd64 on QEMU TCG `-cpu max`, 2 vCPU | same Proton-CachyOS (Wine 11.0), Steam Runtime 4 root, Heroic EAC v2 | no | F0 | not applicable | 3.502 | S2 |
| 3 | QEMU TCG x86, full game | same kernel version (6.12.107+deb13-amd64, `xorg.log` in `serial.log`), UTM 5.0.5 (QEMU 10.0.12), `-cpu max`, 4 vCPU, 8 GiB, Neptune | same, `Launcher.exe` through authentication plan | yes | F0 (seven authenticated launches on Sept. 30, from 19:04 to 21:15 UTC, all with `ROCKET_EAC_SUCCESSFULLY_LOADED_IN_GAME=1`) | yes once: 91.54 s (233.73 to 325.27), voluntary exit; the six others stop for reasons unrelated to EAC | EAC active at 144.93 (last run) | S3, S3b |
| 4 | QEMU TCG x86, `lo` down (first tries) | same | same | no | F5 (505, cURL 6) | no | not recorded | S4 |
| 5 | Native Windows (x86 PC) | Windows, Windows EAC module | not applicable | yes | Windows 301 | yes, 15.84 s visible | not applicable | S16 |
| 6 | WSL2 (Hyper-V), x86 | 6.18.33.2-microsoft-standard-WSL2 | WineHQ 10.0.0.0~noble-1, Ubuntu 24.04, old module | no | F3 | no | 0.019 (502), 0.188 (208) | S5 |
| 7 | OrbStack, Rosetta, Debian Wine 10, old module (Sept. 28) | 7.0.14-orbstack (ARM) + Rosetta | Wine 10.0~repack-6, Debian trixie sysroot | no | F4 | no | 36.838 to 37.539 | S6 |
| 8 | same, probable new module (Sept. 29) | same | same, with PLT audit library | no | F4 | no | 0.077 and 0.080 | S6b |
| 9 | OrbStack, Rosetta, Wine started as PID 1, old module (Sept. 28) | same | Wine 10.0 Debian | no | F2 | no | 0.039 | S7 |
| 10 | OrbStack, Rosetta, GE-Proton11-7 (Wine 11) | same | GE-Proton11-7 direct then through UMU and Steam Runtime 4, old module | no | F4 | no | 79.287 (direct), 77.669 (UMU) | S8 |
| 11 | OrbStack, Rosetta, Proton-CachyOS 1784681208 through UMU, with and without `PROTON_EAC_RUNTIME` | same | same runner as Bazzite, new module | no | F4 in both cases | no | observation 30.303 and 30.927 (UMU preparation included) | S9 |
| 12 | OrbStack, unmodified FEX ARM Linux, FEX guest ptrace disabled (`FEX_GUEST_PTRACE` not set) | real ARM 7.0.14 kernel, FEX ARM64 | Cachy Wine (readonly-section copy), `PROTON_EAC_RUNTIME` set, FEX x86 rootfs, `--platform linux/arm64` container | no | F4 (two tries) | no | 0.074 and 2.618 | S14 |
| 13 | HyperLinux + Rosetta (Sept. 28 to Oct. 1) | Linux syscalls on Darwin, Hypervisor.framework per process, Rosetta | Wine 10.0~repack-6 then Cachy Wine + steamrt4 | no | F1, then F2, then F4 | no | F2: 0.027 to 0.035; F4: 0.731 to 0.914 (tries of Sept. 29 and 30); with Cachy Wine on Oct. 1, 37.3 to 42.3 observed from the EAC launch, not brought back to the mapping | S10 |
| 14 | HyperLinux + native FEX (Oct. 1 to 2) | same with original FEX ARM64 (SHA-256 `637943d0...`), then a numbered diagnostic clone (`670825e8...`); guest ptrace disabled up to and including the `real-parent` run, enabled from Oct. 2 | Cachy Wine, x86 sysroot | no | F1 (0.011 to 0.012), then F2 (0.061), then F4 (3.432 to 7.024) | no | see verdict column | S11 |
| 15 | HyperLinux + native FEX, authenticated plan (`Launcher.exe`) | same | same | yes | launcher 208, GUI error 256, no VM label | no | 393.734 (total) | S15 |
| 16 | ARM Linux 6.12.111 under QEMU HVF + FEX + Wine | `-accel hvf -cpu host -machine virt`, 4 KiB pages | Wine through FEX, x86 root of the Neptune image (inferred) | yes | F3 (208) | no | totals 94.5; 94.7; 245.2 | S12 |
| 17 | Voile, Wine Darwin 11.0 | macOS, D3DMetal | Voile's Wine 11.0 | no | F1 (206) | no | 3.89 and 4.91 (totals) | S13 |

Notes on the cells.

- Row 3: the "91.54" duration is the gap between `protected_session_start_elapsed_s` and `protected_session_end_elapsed_s` of `validation.json`. "EAC active at 144.93" is `eac_enabled_elapsed_s`, measured from the start of the observer, not from the start of the mapping.
- Rows 7 and 8: the delay goes from 37 s to 0.08 s on the same OrbStack stack between September 28 and 29. The EAC CDN changed module on September 29 at 11:37:15Z (S6c). The module of row 8 is not hashed, hence "probable".
- Row 13: the progression F1 to F2 then F4 is in chronological order (CHR:142, 146, 182-184, 195).
- Row 14: F1 = three tries (19:02, 22:04, 22:45 UTC on Oct. 1), F2 = one try (23:22), F4 = three tries (23:45, 00:46 and 01:57).
- Row 16: the VM label is attested by `FEXN/README.md:170` and by a file search (without displaying the content) that points to the EAC log of the third try (`arm-game-20260930T233737Z`). The first two tries (launcher exit codes 255 and 195) have no readable EAC verdict in the public files.

### 1.3 Sources of the table

- S1: `DIAG/rocket-bazzite-return-20260929/ANALYSE.md:17-21` (tries, session t=80.53 to 153.35), `:31-33` (host and runner), `:39` (payload); `CHR:276`; received logs `DIAG/rocket-bazzite-return-20260929/received/journaux/essai-01/eac-bootstrapper.log` (mapping 11:51:52.026, 301 at 11:51:52.546) and `essai-02/eac-bootstrapper.log` (11:54:03.610 and 11:54:03.872).
- S2: `P/qemu-fullsystem-eac-20260930/run-20260930T161531680277Z-eac/eac-journal-extracted.txt:149` (mapping 16:15:50.085), `:155` (301 at 16:15:53.587), `eac-evidence.json` (`launcher_301`, `bootstrapper_reports_module_initialized`, `executable_memfd_observed`); `result.json` (`VM_KERNEL=6.12.107+deb13-amd64`, `-accel tcg -cpu max -machine pc -m 4096M -smp 2`).
- S3: `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/validation.json`; `serial.log:893-902`; `config.plist` (CPU `max`, 4 cores, 8192 MiB, TCG); `P/qemu-neptune-graphics-20260930/version.log` (QEMU 10.0.12); `PERF/baseline-20260930T214616Z/manifest.json` (critical hashes, `online_validation`).
- S3b: `serial.log` (marker `ROCKET_EAC_SUCCESSFULLY_LOADED_IN_GAME=1`) of `P/qemu-neptune-graphics-20260930/authenticated-game-crash-20260930T190429Z`, `corrected-client-game-attempt-20260930T192209Z`, `gui-decoder-fatal-attempt-20260930T194508Z`, `eac-game-title-auth-expired-20260930T200739Z`, `cached-9p-auth-expired-20260930T202603Z`, `block-disk-auth-success-render-stall-20260930T205551Z` and `game-query-fix-20260930T211549Z`; stop causes of the first six in their `finding.json` when they exist (expiry of the exchange code `EOS_Auth_ExchangeCodeNotFound`, render stall) or in `serial.log` (`ROCKET_GAME_PROCESS_GONE=1`).
- S4: `P/qemu-fullsystem-eac-20260930/static-dns-loopback-path.md` (static analysis and first try); `eac-evidence.json` (`loopback_down_reproduced`, `loopback_up_control_passed`); `guest-run.sh:38,69`.
- S5: `DIAG/rocket-wsl-eac-return-20260929/ANALYSE.md:3,23-27,37-38`.
- S6: `LR/eac-static-210/analysis.md:7` (12:58:10.848 to 12:58:47.922), `:24` (13:07:06.530 to 13:07:43.415); `LR/seal-comparison/README.md:11` (37.539 and 36.838); `LR/eac-linux-environment-inventory.md` (Docker 29.4.0, kernel 7.0.14-orbstack, `/mnt/rv/[rosetta]`).
- S6b: `P/eac-ptrace-audit-linux-20260929/run-20260929T142857833442Z/anticheatlauncher-0.log` (14:29:02.522 to 14:29:02.599) and `P/eac-ptrace-audit-regset-linux-20260929/run-20260929T144112227623Z/anticheatlauncher-0.log` (14:41:16.423 to 14:41:16.503); `CHR:332,347`.
- S6c: `P/eac-current-payload-20260929/README.md` (first paragraph: 13,825,695 bytes, `Last-Modified` 11:37:15 UTC on September 29); old module 9,681,573 bytes, SHA-256 `ce28ef66...` (`LR/README.md`).
- S7: `LR/pid1-control/README.md:12-15,25-27`.
- S8: `LR/eac-ge-120s-analysis.md:9-13`; `CHR:164`; `LR/umu/ab-diagnosis.md`.
- S9: `P/eac-heroic-bridge-ab-20260929/RESULTATS-HEROIC.md` (table and durations).
- S10: `CHR:124,142,146,182-184,195,213,219,229-233,241,278,505-507`; `P/eac-current-payload-20260929/README.md`; `P/eac-remote-poke-20261001/run-20261001T061958296093Z/result.json` (Cachy Wine direct, 37.33 s observed), `P/eac-setregset-diff-20261001/run-20261001T063442877259Z/result.json` (42.30 s), `P/eac-jit-invalidation-20261001/run-20261001T084938884685Z/result.json` (38.71 s).
- S11: `FEXN/README.md:3,11,317,321,333-335`; logs specific to the run: `FEXN/eac-fex-native-<run>/prefix/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat/e6bcca5b37d0457ca881aec508205542/da32ae9c12ae40e8a112c52e1f17f3ba/anticheatlauncher.log` for `anonymous-shared-20261001T185648813375Z`, `proc-root-alias-20261001T215909862880Z`, `libc-maps-diagnostic-20261001T223956127723Z`, `topdown-sparse-diagnostic-20261001T231755792951Z`, `real-parent-20261001T234249771714Z`, `arm-remote-regset-20261002T004602387151Z`, `traceexec-20261002T015655495806Z`. The copies found under `inherited-private-logs/` are inherited logs, not to be confused with the fresh logs.
- S12: `PERF/arm-game-20260930T233737Z/result.json` (`eac_loaded: False`, `eac_initialized: False`, `wall_s` 245.234, QEMU `-accel hvf -cpu host -machine virt -smp 4 -m 8G`); `PERF/arm-game-20260930T232623Z/result.json` and `PERF/arm-game-20260930T233320Z/result.json` (94.47 and 94.68); `FEXN/README.md:170`; `PERF/arm-hvf-witness-20260930T230613Z/result.json` (FEX witnesses: CPU 0, ptrace 199/199, breakpoint 44/44; the four earlier tries exit with 11).
- S13: `CHR:20-27`.
- S14: `FEXN/eac-linux-fex-mapping-reference-20261001T191700585805Z/result.json` and `...T200811099559Z/result.json` (`launcher_codes [210]`, `callback_codes [509]`, `vm_rejection_literal false`); logs under `prefix/...`: 19:17:12.837 to 19:17:12.911 and 20:08:24.217 to 20:08:26.835.
- S15: `FEXN/eac-fex-native-epic-fork-fd-backpressure-20261002T073702835631Z/public-terminal-summary.json` (`launcher_codes [208]`, `original_eac_gui_error 256`, `eac_vm_rejection_literal false`).
- S16: `DIAG/rocket-x86-return-20260929/ANALYSE.md` (Windows: 301, visible session 15.84 s; WSL2: GETREGSET probe 13/13).

### 1.4 Reading the matrix

- **Measured.** All F4 rows share an ARM64 host that translates x86-64 (Rosetta or FEX); all F0 rows share an x86-64 kernel on a real x86 CPU or a fully emulated one.
- **Measured.** The 301 under QEMU is reproducible: eight launches out of eight on September 30 (one unauthenticated, seven authenticated). The stops on those days have causes other than EAC: expired exchange code, render stall, disappearance of the game process (S3b). Only one of these runs produced a protected session.
- **Measured.** The 509 delay depends on the module version, not on the stack: 37 s to 79 s with the old module on OrbStack, 0.08 s with the new one on the same stack (S6, S6b, S8). A module that fails in 0.08 s on a fast CPU and succeeds in 3.5 s on a software-emulated CPU (TCG) shows that the verdict is not due to a timeout.
- **Measured.** Row 2 gives 301 without authentication. All F4 rows are also without authentication. **Inferred.** Epic authentication is therefore not what separates 301 from 509 for module initialization. The Bazzite control without authentication (request `docs/research/rocket-bazzite-unauthenticated-control-prompt.md`) never got a reply in `DIAG/`; it became redundant with row 2.
- **Inferred.** Rows 13 to 15 mix runtime states that changed between each try. They cannot be read as a single runtime that would have passed all the controls (instruction from `TECH:45`).

## 2. Wine builds, rootfs, environment variables, launch, delta

### 2.1 Wine builds

| Build | Where | Version and identity | Results |
| --- | --- | --- | --- |
| Debian Wine 10 | OrbStack (rows 7 to 9), HyperLinux + Rosetta (row 13) | Debian trixie packages `wine`, `wine64`, `wine64-preloader`, `libwine` 10.0~repack-6; sysroot of 235 packages with `libc6 2.41-12+deb13u4` (`B/linux-root/README.md`, `B/linux-root/lock.json`) | F4 on OrbStack, F1 then F2 then F4 on HyperLinux |
| GE-Proton11-7 | OrbStack (row 10) | Wine 11, `WINEARCH=wow64`, started directly then through UMU 1.4.4 and Steam Runtime 4 (`LR/eac-linux-environment-inventory.md`, `LR/umu/ab-diagnosis.md`) | F4 at 77.669 and 79.287 s (old module) |
| Proton-CachyOS 11.0 | Bazzite (row 1), QEMU (rows 2 to 4), OrbStack (row 11), HyperLinux (rows 13, 14), FEX on OrbStack (row 12) | `1784681208 cachyos-11.0-20260703-slr`, Wine 11.0; "readonly-section" copy of the repository with ntdll `e6ad055d8bf9542c3ac8ad520c701c78c3de7b3df40c05a6d3e71a6075916b41` (`P/qemu-fullsystem-eac-20260930/staging-manifest.json`, key `cachy_ntdll_sha256`); official archive checked `62ff4b27...` (`P/bazzite-cachyos-runtime-20260929/provenance.json`) | F0 on x86, F4 on ARM |
| WineHQ 10.0.0.0~noble-1 | WSL2 (row 6) | WineHQ packages under Ubuntu 24.04; not the same set as Debian 10.0~repack-6 | F3 |
| Voile's Wine 11.0 | macOS (row 17) | CrossOver-based, Mach-O, no Linux ABI | F1 (206) |

The `/opt/ge-proton` path in the QEMU VM is a historical name: its content is the Proton-CachyOS above (`/opt/ge-proton/version` = `1784681208 cachyos-11.0-20260703-slr`, bootstrapper log `Wine version: 11.0`).

### 2.2 Composition of the roots and kernels

- **QEMU (rows 2 to 4).** Debian kernel `6.12.107+deb13-amd64` (SHA-256 `2b2358b37674d2505350528875bb17afae2a36522a9e8a9417eaca65a7da0e08`, official Debian trixie netboot; `P/qemu-x86-regset-control-20260929/README.md`), home-made initramfs (`init.c`), read-only `root.ext4` root built from `root-stage` (`/etc/os-release`: "Steam Runtime 4"), `/opt/ge-proton` (Proton-CachyOS), `/opt/heroic-eac-runtime` (v2 component, six files identical to Bazzite), `/x11` (Xvfb and xkbcomp), `/game` (78 MiB Win64 subset for the bootstrapper only; full game on read-only ext4 for the protected session), prefix `prefix.ext4` copied from `P/eac-heroic-bridge-ab-20260929/results-cachyos-current-001/B/prefix` (`staging-manifest.json`, `source_paths`).
- **OrbStack (rows 7 to 12).** Docker 29.4.0 on ARM kernel `7.0.14-orbstack-00380-ga7e0a2dc9535`, container `--platform linux/amd64 --user 501:20` on the Debian image `sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c`, `/usr/lib`, `/usr/share/wine` etc. mounted read-only from the repository's Debian sysroot, default Docker seccomp, no added capability (`LR/eac.json`, `LR/eac-linux-environment-inventory.md`). Rosetta: `/mnt/rv/[rosetta]` in the maps.
- **HyperLinux (rows 13 to 15).** No guest Linux kernel: a small EL1 shim under Hypervisor.framework per process translates the Linux syscalls (upstream HEAD `8048a96...`, private patches). Debian root (row 13) then restored Steam Runtime 4 tree (`restore-steamrt-tree.py`) for Cachy Wine. Row 14: unmodified FEX ARM64 and FEXServer (FEX SHA-256 `637943d0185ca3774cbc261c6c7152be2c54758a71e237769d5a27a2dea3b33f`, FEXServer `89e286c41c844ad596ed714a4449e895d9f00bda3cacfbf991f3df2a56d8d0b3`), x86 sysroot with `ld-linux-x86-64.so.2` (`FEXN/handoff-native-fex-20261002-traceexec.md`).
- **ARM Linux under HVF (row 16).** Debian arm64 kernel `6.12.111+deb13-arm64`, 4 KiB pages (`PERF/arm-hvf-witness-20260930T230613Z/serial.log`), `arm-root.ext4`, x86 image `native-display-root.qcow2` of the QEMU base mounted under `/mnt/x86root` (**inferred** from the `/mnt/x86root/*` steps of `serial.log` and from the command line in `result.json`), private FEX with guest ptrace.
- **WSL2 (row 6).** Ubuntu 24.04.5, live Python runner as Wine's parent, game mounted as read-only EROFS (`DIAG/rocket-wsl-eac-return-20260929/ANALYSE.md:15`).

### 2.3 Relevant environment variables

QEMU, bootstrapper only (`P/qemu-fullsystem-eac-20260930/guest-run.sh:99-112`):

```
WINEARCH=win64
WINESERVER=/opt/ge-proton/files/bin/wineserver
WINELOADER=/opt/ge-proton/files/bin/wine
WINEDLLPATH=/opt/ge-proton/files/lib/wine
LD_LIBRARY_PATH=/opt/ge-proton/files/lib:/opt/ge-proton/files/lib/wine/x86_64-unix:/usr/lib/x86_64-linux-gnu:/usr/lib
WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1
PROTON_EAC_RUNTIME=/opt/heroic-eac-runtime
WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
WINEDEBUG='err+all,warn+winstation,fixme-all'
DISPLAY=:89 XAUTHORITY=/xauth XDG_RUNTIME_DIR=/run/user/501 XDG_CACHE_HOME=/tmp/cache
HOME=/home/test USER=wine LOGNAME=wine LANG=C.UTF-8
WINEPREFIX=/prefix
```

QEMU, protected session (`PERF/baseline-20260930T214616Z/guest-game.sh:21-33`): `WINEPREFIX=/prefix`, `PROTON_EAC_RUNTIME=/opt/heroic-eac-runtime`, `WINEFSYNC=1` (futex_waitv of the guest kernel), `XDG_CACHE_HOME=/tmp/cache-wine`, `WINEDEBUG='err+all,warn+module,fixme-all,+seh'`, `FONTCONFIG_FILE` and `FONTCONFIG_PATH` pointing to `/run/graphics`. The DLLs `d3d11.dll`, `dxgi.dll`, `libwinpthread-1.dll` and `nptunix.dll` of the Neptune client are copied into `system32` (no expected effect on EAC).

OrbStack, Debian Wine 10 (`LR/run-inside.sh:3-14`): `HOME=/reference WINEPREFIX=/reference/prefix WINEARCH=win64 WINELOADER=/usr/lib/wine/wine64 WINESERVER=/usr/lib/wine/wineserver64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG='err+all,fixme-all' DISPLAY=:90`, Debian `LD_LIBRARY_PATH`; GE variant: `WINEPREFIX=/reference/prefix-ge WINEARCH=wow64 WINELOADER=/opt/ge-proton/files/bin/wine`. `PROTON_EAC_RUNTIME` is not set in these tries of September 28.

Native HyperLinux with FEX (profile of October 2, `FEXN/handoff-native-fex-20261002-traceexec.md`): `HL_MMAP_TOPDOWN=1`, `HL_PROC_SELF_EXEC=1`, `HL_SYSROOT_NO_BASENAME_FALLBACK=1`, remote MAPS/PEEK/POKE access active, `HL_PROC_REMOTE_SETREGSET=1`, `FEX_GUEST_PTRACE=1` for the owned controls; on the Wine side `WINELOADERNOEXEC=1`, `LD_BIND_NOW=1` (`FEXN/README.md:88-93`). Only one run at a time in the private namespace.

### 2.4 Shape of the EAC launch commands

Without credentials, the two shapes are:

1. Bootstrapper only, unauthenticated (rows 2, 4, 6 to 14): `<wine> Z:\game\Binaries\Win64\RocketLeague_EAC.exe --verbose`, current directory `Binaries/Win64`, non-root user (UID 501 under QEMU and OrbStack), a real live parent except in the PID 1 control. QEMU: `setpriv --reuid=501 --regid=501 --clear-groups timeout 600s "$WINELOADER" 'Z:\game\Binaries\Win64\RocketLeague_EAC.exe' --verbose` (`guest-run.sh:276-278`). OrbStack: `timeout -k 3 <duration> "$WINELOADER" 'Z:\game\Binaries\Win64\RocketLeague_EAC.exe' --verbose` (`LR/run-inside.sh:53-55`).
2. Authenticated plan (rows 3, 15, 16): `<wine> Z:\...\Binaries\Win64\Launcher.exe <arguments provided by a host-side broker> [-epicovt=Z:<temporary file containing the ownership token>] -windowed -ResX=800 -ResY=600` (`PERF/baseline-20260930T214616Z/guest-launch.py:35-36,49`; `serial.log:893`: `ROCKET_PUBLISHER_EXECUTABLE=Binaries/Win64/Launcher.exe`). The plan is received by a small local HTTP server; no secret is copied into this report nor read to write it.

The bootstrapper is started with `--verbose` to emit the `[EAC Callback] Code: N` lines (`LR/eac-static-210/analysis.md:24`); without this option, the logs contain the launcher result but not the callback (case of the authenticated plan, row 15).

### 2.5 Exact delta between the QEMU success and the failures

The tightest pair in every respect except the kernel and CPU stack:

| Factor | QEMU, row 2 (301) | HyperLinux + Rosetta, row 13 (509), tries of Oct. 1 | Identical? |
| --- | --- | --- | --- |
| Wine | Proton-CachyOS 1784681208, ntdll `e6ad055d...` | same (`cachy_selection.ntdll_sha256` in `P/eac-remote-poke-20261001/run-20261001T061958296093Z/result.json`) | yes |
| Libraries | Steam Runtime 4 | "Direct Cachy Wine with Steamrt4 libraries; no UMU or pressure-vessel" (same `result.json`) | yes (same family) |
| Original prefix | `.../eac-heroic-bridge-ab-20260929/results-cachyos-current-001/B/prefix` | identical `prefix_origin` | yes |
| Heroic EAC v2 component | present, `PROTON_EAC_RUNTIME` set | hashes recorded (`heroic_before`) | yes |
| Synchronization | `WINEESYNC=0 WINEFSYNC=0 WINE_DISABLE_FAST_SYNC=1` | identical `sync_overrides` | yes |
| Four game binaries | `903e7aba...`, `ca3a9075...`, `40d1bb2d...`, `ea35d2ef...` | same hashes before and after | yes |
| EAC payload | 13,825,695 bytes | 13,825,695 bytes | yes (size and HTTP 200; the bootstrapper body is not hashed in these runs) |
| Authentication | none | none | yes |
| Wine's parent | `timeout`, init shell | real parent (PID > 1) | yes |
| Loopback | `lo` enabled (`guest-run.sh:69`) | working in these tries (HTTP 200 and module loaded); an earlier AFD_CONNECT defect tied to the refusal of `tgkill` between processes is documented (`P/cachyos-loopback-controls-20260930/README.md`) | yes at the date of the tries |
| **Kernel** | real Linux x86-64 6.12.107 | HyperLinux syscall layer on Darwin | **no** |
| **CPU** | fully emulated x86-64 (TCG `-cpu max`) | Apple ARM CPU + Rosetta | **no** |

**Inferred.** For the unauthenticated initialization verdict, the discriminating factor is the kernel and CPU stack. Row 12 (real ARM Linux kernel, unmodified FEX, 509 in 0.074 s and 2.618 s) also removes the HyperLinux syscall layer from the list of suspects for the 509: the Linux facts (procfs, memfd, sealing, native ARM ptrace) there are those of a real kernel. What remains are the facts specific to the x86 architecture that the translator must reproduce: x86 ptrace view, CPUID, `cpuinfo` leaves, auxiliary vectors, signal frames.

**Known differences not neutralized between the pair.** Read-only QEMU root and home-made initramfs versus mounted HyperLinux sysroot; 4 KiB pages in the guest versus 16 KiB host pages; Xvfb in the guest versus Xvfb on macOS. None has shown a link with the verdict.

## 3. Live hypotheses and what each control ruled out

Statuses: **excluded**, **closed for a specific code**, **live**, **untested**. The "Remaining experiment" column indicates whether a discriminating experiment remains to be run.

### 3.1 The seven requested hypotheses

| # | Hypothesis | Controls and measurements | Status | Remaining experiment |
| --- | --- | --- | --- | --- |
| H1 | ARM and x86 libc mixed in `/proc/self/maps` | Static resolver: first line whose path contains `/libc-` or `libc.so`, address = start of range + file offset, then ELF `e_machine=62`, `ET_DYN`, `e_phentsize=56` (`LR/eac-static-210/maps-resolver.md`). Under native FEX, ARM libc listed first (machine 183, address 8,593,539,072) versus x86 libc higher (19,105,316,864): 507/206 in 0.011 s (`FEXN/libc-maps-diagnostic-runtime/README.md`, `FEXN/proc-libc-layout-control/qualified-order-difference.json`). A top-down allocation places the x86 libc first: 507 becomes 502/208 (`FEXN/topdown-sparse-diagnostic-runtime/eac-result-summary.json`, comparison `prior_callback 507` then `current_callback 502`). FEX on real ARM Linux lists the x86 libc first (address 140,737,350,791,168). | Closed for F1, excluded for F4: with the x86 libc first and the ARM libc still present further on, the 509 appears (`FEXN/real-parent-control/eac-result-summary.json`, `first_machine 62`, `x86_headers 1`, `arm_headers 1`) | no, but keep the guest view free of ARM objects |
| H2 | Translated or unfaithful ptrace views | Rosetta returns 272 ARM64 bytes for NT_PRSTATUS, a request for 216 gives its X0..X26 prefix (CHR:176, 313, 345). On OrbStack, the PLT audit sees 68 EAC `ptrace` calls, all returning 0: TRACEME 1, SETOPTIONS 1, GETREGSET 66 (CHR:332; `P/eac-ptrace-audit-regset-linux-20260929/README.md`). The audit does not see the requests issued through a direct `syscall` instruction: no `PTRACE_SYSCALL` appears there while it is observed under HyperLinux (CHR:229). The 66 stops read show, in X8 (ARM64 syscall number), 51 `mmap`, 1 `memfd_create`, 1 `write` of 7,757,608 bytes (0x765f28), 2 `fcntl`, 1 `openat`, 2 `fstat`, 2 `lseek`, 2 `read`, 2 `close`, 1 `munmap` and 1 `exit_group` with argument 1 (`P/eac-ptrace-audit-regset-linux-20260929/run-20260929T144112227623Z/regset-memory-report.json`, key `word8_values`; `P/eac-ptrace-target-role-20260929.md`, CHR:345). Under HyperLinux the same child also reads `RocketLeague.exe` before `exit_group(1)` (CHR:241, 536). Under HyperLinux with an x86 view through RSP, three correct x86 contexts (RAX -38 at entry, RCX = RIP, R11 = EFLAGS): 509 unchanged (CHR:505-507). Under HyperLinux + Rosetta (diagnostic runtime), EAC also issues a 216-byte SETREGSET where only one word differs, RIP minus 1 (`P/eac-setregset-diff-20261001/run-20261001T063442877259Z/setregset-diff-summary.json`), and queries `newfstatat("/proc/<child>/maps")`, which returned ENOENT before the fix (`P/eac-remote-maps-finding-20260930/README.md`). Under native FEX: TRACEME succeeds, `SETOPTIONS(0x11)` returns -95 (`FEXN/README.md:333-335`). FEX with guest ptrace disabled on a real ARM kernel (EPERM for everything except six requests, `P/fex-eac-feasibility.md`): 509 (row 12). The owned witnesses under FEX pass 199/199 and 44/44 (S12). | Live. No try combines a complete faithful x86 view and all the rest of the contract. On OrbStack no kernel refusal is observed and the child exits on its own with 1: the register view can only act through the parent (decision to kill the child or to report the failure) or through an unobserved exchange. The reason for this `exit_group(1)` of the child is unknown. | **yes** (X1, X2 and X6, section 3.3) |
| H3 | CPU identity and `cpuid` | FEX (pinned source `0df84d38...`) sets the hypervisor bit to 1 by default unless `HideHypervisorBit()` and answers `FEXIFEXIEMU` to leaf `0x40000000` (`FEXCore/Source/Interface/Core/CPUID.cpp:451-452,984` in `P/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038`). No try modified the CPUID ("FEX identity and CPUID remain unchanged", `FEXN/README.md:63`). Under HyperLinux + FEX, no VM label (`vm_rejection_observed false`, `FEXN/eac-fex-native-first-mapping-result-20261001.json`). WSL2 and ARM Linux under HVF + FEX receive the VM label, QEMU TCG and OrbStack do not. Leaf 1 ECX bit 31 and the hypervisor leaf of QEMU TCG are not measured. | Live, partly excluded for the VM label alone: FEX's default CPUID did not trigger F3 under HyperLinux. Nothing says which leaves EAC queries. | **yes** (X3) |
| H4 | Timing | Delays 0.011 to 7.024 s on FEX, 0.07 to 0.91 s on Rosetta with the new module, 36.9 to 79.3 s with the old module, 3.50 s for the TCG success. One thread active at 100% for 79 s on OrbStack/GE (`LR/eac-ge-120s-analysis.md`, threads section). | Excluded as a global cause: the success happens on a software-emulated CPU and the 509 delay follows the module version. The fine-grained controls (rdtsc, clocks) are not tested. | no (low value) |
| H5 | Epic server-side acceptance | The 301 vs 509 verdict happens before any account session and without authentication (rows 2 and 11 to 14). Both F0 environments got an authenticated protected session (rows 1 and 3). The ARM stacks never initialized the module: the server was not exercised. Authenticated plan under native FEX: launcher 208 without the game or matchmaking being reached (S15). | Untested for the ARM stacks, unrelated to the observed rejections | no, before module initialization |
| H6 | Product configuration | Same `ProductId e6bcca5b...`, `SandboxId 9773aa1a...`, `DeploymentId da32ae9c...` in all logs (`P/qemu-fullsystem-eac-20260930/run-20260930T161531680277Z-eac/eac-journal-extracted.txt:11,13,15`; `LR/eac-bootstrapper-first.log:6-8`); Linux module active for the product: CDN `.../linux64` at HTTP 200 everywhere, protected sessions on Bazzite and QEMU. | Excluded | no |
| H7 | Wine version | Debian Wine 10.0, GE-Proton11-7, Proton-CachyOS 11.0 all give 509 on OrbStack/Rosetta; Proton-CachyOS 11.0 gives 301 on Bazzite and on QEMU. | Excluded as a discriminant. Cachy under OrbStack goes through UMU, not direct Cachy, but direct Cachy also gives 509 under HyperLinux. | no |

### 3.2 Additional hypotheses

| # | Hypothesis | Measurements | Status | Remaining experiment |
| --- | --- | --- | --- | --- |
| H8 | Wine started as PID 1, or parent metadata missing | `pid1-control` on a real kernel: 502/208 "Launch Error: 256" in 0.039 s (`LR/pid1-control/README.md:12-15,25-27`). EAC reads `getppid`, `/proc/<ppid>/comm`, `exe`, `cmdline`, `status` and opens `/tmp/eac.flag` (`P/eac-parent-proc-comparison.md`). HyperLinux: 502/208 becomes 509/210 when these entries exist (CHR:195); native FEX: PID 1 gives 502/208 (0.061 s), a real parent PID 2 gives 509/210 (`FEXN/real-parent-control/eac-result-summary.json`, `comparison`). | Closed for the unauthenticated tries: PID 1 is enough to produce F2 on a real kernel, and a real parent removes F2 in both HyperLinux runtimes. The F2 of the authenticated plan (row 15) is not explained. | no, regression guard |
| H9 | Explicit VM | F3 label: WSL2 in 0.019 s; ARM Linux under HVF with FEX (S12). Absent: QEMU TCG, OrbStack, HyperLinux. Epic's docs say the client interface does not support VMs (section 5). HyperLinux saw `ENOENT` on DMI and PCI paths without receiving F3 (CHR:197). | Live: signals used unknown | **yes** (X3, measurement without modification) |
| H10 | EAC module version | Old module 9,681,573 bytes (file SHA-256 `ce28ef66...`, decoded ELF `c6bf6c7d...`), new one 13,825,695 bytes (SHA-256 `74b6cab9...`, decoded ELF `1b9faa54...`), CDN change on Sept. 29 at 11:37:15Z (S6c). 509 before and after on ARM; 301 only with the new module, on x86. The F4 delay goes from 37 s to 0.08 s. | Excluded as a discriminant of the result, but a drift variable: Epic activates modules without notice | no; pin a copy |
| H11 | Loopback and DNS | QEMU: `lo` down gives 505 before any `getaddrinfo` (local socket pair on 127.0.0.1 built before the DNS thread, `static-dns-loopback-path.md`). `lo` up gives 301. HyperLinux had failing local connections (`P/eac-cachyos-dns-trace-20260929/README.md`); the documented chain is a refusal of `tgkill` between processes, the AFD_CONNECT completion APC does not arrive (`P/cachyos-loopback-controls-20260930/README.md`). The tries of October 1 get HTTP 200 and load the module. | Closed | no |
| H12 | Memory sealing (`F_ADD_SEALS`) | Allowed/refused pair on OrbStack: 509 on both sides (37.539 and 36.838 s). Under HyperLinux and native FEX, EINVAL (errno 22). | Not conclusive: no isolated effect, but the child does the `fcntl` just before `exit_group(1)` | no, to include in the complete contract |
| H13 | JIT invalidation and code modified through ptrace | Rosetta SETREGSET/POKE candidate: 74/76 versus 76/76 for the reference x86 kernel; Rosetta reuses a stale translation after the code is restored (`P/ptrace-remote-setregset-runtime-20261001/README.md`). For FEX: `InvalidateGuestCodeRange` to use (`TECH:129`). | Specific to Rosetta; to recheck under FEX | yes, for FEX only |
| H14 | Container, seccomp, namespaces | OrbStack uses the default Docker seccomp, no added capability; QEMU without a container; Bazzite with pressure-vessel. No EAC `EPERM` observed (`LR/eac-linux-environment-inventory.md`). | Not discriminating | no |

### 3.3 Remaining discriminating experiments

Ranked by decreasing ratio between information gained and cost. None masks an identity, alters EAC or invents an answer.

- **X1. EAC's ptrace and syscall sequence in an accepted environment.** Replay the already qualified PLT audit library (`P/eac-ptrace-audit-linux-20260929`, `P/eac-ptrace-audit-regset-linux-20260929`) in the bootstrapper under QEMU TCG (or under Bazzite). Expected outputs: ptrace requests and values, number of GETREGSETs, presence of POKE and SETREGSET, exit status of the child (0 or 1), `waitpid` outputs. It is the only way to know whether the child also exits with 1 in an accepted environment. Constraint: requires QEMU or a physical Linux PC, out of scope for this effort without explicit agreement.
- **X2. Complete ptrace contract in the target engine.** Implement `SETOPTIONS(0x11)` (exec events included), SYSCALL stops, 216-byte GETREGSET in x86 format, PEEK/POKE with JIT invalidation, SETREGSET (RIP minus 1 at an INT3), `/proc/<child>/maps`, `F_ADD_SEALS`, then rerun the unauthenticated bootstrapper. Criterion: 509 becomes 301, or the verdict changes and points to the next step. This is the work in progress under native FEX (`FEXN/handoff-native-fex-20261002-traceexec.md`).
- **X3. Survey of the VM and CPU signals, without modification.** An owned probe reads CPUID (leaf 1 ECX bit 31, leaves `0x40000000`+, brand), `/proc/cpuinfo`, `/sys/class/dmi/id/*`, `/sys/bus/pci/devices/*/vendor`, `/sys/hypervisor`, `/proc/device-tree`, `uname`, `AT_HWCAP` and `AT_PLATFORM`. To run under QEMU TCG (accepted), ARM Linux under HVF + FEX (VM refusal), OrbStack (509), HyperLinux + FEX (509), WSL2 (VM refusal; requires the user's PC). Result: the list of signals that separate "refused" from "not refused". Also: a numbered counter in the CPUID handler of a private copy of FEX to record the leaves queried by the module.
- **X4. procfs check on the target stack.** Owned probe: first libc line with offset 0 and machine 62 header, `/proc/<child>/maps` readable by the tracer, `/proc/<ppid>/{comm,exe,cmdline,status}`, `readlinkat(/proc/self/exe)`, names `/memfd:<name> (deleted)`.
- **X5. Epic qualification.** After X2 only: more than 10 minutes in a protected session then joining another protected session (section 5).
- **X6. Reason for the child's `exit_group(1)`, on a real kernel.** OrbStack with Rosetta or with FEX gives the same 509 as HyperLinux, with all Linux facts genuine. To know what the child compares before exiting, record without modifying anything the sequence of its system calls (path of the `openat`, length of the `read`, offset of the `lseek`, result of `F_ADD_SEALS`, hash of the content written into the memfd) with the validated audit tool. The known limit is that this audit only sees the calls going through the PLT. This experiment answers "why 1" independently of the ptrace view.

## 4. What the QEMU success implies for a new runtime

### 4.1 Contract observable by the module

List of the facts that the module was seen consuming on the ARM stacks (where it progresses up to the 509), completed by the facts of the kernel that gave the TCG success. The QEMU session was not traced: no element is confirmed as required by a trace in the accepted environment.

1. **Kernel.** QEMU exposes a real Linux x86-64 `6.12.107` (`uname -m` = `x86_64`, `VM_MACHINE=x86_64`). The VM kernel is the one that also passes the x86 ptrace witness: 199/199 controls, 152/152 field comparisons, exactly 216 bytes for NT_PRSTATUS whatever the requested length (216 or 272), `RAX = -ENOSYS` at syscall entry, `RCX = RIP`, `R11 = EFLAGS`, `orig_rax` kept (`P/qemu-x86-regset-control-20260929/README.md`, `semantic-evaluation.json`).
2. **System calls exercised by the bootstrapper's loader** (read in the code, and observed up to loading on OrbStack): `getpid`, opening and reading of `/proc/<pid>/maps`, `memfd_create` with flag 2, `write` of the prepared module, `dlopen` through `/proc/self/fd/N` then `dlsym` (`LR/eac-linux-environment-inventory.md`, section "Syscalls actually supported").
3. **Calls of the module at load time** (observed on the ARM stacks, not under QEMU): fork of the helper child (`clone` with `0x1200011`), `PTRACE_TRACEME`, `SIGSTOP`, `SETOPTIONS(0x11)`, `PTRACE_SYSCALL`, 216-byte GETREGSET, `waitpid` with status `0x857f` at syscall stops, `tgkill`, `kill SIGKILL`, `fcntl(F_ADD_SEALS, 0xf)`, `readlinkat(/proc/self/exe)` (37 bytes), `newfstatat(/proc/<child>/maps)`, PEEK/POKE and SETREGSET with RIP minus 1 at an INT3 (`P/eac-ptrace-target-role-20260929.md`, `P/eac-remote-maps-finding-20260930/README.md`, `P/eac-setregset-diff-20261001/...`).
4. **Network.** `127.0.0.1` over TCP (listen, connect, accept, send, recv) built before the DNS thread; without it 505 (`static-dns-loopback-path.md`). Then DNS, TLS and HTTP 200 to `modules-cdn.eac-prod.on.epicgames.com`.
5. **Process.** Wine not PID 1, with a real parent readable in `/proc`. Non-root user. An absent `/tmp/eac.flag` is not blocking (ENOENT in all tries).

### 4.2 CPU

- **Measured.** The CPU model requested from QEMU is `-cpu max` in TCG (`result.json`, `config.plist`, `run-graphics.py`).
- **Read.** According to the reading of the QEMU 10.1 code recorded in `P/qemu-system-x86-witness.md`, this model exposes under TCG SSE2, FXSR, XSAVE, AVX and other extensions. No probe in the guest checked it. The real CPUID values of the QEMU guest (hypervisor bit, brand) are not recorded.
- **Unknown.** The CPUID leaves that the module queries. No survey.
- **Inferred.** FEX's hypervisor bit did not trigger F3 under HyperLinux, so it is not on its own a sufficient condition for F3. The sufficient condition is not identified (H9). A Darwin runtime with FEXCore is not a virtual machine in the sense of the guest kernel, but FEXCore will announce by default the hypervisor bit and the `FEXIFEXIEMU` brand (code read, H3). This value is to be measured (X3) before any decision.
- **Rule.** No masking of the hypervisor identity is proposed here.

### 4.3 procfs content

The module was seen opening or reading: `/proc/self/maps` (parser: at least six fields, `start-end` in hexadecimal, offset in hexadecimal, path from field 5; first libc line; address = start + offset; ELF machine 62, `ET_DYN`, `e_phentsize 56`), `/proc/<ppid>/comm`, `exe`, `cmdline`, `status`, `/proc/self/exe` (37-byte link `.../wine64-preloader.static` in the Debian root), `/proc/self/fd/N` (module in memfd), `/proc/<child>/maps`, DMI and PCI paths (ENOENT under HyperLinux, without a VM verdict), the game's `runtime.conf` certificate (`LR/eac-static-210/maps-resolver.md`; CHR:197; `P/eac-remote-maps-finding-20260930/README.md`; `P/eac-parent-proc-comparison.md`).

In the QEMU protected session, the `/proc/<game>/maps` view of the `RocketLeague.exe` process (pid 347), copied by the observer at the end, shows (**measured**, `P/qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt:1089-1098`): the first line whose path contains `libc.so` is line 1098, `/x11/usr/lib/x86_64-linux-gnu/libc.so.6` at offset 0 (`r--p`), preceded by `libcap`, `libcups` and `libcom_err`, which do not match the `libc.so` pattern; an object mapped from `/memfd:5a9729b5-d83d-5dd7-3b14-3d69f8 (deleted)` (inode 1026, segments `r--p`, `r-xp`, `r--p`, `---p`, `r--p`, `rw-p`, then a second 5.3 MiB `r-xp` segment at offset 0x21f000) precedes this libc by a few pages; a search for `rosetta`, `aarch64`, `arm64`, `FEX` and `/mnt/rv` in this file finds nothing. **Inferred**: this memfd object is the EAC module (no hash was taken in this session; the first unauthenticated QEMU try observed executable memfd mappings, `eac-evidence.json`). The capture dates from second 684 of the observer, about 6 minutes after the end of the protected session (325 s): the module stayed mapped.

### 4.4 What the protected session really exercised

- **Duration and milestones (measured).** Public matchmaking `TryToPlayOnlineWithAntiCheat PlaylistId=3`, map `TrainStation_Dawn_P`, server welcome at 224.42 s, join at 232.64 s, start of protected session at 233.73 s, end at 325.27 s, i.e. 91.54 s; voluntary exit by the user; no complete match observed; micro-stutters and missing glyphs (`validation.json`).
- **Load (measured).** One `RocketLeague.exe` process with 39 threads, `VmRSS` 4,311,204 kB (4.11 GiB), `futex_wait_multiple` dominant (`WINEFSYNC=1`), network threads (`HttpManagerThread`, `LibwebsocketsThread`, `RtcSignalingThread`) active (`outputs/cpu-task-status.json`). Observer at 684 s: game alive, `launcher_code 0` (`outputs/process-status.json`).
- **ptrace activity (unknown).** No ptrace trace or syscall audit was recorded during the session or during the QEMU boots. The scripts only record process lists, thread statistics, maps, EAC logs (`guest-launch.py`). The x86 ptrace witness on the same kernel (199/199) proves the kernel's capability, not the use EAC makes of it.
- **Duration required by Epic (section 5).** 91.54 s is far below "more than 10 minutes" and no second session was joined.

### 4.5 Consequences for a Darwin execution with FEXCore

Each point is a requirement drawn from the controls; the proof that its absence causes the 509 is missing for most of them.

1. A consistent Linux x86-64 ABI for Wine and the module in the same process: complete x86 ptrace (see X2), memfd with sealing, `clone`, `waitpid`, signals, loopback.
2. An exact and ordered guest VMA table, without host objects, with guest file names, exact offsets and `/memfd:<name> (deleted)` for the memfds. Linux FEX already had the x86 libc first; HyperLinux got it through top-down allocation.
3. A process model with a real parent, a guest PID other than 1 for Wine and readable `/proc/<pid>` entries for the parent and the child.
4. 4 KiB guest pages on a 16 KiB host: not seen by EAC, but required by Wine and FEX.
5. The F3 rejections are explicit and are not to be worked around. Measure X3 (signals really visible) and, if possible, X1 (EAC's sequence in the accepted environment) before fixing the values that the runtime synthesizes for `/proc`, `/sys` and CPUID.

## 5. Epic qualification requirement and statements on VMs, translation and Wine outside Linux

The online check was done on 2026-10-02 with `curl` on the public pages (text extracted from the page data, without running any script). The help pages `epicgames.com/help/...` and the Rocket League page return HTTP 403 to an automated client: their content is only known through the repository's paraphrases.

### 5.1 Qualification (Epic checklist)

Page: https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/anti-cheat-integration-check-list?lang=en-US

Success criteria repeated at several steps of the checklist (start with the bootstrapper, with client protection only, with all protections), exact quote:

- "Your game should function normally."
- "The player should be able to join protected multiplayer game sessions with other players successfully and remain connected for more than 10 minutes."
- "The player should be able to disconnect from one protected multiplayer game session and join a different one successfully."

**Read.** The repository paraphrases this as "more than ten minutes of protected session then a reconnection" (`TECH:222`). The source text speaks of successfully joining a different protected session after disconnecting, not of reconnecting to the same session. **Inferred.** The checklist is a pre-release test for the game's integrator, not a requirement addressed to third-party runtimes; the repository uses it as a success bar. It says nothing about VMs, Wine or translation.

### 5.2 Statements on virtual machines

Page: https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/anti-cheat-interfaces?lang=en-US (already cited by `TECH:88`).

- "Note: The two Anti-Cheat Interfaces support virtual machines differently: The Anti-Cheat Server Interface does support virtual machines (VM). The Anti-Cheat Client Interface does not support virtual machines (VM)."
- Help page cited by the repository (`TECH:36`, `CHR:73`): https://www.epicgames.com/help/c-32735058/c-37477814/a21516107 (HTTP 403 for curl; its paraphrase in the repository is consistent with the sentence above).

**Measured.** The explicit refusal with the F3 label exists and was received in two environments (WSL2, ARM Linux under HVF + FEX). It was not received in QEMU TCG, OrbStack or HyperLinux. Epic describes an absence of support, not a list of refused VMs. The verdict therefore cannot be generalized to VMs (`P/runtime-alternatives-20260929/README.md:14`).

### 5.3 Statements on Wine, Proton and non-Linux hosts

- Announcement (dated 2021 by `TECH:86`): https://onlineservices.epicgames.com/news/epic-online-services-launches-anti-cheat-support-for-linux-mac-and-steam-deck. Quotes: "Today, we extend support to Linux and Mac for developers who maintain full native builds of their games for these platforms." and "support for the Wine and Proton compatibility layers on Linux is included". Nothing on Wine on macOS.
- Current documentation (`anti-cheat-interfaces`): "Linux Wine and Proton Support: It is possible to run many Windows games on Linux using the Wine or Proton compatibility layers and the anti-cheat client protection can support this configuration. To enable support for your game, you must be using SDK version 1.14 or greater and activate a client module for the Linux platform [...] Players running the game using Wine or Proton will use the Linux client module [...]".
- `using-anti-cheat` page (https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/using-anti-cheat): the NullClient error happens when "The player is launching the game on a platform where no client module has been activated, for example using Wine/Proton when no Linux module has been activated." And: "At each game start, the anti-cheat bootstrapper fetches the latest configured anti-cheat client module for your game"; "A new anti-cheat module might be activated automatically if a major compatibility or user security issue is discovered." These two sentences explain the module drift of September 29 (H10).
- Translation: the same page notes that modules 2508.3, 2509.2 and later contain "a compatibility update for xtajit64se.dll which is the latest implementation of Microsoft's x86/x64 emulation layer for Windows on ARM". The interfaces page writes: "Players must have Secure Boot, TPM, and HVCI enabled when they use the Anti-Cheat Interfaces on the following versions of Windows for x86-64" followed by a list that includes "Windows on Arm", then "These security features are enabled by default on Windows on Arm, and if disabled, players are prompted to enable them." **Read.** Epic therefore explicitly adapts its module to Microsoft's x86 emulation layer on Windows on ARM, under system security constraints. **Unknown.** No statement found on Rosetta, FEX, Box64 or a translation on Linux or macOS.
- Apple silicon: https://dev.epicgames.com/docs/epic-online-services/platforms/mac?lang=en-US says "From EOS SDK version 1.16.4, the anti-cheat module supports Apple silicon." The Korean version (`?lang=ko`) still says that the module does not support it; it is outdated. This resolves the contradiction noted by `TECH:94`. It concerns games with a native Mac version; Rocket League has none.
- Linux ARM64: `TECH:88` and `P/fex-eac-feasibility.md` state that Epic excludes Linux ARM64. This sentence was not found in the current text of the `anti-cheat-interfaces` and `using-anti-cheat` pages (searches for "ARM", "arm64", "aarch64"). **Not checked**: to recheck in another page or an earlier version before relying on it.
- Rocket League: EAC mandatory on PC since April 28, 2026, Linux and Steam Deck announced, macOS not supported (`TECH:84`, https://www.rocketleague.com/news/easy-anti-cheat-comes-to-rocket-league-on-pc-today?lang=en and https://www.epicgames.com/help/c-37599050/a13980741 ; not reread online, HTTP 403).
- CodeWeavers (paraphrase from the repository, not reread online): support depends on the publisher and does not cover CrossOver Mac (`TECH:90`, `CHR:71`): https://www.codeweavers.com/blog/mjohnson/2026/8/31/why-do-most-games-with-anti-cheat-not-work-with-crossover-mac .

## 6. Unknowns, limits and method notes

Unknowns.

1. EAC's ptrace calls and the exit status of its helper child in an accepted environment (QEMU, Bazzite).
2. The precise condition that triggers F3 and the reason for F4. The code path of the module that produces "(#1)" is not located (`P/eac-current-module-static-20260929/README.md`).
3. The exact identity of the module loaded in each run (only a late HyperLinux run has the mapped SHA `1b9faa54...`, `P/eac-remote-maps-finding-20260930/README.md`).
4. The second at which the VM label appears in the ARM try under HVF (the EAC log is a `.private` file, not read); the verdict of the first two ARM tries under HVF.
5. The result of the Bazzite control without authentication (no reply received).
6. The risk of an account sanction for a run in a VM with EAC active: no data in the repository.
7. The exact cause of the GUI error 256 of row 15 (process and parent not analyzed).

Limits.

- The durations of row 11 include the UMU preparation. They cannot be compared with the mapping to callback delays.
- Rows 13 to 15 rely on unqualified private runtimes (four-mode Nix matrix unavailable, `FEXN/README.md`).
- The check of the Epic pages gives their text as of 2026-10-02; these pages change. The checklist may be different at the date of the next reading.

Method notes and deviations.

- **Files whose name suggests a secret.** (a) A `grep -rIl` (which only lists file names) pointed to `PERF/arm-game-20260930T233737Z/outputs/eac.private.log` as containing "Cannot run under Virtual"; the content was not displayed or copied. The same search may have gone through other `.private` files without displaying their content. (b) Two files whose name contains `auth` were read in full: `PERF/arm-game-20260930T233737Z/auth-broker.log` (four `EPIC_BROKER_*=1` status lines, no value) and `FEXN/authorized-epic-helper-path.json` (path of a helper and its SHA-256). (c) The request `docs/research/rocket-bazzite-unauthenticated-control-prompt.md` was read; it is a procedure text without secrets. (d) The files `plan.json`, `launch-record.private.json`, `epic-launch-plan*.private.json`, `result.json` of the authenticated runs under native FEX and the `*.private.log` logs were not opened. The `result.json` files of the three ARM tries under HVF (QEMU command line and statuses) and the public summaries `public-terminal-summary.json` and `public-live-summary.json` were opened.
- An accidental attempt to run `--version` on the file `UTM.app/.../qemu-x86_64-softmmu` failed immediately (`exec format error`): no QEMU process was started.
- No game, EAC, UTM or QEMU was launched. No writes outside this report; the copies of Epic pages and the extraction script are in the session temporary directory. The reads of Epic pages through `curl` are anonymous GETs of public pages.

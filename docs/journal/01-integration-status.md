# Engine and kernel integration status, October 2, 2026

Short document, updated at each integration milestone. Engine details in `fexcore-darwin-port.md`, kernel details in `src/lkd/docs/status/`.

## Engine (FEXCore on Darwin, `src/fexdarwin`)

MEASURED. x86-64 code run by the FEXCore JIT in a native macOS arm64 process, without Rosetta or virtualization. Owned witnesses, all replayed with an 8 TiB guest base: integers 9524/9524, SIMD and x87 109/109 identical to the Rosetta oracle, TLS 25/25, memory 39/39, self-modifying code 20/20, syscalls 50/50, memory below 4 GiB 19/19 (including 0x7ffe0000), threads 13/13, jumps to an unmapped address 15/15, deep recursion 4/4, recovered stack overflow 7/7. Performance on C kernels: at Rosetta's level (0 to 23% slower), guest base at most 4%.

## Kernel (LKD, `src/lkd`)

Core: dispatch, guest memory protected against faults, ABI translations, 599 assertions. mm and fs modules in progress (agents), task, sig, procfs, net and ptrace to be started.

## Hookup

`src/fexdarwin/src/LkdGlue.cpp` implements `lkd_engine_ops` on FEXCore: registers in `user_regs_struct` format, FXSAVE and XSAVE FP state, interruption by a reserved host signal, thread creation, code invalidation, `rt_sigframe` frames, fork locks. `LkdLoader.cpp` loads the executable and its PT_INTERP through LKD system calls (openat, pread64, mmap, mprotect), builds the initial stack and the auxv with AT_PAGESZ = 4096. Mode enabled by `FEXDARWIN_LKD=1`, guest root `FEXDARWIN_ROOT`.

MEASURED (14:58): `lkd_task_create_main` creates task 2; the loader's first `mmap` call answers ENOSYS, expected as long as the mm module is not delivered.

Additions of 15:20 (compiled, witnesses outside LKD replayed without regression; not yet exercised in LKD mode for lack of the mm, task and sig modules):

- Interruption of a thread running in translated code. The reserved host signal (SIGUSR2) arrives in the handler; if the PC is in the JIT's code, outside a deferred-signal section and outside a system call sequence, the guest registers are spilled (same method as FEX on Linux for asynchronous signals) and the thread restarts on a trampoline that calls `lkd_signal_next` on the thread's ordinary stack (so it can block there for a ptrace stop), then goes back into the dispatcher loop. In the other cases (dispatcher, host C++), a retry thread resends the signal every millisecond as long as the request holds and the thread is not in LKD. Witness `w15_async` written: tgkill to a thread that loops without a system call, kill to the process while the main thread loops, check of the integer and SSE registers after five interruptions.
- Synchronous faults in LKD mode: `lkd_fault` then the same trampoline, instead of the old internal signal table.
- `exec_image`: in-place loading after the point of no return (translated code invalidated over the whole window, registers of a fresh process, EFLAGS 0x202, FPU reset to zero, AT_EXECFN = path passed to execve). The loader goes through `lkd_dispatch` (internal kernel call) and not `lkd_syscall`: a tracer in PTRACE_SYSCALL must only see the entry and exit of execve, not the loader's mmaps.
- (16:10) The two leftovers of this list are done: MAPERR/ACCERR classification and `[stack]` (through the mm module's `start_stack`), see below.

## Finding that steered the work

Announcing 16 KiB pages to glibc makes ld.so fail ("ELF load command address/offset not page-aligned" on libc.so.6, measured with the old host). The mm module's logical 4 KiB view is therefore mandatory, as planned by the decision note.


## First end-to-end pass on LKD (16:00 to 16:15)

MEASURED, `fexdarwin-run` host linked to a private snapshot of `liblkd.a` (core, mm, fs, task modules delivered; sig in progress), logs under `evidence/fexcore-darwin/runs-lkd/`:

- Nine witnesses out of thirteen pass in LKD mode: `w01_hello`, `w03_simd`, `w04_tls`, `w05_mem` (39/39), `w06_smc` (20/20, full SMC mode), `w08_sys`, `w09_lowmem`, `w10_threads`, `w13_deeprec`. The four that fail (`w02_int`, `w11_badjump`, `w14_overflow`, `w15_async`) all go through signal delivery: `lkd_signal_next` returns -1 despite an installed handler (sig module in progress, reproduction passed to its agent). On the engine side, the interruption works: the thread that loops without a system call is stopped at `rip 0x2aaaaaaaf7ea` and reaches the safe point.
- Debian glibc 2.41 programs, dynamic, through `ld.so`, Debian root mounted read-only: `/usr/bin/true`, `/bin/ls /`, `/bin/uname -a` (`Linux obelisk 6.12.0-lkd ... x86_64`), `dash -c` with command substitution (real fork). `vfork` then `execve` then `exit_group` of the child, SIGCHLD delivered to dash's handler (frame built, `rt_sigreturn`), `wait4` returns the child and `status=0` is printed; then the process does not exit (wait lost in `wait4` between two host processes, reproduction passed to the task agent).
- Wine GE-Proton11-7: `/opt/ge-proton/files/bin/wine --version` prints `wine-11.0 (Staging)` and exits with 0. The `socket(AF_UNIX)` calls answer ENOSYS (net module in progress); that is the next condition for wineserver and wineboot.

Fixes made to get there:
- Engine: 8 TiB window base passed to LKD (otherwise the tests' identity mode); FEXCore created after the window reservation, because the dispatcher burns the guest base in as a constant; full SMC mode by default in LKD and the full mode's CRC computed on the host copy (`Core.cpp`); executable range queries and MAPERR/ACCERR classification read from LKD's VMA table (`LkdShim.c`, the only file that includes LKD internal headers); guest stdio; start of the heap and of the stack passed to mm; initial stack in Linux order and process layout for `/proc` and `PR_GET_AUXV`; load addresses the Linux way (`ELF_ET_DYN_BASE` = 2/3 of the window, interpreter by top-down mmap); `fork_child`, end of the main thread alone, SIGPIPE ignored; `FEXDARWIN_BIND` mounts and read-only root `FEXDARWIN_ROOT_RO=1`.
- Witness `w05_mem`: the 16 KiB alignment assertion came from the old host; Linux guarantees 4 KiB, the assertion follows Linux.

## Signals, fast SMC and complete witnesses on LKD (19:00 to 19:40)

MEASURED, logs `evidence/fexcore-darwin/runs-lkd/`:

- The thirteen witnesses of the batch pass in LKD mode (`w01` to `w06`, `w08` to `w11`, `w13` to `w15`) with the ET_EXEC binaries of `witnesses/elf-x86_64/bin-exec/` (`EXEC=1 ./build.sh`). The failures of `w02`, `w11`, `w14`, `w15` came from our binaries: static PIEs whose RELATIVE relocations nobody applied (the signal handler read from the structure was 0, that is SIG_DFL). A Linux kernel does not apply them either; the old host did it in its place. Found by the sig agent (`docs/status/sig.md`, `docs/requests/sig-4.md`).
- The loader forgot the page of a PT_LOAD segment without file data whose bss fits in a page (`filesz == 0`); Linux maps it (`elf_load` calls `vm_brk_flags` from the start of the page). Fixed in `LkdLoader.cpp`.
- `w07_fault` prints `PASS 99/99` then the `ret` of `run` jumps to a stack address (stack corrupted after the last overflow test); same symptom as on the old host, so not specific to LKD. Not elucidated; `w14` covers the same scenario and passes. To be taken up again if a real program shows the same defect.
- Exact asynchronous interruption. `w15_async` failed 3 times out of 8 (register invariants): FEX rebuilds RIP from a table that only contains instructions with side effects and at best "as close as FEX can get" (`Core.cpp`, `RestoreRIPFromHostPC`); an interruption in the middle of the translation of an x86 instruction replays it after the handler. Fixed in FEXCore: a table entry for each guest instruction, and `IsHostPCAtGuestInstructionBoundary` (host PC exactly at the first byte of the translation of an instruction); the kick signal handler only interrupts in that case, otherwise the retry thread resends the signal one millisecond later. Result: 30 runs of `w15_async` out of 30 successful (16 checks), maximum delivery latency 16 ms. Real stake: Wine suspends and reads the context of threads with SIGUSR1; an inexact resume would corrupt the game.
- Fast self-modifying code detection (`mtrack`) restored: the mm module removes write access from host pages that contain translated code (`lkd_mm_watch_code`), the first write comes back through the handler (`lkd_mm_code_write_fault`, which calls `invalidate_code`), including for the kernel's copies (patch `usermem.c`, request mm-5). `w06_smc` 20/20 in this mode. Detail not to filter: macOS reports a write to a non-resident read-only page as a translation fault (DFSC 0x06), not a permission fault.
- Performance (`w12_bench` under LKD, two runs): integers 393/403 ms, memory 38/37, floats 371/369, sort 183/185; Rosetta 379/30/351/141. The complete RIP table and LKD mode cost about 4% on integers.
- sig module finished (703 assertions), mm finished (685 assertions + `test_task_bigwin`), task (616), fs (747). net, procfs and ptrace being resumed after two cuts by the API session limit.

## Machine incident of 2026-10-02, 20:21: kernel panic, cause and safeguards

MEASURED (`/Library/Logs/DiagnosticReports/panic-full-2026-10-02-202119.0002.panic`): "zone map exhausted while allocating from zone [VM map entries] ... (10G, 177893865 elements allocated)", 332 live `fexdarwin-run` processes. The last Wine attempt (`logs/wine-linux-m2-version-013.log`, Wine built from voile-wine, from 20:18:46 to 20:20:23) launched Windows processes in a loop (identifiers 0x2c to 0x3e4, "failed to create desktop window"), each being a host process; runs whose timeout wrapper had been killed left their processes alive. MEASURED after reboot: an ordinary guest process has about 400 VM regions (dash 400, Wine client 380 to 410, wineserver 460).

Safeguards added (`src/fexdarwin/src/LkdGlue.cpp`, `scripts/run-with-timeout.sh`, `src/lkd/src/task/task_priv.h`):
- each host process follows `FEXDARWIN_SUPERVISOR_PID` (the wrapper, or failing that the first process of the family) and stops within 0.5 s when it disappears; MEASURED: `kill -9` of the wrapper, the 5 guest processes disappear in less than 2 s;
- each process stops by itself beyond 200,000 VM regions;
- at most 96 live processes per family (512 before);
- working rules: never a guest outside `run-with-timeout.sh`, at most two agents launching guests at the same time, count of the remaining `fexdarwin-run` after each attempt.

### Real cause (20:40) and EAC probe campaign

MEASURED (`build-lead/chunk.c`): XNU splits an anonymous mapping whose maximum protection is not zero into 128 MiB entries (`ANON_CHUNK_SIZE`). The 2^46-byte guest window reserved by `mmap(PROT_NONE, MAP_NORESERVE)` cost 524,288 kernel table entries per process; `vmmap` merges them on display (320 regions displayed). 332 processes × 524,288 = 174 million, which is what the panic reports (177 million). The same window reserved by `mach_vm_map` with zero maximum protection is a single entry. Fix in `src/lkd/src/mm/window.c` (reservation and reset to empty with zero maximum protection, recreation as anonymous memory before any access, region by region): 10 mm test programs and `test_task_bigwin` pass; a guest process now counts 120 to 160 entries (counter `FEXDARWIN_REGION_LOG=1`). The 200,000-region safeguard stays in place; before the fix, it had cleanly stopped the probe that reached it.

EAC probes (`witnesses/eac-probes/run-probes.py`, 39 x86-64 probes from the HyperLinux campaign, expected values from real Linux, `evidence/eac-probes/20261002T204027/`): 1,592 checks out of 1,824 pass. Passing entirely: lifecycle of `/proc/<pid>`, general and FP register sets, siginfo contract, stops on signals and double INT3, tgkill between processes and between threads, brk state. Grouped failures: POKEDATA into read-only code (EIO instead of Linux's forced write) and rewritten code not re-executed; exec event stop (GETEVENTMSG, GETSIGINFO); a few probes that exit before their first check. Passed to the integration agent.

### EAC probes after integration (21:00)

MEASURED (`evidence/eac-probes/20261002T205719/`, 40 probes, 4,376 checks, the probes now run to the end): 38 failures (232 out of 1,824 before). Fixes: forced POKE into read-only code sharing its host page with data (`src/mm/remote.c`); tracer link wrongly rewritten after the exec of a fork child (`src/ptrace/life.c`, GETEVENTMSG and GETSIGINFO in ESRCH on 14 probes); invalidation of translated code requested by the mm service thread (engine: `InvalidateCode` without a guest thread). The remaining 38: 31 checks from probes that do a `MAP_FIXED` at 0x7ffff7ff0000 or 0x7ffffdffb000, beyond the 2^46-byte guest window (architecture limit: with an offset base, the macOS user space stops at 0x7ffffe000000 and the nano malloc zone occupies 0x600000000000, so the complete 128 TiB Linux space is not reachable; Wine and EAC do not ask for these fixed addresses, the addresses EAC reads come from `/proc/<pid>/maps`); 6 checks where `CONT(SIGTRAP)` at a delivery stop of SIGTRAP without a handler kills the tracee with status 5, Linux behavior according to our reading (expected value without a Linux oracle in the probe's directory, old HyperLinux run failing too).

## EAC 301 on the fexdarwin + LKD + Linux Wine stack (22:12)

MEASURED (`evidence/eac-runs/20261002T221224/`, authenticated launch by `run/eac-run.py`, redacted EAC log `files/.../anticheatlauncher.log`): the EAC 1.9.3 bootstrapper picks `linux64`, downloads the module (13,825,695 bytes, HTTP 200 from modules-cdn.eac-prod.on.epicgames.com), writes "Starting Wine module mapping, Wine version: 11.0" then, 2.4 s later, "Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'", "Successfully initialized the Easy Anti-Cheat module, waiting for game window to become visible", "Terminating with: 0". `RocketLeague.exe` is loaded in a new process. The tracer shows the known sequence (return to address 9 in the JIT, "NoExec instruction in entry block: 9"). Fingerprints of the original game unchanged. Reference: real Linux PC 301 in 0.5 s, QEMU TCG 301 in 3.5 s; all previous ARM stacks 509/210 or 502.

What unblocked it: (1) the bootstrapper asks for ten RWX allocations at fixed addresses from 0x7001'0000'0000 to 0x700a'0000'0000 and printed "Failed to initialize Wine helper buffer" with a 2^46 guest window (found by the Wine agent with `+virtual`); the guest window now goes from the host base chosen at startup (end of the highest occupied region above 0x7d80000000, MEASURED: a 12.8 GiB reservation placed by ASLR sometimes reaches 0x7cb9400000) up to 0x7ffffe000000, that is about 127.5 TiB of guest addresses, a single kernel entry; stack at the top of the window, PIE executables at 0x555555554000 like Linux; 16 witnesses pass, 20 startups out of 20 without a reservation failure. (2) the display driver presents the error dialog box (it stayed invisible); (3) persistence of the prefix's registry (wait for wineserver).

Next step: the game process (window, D3D11 through Neptune to Metal). Defects seen in this same attempt: `MAP_SHARED` refused at 0x6fffffd17000 (address and offset not congruent modulo the 16 KiB host page), Wine allocations at the top of the space (0x7fffffb50000 and above, outside the window), AF_NETLINK missing, IPv6 option 62 not handled.

## Game at the main menu, connected to Psyonix with EAC active (22:54)

MEASURED (`evidence/eac-runs/20261002T225400/`, `files/.../Launch.log` and `anticheatlauncher.log` redacted), after the wine-7 fix (memfd phase and shared code views): EAC 301; `RocketLeague.exe` loads `EOSSDK-Win64-Shipping.dll`, our Neptune `dxgi.dll`/`d3d11.dll`, `xaudio2_9redist.dll`; "Found D3D11 adapter 0: Apple M5 Max", "Shader platform (RHI): PC-D3D-SM5", 466,923 shaders loaded; Psyonix HTTPS requests 200; EOS sign-in by exchange code, "Platform auth ticket completed with success 'True'", "PsyNet auth RPC succeeded", "Login successful", PsyNet WebSocket connected, map `MENU_Main_p` loaded and put into play; Neptune rendering creates a swap chain of 3 frames 2560x1440 presented by wsd in fullscreen. Remaining defect: no frame reaches the screen (wsd `presented=0`). Durations: menu at 76 s, Psyonix connection at 100 s (shader cache loading 30 s).

## First display of the game through Metal (23:07)

MEASURED (`evidence/eac-runs/20261002T230701/`, capture of the game's window only `evidence/wine-linux/game-keep/rl-title-20261002T230701.png`): the title screen "ROCKET LEAGUE SEASON 24" is rendered by the Neptune D3D11 client, the DXMT render server on Metal, and presented by wsd in a 1280x720 window (windowed mode set in the prefix's user configuration, `TASystemSettings.ini`, never in the installation). 96 presents, 95 displayed; average fence wait 2.3 ms. Pacing during loading: 148 ms per frame on average over the first 60 (about 7 frames per second). Open defects: presents stop after 96 frames (the game log stops at 113.9 s); the render server ignores VSSetShader/PSSetShader calls ("argument handle is unknown") and reports unregistered objects of types 61 and 65.

## Main menu stable at 60 frames per second (2026-10-03, 00:30)

MEASURED (`evidence/eac-runs/20261002T233036/`, capture of the game's window `evidence/wine-linux/game/m2-win1084.png`, which shows the player's Epic display name): main menu reached and stable, 1,680 presents in 150 s. The block after 96 frames came from Wine: `gethostbyname` goes through nsiproxy, which read the interface list through `if_nameindex()`; without AF_NETLINK in LKD it returned NULL, nsiproxy dereferenced it while holding its lock, and all following calls stayed blocked. Fixed in Wine (`SIOCGIFCONF` fallback, `GetAdaptersAddresses` without an interface returns ERROR_NO_DATA like Windows); LKD request wine-8 for real interfaces. Frame time at the menu: 16.1 to 16.4 ms (60 Hz display), submit-to-display latency 12 to 16 ms. CPU profile of the game at the menu, waits excluded: about 93% in LKD system calls (`getrusage` in a loop 1.4 s out of 5 s; `mincore` for VmRSS of `/proc/<pid>/status` 0.4 s), 5% JIT code, 1.5% FEX and LKD C++. Remaining: 14 shader pairs never created on the host side (28 commands rejected out of thousands, menu visually correct), input for playing, offline play attempt, then public match.

## 3D play offline and clean exit (2026-10-03, 01:01)

MEASURED (`evidence/eac-runs/20261003T005434/`, capture `evidence/wine-linux/play/d1-win1362.png`): navigation title screen, menu, Training, Free Play through injected input (wsdctl), map `CHN_Stadium_Day_P` in Free Play (offline, without another player), driving 70 s. Frame time in 3D: 23.0 ms on average over 2,160 frames (about 43 fps), fence 2.4 ms; wsd 22.5 ms on average, maximum 129 ms, display latency 13.7 ms; menu 16.1 ms. CPU during driving: game process, active share, JIT 43%, system calls 42% (sockets), host C++ 15%; render server 86% of a core, mostly memory copies (memmove). Shaders: 0 unknown identifier at the menu and in 3D. Exit through the game's menus: code 0 at 441.9 s, "Exit: Exiting.", EAC "Launcher finished with: 301" then "Terminating with: 0", no remaining process, fingerprints unchanged. Defects before a public match: about 1 s between an input and its effect; interface text (Scaleform) partly drawn; 3D pacing limited by the render server's copies.

## Regression "stuck on the initialization screen" fixed (2026-10-03, 02:35)

Symptom (MEASURED, attempts `20261003T014348` to `022545`, 8 attempts out of 8): 0 to 3 frames presented then nothing more; the game continues its loading until ~80 s then everything stops. The last good attempt was `013654`.

Cause (MEASURED):
- Dump of the guest threads, without changing anything for the guest: new tool `FEXDARWIN_DUMP_DIR` in `LkdGlue.cpp`. It gives, per thread, the current system call, its arguments, the futex word and the owner of the glibc mutex, as well as the return addresses with module and offset.
- Result: the present thread (`npt_unix_wsi_wsd.c:277`) waits for `g_lock` while it is itself its owner (`__owner` = its tid). The drain (line 322) and the driver's event thread (`on_presented`) wait behind it.
- No source takes this lock twice. The unlock was lost.
- The recompilation of nptunix at 01:43 moved `.plt` (0x4020–0x41d0) into the same 16 KiB host page as `.got.plt`, `.data` and `.bss` (where `g_lock` lives at 0x65a0).
- Each call through the PLT retranslates this code and reprotects the page (SMC mtrack). Each write to `g_lock` therefore goes through `lkd_mm_code_write_fault`.
- Under contention, the `MAX_RETRIES` bound (4 consecutive "retry" answers, "watched" state read only before the handling) refused legitimate writes. The guest received SIGSEGV.
- Wine then abandons the current unix call and comes back to the PE side with the mutex held. The next present blocks.

Fix (LKD, `src/lkd/src/mm/watch.c`):
- `mm_unwatch_for_write` distinguishes "page found watched or protection changed" (2) from "already writable, nothing done" (1).
- Only case 1 counts toward the bound, now 1000.
- A Linux write to a writable VMA must never fail because of code watching.

Witnesses (`witnesses/glibc-x86_64/`):
- w23_codedatapage reproduces the defect (SIGSEGV, rc 139 before; PASS after);
- w22_mutexsig (mutex under signals) passes;
- w24 (`cond_timedwait`) passes;
- non-regression: w06, w10, w15, w17, w18, w19 and w21 pass.

Verification (MEASURED, `evidence/eac-runs/20261003T023459/`):
- EAC initialized;
- menu reached, music started at 77 s;
- 3,540 frames presented in 150 s, about 16 ms per frame at the menu;
- game fingerprints unchanged.

The mapped texture copy flush (Scaleform text) is re-enabled by default (`NPT_FLUSH_MAPPED_COPY=0` turns it off). It had been wrongly blamed.

To do (performance): the mixed PLT+data page causes a storm of SMC invalidations in any library whose data shares a host page with code. Leads: finer watching, or linking our .so files at 16 KiB (`-z max-page-size=16384`).

## Scaleform text that disappears: fixed (2026-10-03, 02:56)

Cause (MEASURED, client trace `NPT_TRACE_STAGING`, attempts `20261003T024536` and `025108`):
- Rocket League's glyph cache copies its atlas (GPU texture 1024x1024 R8) into a STAGING texture (CPU write-only access), then maps it with `D3D11_MAP_WRITE`.
- It then adds the new glyphs and sends the whole mapped image to the atlas through `UpdateSubresource`.
- The Neptune renderer (`src/graphics-spike/virgl/src/neptune/npt_resource.c`, synchronous map) only copied the content back into the shared slot for a READ access.
- The game therefore saw an empty image apart from the current glyphs: each upload erased the old ones. Measurement: non-zero bytes sent 17,617, 24,879, 7,313, 946… instead of growing.
- The client fix of 01:40 (copy from a mapped texture) targeted a sequence the game does not use: the trace never saw it.

Fix:
- For any D3D11 Map that is not DISCARD (WRITE, READ_WRITE, NO_OVERWRITE), the renderer copies the current content into the slot, as D3D11 semantics require.
- The D3D12 path is unchanged.

Verification (MEASURED, attempt `20261003T025436`, capture `evidence/wine-linux/play/t2-menu-win1846.png`):
- the atlas grows without ever going down (17,617 → 117,736 non-zero bytes);
- all menu texts are displayed: buttons, challenges, player name, rank.

## Player session of 2026-10-03 (02:57–03:57): display lost when entering Free Play

MEASURED (`evidence/eac-runs/20261003T025713/`):
- Authenticated launch, EAC active, menu with complete text. Free Play loaded at 151 s.
- Right after, the driver receives `WSD_RELATIVE_MOUSE` (mouse captured, last line of `wsd.log`: "relative mouse on").
- Then the game's two connections to wsd are lost ("lost the connection to the window server wsd"). wsd disappears without an exit message, without a crash report and without "signal 14": that is the signature of a force quit or of a Quit through the Dock.
- The game continues without display until the one-hour limit (log up to 3,542 s, rc 142). Fingerprints unchanged.

Probable cause (READ, `src/wsd/wsd.m`):
- In relative mode, wsd calls `CGAssociateMouseAndMouseCursorPosition(false)`, which acts on the whole system.
- Losing focus (Cmd+Tab, notification) did not give the cursor back: it stayed frozen everywhere.
- Stopping wsd was then the only way out.
- Automatic reproduction (60,000 relative movements at 1000 Hz in Free Play, `run/mouse-stress.sh`, attempt `20261003T035930`): wsd holds. But the screen was asleep (window occluded) and the focus scenario could not be replayed.

Fixes (wsd):
- focus loss: cursor given back and relative mode suspended;
- focus return: relative mode resumed;
- a Quit (Dock or menu) is now logged;
- `wsdctl move COUNT HZ` (test input) injects relative movements.

Not validated on screen: to check with the user.

## Keyboard and sound (2026-10-03, 10:35)

Keyboard:
- Cause: the game windows are created borderless, because Wine draws the frame itself. A borderless `NSWindow` refuses key window status, so no `keyDown` arrived (`key=0` in all window states). Injected keys worked because they were sent directly to the window.
- Fix: subclass `WsdNSWindow` (`canBecomeKeyWindow`/`canBecomeMainWindow` → YES) in `src/wsd/wsd.m`.
- Measurement (`evidence/eac-runs/20261003T103502/wsd.log`): game window `key=1 app_active=1`.

Sound:
- Cause: Wine built without a Linux audio driver, and no sound server on the host side ("No driver from pulse,alsa,oss,coreaudio").
- Fix: new Wine driver `wineobeliskaudio.drv`, derived from wineoss for the timing logic.
  - The frames go into a shared ring (memfd, `struct wsd_audio_ring`), passed to wsd through SCM_RIGHTS.
  - wsd plays them through one CoreAudio AudioUnit per stream, whose callback reads the ring directly: `WSD_AUDIO_OPEN/START/STOP/RESET/CLOSE` in `wsd_protocol.h`.
  - Opening is synchronous (PING/PONG): a refusal from wsd makes the stream fail cleanly.
  - `obeliskaudio` is at the head of mmdevapi's default list. No MIDI or aux. No capture yet.
- Measurements:
  - native client: 48,000 frames read in 1.12 s;
  - event-driven shared WASAPI (`src/wine-linux/tests/wasapitone.c`): position 2.000 s after 2.154 s, in float 48 kHz stereo;
  - the game opens its stream (format 4 = float, 48 kHz, 2 channels).
- Limits: Wine's per-stream volume is not applied (only total silence is, like wineoss); no microphone.

## Map loads: from 26–38 s to 2–5 s (2026-10-03, 14:57)

MEASURED (`sample` profile of each guest process, triggered by each `LoadMap:` of `Launch.log`, launches through Obelisk):
- Before: Free Play 37.8 s, back to menu 18.4 s, startup up to the menu 80.6 s. The game's main thread spent 69% of the load blocked in `read` on a pipe (Wine waits), 10% in `getdents64`, 10% in JIT code. The render server and wineserver were almost idle.
- `getdents`: `snap_add` compared each name with all previous ones. `CookedPCConsole` contains 20,746 files, that is about 215 million `strcmp` and 357 ms per listing. Wine lists the folder on each case-insensitive lookup that fails. The names of a host `readdir` are unique: only mount points and virtual folders keep the check. Listing: 6.4 ms (test `getdents_large_directory`).
- LKD was compiled in Debug (without optimization) by default, and that binary is the one that went into the `lkd` archive. `lkd/build.sh` now compiles in Release. The whole suite passes in Release (the bound test of `mm/watch.c` still expected the old bound of 4).
- After the two fixes: Free Play 4.6 s, menu 1.6 to 2.3 s, startup 28.3 s, online arena 3.6 s. The online match held until the player left, without "Error: 42". During the Free Play load, no process is saturated (wineserver at about 6%).

## Lag spikes and frame that goes backward (2026-10-03, 15:55)

Symptoms reported by the player: big lag spikes in game, the menu staying slow after a match, and the whole frame going backward for the time of one frame.

MEASURED, in three steps:
- The afternoon's `sample` profiles skewed the sessions. They suspend each thread about a thousand times per second, and Free Play dropped from 60 to about 25 fps while they ran. Without a profile, Free Play holds 60 fps. Hence the internal log `FEXDARWIN_HITCH_LOG` (FEX translation time per thread, SMC faults and invalidations, 50 ms windows, faulting pages every 10 s) and the file `fexdarwin.env`, which enables it without touching Obelisk's launcher.
- SMC storm: about 19,000 write faults per second in the game process during the whole match, 0.9 s of handling per 10 s slice, almost all on `nptunix.so+0x3000`. The end of its code and its data (`.got`, `.data`, `.bss`) shared a 16 KiB host page. By linking the 27 Unix `.so` files of voile-wine with `-z max-page-size=16384` (Voile, `build-linux.sh`), it drops to 4 faults per 10 s, and the spikes disappeared from what the player feels.
- Frame going backward: wsd has its GPU copy 64 pixels of the buffer it draws. Frame N is sometimes drawn with the pixels of N−3, which are the previous content of the same swap chain buffer. Read back by the CPU 2 to 8 ms later, the buffer contains the right frame: the copy arrives, but after the "GPU done" signal. Side fix in vtest (neptune): retiring a fence on a Neptune event ring took the previous submissions of the same ring with it. It was a real defect (premature signal, then use after free at the next retirement), but it was not the cause: after the fix, about one frame per minute still goes backward. Next lead: order the present on the real end of the DXMT command buffer (D3D11 event query on the render server side) rather than on the shared event.


## Host PID for Steam: `lkd_host_pid` and `steam-hostpid.so` (2026-10-10, 08:20)

Context: ELDEN RING (Steam) path on the Linux engine. Valve's Linux `steamclient.so`, in the guest, talks TCP to Obelisk's arm64 Steam engine and attaches `getpid()` to each IPC message. The engine cuts the pipe (RST) 2 to 3 s after the connection if this PID is not a live macOS process. LKD's guest PIDs (2, 3...) are not.

- LKD: `/proc/<pid>/lkd_host_pid` (and `task/<tid>/`, `/proc/thread-self/`) gives the macOS PID of the host process, read from the family registry. It stays accurate after a fork, an exec and the migration of a vfork child. It does not appear in the listing of `/proc/<pid>`. Details: `lkd/docs/status/procfs.md`.
- `guest/steam-hostpid.so` (x86-64 library, zig, glibc 2.36): `getpid()` returns this host PID only when the caller is in `steamclient.so` (return address, `_dl_find_object`). For any other caller, it returns the guest PID.
  - Why filter by caller: ntdll sends `getpid()` to wineserver (`unix_pid` of `init_first_thread`), which signals the threads through `tgkill(unix_pid, unix_tid)` and reads their memory through `process_vm_readv` (`server/ptrace.c`). A host PID would give ESRCH there. Limiting the preload to the game's tree is not enough: the game process is itself a Wine process.
  - The 22 `getpid` calls in `steamclient.so` (build 1788652215) are `call`s through the PLT, without a tail jump: the return address is indeed in the `.so`.
  - The result is cached, with the guest PID as the key: a fork (guest PID never reused) rereads the file.
- MEASURED, witness `tests/glibc-x86_64/hostpid.sh` (w25): main thread, thread, fork, exec, `posix_spawn`, then the library called from a fake `steamclient.so`, from a thread and after a fork. Each announced PID is a live `fexdarwin-run`, with the right macOS parent (`ps`). PASS.
- MEASURED, live Steam probe (Obelisk session of ELDEN RING, only the `Steam3Master` port read):
  - with the library: full session (pipe, user, license, encrypted ticket `eResult=1`), 25 s stable, exit 0;
  - without the library (control): RST about 2 s after the connection, fatal assertion, exit 1.
- For Voile and Obelisk: ship `guest/bin/steam-hostpid.so` in the Linux runtime, and preload it (`LD_PRELOAD`) in the launcher's guest. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/05-lkd-pid-hote.md`.

## Orphans: a live and readable parent (2026-10-10, 10:50)

Context: ELDEN RING on the Linux engine. Its EAC bootstrapper, launched by `steam.exe`, always ended in `208, 'Launch Error: 256'` (sailover report 07, § 4). EAC reads `getppid()` then `/proc/<ppid>/{comm,exe,cmdline,status}` (EAC digest, H8). But Wine creates each process through a double fork (`ntdll/unix/process.c`, `spawn_process`): the intermediate process exits at once and the created process becomes an orphan.

What LKD did (MEASURED, probe `ppid-probe.sh`: `ping.exe` launched by `cmd.exe`):
- without a subreaper, the orphan went to pid 1. No LKD family has a pid 1 (pids start at 2): `getppid()` returned 1, and `/proc/1` did not exist;
- `/proc/<pid>/status` did worse. It showed the dead intermediate process (`ping.exe`: `PPid 51`; `winedevice.exe`, `plugplay.exe`, `svchost.exe`: 12, 22, 28 or 34), because procfs returned the local copy of the ppid, which the process only rereads if it calls `getppid` itself;
- a parent whose host process dies without going through its exit (host SIGKILL) kept its children: their ppid stayed on it.

Fix in lkd (`task/wait.c`, `task/family.c`, `task/tasks.c`, `procfs/pid.c`), on the model of `kernel/exit.c` v6.12:
- the family's init is its first process registered without a parent in the family, the one the engine launches (at Obelisk, the `sh` of `launch.sh`). The registry keeps its pid in its header (version 2). Like the `child_reaper` of a Linux pid namespace, it receives the orphans that have no live subreaper among their ancestors;
- `PR_SET_CHILD_SUBREAPER` and `PR_GET_CHILD_SUBREAPER` already existed. The subreaper keeps priority, and the flag does not pass to children;
- the orphan takes SIGCHLD as its exit signal (`reparent_leader`). A reparented zombie is announced to the new parent, which can reap it through `wait4`;
- the same reparenting happens when a parent's host process dies without its exit. The parent death signal then goes out with SI_USER and the pid of the dead parent, like `SEND_SIG_NOINFO`;
- `/proc/<pid>/status` and `stat` reread ppid, pgid, sid and the exit signal from the registry before rendering.

The first process does not become pid 1. Reasons:
- EAC requires a live and readable parent, not pid 1. On the Bazzite PC, the game has `pv-adverb` as its parent, a subreaper; under HyperLinux, a `dash` with pid 1 also passed the parent check (EAC digest, § 1.3 and H8);
- Wine as pid 1 (PPid 0) gives exactly 256 on a real kernel (`pid1-control`). With the first process numbered 1, any launch where Wine is that first process would be exposed to it;
- a real pid 1 also has the `SIGNAL_UNKILLABLE` protection, the exclusion from `kill(-1)`, and its death stops the whole pid namespace. LKD has none of these rules: numbering the first process 1 without them would invent half an init;
- Rocket League's bootstrapper already has this `sh` (pid 2) as its parent and gets 301. Nothing changes for it;
- LKD does not give pid 1 by documented choice (`first_pid` is 2 by default, task status, point 1). `first_pid = 1` remains possible through configuration, and the family's init is then pid 1.

MEASURED:
- LKD suite: 42 programs pass; `test_sig_futex` exceeds its timeout as on the intact tree, at the same case (`waitv`). `test_task_proc` counts 21 cases (260 assertions) and `test_procfs_other` 6 cases. The five new cases fail without the fix;
- guest witness `w26_reaper`: 8 failures with the `fexdarwin-run` of the -13-dev runtime, PASS with the new one;
- ppid probe: before, `ping.exe ppid=51 parent_comm=UNREADABLE`; after, `ppid=2 parent_comm=sh`, like all Wine processes;
- ELDEN RING, bootstrapper behind `steam.exe` (`er-direct.sh` in mode B, runtime and prefix copied, `/opt/game` read-only, Obelisk's Steam binds): before, `208, 'Launch Error: 256'` 0.9 s after the start of the mapping; after, the mapping lasts 89 s and ends in `210, 'Unexpected error. (#1)'`. That is the verdict of the direct launch (mode A) of report 07, another work item;
- Rocket League (`rl-direct.sh`): `301, 'Easy Anti-Cheat successfully loaded in-game'` 0.39 s after the start of the mapping.

Still open: the family's init does not have the rules specific to pid 1 (see above), and the first process keeps `ppid` 1 without `/proc/1`. Linux would give 0 for a parent outside the pid namespace. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/08-lkd-orphelins.md`.

## ELDEN RING: EAC's 210 comes from the name of Wine's preloader (2026-10-10, 11:20)

Context: ELDEN RING's EAC bootstrapper (EOS, 1.9.4) ended in `210, 'Unexpected error. (#1)'` after 81 to 89 s of mapping, launched by the launcher's `sh` or, since the orphans fix, behind `steam.exe`. With the same engine, the same prefix and the same launcher, Rocket League gets 301 in 0.4 s.

MEASURED, complete LKD traces of both bootstrappers (local instrumentation, not committed):
- The two launch modules are not of the same generation. ELDEN RING: 9,168,824 bytes, GCC 7.5, a single RWX PT_LOAD whose code rewrites itself. Rocket League: 13,825,695 bytes, GCC 11.4, separate segments, nothing RWX. RL's module from before September 29 (9.7 MB) was probably of the first kind.
- Up to the fork, both modules make the same calls, with the same answers: `/proc/<pid>/maps`, `dlopen` through `/proc/self/fd/N`, `/proc/<pid>/status`, `/proc/<ppid>/{comm,cmdline,status}`, `/proc/mounts`, DMI and PCI (ENOENT), `readlink(/proc/self/exe)` then the `realpath` of its target, `runtime.conf` (ENOENT). No ENOSYS, EINVAL, EPERM or ESRCH specific to ELDEN RING, no `process_vm_readv`.
- The 81 s are computation in the module's thread: one or two system calls per 10 s slice, about 33,000 SMC write faults per second, as many invalidations and retranslated blocks. The code runs in the page it writes (`rbp` at module+0x5d2d3, `rip` between module+0x5d546 and +0x5e96b).
- Then come the fork and the known ptrace relationship (TRACEME, SIGSTOP, SETOPTIONS 0x11, SYSCALL, GETREGSET), three `clock_gettime` followed by the tracer, the memfd of the second ELF (6,130,496 bytes, consistent headers), the seals, the read of the first and last page of `eldenring.exe`. Then `readlink("/proc/self/exe")` returns `/opt/wine/lib/wine/x86_64-unix/wine-preloader` and the child does `exit_group(1)` without any other call. The tracer reads status 0x100 and the bootstrapper writes 210 (#1).
- With the same `readlink` answer, Rocket League's child does `newfstatat(.../wine64)` (ENOENT), then `execve` of `[wine-preloader, wine, Z:\...\RocketLeague.exe]`.

Cross-checks, on a private copy of the Wine tree (no EAC or game file touched):
- `x86_64-unix/wine64-preloader` as a real file, `wine-preloader` as a link to it, `wine64` as a link to `wine`: the child runs `[wine64-preloader, wine64, Z:\opt\game\Game\eldenring.exe]`, injects its module, and the bootstrapper writes `301, 'Easy Anti-Cheat successfully loaded in-game'`. Launched by the `sh` as well as behind `steam.exe` (Obelisk's Steam binds). Rocket League stays at 301 (0.38 s) with this tree;
- same 301 with the preloader placed at `bin/wine64-preloader`.

The old-generation module therefore wants a 64-bit preloader named `wine64-preloader`, Wine's name before the new WoW64. GE-Proton 11 still ships `wine64` and `wine64-preloader` next to `wine` and `wine-preloader`. Voile's Wine 11 only has `wine-preloader`. INFERRED: a real Linux with this same Wine tree would give the same 210, since the calls and their answers do not differ. Nothing to fix in LKD for this verdict: the fix is in the Wine tree of Voile's Linux runtime.

The slowness remains. Once the child is accepted, the mapping lasts 234 to 240 s: the tracer runs the same self-modifying code between its ptrace stops. `--smc=full` brings the 210 path down to 1.5 s, but breaks the injection: the block of return address 9 is retranslated in a loop (`NoExec instruction in entry block: 9`, more than a million times) and the tracer never sees the SIGSEGV. The clean way is in FEXCore, a per-block validation (`ForceFullSMCDetection` exists, reserved for mono hacks) for pages that take write faults in series. Not done here: FEX does not accept generated contributions.

EFLAGS (fixed in fexdarwin): FEXCore's `ReconstructCompactedEFLAGS` also adds the bytes of its NZCV copy (`flags[24..27]`) to bits 24 to 31. MEASURED: bit 31 appeared in ptrace's eflags (and in the r11 that LKD derives from it at syscall stops), and in the `uc_mcontext` of signals, which Wine copies into the Windows CONTEXT. `GuestEflags` (`include/Host.h`) keeps only the defined bits of RFLAGS, bit 1 set to 1, for `GetRegs` and signal frames. Witness `w27_eflags`: 11 failures out of 12 contexts with the engine of the -13-dev runtime, PASS after. w22, w23, w24 and w26 pass. EAC probes: 4,376 checks, the same 38 failures before and after. EAC's verdicts do not change (301 and 210 as before). Report: `sailover/build/experiments/eldenring-linux-20261010/reports/09-eac-210-eldenring.md`.

## Code rewritten nonstop: the page leaves write tracking, ELDEN RING's EAC mapping goes from 233 s to 2.8 s (2026-10-10, 12:40)

Context: with the renamed preloader (previous section), ELDEN RING's EAC launch module returns 301, but its mapping lasts 233 to 240 s. Rocket League takes 0.4 s on the same stack.

MEASURED, engine profile of `c545aa6` (`FEXDARWIN_PROFILE=1`, `FEXDARWIN_HITCH_LOG=1`):
- All faults fall in a single 16 KiB host page, `memfd+0x5c000` of the module, in its RWX PT_LOAD. It is the page of the loop that does the writes.
- 352,112 SMC write faults in 11.26 s, as many invalidations, as many retranslated blocks. Each write costs a fault, the invalidation of the page and a new translation, about 30 µs in all.
- The forked child takes up the same page at the same rate.
- It is mostly data that the module writes. Once the page is switched to verification, the bootstrapper process only counts 130 SMC faults and 256 invalidations over its whole life.

Fix, in FEX and fexdarwin (LKD is not modified):
- Zagrios/FEX `3d0290743`. An instruction without decoded bytes (read in a non-executable page, invalid opcode) is no longer verified. `ValidateCode` on zero bytes compared the expected CRC to TMP1, which nothing had computed. The block was removed and retranslated in a loop, hence the "NoExec instruction in entry block: 9" of `--smc=full` and the SIGSEGV that EAC's tracer never saw. `w11_badjump` in `--smc=full`: 1,290,237 retranslations and 5.45 s before (the loop ends by chance here), 2 retranslations and 0.24 s after.
- Zagrios/FEX `d08fd1541`. `SyscallHandler::NeedsCodeValidation(Start, Length)`: in mtrack mode, the instructions of a block that fall in a page for which the frontend answers true get the full mode's byte verification. FEXCore asks the question once per code page of the block, invalidation mutex held.
- fexdarwin, `CodeValidation` (`3932460`). A host page that takes 128 SMC write faults in 100 ms leaves write tracking until the next exec. The fault handler marks it, then invalidates it itself: LKD's invalidation can be missed if another thread removed the watch in the meantime, and a translation started before the mark can finish after it without watching the page. `MarkGuestExecutableRange` no longer asks LKD to watch a marked page. The other pages stay tracked.

MEASURED, private runtime and prefix, renamed Wine tree from report 09:

| Attempt | Before | After |
|---|---|---|
| ELDEN RING, launched by `sh` | 301 in 232.6 s (240.2 s with profile) | 301 in 2.74 to 2.80 s, 6 attempts including 2 with profile |
| ELDEN RING behind `steam.exe` | 301 in 235.1 s (report 09) | 301 in 2.74 and 2.75 s |
| ELDEN RING, original Wine tree | 210 after 85 s | 210 after 0.18 s |
| Rocket League | 301 in 0.40 and 0.43 s | 301 in 0.38 to 0.42 s, 6 attempts |

- No Rocket League page leaves tracking: at most 10 SMC faults per process and per 10 s slice, far from the threshold. D3D11 through DXMT and D3D12 through D3DMetal: exit 0, readbacks MATCH, medians equal within noise.
- A verified page has a cost. A tight ALU loop runs 1.7 times slower in it (378 ms versus 225 ms), and the page stays verified until the exec even when the writes stop. I did not add a return to tracking: the page would have to be invalidated to get blocks without verification again, and no measured case requires it.

Witnesses:
- `w28_smcstorm` (new, standalone): a storm of 20,000 writes, then rewrites between calls, write into the current block, hot loop that patches its function, instruction straddling a tracked page and a verified page, code written by the kernel (`read` from a pipe), call into the page made non-executable (SIGSEGV at the right address). 26/26 in mtrack and in full, 15/26 in `--smc=none`. The storm takes 0 to 1 ms instead of 130 ms.
- `w29_smcthreads` (new, glibc): one thread patches the code, another runs it, a third writes data into the same page. W29 PASS 5 times out of 5 after. Before, 2,390 to 6,153 rounds out of 20,000 return the old value. Cause, already there before this work: write tracking only protects a page after translating its code, and a write that falls between the two is never seen. INFERRED: upstream FEX has the same order (`MarkGuestExecutableRange` after compilation). Not fixed for the pages that stay tracked.
- `w01` to `w21` (except `w07`, which exceeds its timeout before as after), `w23`, `w24`, `w26` and `w27`: same results before and after. `w06_smc`: 20/20 in mtrack and in full, 12/20 in none, as before.
- `w22_mutexsig` sometimes fails on both engines, through a jump to an invalid address: 1 time out of 66 before, 2 times out of 67 after. `w26_reaper` once took the FEX dispatcher's SIGBUS right after a fork (request `wine-5`, already known), then 6 passes out of 6.
- LKD suite: 42 programs pass, `test_sig_futex` exceeds its timeout as on the intact tree. EAC probes: 4,376 checks, the same 38 failures.
- Fingerprints of the game and EAC binaries unchanged.

Limits:
- Rocket League's FPS in a match are not measured. Without an Epic session the game quits after 19 s without displaying a frame, and the Obelisk app was being used for other work. Since no RL page leaves tracking, FEXCore produces the same code as before; there remains one question per code page at each compilation, which returns false without searching as long as no page is marked.
- An invalid instruction in a verified page is not re-verified. If a program patches its bytes in place later, the block still raises SIGILL. I have seen no program do so.

Report: `sailover/build/experiments/eldenring-linux-20261010/reports/11-fex-smc-eac.md`.

## SCM_RIGHTS: emulated devices, synthetic directories and the other objects without a host fd pass from one host process to another (2026-10-10, 13:15)

Context: with the -13-dev2 runtime, ELDEN RING's EAC returns 301 in Obelisk, then the game dies a second later: `lkd: net: SCM_RIGHTS of a dev object cannot cross host processes (-EOPNOTSUPP)` and `wine client error:198: sendmsg: Operation not supported`. `steam.exe` creates `start_protected_game.exe` without standard handles, Wine gives it `/dev/null` as fd 0 and 1 (`ntdll/unix/process.c` `set_stdio_fd`). Under LKD it is the emulated `/dev/null`, without a host fd. The Wine process of `eldenring.exe`, launched by EAC's child, has no Wine parent and sends its fds 0, 1 and 2 to wineserver (`env.c` `get_initial_console`). `net_scm_export` refused any object without a host fd. Same refusal for the synthetic directory `/games` (sailover report 06). Probe: `wine cmd /c ver </dev/null` failed the same way.

Inventory of descriptor kinds (before):

| Kind | host fd | SCM_RIGHTS before |
|---|---|---|
| file, host directory, pipe, socket, tty, host shm, memfd | yes | host fd (memfd: deleted file, without name or seals) |
| eventfd | no (shm region and pipe) | 3 host fds |
| ntsync, device and objects | no (anchor) | the anchor |
| /dev/null, zero, full, random, urandom | no | refused |
| synthetic directory, directory of /proc, /dev, /sys | no | refused |
| O_PATH | no | refused |
| generated file of /proc and /sys, /proc/<pid>/mem | no | refused |
| signalfd | no (notification pipe) | refused |
| epoll, timerfd, inotify | kqueue | refused (macOS does not pass a kqueue: EINVAL) |

Fix (`4ad078c`, witness `7f98f29`), general mechanism: `lkd_fd_ops.export_fds` says what carries a description, the receiver rebuilds it (`net_scm_import`).
- Host twins. `/dev/null`, `/dev/zero`, `/dev/random` and `/dev/urandom` send the macOS device with the same behavior, opened at the first send with the guest access mode. A twin that arrives without a guest path, through SCM_RIGHTS or as standard input of the runtime, becomes the Linux device again (1:3, 1:5, 1:8, 1:9): `lkd_procfs_dev_adopt`, called by `fs_wrap_host_fd`. Reads and writes stay in LKD. MEASURED: macOS refuses `write` on `/dev/urandom` (EPERM), returns POLLNVAL to `poll` and EINVAL to `kevent` on its devices; `F_SETFL` O_NONBLOCK and O_APPEND pass there and are shared through SCM_RIGHTS.
- Records (`src/core/fdrec.c`). An object without a host form goes out as a 16 KiB POSIX shm object, deleted as soon as it is created, with the signature `LKDFREC1`, its type, the Linux status flags and guest-level data only (no host path, no pointer). The owner module reopens the thing in the receiver, with the receiver's rights: synthetic and virtual directories, O_PATH (global guest path, `fs_rec_import`); `/dev/full`, without a macOS twin, and O_PATH devices; generated files and `/proc/<pid>/mem` (canonical path and position); signalfd (its mask; it reads the receiver's signals, like `signalfd_dequeue` on `current`).
- epoll, timerfd and inotify stay refused: their state is a kqueue and a table of this process.

Gaps: the receiver of a record has a new description of the same thing. Position and flags changed afterwards are not shared, the rule of `fd.h` for objects without a host fd. Noticed in passing, not fixed: a pipe created by LKD arrives non-blocking at the receiver (its host fd always is, MEASURED by `F_GETFL`); an EFD_NONBLOCK eventfd arrives blocking; a memfd arrives as an ordinary file.

MEASURED:
- `test_net_scm` (new, 5 cases, 38 assertions): devices there and back, host `/dev/null` as standard input, synthetic directory, `/dev`, `/proc/<pid>`, O_PATH, `/proc/<pid>/status` at its position, signalfd, between two host processes. The 5 cases fail on the previous tree. Suite: 43 programs pass; `test_sig_futex` exceeds its timeout at the same case (`waitv`) as on the intact tree.
- Witness `w30_scmdev` (glibc, new): nothing received with the engine of `c545aa6` (same LKD as `97f227e`), `W30 PASS` after. `w22` to `w27` pass with the final engine (`w25` through `hostpid.sh`).
- Wine probe, prefix and runtime copied: `wine cmd /c ver </dev/null` returned 1 with `wine client error ... sendmsg: Operation not supported`, returns 0 (`Microsoft Windows 10.0.19045`). `wine cmd /c ver` alone: 0 before and after. Two Wine sessions in a row (`wineserver -p0` between them) sometimes stay blocked, with the old engine too (1 time out of 3): the second `cmd.exe` waits for an answer from wineserver that does not come. Pre-existing, not handled.
- Rocket League (`rl-direct.sh` copied, private runtime and prefix, `/opt/game` read-only): `301` in 0.39 s.
- ELDEN RING behind `steam.exe` (private prefix, without a Steam host): `301` in 2.7 s, no more SCM error nor Wine client error.
- ELDEN RING launched from Obelisk, engine of `4ad078c` placed by hand (with the SMC fix of `3932460`): `301` in 2.74 s (instead of 241.5 s in report 10), no more SCM error. No Cloud question, save and game binaries unchanged.

The game goes no further, and it is no longer this defect. The Wine process of `eldenring.exe` disappears less than a second after the 301, without a message (its error output is the `/dev/null` of `set_stdio_fd`), without a window or D3D12 device. MEASURED, LKD trace of a private attempt behind `steam.exe`: after the exec, the process talks to wineserver, creates five threads; one launches a program with its output into a pipe (`clone3` then `execve`, like `popen`), which ends in 127 (INFERRED: `sh -c` of a command not found); then the five threads leave their waits together through EINTR and the process disappears without `exit_group`: a fatal signal. fexdarwin reports no fault. The direct launch of report 09 (`er-ge2-mtrack`, the launcher's input/output, so without this defect) already ended the same way: the whole session stopped 2 s after the 301, without a normal exit of the game. INFERRED: it is the next blocker, independent of SCM_RIGHTS. The name of the command and the signal are not identified. The `0xdeadba` crash, `SteamAPI_Init` and the fps are not reached. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/12-lkd-scm-devices.md`.

## ELDEN RING after its 301: a CrossOver dlopen before GS points to the TEB (2026-10-10, 13:55)

Context: the previous entry left the `eldenring.exe` process dying less than a second after EAC's 301, from an unknown fatal signal. LKD had no way to say which signal kills a process.

LKD fix, diagnostic only: with `LKD_TRACE`, the signal that ends a process gives a line `lkd: pid N dies of signal S in tid T: ...`, with the sender (si_code <= 0) or the faulting address and the rip. It goes out from `complete_signal` and from `lkd_signal_next`. Without `LKD_TRACE`, nothing changes. Case `fatal_signal_trace` in `test_sig_basic`.

MEASURED, private attempts behind `steam.exe` (-13-dev2 runtime copied, engine `f587e5a3` then engine of this commit, private prefix, without a Steam host):
- The death: `lkd: pid 77 dies of signal 11 in tid 77: fault at 0x30 (si_code 1), rip 0x7f827548df5c`. Neither kill nor tgkill: a fault of the main thread, before Wine installs its handlers (`signal_init_process`), hence default action.
- The rip is in EAC's second ELF, injected into the game (memfd, loaded RWX): `module+0x48df5c`, `movq %gs:0x30, %rax`. EAC reads the TEB through GS to walk the PEB's list of Windows modules.
- The thread's last call before the fault: `openat("/opt/wine/lib/wine/x86_64-unix/cxcompatdb.so") = ENOENT`. In voile-wine, `start_main_thread` does a `dlopen` of this CrossOver library (CW Hack 24067), just before `server_init_process_done`. On Linux, Wine only sets the GS base to the TEB afterwards (`init_syscall_frame`, `arch_prctl(ARCH_SET_GS)`). GS is 0, the read lands on 0x30. Voile ships no `cxcompatdb.so`.
- A/B: the runtime's `ntdll.so` with the call to `dlopen` replaced by `xor eax,eax`: no more fault, the game continues up to steamclient. An empty `cxcompatdb.so` in its place (dlopen succeeds): same fault at 0x30. EAC therefore reacts to any `dlopen` from this thread. Proton and upstream Wine do no `dlopen` at this point.
- The program launched like `popen` is `sh -c -- "lshw -C display 2>/dev/null"`: `lshw` is missing, `sh` returns 127, EAC reads an empty output and continues. The 255-byte file is `/proc/cpuinfo`. The five threads (tids 78 to 82) are created during the injected `dlopen` of EAC's module, before the rest of Wine's initialization. None of this kills the game.

Fix, outside this repository: voile-wine `1aeb924` removes this `dlopen`. voile-wine `829f80c` also takes Proton's fix for this case (`aa83aa0a89`, "Set thread teb earlier in start_main_thread()", CW-Bug-Id #22218): on Linux x86-64, GS points to the TEB from `init_files` on. MEASURED: with this change alone, `dlopen` kept, no more fault at 0x30 either. Voile `787c237` then `eda2e17` take these commits. `ntdll.so` rebuilt with Voile's recipe (the same recipe reproduces the installed `ntdll.so` byte for byte), then put into the private runtime:
- ELDEN RING: 301, then the game loads steamclient, fails for lack of a Steam host (`Failed to connect to Steam`, expected outside the app) and exits. At its exit, an EAC thread (tid 78) jumps into glibc's `main_arena` (fd/bk pattern of a freed chunk), then Wine's SIGSEGV handler faults in turn on this thread without a TEB. INFERRED: use after free on EAC's side during the exit. Not handled.
- Rocket League (`rl-direct.sh` copied, without Epic): 301 in 0.37 to 0.39 s, with the first commit alone then with both.

Seen in passing, not fixed:
- EAC removes `LD_PRELOAD` from the game's environment: `steam-hostpid.so` is no longer loaded in `eldenring.exe`. steamclient.so will therefore send the guest pid to Obelisk's Steam engine there. To check in the app, with the Steam host.
- `semget` (System V IPC) returns ENOSYS: steamclient.so shows it as an assertion (`threadtools.cpp (2526)`), then wants to write a minidump with `clone(CLONE_FS | CLONE_FILES | CLONE_UNTRACED)` without CLONE_VM, which LKD refuses (EINVAL).
- The Debian root has neither `/etc/os-release`, nor `/etc/machine-id`, nor `/etc/hostname`, which EAC reads.

Suite: 43 programs pass, `test_sig_futex` exceeds its timeout at the same case as before. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/13-eldenring-apres-301.md`.

## ELDEN RING in Obelisk: Steam holds, then Wwise gives up for lack of a CPU mask (2026-10-10, 14:45)

Context: the previous entry left ELDEN RING passing its 301 in a private attempt, without a Steam host. First attempt in Obelisk (-13-dev2 runtime, `ntdll.so` from voile-wine `829f80c`, engine `f587e5a3`).

MEASURED, three launches from the "Launch" button:
- Launch 1: 301 in 1.37 s, fullscreen window 2560x1440, D3D12 device. Obelisk's Steam engine closes the game's connection one second after it opens, then steamclient.so stops: `fatal stalled cross-thread pipe (pipe is disconnected)`. It is the guest pid from the entry of 2026-10-10, 08:20: EAC removes `LD_PRELOAD`, `steam-hostpid.so` is missing in `eldenring.exe`.
- Fix outside this repository: Voile writes `/etc/ld.so.preload` (`/opt/obelisk/steam-hostpid.so`) in the root. glibc's loader reads it at each exec, whatever the environment. The traces of report 13 show that only ld.so reads this file (one `access` per exec), never EAC's module.
- Launch 2: `steam-hostpid.so` is in the game's module list, the Steam connection holds 42 s. The game creates its swap chain, then stops on `movl $0xdeadba, 0` (`eldenring+0x1ebb809`), the crash of report 07. On the stack: `Failed MagicOrchestra_Wwise::MOSoundManager::Initialize()` and `DL_PANIC is firing core dump...`. Just before: `wineserver: file_set_error() can't map error: Function not implemented`.
- Cause: Wwise's `AkCreateThread` (`eldenring+0x2385af4` to `0x2385b47`) creates the thread, then calls `SetThreadAffinityMask`. On failure, it closes the thread and returns AK_Fail. wineserver applies this mask through `sched_setaffinity(unix_tid)` on a game thread, so one of another host process, and LKD answered ENOSYS.

LKD fix: `sched_setaffinity` on a thread of another host process goes through the family service (opcode `TASK_OP_SET_AFFINITY`). The process that owns the thread sets the mask and publishes it. Case `affinity_of_a_thread_in_another_host_process` in `test_task_proc`: ENOSYS before, success after.

MEASURED after the fix:
- Launch 3: 301 in 1.39 s, no more `can't map error`. mmdevapi and `wineobeliskaudio` are loaded: Wwise initializes. The swap chain goes to `WSI up 2560x1440`, one frame is presented.
- The game then stops in Neptune: `ucrtbase+0x7b1b0` reads address 1, called by `npt_id3d12object_default_SetName` (d3d12.dll). The caller, `eldenring+0x1e8eae5`, does `call *0x210(%rax)` with `(1, &{va, 2}, &{2})`: it is slot 66, `ID3D12GraphicsCommandList2::WriteBufferImmediate` (markers). In Neptune's d3d12.dll, the v0 table of `ID3D12GraphicsCommandList` plus 0x210 lands on `npt_id3d12heap1_default_vtbl_storage + 0x30`, the `SetName` of the next table. INFERRED: the list was created with the v0 IID, and the game uses it as a v2 without QueryInterface. That works under Windows and vkd3d-proton, where a single table covers all versions. Neptune gives the wrapper the table of the requested IID. To be handled in Neptune, not done here.
- Rocket League, private attempt with this engine and `/etc/ld.so.preload`: 301 in 0.41 s. Without `/opt/obelisk`, the loader writes one `cannot be preloaded ... ignored` line per process, with no other effect.

Suite: 43 programs pass, `test_sig_futex` exceeds its timeout at the same case as before. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/14-eldenring-app-titre.md`.

## ELDEN RING: the engine ran out of host address space, then wsd did not display the second swap chain (2026-10-10, 17:10)

Context: after the fix of Neptune's vtables, ELDEN RING presented 61 frames in Obelisk, then the engine died: `[fexdarwin] FATAL unhandled fault: signal 11 (si_code 2) fault address 0x10005297`, outside the JIT code, with an identical guest state at each launch.

MEASURED:
- The guest state is that of the child of a `clone3` that has not run anything yet: rip = `libc.so.6+0x110529`, right after the `syscall` of `__clone3`, rax = 0, rdx = `start_thread`.
- With the old engine and a survey of the regions through `proc_pidinfo` (without suspending the process): pc = `fexdarwin-run+0x98390`, that is `FEXCore::LookupCache::FindBlock+32`. A variant of the same tree that writes the registers: called by `CompileBlock+132`, `this->L1Pointer` is 0xfffffff. That is -1 + 128 MiB + 128 MiB: the mmap of the new thread's lookup cache (260 MiB) failed, the assertion that checks it does not exist in Release, and the cache was built on the -1. 0xfffffff + 0x529 x 16 + 8 = 0x10005297.
- The cause of the failure: the host's address space was full. macOS 27 keeps [0xfc0000000, 0x7000000000) for itself (VM_MEMORY_GUARD, present in every process), the guest window started at 0x7d80000000. 54 GiB remained for the host, of which libmalloc reserves 12 or 24. Each guest thread takes about 280 MiB of it (FEXCore lookup cache, call-ret stack, host stack). Survey one second before the fault: 53.4 GiB out of 54 occupied, largest hole 387 MiB, about 110 threads.

Fixes:
- FEX `7f6e2a3a7`: a lookup cache that could not be reserved says so (`IsAllocated`), `InitializeCompiler` stops before using it and `CreateThread` returns nullptr.
- fexdarwin: the guest window starts at 1 TiB, which leaves 576 GiB to the host (LKD's 300 threads per process ask for 85). Guest addresses up to 0x7efffe000000, still above EAC's fixed mappings at 0x7001'0000'0000. A thread that the host cannot house returns EAGAIN to `clone`, like Linux without memory for a new task. A failed `pthread_create` no longer leaves the thread in the list, and finished threads are reaped before creating one. `FEXDARWIN_GUEST_BASE` now places the whole window.
- fexdarwin: a fatal host fault writes the load address (for `atos -l`), the offset and symbol of pc and lr, the regions of pc and of the faulting address, the registers and the chain of frame pointers, then the guest thread: its system call, module and offset at rip, code bytes, return addresses on its stack. Lock-free read (`mach_vm_read_overwrite`, `lkd_mm_find_nowait`). Nothing runs before a fatal fault.
- Witness `w31_threadvm`: 280 waiting threads, then join and 50 more. With `FEXDARWIN_GUEST_BASE=0x7f00000000` (the old place): the old engine dies at 0x10005297 in 0.3 s, the new one returns EAGAIN after 123 threads and passes. Without the variable: 280 threads.

The next blocker, in wsd: ELDEN RING recreates its swap chain before the title. Neptune then removes the overlay plane (region 0x0) then presents at the same rectangle. wsd kept this rectangle as current and never placed the new plane's layer: it stayed 0x0, no frame was shown (black window), and each present waited for the 100 ms of Neptune's pacing timeout (10 fps, `1380 presents (0 shown events)`). Fix: a new plane is always placed. Test `tests/graphics/wsd_overlay.c`: 0 frames shown on the recreated plane before, 29 out of 30 after.

MEASURED in Obelisk with both fixes: EAC's 301, first frame at 17:02:09, second swap chain at 17:05:52, then ELDEN RING's title screen at 60 fps (120 frames: average 16.67 ms, min 12.5, max 20.8). Quit through Obelisk, Cloud synced, no question. Loading still takes 3 min 40: in the profile (previous launch), the game's "CR2.Device(HDD)" thread runs at 100% of a core for one minute in translated code, without SMC faults or notable JIT time, and the rest waits. To be studied separately.

Suites: LKD 44 programs including `test_mm_map` (case `find_nowait`), `test_sig_futex` exceeds its timeout at the same case as before; witnesses w01 to w31 as before (w07 exceeds its timeout after 99/99); EAC probes 38 failures out of 4,376, the same; Rocket League 301 in 0.39 s. The dispatcher SIGBUS in a fork child (wineboot) is that of request `wine-5`, not handled. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/16-fexdarwin-fault.md`.

## ELDEN RING starts online: a 4 GiB mmap walked all VMAs for each host page (2026-10-10, 19:05)

Context. With the text back, ELDEN RING displayed "Unable to start in online mode due to a detected frame rate issue" at the title screen, then an OFFLINE menu. Just before the box, presents 361 to 420 of the second swap chain lasted 162.8 ms on average, that is about 9.7 s of stall, with a fence timeout exceeded. At that moment the game creates two committed UPLOAD buffers of exactly 4 GiB. Neptune backs each with a host shm object that the game maps MAP_SHARED, and the render server imports it into D3DMetal.

MEASURED, creation of a 4 GiB buffer in the D3D12 harness (`online18`, mode `bigup`): 1,265 to 1,284 ms. Breakdown:
- host side alone, under Rosetta: shm object 0.01 ms, mmap 0.003 ms, heap import 0.004 ms, placed buffer 66 ms;
- the rest, about 1.2 s, is the 4 GiB MAP_SHARED mmap in the guest, in LKD.

The cause is `mm_watch_alias_sync` (src/mm/watch.c). A new shared view must be born write-protected on the pages where another view of the same bytes carries translated code. For each of the 262,144 16 KiB host pages of the view, the old loop looked for aliases by walking all the process's VMAs, watch lock held. The cost follows pages × VMAs. Witness w32, 4 GiB mmap: 37.6 ms with 18 mappings, 13.6 s with 2,017, more than 120 s with 8,017. Meanwhile, every thread that was translating code waited for the lock.

Fix (12eeeba). The inner pages of the view belong only to the new VMA. Another view of the same object therefore holds them at a constant offset, or not at all. A single walk of the VMAs finds these views, then a sweep of each one's watch bits (64 pages per word) marks the corresponding pages. The two edge pages, which a neighboring VMA can share, keep the page-by-page lookup. Beyond 32 views of the same object, it falls back to the old loop. The semantics do not change: the case `late_view_of_a_large_object` of test_mm_watch places the code at 32 MiB in the object and checks the watched page of the late view, its unwatched neighbors and the invalidation by a write.

MEASURED after:
- w32: 0.032 ms (18 mappings), 0.072 ms (2,017), 0.258 ms (8,017). The code rewritten through a view created afterwards does run in the RX view, at 32 MiB in the object as at the start.
- test_mm_watch, views of 64 MiB and 1 GiB with 4,000 VMAs, native: 7.4 s before, under 250 ms after.
- `bigup` harness (font 4 GiB + 1 KiB, then two 4 GiB buffers, data read back): 63 to 160 ms per buffer instead of 1.27 s (once 460 ms out of 27 creations), sequence 0.23 to 0.31 s instead of 4.4 s (once 0.64 s). `bigframes` with the sequence on the render thread: longest frame 247 ms instead of 4,727 ms. 1,000 UPLOAD buffers of 64 KiB: 0.12 ms each instead of 0.17.
- The remaining 66 ms come from the d3dmetal-native wrapper: it adds each imported buffer to an MTLResidencySet and commits it at once, which pins the whole buffer. MEASURED: +259,745 wired 16 KiB pages for a 4 GiB buffer; with `DMN_RESIDENCY_SET=0`, the placed buffer costs 0.1 ms. ELDEN RING thus wires 12 GiB for its three buffers. Not handled.

MEASURED in Obelisk with this engine (two launches). The first did not reach the title screen in 300 s: loading 4 times slower than usual (heap imports 35 to 45 per 10 s versus 160), cause not found. The second, same engine, `FEXDARWIN_PROFILE=1`: EAC 301 in 1.38 s, title screen 48 s after the first frame. Presents 361 to 420: 41.9 ms on average, no fence timeout exceeded; mmap of the whole process 417 calls, 94 ms over the period of the first 4 GiB buffer. No frame rate box. After "PRESS ANY BUTTON" and the improperly closed session warning, the game displays "Logging in to the ELDEN RING game server…" then the server information: it starts online.

Suites: LKD 43 programs pass, `test_sig_futex` exceeds its timeout at the same case as before; witnesses w01 to w32 as before (w07 timing out after 99/99, w03 identical byte for byte); EAC probes 38 failures out of 4,376, the same; Rocket League 301 three times out of four (0.38, 1.37 and 0.39 s). The fourth launch returned 210: FEX dispatcher SIGBUS in a fork child, the signature of request `wine-5`, intermittent and pre-existing. Seen in passing, already present with the old engine: a host SIGILL at the end of a guest thread (`EndGuestThread`, in `pt_find_tracee` or the munmap of `UnregisterThread`), at teardown. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/18-demarrage-online.md`.

## The SIGBUS at fork (request wine-5): the child received a stale JIT write state (2026-10-10, 20:40)

Context. Since 2026-10-02, a fork child sometimes dies before its first guest instruction: `[fexdarwin] FATAL unhandled fault: signal 10 (si_code 1)`, pc equal to the faulting address, in FEX's dispatcher, on return from `clone` with fork flags (rdi = 0x1200011, rax = 0). When the child is a process of the EAC launcher, Rocket League ends in 210. In the day's attempts, about one Wine startup out of 30 to 60 (often the fork of wineserver that detaches), and one Rocket League launch out of four in report 18.

MEASURED in the faults already recorded (fatal fault report): esr 0x8200000f, a level 3 permission fault on execution; the pc's region is the dispatcher buffer, 64 KiB rwx/rwx; the offset in the region is always 0x5f4 or 0x604, the first instruction run after the return from the syscall. The page is there and executable, but not for this thread.

MEASURED natively, without FEX (`tests/darwin-process/f7_fork_jitstate.c`, fact F7 of `darwin-process-facts-a.md`): the thread of a fork's child receives the MAP_JIT write state that the parent's thread had at its last context switch, not its real state. A parent blocked or preempted in write mode, then back in execute mode without a context switch, gives 37 to 39 children out of 40 that die in SIGBUS at the first JIT code. A system call or a page fault in write mode leaves nothing stale; a thread created by `pthread_create` is not affected; a `pthread_jit_write_protect_np(1)` in the child is enough.

On the engine side, FEXCore's `JITWriteScope` toggles this mode at each translation (block copy), block linking and unlinking: 0.04 to 2.5 µs per window, measured with a per-thread ring of windows in a diagnostic engine never committed. A probe in the child (execution of a MAP_JIT page under a SIGBUS handler) saw each stale state, then corrected it. A preemption during one of these windows, then no context switch until the fork: the fault. LKD's system calls stay in the process and often do not switch, which leaves the time.

Fix: `ForkChild` resets the thread's state, execution outside a write, before any return to JIT code.

Witness `w33_forkwx` (README of the glibc witnesses). A caller of 60,000 direct calls to 256 leaves is translated once. At each round, the leaves are remapped, the caller runs (all sites are linked), the leaves are unmapped (the engine undoes the 60,000 links at once), then fork. 2 active threads per CPU load the machine. The burst must last at least one scheduling quantum: 0 deaths out of 1,000 rounds with 20,000 sites, 14 out of 500 with 60,000, 7 out of 200 with 150,000. Engine of 8ddfed2: 15 children dead out of 600 rounds, each with the dispatcher fault at offset 0x5f4. Fixed engine: 0 out of 600, twice. The simpler modes (fork after translation, or fork then exec like wineserver) did not reproduce: 0 out of 2,000 and 0 out of 600.

Suites with the fixed engine: LKD 43 programs pass, `test_sig_futex` exceeds its timeout at the same case as before; witnesses w01 to w33 as before (w07 timing out after 99/99, w03 identical byte for byte, w25 hostpid PASS, w31 also with the old base); EAC probes 38 failures out of 4,376, the same probes and the same first failures; Rocket League (`rl-direct.sh`, private runtime) 301 twelve times out of twelve, including six under load (EAC mapping 0.60 to 0.70 s). D3D12 and D3D11 harnesses of report 18, two passes: correct readbacks, the known gap of the renderpass resolve, the worker's death reported without blocking. The diagnostic engine had already given 301 for 12 launches, 264 forks, no stale state: the fault depends on the machine's load. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/19-sigbus-fork.md`.

## ELDEN RING: the 5-minute load came from the Metal shader cache, emptied by DXMT (2026-10-10, 22:20)

Context. Between the first frame and the title screen, ELDEN RING took 48 s, 88 s, 3 min 43 s or more than 300 s, with the same engine. In report 16, the "CR2.Device(HDD)" thread burned a core for one minute.

MEASURED (9 launches from Obelisk, profile, dumps and surveys every 5 s):
- a slow launch (275 to 298 s) has `MTLCompilerService` at 90-98% of a core in each survey, from the first frame to the title. CR2 burns 147 to 183 s of CPU there without a system call: it busy-waits;
- a warm launch (7 to 48 s) has no Metal compilation, and CR2 only takes 3 to 10 s there.

The cause: the two render servers, DXMT's arm64 one and D3DMetal's Intel one under Rosetta, ran outside any bundle. Metal stored their compiled functions in a single cache, `$DARWIN_USER_CACHE_DIR/com.apple.metal/16777235_355/`. Each startup of DXMT's server emptied it: after an ELDEN RING, `functions.data` is 46.4 MB and about 7,500 entries; after a Rocket League, 320 KB; after the D3D11 harness on DXMT, 128 KB. Two Rocket League runs then ELDEN RING: 296 s up to the title.

Fix in Obelisk's launcher (sailover `787942a`, local, not pushed): each server runs from a helper bundle inside the game's one, `<game>.app/Contents/Helpers/<backend>.app`, with its own identifier, hence its own Metal cache. Two macOS facts measured along the way:
- for a moment after the startup of an executable from a bundle, macOS refuses any new write into this bundle (EPERM, also outside the sandbox; 0.3 s later, they pass). The helper bundle is therefore ready before wsd;
- Rosetta translates each new file at its first launch: 0.34 to 0.55 s for a fresh copy of the Intel server, 9 ms afterwards. The copy is therefore kept from one launch to the next and translated before the game, outside EAC's window.

MEASURED after: ELDEN RING's cache (46.5 MB) does not move during two Rocket League runs, and ELDEN RING reaches the title 7 s after its first frame. EAC 301 at each launch (mapping 1.38 to 1.43 s).

Remaining, with a warm cache: the title arrives 7-8 s or 43-48 s after the first frame. In the slow ones, the main thread spins in NtYieldExecution in a module of more than 11 MB, probably `eossdk-win64-shipping.dll`. Outside our stack. D3DMetal's own cache (`d3dm/virgl_render_server`) has not been written since 17:20, perhaps because the server ends through SIGTERM. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/19-sigbus-fork.md`.

## ELDEN RING: the sound crackled all the time, and the launch goes from 156 to 66 s before the first frame (2026-10-10, 23:30)

Context. The user heard the sound "glitch" when the menu opened. Measurement with a diagnostic wsd never committed (atomic counters in the AudioUnit callback, one line per second): CoreAudio reads 512 frames per callback, 94 times per second, and the ring of the `wineobeliskaudio` driver often had fewer. 860 short reads in 154 s, 5.6 per second, 6.3 ms of silence per second, and spikes at each menu transition (1,056 frames right after a press). It was not only the menu opening: the sound crackled all the time.

The cause. In shared mode mmdevapi imposes a buffer of 3 periods (30 ms) and FAudio keeps it roughly full. But the driver only copied the frames written by the client into wsd's ring at its next tick, up to one period later: the ring had about one period less than what the client had written, and its level went down to 480 frames, below the 512 of a callback.

Fix (voile-wine `f04f2bb`, Voile `451d1e1`): `ReleaseBuffer` pushes the frames into the ring at once; the timer keeps topping it up. MEASURED: 0 short reads over 146 s (title, session box, information window, main menu), then 0 over 91 s at the next launch; minimum ring level 576 frames instead of 0 to 480. Latency drops by at most one period. The same driver is used for Rocket League.

Time zone. The guest root has no `/etc/localtime`: everything ran in UTC, and glibc retried this file at each `localtime()`, about 80 times per second in ELDEN RING (1.9 µs instead of 0.04, measured in the guest). Obelisk's launcher now passes `TZ` to the guest, the POSIX rule that ends the Mac's zone file (sailover `d63ca60`, local): 0 lookups afterwards, correct local time.

On the Obelisk side (sailover, local commits not pushed), measured to the click by temporary timestamps:
- the verification of the 55 GB before launch took 2 min in the Debug app, because `sha1` ran there without optimization (284 MB/s per thread versus 3,025 MB/s); these crates are optimized even in Debug (`03d1f1e`): 11 to 13 s, with the SSD capping at 6.8 GB/s;
- Cloud sync took 25 s, of which 15 were lost on a CM server that accepted the TCP connection without finishing the websocket handshake (port 27018, through the user's VPN); the servers are now tried in a race staggered by one second (`3074347`): 9 to 10 s.

MEASURED, from the click to the first frame: 156 s at the first launch of the evening, 66 s in steady state (73 s when Obelisk has just been rebuilt and re-extracts its Steam runtime). Remaining before the engine: verification 11 s, Cloud 9 s, startup of the native Steam host 10 s. The game's startup up to the first frame (34 to 37 s) and the black screen before the title (8 to 48 s depending on the launch) wait for EOS and the network: the main thread yields in a loop in `eossdk-win64-shipping.dll` and waits in `win32u`, with a single HTTPS connection established to `api.epicgames.dev`. Nothing attributable to the engine.

Ruled out after measurement: restarting the render server less often (8 startups before the first frame, but each ready in less than 0.5 s); a vDSO for `clock_gettime` (2 million calls per second on the main thread at the menu, but it is its frame wait loop: cheaper per call, it would not consume less). In the menus, stable 60 fps, GPU at 22%, fence wait 0.8 ms. The game itself has not been measured: loading the save would modify it and Obelisk would send it to the Cloud. Report: `sailover/build/experiments/eldenring-linux-20261010/reports/20-perf-elden-ring.md`.

## ELDEN RING in game: two engine freezes found and fixed (2026-10-11, morning)

Context. First ELDEN RING sessions on the Linux engine, loaded by the user. Four permanent freezes (frozen frame, mouse captured): three during the world load, one in game at the moment of a hit taken.

Tool. The thread dump (`FEXDARWIN_DUMP_DIR`) stayed empty for the game: it was itself waiting for the lock at fault. It now first writes the native stack of each host thread, without taking any lock (`af36ede`). A watcher outside the repository triggered the dump after 20 s without a frame.

Freeze 1, the VMA table lock (`efc8606`). MEASURED (dump of the frozen game): 30 threads wait for `mm->vlock` for reading, one `munmap` waits for writing, no thread holds it, the 68 others sleep in ntsync. Reproduced outside the game by the witness `w34_vlocksig` in less than a second, at each attempt, and only when the threads that use the lock receive signals. Cause: on macOS 27.0.1, a `pthread_rwlock` whose waiting threads receive signals stops waking anyone (fact G1, native program `g1_rwlock_signals`: freeze in 4 s, with or without `SA_RESTART`), and fexdarwin's kick is a signal. Fix: a one-word lock on `os_sync_wait_on_address`. `w34` passes 3 times out of 3, and the path of copies to guest memory (that of `clock_gettime`, 2 million calls per second in the game) goes from 27 to 91-99 million operations in 20 s.

Freeze 2, the blocking mode of pipes passed through SCM_RIGHTS (`d924c5b`). MEASURED (game log): `wine client error:148: write: Resource temporarily unavailable`, then `xinput_cs wait timed out ... blocked by 0148`. wineserver creates each thread's request pipe and sends its write end to the client; once received, it became non-blocking, because an LKD pipe is always `O_NONBLOCK` on the host. A request that found the pipe full failed, Wine ended the thread, and that thread held XInput's lock (a controller vibration at the hit taken, deduced). Fix: toward another process of the family, a pipe end goes out with a record carrying its guest flags; toward a host program (wsd, render server), nothing changes. Witness `w35_scmpipe`: 614,628 EAGAIN before, 3 passes out of 3 after.

Verifications: LKD tests (`test_sig_futex` times out on `waitv` as before these changes), witnesses w22 to w33 and w35, `w30_scmdev` fails as before, Rocket League EAC 301 twice with each engine. In game: session from 11:52 to 12:01 without a freeze, quit by the user. Engine installed by hand in the dev runtime: `fexdarwin-run` cae1f598, backups `*.87493e9b`, `*.9e4dd94b` and `*.e759f92c`.

Still open: `test_sig_futex` (`waitv`) and `w30_scmdev`, which already failed; a pipe end sent to a guest process of the same host process keeps the old path.

## wsd gives the mouse back when the game no longer shows frames (2026-10-11, afternoon)

Context. This morning's freezes left the mouse captured by the frozen game, and the user could no longer do anything on the Mac. The relative mode capture (`CGAssociateMouseAndMouseCursorPosition`) applies to the whole session, and only a focus loss released it.

Fix (`54464f7`). wsd notes when each window displayed its last new frame, on the base plane as well as on the overlay plane, where ELDEN RING and Rocket League present. A one-second timer looks at the windows that hold the pointer, in relative mode or with a clip rectangle. Per frame, there is only this timestamp in addition. After 5 s without a frame, wsd reassociates the pointer, shows the cursor again and stops bringing the pointer back into the clip rectangle. Movements no longer go to the game as relative deltas, otherwise it would receive them all at once on resume. The first frame displayed afterwards restores relative mode if the window is active, and hides the cursor again if the client had asked for it. The client is not notified of anything. Coming back into the window during the freeze does not recapture. Nothing is watched before the window's first frame.

MEASURED:
- Test client outside the repository against a private wsd, with a clip larger than the window so as not to trap the real pointer. 7 s of clip before any frame, no trigger. Base plane frames stopped, release at 5.8 s; overlay plane frames, resume; new stop, release at 5.7 s, then resume.
- ELDEN RING in game, mouse captured, game process stopped 20 s by `kill -STOP`: release at 5.5 s, pointer free and cursor visible (seen by the user). After `kill -CONT`, `frames shown again after 23.6 s, relative mouse resumed`, and the mouse drives the camera again.
- The loading of the save, just before, stayed 8.4 s without a frame: mouse released, resumed at the first frame. A silent load of more than 5 s therefore shows the cursor until a frame comes back.

wsd installed by hand in the dev runtime: `e16992cc`, backup `wsd.8e706708`.

## The two "already present" failures were not engine bugs (2026-10-11, afternoon)

Correction of the morning entry, which gave them as open.

- `test_sig_futex` blocked on the `waitv` case (`827b74b`). Its "in 20 ms" deadline came from the host's `CLOCK_MONOTONIC_RAW`, which counts sleep, and went out as guest `CLOCK_MONOTONIC`, which LKD maps to `CLOCK_UPTIME_RAW`, stopped during sleep as under Linux. MEASURED: 2,180 s of difference on this Mac, hence a 36-minute wait. The neighboring case already took the guest clock. After: 13 cases, 3 times out of 3; LKD suite 44 out of 44.
- `w30_scmdev` failed because I launched it without its synthetic directory (`FEXDARWIN_BIND="/lkdsyn/w=..."` and its two arguments, see the README). Launched that way, it passes with the engine from before as well as with the one from after the fixes. The witness now says which descriptor the child could not open (`5ccfaf2`).

No engine fix was needed.

## ELDEN RING: memory, d3dmetal-native's residency set turned off (2026-10-11, 15:00)

Measurement every 5 s during two sessions of about ten minutes, same route (title, loading the save, game), 36 GB Mac already loaded by other apps (13 to 15 GB of swap at the start). Tools outside the repository: `vm_stat`, `vm.swapusage`, `kern.memorystatus_vm_pressure_level`, IOAccelerator "Alloc system memory", `top` (footprint and compressed memory per process).

MEASURED, session A (residency set, the default): the three 4 GiB UPLOAD buffers arrive at the title screen; wired memory 13.3 GB on average with spikes at 24.4 GB during loads; allocated GPU memory 26.3 GB on average; 4 memory pressure alerts; 3.3 GB sent to swap; 3.9 GB compressed out of the game's 6 GB; 48 fps.

MEASURED, session B (`DMN_RESIDENCY_SET=0`, each command buffer declares its resources): wired 12.6 GB on average, 14.0 at most; GPU memory 14.6 GB (−11.7, the three buffers); no alert; nothing sent to swap; 12 MB compressed for the game; 49 fps; same share of frames held more than 100 ms (4.7 and 4.8%) and three of more than one second in both. No rendering error seen by the user. Caveat: B started with 10 GB free, A with 0.7; the drop in GPU memory and the end of the wired spikes come from the mode, the gap in swap and compression partly from the starting point.

DEDUCED: the 13 GB wired continuously are the game's working set (its video memory on PC), not waste. Change: Obelisk's launcher passes `DMN_RESIDENCY_SET=0` to the D3DMetal render server (sailover `0dad6ee`).

## ELDEN RING: where the frames go, and two attempts ruled out (2026-10-11, 16:00)

MEASURED, at 2560×1440, three sessions of 3 to 6 minutes of play, per-thread profile (`FEXDARWIN_PROFILE=1`) and GPU load every 2 s:

- 32 to 35 fps; the GPU is busy at 33 to 36% (57% at most) and the fence wait at present time stays under 1.5 ms: the GPU is not the limit.
- The game takes 6 to 7 cores. The 14 `GXWorker` threads take 5.6 of them spinning between two tasks: per thread and per second, about 37,000 `sched_yield` (4 µs each in game) and 74,000 `getrusage(RUSAGE_THREAD)` (1.4 µs), plus long ntsync waits (585 µs on average).
- The main thread (`eldenring.exe`) only uses 52 to 56% of a core: it waits. Per second: 20,000 to 26,600 ntsync calls (6.4 µs on average), 3,600 futex waits (45 to 60 µs), 2,500 to 3,200 reads of wineserver replies (48 µs the round trip), 386,000 `clock_gettime` (0.08 µs the complete call according to `w36_syncpp`: a vDSO would not be worth it).

Ruled out, pacing turned off (`guest NPT_WSD_PACING=off`): 32 fps versus 35 with pacing; the 2.8 ms of waiting per frame are not on the critical path.

Ruled out, a 5 µs busy wait before sleeping in `fx_sleep` (futex and ntsync). Witness `w36_syncpp`: the ntsync round trip goes from 8.4 to 3.1 µs, the private futex from 8.0 to 0.16 µs; LKD tests, witnesses and Rocket League's EAC without regression. In game: 32 fps, 6.7 cores instead of 6.0; the main thread's futex waits get shorter (60 to 45 µs) but its ntsync calls get longer (6.4 to 8.0 µs), the busy wait adding itself to the ones that sleep anyway. No gain: the change is not kept.

Still to measure: which requests the main thread sends to wineserver 2,500 times per second.

## Rocket League frozen after a Space switch (2026-10-22, Obelisk issue #14)

Context. A player (JAWE-B15S, M4, macOS 27.2) and the user: switching to another Space, clicking another window then coming back leaves Rocket League frozen for good, only on the Linux engine. In the user's log (16:56), nptunix stops at 411 presents while Neptune keeps sending vertex buffers: the game still draws but no longer presents.

Cause, in two steps:
- wsd. When macOS no longer shows the window (covered, on another Space), it no longer takes back its drawables, and `[CAMetalLayer nextDrawable]` blocks the main queue for up to 1 s (`allowsNextDrawableTimeout`). Meanwhile no message is read and no EV_RELEASE goes out. Neptune's reuse gate (100 ms per frame) expires several times in a row and, after 4, `Present` returns `DXGI_STATUS_OCCLUDED`.
- Neptune. `Present(DXGI_PRESENT_TEST)` returned OCCLUDED as long as the streak was 4, and only the gate of a real Present reset it to zero. Rocket League (`RocketLeague.exe`, 0x140d695b0), once OCCLUDED is received, only calls test Presents until a test answers something else: it never presented again.

MEASURED (test client outside the repository that imitates Neptune: 3 frames, 100 ms gate, one frame every 4 ms, PING for the main queue):
- Window covered by a second opaque wsd window, old wsd: PING at most 880 ms, 9 gate expirations in a row. `nextDrawable` lasted 997 ms, then 1002 ms in another attempt, at the moment the window disappears, and about 8 ms afterwards as long as it stays hidden. The occlusion notification arrived before this block in the first attempt, after it in the second.
- Window moved off screen: no block (the display link stops), and the mailbox keeps returning the replaced frames.
- On screen, `nextDrawable` waits about 8 ms at each tick at 240 Hz, so the main queue is busy most of the time. Not handled.

Fixes:
- wsd (`783b3c9`). A window without `NSWindowOcclusionStateVisible` keeps its pending frame without asking for a drawable; a more recent frame replaces it and returns it, as before. One log line at each occlusion change. The block of about 1 s at the moment the window disappears remains possible when the notification arrives after it; only a `nextDrawable` off the main queue would remove it.
- Neptune (`bce5c33`, neptune repository). A test Present looks again at the frame the next Present will take: if it has been returned, the streak is cleared and the test answers S_OK. D3D11 and D3D12 swapchains.

Verifications: `wsd_overlay` and `wsd_client` pass. With the new wsd, the off-screen window no longer gives any expiration. The user redid the scenario on Rocket League and Fall Guys: the game resumes. Installed by hand in the dev runtime: wsd `6e6d4a4b` (backup `wsd.9cb05490`) and `windows/{d3d11,dxgi,d3d12}.dll` (backups `*.dll.20261015`).

## Dead by Daylight on the Linux engine: thread limit, a Neptune command too short and a 10-bit swapchain (2026-10-22)

Context. Dead by Daylight (Epic, Unreal 5, Easy Anti-Cheat), Linux engine chosen in the settings, DXMT. Easy Anti-Cheat loads its Linux module (`anticheatlauncher.log`: "Easy Anti-Cheat successfully loaded in-game"). Three blockers one after the other, each found in the game log.

1. Crash at startup. MEASURED: `CrashContext.runtime-xml` gives `EXCEPTION_ACCESS_VIOLATION reading address 0x0` at 0 s; with `+seh` the fault is in the game (rip 0x14140ecc8, rax 0), right after `lkd: clone: 300 threads in one process, refusing more (-EAGAIN)`. INFERRED: the game reads the null handle of the refused thread. Fix `4095e5d`: 512 threads per process. 512 is the size of `SIG_WAKESET_MAX` (a group exit still wakes each thread); at about 280 MiB of host address space per FEX thread, 512 threads fit in the 576 GiB below the guest window. The test rule does not change (never more than 300 threads).
2. Black screen in fullscreen. MEASURED: `neptune: ring fatal error`, and on the render server side `failed to peek 8 bytes` then `dispatch error for cmd_type=0xff004c1b`, that is `ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews`. READ: the computed size counted 8 bytes per handle even without an array; with a KEEP count (0xFFFFFFFF) the sum truncated to 32 bits was exactly 8 bytes less than what the encoder writes, and the host reads the commands end to end without looking at `cmd_size`. Fix neptune `e34ced6`.
3. Banded colors, red turned blue. MEASURED: swapchain `fmt=24` (`DXGI_FORMAT_R10G10B10A2_UNORM`) and `shared texture: unmapped DXGI format 24, assuming BGRA8`. Fixes: neptune `acda52d` (virgl format 8), voile-wine `8f86ea35894` (nptunix, wsd format 3) and wsd `e1adcf7` (`MTLPixelFormatRGB10A2Unorm`, sampled into the BGRA8 drawable).

Ruled out: installing the VC++ from the game's installer (14.29) in the Linux prefix. Wine's `msvcp140` announces 14.51 and the game is linked with 14.44; a native `msvcp140` older than 14.40 is precisely what makes `_Mtx_lock` crash on a null pointer.

Verification: the user launched the game, menus with the right colors. Remaining: the game stutters, not measured. Installed by hand in the dev runtime: `fexdarwin-run` `f8008d33` (backup `.2a763995`), wsd `e9530034` (backup `wsd.e293795e`), `nptunix.so` `772e9151` (backup `.19f082eb`), `windows/{d3d11,dxgi,d3d12}.dll` (backups `*.dll.20261022`).

## Fullscreen: the game window stayed in front of the other apps after Cmd+Tab (2026-10-23)

Context. Rocket League in fullscreen on the Linux engine: after Cmd+Tab, the other app becomes active and takes the keyboard and mouse, but the game's frame stays in front of it. The browser opened by a link from the game (`OPEN_URL`, `9d09a1d`) stayed hidden the same way.

Cause. READ: `setFullscreenMode:` put the window at level `NSMainMenuWindowLevel + 1` (25) for the whole duration of fullscreen, and a level does not change when the app loses focus. The presentation options (Dock and menu bar hidden) have nothing to do with it: macOS only applies them while the app is active. Wine's Mac driver does the opposite: `-[WineWindow minimumLevelForActive:]` only raises a fullscreen window while the app is active, and `adjustWindowLevels:NO` brings it back down in `applicationWillResignActive:`. Same family: a window shown on top of the fullscreen window (dialog, crash box) stayed at the normal level, hence below, like a topmost window (level 3).

MEASURED (test client outside the repository: three wsd windows, fullscreen then topmost then dialog, a click and Cmd+Tabs posted, layers read from `CGWindowListCopyWindowInfo`):
- Old wsd: after Cmd+Tab, the game stays at 25 in front of the active app and the Dock. The dialog is at 0 and the topmost at 3, both below the game. Even without ever having been active (activation refused), the fullscreen window covered Safari.
- New wsd: with wsd active, topmost 26, dialog 25, game 25, the dialog in front of the game. After Cmd+Tab, the game and the dialog go to 0 behind the other app and the Dock comes back. On return, the same order is restored. Once the game window is destroyed, the dialog goes back down to 0.

wsd fix (`f41caa8`). `adjust_levels` takes up `adjustWindowLevels:`: from back to front, in the window server's order (active Space, topmost first), each window takes the level its state asks for and at least that of the windows behind it, and the topmost windows stay above the others. The pass runs when wsd loses or regains focus, when the active Space changes, and when a window is shown, hidden, destroyed, or enters or leaves fullscreen. A window that is shown first takes the level of the one in front, so as to be placed in front of it.

Outside wsd: the driver (`wineobelisk.drv`) does not send the window order (`insert_after` ignored) and only reads `WS_EX_TOPMOST` at creation.

Verification: the user tested Rocket League in fullscreen (Cmd+Tab, link opened from the game, back to the game). Installed by hand in the dev runtime: wsd `fdb3ba42` in `v26.3.0-18` (backup `wsd.v26.3.0-18`).

## What Wine's Mac driver did and wsd did not have (2026-10-23)

Context. Comparison of `winemac.drv` (the Windows engine's one) with wsd and `wineobelisk.drv`: our driver had 10 entry points, winemac about fifty. Taken up in one batch, except the big work items and the lowest priority one.

MEASURED. A test program (`wsdtest.exe`, outside the repository) launched by Obelisk's `launch.sh` on an APFS clone of the runtime; on the Mac side, a tool posts keys and clicks then reads the window server's layers, the pointer, `NSCursor.currentSystemCursor` and `pmset -g assertions`. Before → after:
- numeric keypad 5: nothing → `VK_NUMPAD5`, scan 0x4c. `@ #` key of an ISO keyboard: nothing → scan 0x29; key to the right of left Shift: scan 0x29 → 0x56.
- Caps Lock toggled four times: Wine state 1,1,1,1,0,0,0,0 → 1,0,1,0 (wineserver toggles at each press, `set_input_key_state`).
- `SetCursorPos(300,250)`: pointer motionless → at 300,250.
- the game's 32×32 color cursor, `IDC_IBEAM`: Mac arrow → the game's image with its hotspot, the Mac I-beam.
- `WS_EX_NOACTIVATE` popup shown with `SW_SHOWNOACTIVATE`: became key window, and the `EV_FOCUS` that followed activated it in Wine too → leaves the foreground to the game.
- `HWND_TOPMOST` after creation: ignored → level 3, then 0 with `HWND_NOTOPMOST`.
- `SetForegroundWindow` from the game to a window behind: the Mac did not follow → it moves to the front.
- `SPI_SETSCREENSAVEACTIVE` to FALSE then TRUE: nothing → `PreventUserIdleDisplaySleep` assertion taken then released.

Fixes:
- wsd (`0083ffe`). Caps Lock as press and release at each change; `WSD_KEY_ISO`; `CURSOR_WARP`, `CURSOR_DEFINE`, `CURSOR_SET`; `WINDOW_SHOW` 2, `WSD_STYLE_NOACTIVATE`, `WINDOW_FOCUS`, `TOPMOST` applied; `EV_DISPLAY`; `SCREEN_SAVER`; logout, restart or shutdown of the Mac as `NSTerminateLater` (before: cancelled as long as a game was running); App Nap turned off.
- voile-wine `a3ae9c1e336` (`wineobelisk.drv`). Cursors (winemac's table of system cursors, otherwise the image), `SetCursorPos`, `ActivateWindow`, show without activating, `WS_EX_TOPMOST` followed, `SystemParametersInfo`, `EV_DISPLAY` (the desktop process calls `DisplayModeChanged`), winemac's complete key table with the ISO swap. Bug found during the test: `NtUserGetIconInfo` gives the lengths in characters, not in bytes; the I-beam was not recognized.
- obelisk-input (`ded548b`). `IOPMAssertionDeclareUserActivity` on buttons and on sticks beyond a third, at most every 5 s.

Regression found by the user, fixed before the commit. A game in D3D exclusive fullscreen (Sugar, DXMT) opened a link: Safari became the active app, but stayed under the game's frame, and Discord too. READ: Neptune's swapchain puts the window in `HWND_TOPMOST` (`npt_swapchain.c:541`, like DXMT and Windows), and wsd made any topmost window float, even inactive. Rule taken from winemac (`TOPMOST_FLOAT_INACTIVE_NONFULLSCREEN`, its default setting): a fullscreen topmost window no longer floats when wsd is not active. MEASURED: after Cmd+Tab to Teams, fullscreen topmost window at level 3 in front of Teams, then at level 0 behind with the rule.

Not tested: App Nap, logout, the controller, a display change. Big work items left aside: keyboard layout (AZERTY, `ToUnicodeEx`), clipboard, window order (`insert_after`) and multi-display. Also remaining: Wine does not learn that the game goes to the background (winemac puts the foreground on the desktop, the game receives `WM_ACTIVATEAPP(FALSE)`); not done, to be decided.

Installed by hand in the dev runtime `v26.3.0-18`: `engine/wsd` `bddd403a`, `engine/obelisk-input` `9ed3513b`, `wineobelisk.so` `051329fa` (originals as `*.v26.3.0-18`).

## The game goes to the background when another app comes to the front (2026-10-23)

Context. The driver ignored focus loss: for Wine, the game stayed in the foreground whatever app the user was in. It never received `WM_ACTIVATEAPP(FALSE)` and could not pause, mute its sound or minimize itself as under Windows. winemac does it (`APP_DEACTIVATED`, `macdrv_app_deactivated`: the foreground goes to the desktop).

MEASURED (test program outside the repository through Obelisk's `launch.sh`; back to the game by reopening the app like the Dock does, the automated Cmd+Tab following the order of recent apps):
- before: after Cmd+Tab, foreground still on the game, no `WM_ACTIVATEAPP`; a game that minimizes itself on deactivation never minimized.
- after: leaving → `WM_ACTIVATEAPP` off, foreground on the desktop; coming back → `WM_ACTIVATEAPP` on, foreground on the game, fullscreen window at level 25; the game that minimizes itself disappears, then comes back restored with the foreground.

Found along the way. READ: the server's `set_foreground_window` (`queue.c:3831`) refuses the foreground to a process that does not have it and whose queue has no more recent user input. The event thread's `NtUserSetForegroundWindow` only worked because the game had never lost the foreground; once the desktop was in the foreground, the return was refused. Like `macdrv_window_got_focus`, focus now goes through a message to the window, whose thread takes the foreground with `NtUserSetForegroundWindowInternal`. A window still minimized takes it after the `SC_RESTORE` posted before.

Fixes:
- wsd (`8d8ab8d`). `EV_APP_DEACTIVATED` to each client when wsd is no longer active; a window without a minimize button minimized by the client is marked, and comes back with `WINDOW_MINIMIZE` 0; on return (activation or click in the Dock) without a visible window, the largest minimized one comes back (winemac's `-unminimizeWindowIfNoneVisible`).
- voile-wine `29abf1029b0`. `EV_APP_DEACTIVATED`: release of the cursor and foreground to the desktop in the process that has it; focus through `WM_OBELISK_TAKE_FOCUS`; any minimized window goes through `WINDOW_MINIMIZE`.

Verification: the user tried their games. Installed by hand in the dev runtime `v26.3.0-18`: `engine/wsd` `0801e582`, `wineobelisk.so` `da9930a6`.

## Hunt: Showdown 1896: Easy Anti-Cheat frozen on its splash, a return to address 0, and Anybrain (2026-10-23)

Context. Hunt: Showdown 1896 (Steam 594650, CryEngine, Direct3D 12 only through `CryRenderD3D12.dll`), sent to the Linux engine by Obelisk's Easy Anti-Cheat detection, D3DMetal. Steam starts `hunt.exe`, the Easy Anti-Cheat bootstrapper. The game also loads `anybrainSDK.dll`, a second anti-cheat protected by Themida (`.themida` and `.boot` sections, code encrypted on disk).

1. Splash stuck on "INITIALIZING". MEASURED: `anticheatlauncher.log` gets the `linux64` module (200), then stops at "Starting Wine module mapping"; the bootstrapper process at 145 % CPU, one guest thread inside `_sigtramp` → `HandleUnalignedAccess` → `DoCAS+648` in every sample; lldb: `casal` with `EXC_BAD_ACCESS (code=2)` at a page of an LKD memfd mapping that is host `r--`. INFERRED: the unaligned atomic's store, emulated inside the SIGBUS handler, hits a page watched for code; macOS reports it as SIGBUS, which the handler blocks, and replays the store forever (Linux reports SIGSEGV). Fix `627899b`: SIGBUS is let through while the atomic is emulated, and the fault reaches the code-write step. After it: "Easy Anti-Cheat successfully loaded in-game", the bootstrapper ends with 0.
2. `HuntGame.exe` ends at once, `FATAL ... host pc=0`, `lr` in the dispatcher, the same with DXMT and D3DMetal. MEASURED with temporary logs: `CompileBlock(0x1804ba776)` returns a valid host address; `x0 = x1 = x2 = 0` at the crash. READ: the indirect branches compare the popped call-ret entry, then the L1 entry, with the target, and jump to the entry's host address; empty entries are `{0, 0}`, invalidated L1 entries `{stale host, 0}`. A return to guest address 0 matched one. Fix Zagrios/FEX `9f96fa124` (`e0da155` here): a branch to address 0 takes the full lookup, and `FindBlock` skips L1 for it. After it: "NoExec instruction in entry block: 0", and the guest gets its fault.
3. Remaining blocker. MEASURED with `+seh`: the access violation at address 0 is the first exception of `HuntGame.exe`; only ntdll's loader handler runs, then `loader_init "anybrainSDK.dll" failed to initialize`. INFERRED: Themida crashes on purpose after a check of its environment. Ruled out: the hypervisor bit and the `FEXIFEXIEMU` leaf (hidden for one run, same result). Not measured: an `rdtsc` timing check, where the first run of each block includes its translation.

Also seen: the "NoExec instruction in entry block: 9" of every run comes from another process during Easy Anti-Cheat's start, not from the game.

Verification: the user's Rocket League, Fall Guys and Dead by Daylight still run with both fixes. Installed by hand in the dev runtime `v26.3.0-19`: `engine/fexdarwin-run` `a079f402` (backup `fexdarwin-run.v26.3.0-19`).

## HELLDIVERS 2 on the Linux engine: THEIA, guarded host pages and a signal stack that stayed registered (2026-10-26)

Context. HELLDIVERS 2 (Steam 553850, Stingray, Direct3D 12 only, D3DMetal), sent to the Linux engine by Obelisk's Easy Anti-Cheat detection. Its executable is protected by THEIA, the packer of THE FINALS too. Until now it stopped with "E111000D Conflicting software" on both engines, and the earlier conclusion (timing or emulation detection) was wrong. Seven blockers, in the order they appeared.

1. E111000D. MEASURED: Voile's own Linux Wine run natively on an x86 Linux machine gives the same error, Proton passes: not the engine. READ (process snapshot at the packer's handoff, its virtual machine replayed in Unicorn, block traces diffed against Proton's, inputs reduced by delta debugging): the check is a pure memory fold over ntdll's export table, and lld left the Nt/Zw stub block at RVA 0x55720, behind the C code, where GNU ld puts it near 0xea00. Fix voile-wine `77b9e4f`: `signal_x86_64.c` first in `SOURCES`, the block 32-byte aligned. The relinked ntdll passes on the native machine and on the engine.
2. MEASURED with a raw system call probe: `NtWaitForSingleObject(NtCurrentProcess())` returned `STATUS_NOT_SUPPORTED` on the engine, `STATUS_TIMEOUT` natively and in Proton, and the packer's probes went another way after it. Fix voile-wine `e60d93c`.
3. MEASURED: the packer's single-step sequence trapped at the wrong place after an `IRETQ` (native: on its target, b30d2b), and `XSETBV`, which it runs every 20 s, raised #UD ("Invalid instruction in entry block", Wine "Got unexpected trap 0") where it expects `EXCEPTION_PRIV_INSTRUCTION`; the game then crashed later on purpose. Fixes Zagrios/FEX `fa7296a4f` and `f397c1048`.
4. Crash at a jump to the image base + 0xF3DEEEE6. READ: a switch read its jump table (`mov ecx,[r12+rax*4+off]`) from a 4 KiB page still PAGE_NOACCESS (encrypted, decrypted by the packer on the access violation) that shared its 16 KiB host page with decrypted code, readable through the union of rights (I2). Fix `dec2234` (guarded host pages, with FEX's `PostCompile` hook `625a82a83`).
5. MEASURED: 100 % CPU right after "PartialDecode instruction in entry block: 14002BFFE": an instruction runs from decrypted code into an encrypted page, and the fault reported its RIP, whose page the packer had already decrypted. Fix `ba9a79b`: the first non-executable page, as x86's CR2.
6. Direct3D 12. MEASURED: 16,233 upload heaps of 128 MiB, then the game's own crash; its allocator asks `GetCopyableFootprints` for `TotalBytes` only and got 0x0008000000000800, read from the wrong place of the reply. Fix neptune `7e0dd0e`. Then frames froze for seconds: a synthetic test with HELLDIVERS 2's three-queue pattern stalls about 50 s in D3DMetal, not at all when either cross-queue `Wait` is skipped or the submissions are 20 ms apart. Fix neptune `d5ca3d8`: the host waits on the CPU before the backend `Wait`.
7. About half of the launches died between 25 s and 5 min: host exit status SIGILL (4), no handler run, only `EXC_CRASH`. MEASURED: a test program that sends a signal to a thread whose registered `sigaltstack` is unmapped dies exactly that way; logging `sigaltstack(SS_DISABLE)` in `UnregisterThread` gave -1 / `ENOMEM` at every thread exit (macOS wants `ss_size >= MINSIGSTKSZ` even to disable). INFERRED: a kick racing the exit of one of the threads THEIA starts every 0.4 s, which wineserver ends with SIGQUIT. Fix `cc405d3`. After it, 4 launches of 4 lived past 150 s.

A review of the diffs before the commit found and fixed, in the same commits: `PostCompile` closed the page that a guard step had just opened, so each guarded read faulted about six times before the hot-page burst let it through; the bounds on retries counted successful reopens, so the 65th store cycle or the 200th race on one page became a guest access violation; another process reading a guarded page (`/proc/pid/mem`) left it open; a guest signal during a guard step showed the step's trap flag; `POP SS` lost its interrupt shadow under the new TF rule; a `Wait` whose `Signal` is queued behind it on the same ring blocked 10 s (now 100 ms).

Also found: the fixes from the Overwatch work, re-applied wholesale at first, were tried one by one; only `ba9a79b` is needed. `mov` to FS or GS in 64-bit mode is not: THEIA runs it about 74 times and plays on with FEX's decode failure (Rocket League's Easy Anti-Cheat does it too, 47 times).

Remaining. Loading sometimes never ends on the "Daily Incoming Recruit Report" spinner (about 2 launches in 5, with the runtime of before these changes too): every thread waits, the network ones included; not explained. Several `nested exception on signal stack` per launch, before these changes too, in good and stuck launches alike. 17 to 25 fps in the Training Facility. THE FINALS passes THEIA as well (Unreal Engine 5 starts, splash screen) and then crashes executing 0x206e42617, outside every module; not investigated.

Verification (2026-10-26). HELLDIVERS 2 reaches the Helldiver Training Facility, the character walks with W, one launch alive 10 min. Rocket League, Dead by Daylight, Fall Guys and ELDEN RING against the runtime of before these changes, menus or title, 30 s each: same fps (capped by the display or the game), CPU within 2 %, same memory. LKD tests 43 of 44 (`test_fs_glibc` skips without its sysroot). Installed by hand in the dev runtime: `engine/fexdarwin-run`, `wine/lib/wine/x86_64-windows/ntdll.dll`, `x86_64-unix/ntdll.so`, `windows/d3d12.dll`, `graphics/x86_64/libexec/virgl_render_server` (originals in `Voile-backups/ntdll-original-20261026`). On the macOS engine (2026-10-26 evening), Among Us, Unrailed, Hades and PEAK ran the same with both voile-wine changes, but Overwatch stopped with the stubs first (a stack overflow before it created its Metal device; the user confirmed it plays with the old order): voile-wine `93eb44f` puts the stubs first on Linux hosts only, and the macOS engine's ntdll keeps its former layout (same export RVAs, same code).

## THE FINALS: the packer's idle freeze, half the frame rate after it, and frame time (2026-10-29)

Context. THE FINALS (Steam 2073850, Unreal Engine 5, THEIA, `--dx11` through DXMT) played on the Linux engine, but 30 s to 1 min after the last input, in the menu too, it froze 10 to 30 s and came back at half its frame rate, with DXMT and with D3DMetal.

1. The freeze. MEASURED: about 30 s after the last input the packer re-encrypts the game's code (`NtProtectVirtualMemory` by direct system call, then writes): thousands of write faults on pages holding translated code, each one about 1 ms in `mm_alias_pages`, which walked every VMA, some 50,000 once the packer has cut the image with `mprotect`. Fix `2b776cc` (an index of the placements of each shared object): 40 to 96 us per fault, a freeze of 2 to 2.7 s. It still comes at each switch to idle, at a moment without input.
2. Half the frame rate after it. MEASURED: 40,000 single steps a second on the guarded host page `0x14d173000`, where the re-encrypted end of `.text` shares 16 KiB with `.rdata` and the import table; FEX reads it with `LD1R` and `LD1` of one lane, which the guarded load decoder did not know. Fix `c884e70`: menu 20.6 ms a frame after the wave, 11.2 ms, the same as before it.
3. 30 fps in the menu without input: the game's own idle energy saving (`bIdleEnergySavingEnabled`), not the engine.

Frame time, after the two fixes:

4. MEASURED: DXMT committed a command buffer at each `D3D11_QUERY_TIMESTAMP` End, about 150 a frame (Unreal Engine's GPU stats). dxmt `b8e66e3`: menu 91.7 to 111.9 fps (against the 120 Hz display), matches about 16 % faster.
5. The practice range ended in "Out of video memory": DXMT's native builds refused a shared texture other than a linear 2D one, here a 2-slice array the game never opens elsewhere. dxmt `a69fd3e` creates it unshared.
6. MEASURED: the packer runs `syscall` with EAX = -1 about 50,000 times a second on the game thread, each a SIGSYS for Wine's `STATUS_INVALID_SYSTEM_SERVICE`. Fix `612419c` (engine op `sud_shortcut`, Wine's `bad_syscall` registers, through the packer's instrumentation callback): SIGSYS frames 877,000 to 1,600 per 10 s, about 4 % faster.
7. MEASURED: every `mprotect` invalidated every guarded page cache, and the packer makes about a thousand a second: 35,000 cache refreshes in 5 s. Fix `e3cfe41` (a generation per host page): 2,000, about 5 % faster.
8. MEASURED: Neptune's wrapper cache had 1024 buckets for tens of thousands of objects (`npt_com_get_or_wrap` hot). neptune `a4ac69d` (65536): after 3 min in the menu, 10.4 to 8.8 ms a frame. `d3d11.dll`, `dxgi.dll` and `d3d12.dll` must come from one build: a `d3d12.dll` left at 1024 kept HELLDIVERS 2 on a black screen.

Practice range, steady: about 99 fps before these changes, about 130 after.

Rejected: skipping FEX's invalidation of code on the packer's writes, no gain.

Remaining. The frame time still grows over a session (8.3 to about 10.9 ms in 5 min of menu; 10.6 to 14 before these changes) while the game's memory grows by about 50 MB a minute: likely guest objects never released, still hot in `npt_com_get_or_wrap`. Hitches during the first 30 s of a map, when the packer decrypts the new code (17,000 write faults, about 3.6 code invalidations each). The packer's instrumentation callback takes about 25 % of the game thread. The user still sees some stutters in matches.

Verification (2026-10-29). THE FINALS with DXMT and D3DMetal through idle waves, and in the practice range. Rocket League 120 fps against the display, and 180 to 230 fps on the user's 240 Hz display (not split by change); Fall Guys 120 fps; Dead by Daylight 60 fps (its cap); HELLDIVERS 2 as before. LKD tests 43 of 44 (`test_fs_glibc` skips without its sysroot). Installed by hand in the dev runtime `v26.3.0-21-dev2`: `engine/fexdarwin-run`, `graphics/dxmt-native.framework`, `windows/{d3d11,dxgi,d3d12}.dll` (originals in `Voile-backups/finals-freeze-20261029`).

## THE FINALS: where a match spends its CPU, and what the translator changes bought (2026-10-29)

Context. THE FINALS (Direct3D 12 through D3DMetal) in matches on the Linux engine, an M5, measured with a guest-PC sampler that suspends threads without a signal (`7cb2ed4`, reverted with the other measurement code: nothing of it stays in the engine): freezes and 30 to 80 fps.

1. MEASURED: the game thread sets the frame rate, about 0.9 of a core in a match: about half in translated game code, 13 % stalled on the first instruction of blocks (code scattered over the code buffer), 5 % in host signal handling, 10 % blocked. Wine and LKD are about 8 % of it, Neptune's guest DLLs about 4 % of the translated time. The render server (Neptune and D3DMetal, x86_64 under Rosetta) takes 1 to 2.6 cores, the GPU 46 to 96 %.
2. MEASURED on the M5: an x86-ordered store (`stlur`) costs about 2.3 plain ones; an ordered load (`ldapr`) as much as a plain one on a cache hit, 1.5 times on misses.
3. The freezes in a match came from translation: the 128 MiB code buffer filled every one to two minutes and each clear translated the working set again, and the packer's re-encryption and protection changes invalidated unchanged code. Fixes: FEX `91cce7c9f` (512 MiB in 128 MiB zones), `f2efdbdc0` (ReviveTranslations: a dropped translation is mapped again when its bytes come back unchanged; the gaps between rendered frames halved).
4. MEASURED: block mapping took about a fifth of compile time (an ordered page map, a write lock per entry point, the clock read at each L2/L3 hit). FEX `ecabe89a5`.
5. MEASURED: each mmap or munmap moved the guarded page generation of every host page, so every guarded load stub went back to the fault. `da78465`: 5,438 to 1,771 guarded faults a second.
6. MEASURED: the add of the guest base was 7 to 12 % of the game's host instructions. FEX `b41a030fb` (one per guest register, at most four live): 3.3 % fewer instructions on the hottest blocks.

The user felt the whole set a little better; no single step was perceptible on its own.

Rejected:
- Plain stores for the game's own code (no x86 store order outside locked instructions and fences): match loading froze, Unreal's task system needs the order.
- Translating the hottest blocks again to pack them together (a native test gives 8 to 22 % for scattered against contiguous hot code): the new copies were scattered again by the compilations of the other threads, over 175 MiB, and the frame rate did not move. A real packer would have to compile them in one pass into a zone of their own.
- DXMT with `--dx11`: worse, in stutters and in frame rate.
- Direct3D 11 through D3DMetal (`-dx11`; `--dx11` is not an Unreal option and kept Direct3D 12): Unreal's hang detector after 42 s, the RHI thread looping between the game and Neptune's `d3d11.dll`, probably a query that never completes. No game used that path before.

Remaining. Without a hardware TSO mode or a low guest base, both of which macOS keeps to Rosetta, each further translator change is worth a few percent. The render server's share between Neptune and D3DMetal is not measured (`sample` gives wrong stacks under Rosetta).

Verification (2026-10-29). THE FINALS in matches on the final engine; witnesses `w02` to `w32` and the arithmetic bench give the same results as before; LKD tests 43 of 44 (`test_fs_glibc` skips without its sysroot). Rocket League, Dead by Daylight and HELLDIVERS 2 were not played on this engine.

# Linux engine performance and layers to simplify, October 3, 2026

This document measures where the time goes in a frame, in the input latency and at the startup of Rocket League on the
Linux engine. It then puts a figure on each translation layer that could be removed or simplified. It is an analysis:
nothing is decided, and the fixes proposed at the end of the document are not written.

Machine: Apple M5 Max, macOS 27.0.1, display up to 240 Hz. Voile runtime `v26.3.0-8` (Obelisk `a853ad2`), with
`fexdarwin-run` and wsd replaced by the instrumented versions described below. The game is capped at 60 fps.

## Summary

- **At 60 fps, no part is saturated.** In Free Play, the thread that presents the frames waits 15.5 ms out of 16.7 ms.
  The GPU is 27% busy. The render server uses 39% of a core. The game itself uses 88%, a good part of it
  busy-waiting.
- **The game makes 660,000 system calls per second in Free Play** (480,000 in the menu). More than half come
  from a busy-wait (`SwitchToThread`), which Wine translates into `getrusage` + `sched_yield` + `getrusage`. Next come
  about 26,000 requests per second to wineserver, at about 11 µs per round trip.
- **A call to wineserver costs 150 to 280 times more than with Voile's macOS Wine.** It is not FEX: without
  ntsync, each wait and each `SetEvent` make a round trip with wineserver. For pure computation, memcpy and
  file reads, the engine goes as fast as Rosetta.
- **The startup (31.6 s measured, 27.7 s without profile) does not depend on the JIT.** FEX compilation takes less than
  0.1 s over the whole launch: an on-disk code cache would gain almost nothing. `InitOnlineSubsystem`
  (14.8 s) saturates the main thread with an avalanche of file calls. Wine rereads a whole folder for each
  file not found, because LKD does not say that the volume is case-insensitive. A file not found costs
  10.5 ms in a folder of 20,000 files, against 17 µs under the macOS Wine.
- **Input latency.** In the menu, 58 ms until the frame that reacts is sent (median). In Free Play, 33 ms. Add
  15.6 ms until display, while the screen refreshes every 4.2 to 8.3 ms. The driver, for its part,
  passes a key on in 0.05 ms.
- **Proposed order** (details at the end):
  1. tell Wine that the folders are case-insensitive (big gain at startup, small job);
  2. check the wineserver epoll anomaly (up to one core of system time);
  3. lighten `NtYieldExecution`;
  4. count the wineserver requests by type, then decide on ntsync in LKD;
  5. shorten wsd's presentation path.

  In-process D3D11, a native wineserver, the native Unix parts and the FEX code cache are not justified
  by these measurements.

## Measurement tools (added for this report)

- **`FEXDARWIN_PROFILE=1`** (fexdarwin, `HitchLog.cpp`). Every 10 s, as well as at the end of a process and at an
  `exec`, each guest process writes:
  - its user and system CPU;
  - its system calls: count, and time spent in LKD (this time includes the blocking waits);
  - the FEX JIT time, the SMC faults and the invalidations;
  - for each thread, the CPU, named after the guest `comm` (the Windows names of the threads, for example
    `RenderingThread`);
  - the 12 system calls that took the most time.

  Cost: two clock reads per system call. With the profile, the startup went from 27.7 to 31.6 s.
- **`tools/dev/engine-top.c`**. It gives the CPU of each thread of wsd, of `virgl_render_server` and of the
  `fexdarwin-run`, as well as the GPU occupancy (`Device Utilization %` of the IOAccelerator). It reads these figures through
  `proc_pidinfo` and IOKit, without suspending any thread, unlike `sample`.
- **wsd latency probe.** The first test input (`wsdctl tap`) now enables it, whereas before it took
  `WSD_LATENCY_PROBE` in wsd's environment, which the Obelisk launcher does not pass on.
- **`tests/bench/winbench.c`**. Per-call cost of the crossings of a Windows game: clock, `SwitchToThread`, event wait and
  signal, object creation, critical section, file read, round trip between two threads, computation,
  memcpy. `winbench N TEST` runs a single test, so that the profile sees only its calls.
- **`tests/bench/missbench.c`**. Cost of a `GetFileAttributesW` on an absent file, and on a file that is present but
  written with a different case.

Measured session: launch from Obelisk, then menu (20 up/down arrows sent by `wsdctl`), then Free Play
(about 60 s of play, then 8 ball camera toggles with the Space key), back to the menu and exit through the menu.
No online match. Logs: `session1` folder of the session workspace (not versioned).

## One frame (60 fps, 16.7 ms budget)

| Part | Free Play | Menu | Remark |
|---|---|---|---|
| Game, `RenderingThread` | 45% of a core (7.5 ms per frame) | 21% | 314,000 system calls/s in Free Play |
| Game, main thread | 30% (5.1 ms) | 21% | 318,000 system calls/s |
| Game, other threads | 13% (2.2 ms) | 8% | |
| wineserver | 12% (1.9 ms) | 28% (anomaly, see below) | 94,000 system calls/s in Free Play |
| Render server, Neptune decoder (`npt-ring-1`) | 25% (4.2 ms) | 13% | DXMT calls included |
| Render server, `dxmt-encode-thread` | 6% (1.0 ms) | 4% | |
| Render server, other threads | 8% (1.3 ms) | 5% | |
| wsd | about 3% | 3% | |
| GPU | 27% | 26% | average occupancy |
| Fence wait before presentation | 0.5 ms (median) | | nptunix `fence wait` |
| Pacing wait (`NPT_WSD_PACING=after`) | 13 to 16.5 ms | 15 to 16 ms | unused margin |

The time of the game threads includes the busy-wait. The game makes about 118,000 `SwitchToThread` per second in
Free Play, that is about 2,000 per frame. Each costs three system calls, about 1.6 µs in total: the busy-wait
therefore takes about 0.2 core, that is 3 ms of CPU per frame on the two main threads.

The game's system calls in Free Play, per second:

| Call | Count/s | Origin |
|---|---|---|
| `getrusage` | 237,000 | Wine's `NtYieldExecution` (two per `SwitchToThread`) |
| `clock_gettime` | 142,000 | `QueryPerformanceCounter` (150 ns per call in total) |
| `sched_yield` | 118,000 | `NtYieldExecution` |
| `rt_sigprocmask` | 77,000 | two per request to wineserver |
| `read` | 42,000 | wineserver replies, wsd socket, etc. |
| `write` + `writev` | 39,000 | requests to wineserver, presentations, sound |
| `futex` | 1,500 | |

wineserver reads about 26,000 requests per second in Free Play (7,700 in the menu), almost all of which come from the game.
With about 11 µs of latency per call, this makes up to 0.3 s of blocked thread per second, spread across the threads
of the game. The type of these requests is not measured yet (see "Next measurements").

The pacing holds: over 14,820 presentations, the interval is 16.6 to 16.7 ms. There were 23 frames held more than
50 ms (mostly during loads), and 17 "frame back" in about 4 minutes.

## Input latency

Method. `wsdctl tap` sends a key to wsd, which timestamps it. The probe then compares each received frame with the one
from before the input, and timestamps the first one that changes clearly. The `wineobelisk.drv` driver timestamps on its side the read
of the event and its sending to Wine (`WINE_OBELISK_LATENCY=1`).

| Step | Menu | Free Play |
|---|---|---|
| wsd → read by the driver → sending to Wine | 0.05 ms | 0.05 ms (90th percentile 3.5 ms) |
| Input → submission of the frame that reacts (game, render, GPU, fence) | 58 ms median (from 21 to 101 ms, 20 attempts) | 33 ms median (from 27 to 48 ms, 8 attempts) |
| Submission → display (`presentedTime`) | 15.6 ms (median) | 15.6 ms |
| **Total** | **about 73 ms** | **about 49 ms** |

The menu figures are less certain: the menu highlight animates with a fade, and the probe detects it a bit late. In
Free Play, the camera toggle changes the whole frame, which gives a clear measurement.

For the player's real keys, 3.1 ms median separate the time of the macOS event (the hardware) from its
read by the driver. It is the path from macOS to the application, before wsd.

From input to submission, it takes about two frames in Free Play: the game thread reads the input, then the render
thread draws a frame later. This delay is the game engine's, not our chain's.

Submission → display, on the other hand, is ours. A frame waits for:
- the next tick of wsd's display link;
- the drawing into the drawable;
- then the macOS compositor, since the window is in windowed mode.

In total, this makes 15.6 ms, while the displayed intervals fall on multiples of 4.17 ms. We can no
doubt gain 4 to 10 ms here, to be measured more finely (see the "Presentation" layer).

## Startup

| Step (`Launch.log`) | Without profile (17:22) | With profile (20:49) | What happens (profile) |
|---|---|---|---|
| `PreInit` | 2.3 s | 2.6 s | |
| `LoadScriptPackages` | 9.9 s | 10.5 s | the main thread spins in a busy-wait; two other threads load, without saturating |
| `InitOnlineSubsystem` | 12.4 s | 14.8 s | the main thread saturates a core on file calls |
| Other steps | 3.1 s | 3.7 s | |
| **Total** | **27.7 s** | **31.6 s** | |

Profile detail, per 10 s slice, for the game process:
- **From 0 to 10 s.** The main thread takes 8.7 s of CPU for 6.5 million system calls: 4.2 million
  `getrusage` and 2.1 million `sched_yield`. It is the busy-wait of a load that runs on other threads:
  thread 117 takes 3.6 s of CPU, and `AsyncIOSystem` 1.7 s with 957,000 system calls. What bounds this phase is
  not established, since no thread is saturated in it (see "Next measurements").
- **From 10 to 30 s.** The main thread takes 8.3 then 10.2 s of CPU, more than half of it in system time. Over the
  20 s, it makes:
  - 592,000 `newfstatat`;
  - 304,000 `openat`;
  - 304,000 `getdents64`;
  - 255,000 `getxattr`;
  - 240,000 `close`;
  - 120,000 `fstat`;
  - 75,000 `fstatfs`.

  It is Wine's case-insensitive lookup under Linux (`lookup_unix_name`, `dlls/ntdll/unix/file.c`). When
  a name does not exist as is, Wine calls `get_dir_case_sensitivity`: it opens the folder and requests
  `EXT2_IOC_GETFLAGS`. If the `EXT4_CASEFOLD_FL` flag is not there, it rereads the whole folder. LKD does not know this
  ioctl, while the host's APFS volume is really case-insensitive.
- **The FEX JIT** took 27, 14 then 5 ms over these three slices, that is less than 0.1 s over the whole startup. A
  sampling of wineboot (outside the game) confirms: the FEX compilation functions are almost absent from it, and the
  threads wait in the kernel or in file calls.

Cost of a file lookup (`missbench`, 200 to 2,000 calls of `GetFileAttributesW`):

| Case | Linux engine | Voile's macOS Wine |
|---|---|---|
| Absent file, folder of 20 files | 139 µs | 17 µs |
| Absent file, folder of 20,000 files | 10,518 µs | 17 µs |
| Present file, different case | 50 µs | 9 µs |

## Unit costs (`winbench`)

Same program on the Linux engine and under Voile's macOS Wine (x86-64 under Rosetta), in a test prefix.

| Call | Linux engine | macOS Wine, msync (Obelisk setting) | macOS Wine without msync |
|---|---|---|---|
| `QueryPerformanceCounter` | 150 ns | 48 ns | 48 ns |
| `SwitchToThread` | 1,256 ns | 226 ns | 230 ns |
| `WaitForSingleObject(ev, 0)` | 11,839 ns | 66 ns | 8,423 ns |
| `SetEvent` | 10,681 ns | 39 ns | 7,928 ns |
| `CreateEvent` + `CloseHandle` | 20,946 ns | 18,862 ns | 17,156 ns |
| Critical section, uncontended | 3.7 ns | 3.4 ns | 3.3 ns |
| `SetFilePointer` + `ReadFile` of 4 KiB | 957 ns | 1,027 ns | 1,013 ns |
| Event round trip between two threads | 34,715 ns | 5,851 ns | 19,650 ns |
| Computation loop (per iteration) | 1.2 ns | 1.2 ns | 1.2 ns |
| memcpy of 1 MiB | 16.3 µs | 18.5 µs | 18.5 µs |

A request to wineserver breaks down as follows according to the profile:
- on the client side, four system calls: `rt_sigprocmask` twice, `write` of the request, `read` of the reply;
- on the server side, about five: `epoll_pwait2`, `read`, `write`, `gettimeofday`, `clock_gettime`;
- in total, about 16 µs of CPU spread between the two processes.

A basic LKD system call costs about 0.15 µs (`clock_gettime`). The calls that touch the Darwin kernel cost
1 to 2 µs (`write` on a pipe, `sched_yield`).

## The layers, one by one

| Layer | What it costs (measured) | To remove or simplify it | Risk | Expected gain |
|---|---|---|---|---|
| **Case-insensitive lookup (Wine under Linux on APFS)** | Main thread saturated during `InitOnlineSubsystem` (about 1.9 million file calls in 20 s). 10.5 ms per absent file in a large folder. | LKD answers `FS_IOC_GETFLAGS` (`EXT2_IOC_GETFLAGS`) on a folder with `EXT4_CASEFOLD_FL` when the host volume is case-insensitive (`pathconf(_PC_CASE_SENSITIVE)`). No change to Wine. | Low: it is an exact description of the volume. A case-sensitive APFS volume keeps answering "sensitive". | Estimated from −8 to −12 s; measured **−4.2 s** (27.7 → 23.5 s, see "After the case fix"). |
| **`NtYieldExecution` busy-wait** | 118,000 yields/s in Free Play, each = 3 system calls (about 1.6 µs), that is about 0.2 core. At startup, the main thread spends 8.7 s of CPU in it over the first 10 seconds. LKD's `thread_rusage` always returns 0 context switches, so `SwitchToThread` always answers FALSE ("no yield"). | Under LKD: a `NtYieldExecution` without `getrusage` in voile-wine, or else a `getrusage(RUSAGE_THREAD)` that returns a real counter. Make LKD's `sched_yield` as cheap as Darwin's (1.26 µs against 0.23 µs). | Low, but the return value of `SwitchToThread` changes the behavior of the game's wait loops. To be checked. | From −2 to −3 system calls per yield. About 0.1 to 0.2 core in game; less heat on a laptop. Little effect on the pacing, since this time is waiting. |
| **wineserver: system time in `epoll_pwait2`** | Per 10 s slices in the menu, wineserver takes up to 10.5 s of CPU (8 s in system), for the same number of calls as in the quiet slices (0.9 s). This time is hidden in `epoll_pwait2`. | Count the turns of the `do_wait` loop of `lkd/src/net/epoll.c` (kqueue readable while `ep_collect` returns nothing). Fix if it is indeed a wakeup loop. | Low | Up to one core in the menu. Maybe also some wineserver latency. |
| **ntsync in LKD (`/dev/ntsync`)** | Wait or signal = about 11 µs and a round trip with wineserver. Round trip between two threads: 35 µs (against 5.9 µs with msync under macOS). About 26,000 requests/s in Free Play, of unknown types. | A `/dev/ntsync` device in LKD. Wine and wineserver are already compiled with ntsync (`HAVE_LINUX_NTSYNC_H`). The objects must be shared between host processes: state in shared memory, fds passed through SCM_RIGHTS, atomic WAIT_ALL per device instance, waits interruptible by LKD signals. | Medium. The tricky point is multiprocess, and it must change nothing in what EAC sees. | If most requests are waits, sets and resets: about 10 µs per call. Up to 0.2 to 0.3 s less of blocked thread per second, and part of the 12 to 28% of wineserver CPU. To be decided after the request count. |
| **Presentation (wsd)** | 15.6 ms between submission and display, on a 120/240 Hz screen. | First timestamp each step in wsd: display link tick, encoding, `presentedTime`. Leads: present as soon as the fence ends without waiting for the tick, use only 2 drawables, and check whether native fullscreen (without compositor) is possible for a fullscreen window. | Low to medium (pacing, frame behind) | Estimated 4 to 10 ms of latency. |
| **In-process D3D11 (FEX thunk to DXMT)** | The render server takes 6.5 ms of CPU per frame (decoding and DXMT calls). This cost would be moved, not removed. The crossing between processes costs mostly the fence wait (0.5 ms median) and the ring wakeups. | `SetThunkHandler` and `0F 3F` stubs in fexdarwin (no handler is installed today: a `0F 3F` would crash the JIT). The Neptune decoder, DXMT and Metal would also have to be loaded in each `fexdarwin-run`, and made fork-safe. The cheapest version keeps the Neptune ring and replaces only the vtest transport. | High: Metal in a process that does host forks, crash shared with the game, and 813 generated commands whose generator is not versioned. | Low at 60 fps: less than a millisecond of latency per frame. Useful only if one day the render server saturates (pacing beyond 60 fps). |
| **Native Unix parts (`nptunix`, `wineobelisk`) through thunks** | Not isolated in the profile, but small. The driver passes a key on in 0.05 ms. The SMC storms of `nptunix` are solved since the relink in 16 KiB pages. | The same thunks as above. | Medium | Low. |
| **Native arm64 wineserver** | 12% of a core in Free Play, 28% in the menu (anomaly included), that is about 5 µs of server per request. | Port wineserver to Darwin while keeping the semantics of LKD's fds and shared memory as seen by the guests. Big job. | High | Low once ntsync is in place, which removes most requests. Not justified. |
| **On-disk FEX code cache** | JIT of less than 0.1 s over a whole startup, and about 0.5 ms/s in game. | FEX's `DiskCache` exists, disabled by default. For Darwin, it reads guest memory without `G2H` (`DiskCache.cpp:566-578`, `Core.cpp:1029`). | Medium | Almost zero. To be abandoned. |
| **FEXCore assertions** (`-DENABLE_ASSERTIONS=ON` in `fexdarwin/scripts/build.sh`) | The `IRValidation` pass runs at each compilation. The JIT is negligible, so the cost too. | `OFF` in Release. | Low, but checks are lost. | Negligible. |
| **D3DMetal instead of DXMT** | Already ruled out: x86-64 only, closed code, bridge from the guest needed (`graphics-display-audio.md` §1.1, §1.4). | | | |

## Proposed order

1. **Tell Wine about the case** (LKD, `FS_IOC_GETFLAGS`). Small, risk-free, and it is the only gain counted in
   seconds. Measure before and after with `Launch.log` (`Startup_*`) and `missbench`.
2. **Check the wineserver epoll anomaly**: a counter in `do_wait`, then a fix if it is a
   wakeup loop.
3. **Lighten `NtYieldExecution` under LKD**, checking what the return value of `SwitchToThread` changes.
4. **Count the wineserver requests by type** (counter per request code in voile-wine's wineserver,
   displayed every 10 s), then decide on ntsync in LKD.
5. **Timestamp wsd's presentation path**, then shorten it.

The thunks, the native wineserver and the code cache are not justified by these measurements. We will reexamine them if the
game goes beyond 60 fps and the render server becomes the limit.

## After the case fix (21:19)

Fixes: LKD `ad12111` (`FS_IOC_GETFLAGS` answers casefold for a folder of a case-insensitive volume) and
voile-wine `87a1c39b` (on such a folder, an absent 8.3 name no longer makes the folder be reread). The launch is done
from Obelisk, without profile, like the reference measurement of 17:22.

| Step | Before (17:22) | After (21:19) |
|---|---|---|
| `LoadScriptPackages` | 9.9 s | 9.5 s |
| `InitOnlineSubsystem` | 12.4 s | 8.7 s |
| **Total until the menu** | **27.7 s** | **23.5 s** |

`missbench` (absent file, folder of 20,000 files): long name 10.5 ms → 86 µs, 8.3 name 11.3 ms → 77 µs. The
cost no longer depends on the folder size.

The gain is 4.2 s, half the estimate (8 to 12 s). The folder rereads therefore did not explain
all the time spent in `InitOnlineSubsystem`.

The cost of a lookup stays at 50 to 90 µs, against 9 to 17 µs under the macOS Wine. This remainder comes from LKD's path
calls:
- `newfstatat`: 8 µs;
- `openat`: 15 µs;
- `getxattr`: 17 µs.

Natively, each costs 1 to 3 µs, and a Wine lookup makes 5 to 7 of them. It is a lead to profile, with the rest
of `InitOnlineSubsystem` (8.7 s).

## `LoadScriptPackages` and `InitOnlineSubsystem` in detail (21:30)

Profile every 2 s (`FEXDARWIN_PROFILE_MS=2000`), with, for each thread, the system calls that took it the
most time. This launch took 23.7 s, with the case fix: the measurement disturbs little. `LoadScriptPackages`
covers from 21:30:30.4 to 21:30:40.2, and `InitOnlineSubsystem` from 21:30:41.8 to 21:30:50.6.

| Slice | Duration | What limits it |
|---|---|---|
| 30.4–31.6 | 1.2 s | main thread: file calls (24,000 `getxattr`, 15,000 `newfstatat` in 2 s) |
| 31.6–35.7 | 4.1 s | asynchronous loading. `AsyncIOSystem` spends 95% of its time blocked in `read`, waiting for wineserver replies (about 45,000 `read` and as many writes per second, for 4,500 real file reads, `pread64`). Thread 117 makes 4,500 `mmap` and 4,500 `mprotect` per second (30 µs per `mmap`). wineserver handles about 50,000 requests/s, at 37% of a core. The main thread waits by spinning (900,000 `sched_yield` in 2 s). |
| 35.7–37.9 | 2.2 s | main thread: pure computation (100% of a core, almost without system calls) |
| 37.9–40.2 | 2.3 s | main thread: file calls (75,000 `getxattr`, 67,000 `newfstatat`, 9,600 `openat` in 2 s) |
| `InitOnlineSubsystem`, 41.8–50.6 | 8.8 s | main thread at 100%, about 20,000 absent files per second. Each second: 68,000 `newfstatat` (9 µs), 20,000 `openat` (18 µs), 20,000 case `ioctl`, 20,000 `close`. |

Two causes therefore share about 15 s of the 23.7 s:

- **The cost of path calls in LKD (about 10 s).** `fs_resolve` (`lkd/src/fs/vfs.c`) walks the path
  component by component, and `lookup` does a host `lstat` on the full path of each prefix. A path of
  12 components therefore costs 12 `lstat`, each on a long path.

  A sampling of `missbench` (not of the game) confirms it: `lstat` makes 60% of the active samples,
  `open` 17%, `readlink` 7%.

  Lead: first try the rest of the path in a single `fstatat(..., AT_SYMLINK_NOFOLLOW_ANY)`. macOS then makes
  the call fail as soon as a component is a symbolic link, and we go back to the component-by-component walk
  only in that case, for example for Wine's `dosdevices/c:` link. For an absent name, the parent is checked the
  same way. There is no cache nor invalidation, so the semantics do not change.

  Estimate: `newfstatat` from 9 to about 3 µs, `openat` from 18 to 6 µs. This would make about −4 s on
  `InitOnlineSubsystem` and −1.5 s on `LoadScriptPackages`, to be checked with `missbench`.
- **The round trips with wineserver during the asynchronous loading (about 4 s).** We need to know which
  requests are involved (counter per request type) before choosing between ntsync in LKD and a more direct
  asynchronous I/O path.

The rest is computation (2.2 s, at Rosetta level according to `winbench`), plus `PreInit` (2.3 s) and the other
steps.

## After the file fixes (21:56)

Fixes: LKD `daa6775` (folders opened with `O_SEARCH`, 1.6 µs instead of 7.3 µs on macOS), LKD `fcf56ae`
(`getxattr` first consults `listxattr`, 1 µs instead of 5.6 µs for an absent attribute), voile-wine `c803a925`
(a single case check per lookup, instead of two with the `name?` lookup for reparse points).

| Measurement | 17:22 | 21:19 (case) | 21:56 |
|---|---|---|---|
| `PreInit` | 2.3 s | 2.6 s | 1.6 s |
| `LoadScriptPackages` | 9.9 s | 9.5 s | 8.7 s |
| `InitOnlineSubsystem` | 12.4 s | 8.7 s | 6.3 s |
| **Total until the menu (`Launch.log`)** | **27.7 s** | **23.5 s** | **19.2 s** |
| Dock icon → game window | | about 17.8 s | about 14.9 s |
| Dock icon → menu | | about 27.5 s | about 22.2 s |

Benchmarks (`missbench`, `enumbench`):
- absent file: 87–101 µs → 57–64 µs;
- listed folder entry: 24.2 µs → 7.4 µs (macOS Wine: 4.4 µs).

What the game looks for in vain (sample of one path out of 200, `FEXDARWIN_PROFILE_MISSES`): about 240,000 times
the same localization files (`TAGame.INT`, `ProductSlots.INT`…), in override folders that
do not exist:
- `Documents/My Games/Rocket League/Engine/Localization/INT`;
- `…/ProjectX/Localization/INT`;
- `Engine/Localization` in the game folder.

A rejected trial: resolving in a single `fstatat(AT_SYMLINK_NOFOLLOW_ANY)` the folders of a path without symbolic
link. The gain was only 0 to 9%, because Wine's paths go through `dosdevices/c:` or
`dosdevices/z:` then through a mount change. The `deep_paths` test stayed.

What remains before the window:
- the asynchronous loading, limited by the round trips with wineserver (about 4 s, to be counted per request
  type);
- the game's pure computation (about 2.2 s);
- the startup of Wine and EAC (about 3 s).

## Requests to wineserver by type, and uncapped Free Play (22:03 to 22:18)

wineserver request counter (voile-wine, branch `wineserver-request-stats`, `WINESERVER_REQUEST_STATS`)
and unthrottled pacing on the engine side (`NPT_WSD_PACING=off`, `NPT_PRESENT_QUEUE=0`), then the game's fps limit at the maximum.

| Phase | Requests per second | Remark |
|---|---|---|
| Asynchronous loading (before the window) | about 45,000: `event_op` 17,000, `close_handle` and `create_event` 8,000 each, `select` 6,000, `add_fd_completion` 5,000 | about 6 round trips per file read, all on the `AsyncIOSystem` thread |
| Game shutdown (15.7 s between "AK Audio shut down" and "Object subsystem closed") | about 55,000: `select` and `event_op` in equal parts | the main thread wakes the render thread about 26,000 times per second |
| Free Play capped at 60 fps | `event_op` about 19,000, `select` about 16,000 | |
| Uncapped Free Play (about 130 fps) | `event_op` about 41,000, `select` about 8,000, `create_event` and `close_handle` about 2,500 each | |

Uncapped Free Play: about 130 fps (6 to 9 ms per frame).
- Render thread: 82% of a core, about 20% of it busy-waiting.
- Main thread: 56% of its time blocked in round trips with wineserver (about 33,000 per second,
  about 250 per frame).
- Render server: decoder at 52%. GPU at 43%.

ntsync in LKD would make the `event_op` and the `select` local. It is the lead that serves at once the loading, the
shutdown and the pacing beyond 60 fps.

**EAC incident.** With this instrumented wineserver and its counter active, the mapping of the EAC module in the game ("Wine
module mapping") took 1.4 s then 2.4 s, instead of 0.5 s on all the other launches of the day. At the second
launch, the game started before the end of the mapping and launched without EAC (`bAntiCheatEnabled=(False)`). With the
original wineserver, everything came back: 0.5 s and EAC active.

The cause is not understood. This counter therefore stays out of `main`, and must no longer be used during a protected
launch before it has been understood.

This incident also shows a race: the game can start before the end of the EAC mapping when the mapping is slow.

## ntsync in LKD (23:00)

`/dev/ntsync` in LKD (`f90924f`, `lkd/docs/status/ntsync.md`). Wine 11 uses it without change: the waits and
the signals of events, mutexes and semaphores stay in the process.

| Measurement | Without ntsync | With ntsync |
|---|---|---|
| `WaitForSingleObject(0)` (`winbench`) | 11.8 µs | 0.35 µs |
| `SetEvent` | 10.7 µs | 0.16 µs |
| Event round trip between two threads | 34.7 µs | 8.7 µs |
| Rocket League, until the menu (without profile) | 19.1 to 19.2 s | 18.6 s |
| Rocket League, "Quit" → end of the process | 17.6 to 18.1 s | 10.8 to 11.2 s |
| Uncapped Free Play | about 130 fps | about 150 fps |
| EAC (module mapping, `bAntiCheatEnabled`) | 0.5 s, True | 0.5 s, True |

In uncapped Free Play with ntsync:
- the main thread no longer waits for wineserver; it waits for the render thread (ntsync waits);
- the render thread is the limit (88% of a core, about 20% of it in the busy-wait of `NtYieldExecution`).

Frame back in Free Play at 60 fps: 4.5/min (20:52, without ntsync, profile active), 23/min (23:05, with ntsync),
39/min (23:11, without ntsync, `LKD_NTSYNC=0`, same binary). They vary a lot from one session to another, and ntsync
is not their cause. Their known cause remains to be fixed: the "GPU done" signal of the presentation copy
arrives before the content is in place.

## Frame back: cause and fix (October 4, 00:05)

At each `Present`, the Neptune guest driver arms a single-use event. This event is tied to the fence
signaled after the copy into the swapchain image. Its registration (REGISTER) went through the command stream of the
renderer, while the arming (ARM) and the `SetEventOnCompletion` go through the method ring. When the host
decoded the REGISTER last:
- the proxy created by the ARM had only the reference of the arming;
- the presentation fence released this reference when pairing;
- the DXMT event closed before the GPU signaled it;
- the closing of its pipe (`POLLHUP` without token) removed the fence before the copy.

wsd then showed the frame with its content from 2 or 3 presentations earlier.

What showed it:
- **The wsd probe**: at encoding time as well as after the GPU, the memory still contains the old pixels.
  The copy therefore had not happened; it was not a GPU cache.
- **The instrumented render server**: 189 fences removed on a dead fd in 71 s, all swapchain tokens.
  The proxy registered for the token at that moment had never created a pipe: it was a second proxy, created by the
  REGISTER that arrived late.

Fix: Neptune `22d94ed`. The REGISTER and the RELEASE of these tokens go through the method ring, like the
Win32 events of `npt_event_arm`.

| Free Play at 60 fps | Before (23:58) | After (00:04) |
|---|---|---|
| Fences removed on a dead fd | 189 in 71 s | 0 in 2 min |
| Frame back seen by wsd | 45 in about 65 s (39/min) | 2 in 2 min |

The 2 remaining ones fall in the same second, during frames held 50 to 70 ms at loading. The wsd probe now also notes
what the memory contains at encoding and after the GPU.

## wsd presentation and `NtYieldExecution`: measured, left as is (October 4, 00:30)

### Where the 15.6 ms between submission and display go

For this measurement, wsd timestamped each frame:
- reception;
- display link tick, with its `timestamp` and its `targetTimestamp`;
- `nextDrawable` and commit;
- GPU start and end;
- `presentedTime`.

Every 120 frames, it wrote the medians. This timestamping did not stay in wsd. Conditions:
- Free Play at 60 fps, `after` pacing;
- external display at 240 Hz (ASUS XG27AQWMG, one refresh every 4.17 ms);
- the same binaries as in "Frame back".

| Segment (medians per batch of 120 frames) | Free Play |
|---|---|
| nptunix → wsd sending | 0.02 ms |
| Wait for the tick | 3.7 to 4.0 ms |
| Tick delay over the vsync | 0.3 to 1.2 ms |
| `nextDrawable` + encoding | 0.1 ms |
| Commit → GPU start | 0.55 ms |
| GPU (copy into the drawable) | 0.05 ms |
| GPU end → display | 10.8 to 11.5 ms |
| **Submission → display** | **15.6 to 16.0 ms** (11.9 ms when the phase is favorable) |

- **Composition costs two refreshes.** 97% of the frames are displayed 3 refreshes (12.5 ms) after the
  vsync of their tick, about 1% one earlier and about 1% one later. Yet the GPU finished 1.5 ms after this
  vsync. The rest is the macOS composition: the window has two layers, the window's and the game's
  on top.
- **The frame waits almost a whole refresh before being encoded.** The game locks onto the
  "displayed" event and sends its frame right after a tick. When the frame arrives right before a tick, the wait drops to
  0.3 or 0.5 ms, and the total to 11.9 ms.
- **Probe, 8 attempts:** from input to submission, 40.5 ms median (from 27.4 to 42.6 ms). The total is therefore
  about 56 ms.
- nptunix waits 15.8 ms per `Present`. No frame back during the measurement.

Leads with figures, not done: the user judged the gain too small.

| Lead | Expected gain at 240 Hz |
|---|---|
| Present on arrival when no frame of the plane is waiting for display, instead of waiting for the tick | From 0 to 4 ms, depending on the compositor's deadline, unknown |
| Direct display without compositor (a single fullscreen layer) | Up to 2 refreshes (8.3 ms) if macOS accepts it, which is not established |
| 2 drawables instead of 3 | Nothing: with `after` pacing, a single frame is in flight |

On a 120 Hz display, these durations should double, but this is not measured. The 4 to 10 ms estimate of the layers
table assumed such a display.

### `NtYieldExecution`: no change

- **The game ignores the return value.** `RocketLeague.exe` calls `SwitchToThread` from about 70 places.
  - Most are busy-waiting locks: `lock cmpxchg` on a byte, then `pause` 1, 2, 4, 8 and 16 times,
    then `SwitchToThread` at each new attempt.
  - The others are loops on a global flag.
  - After each call, `eax` is reset to zero or reloaded: the return value is never read.
- **A cheaper yield would not save CPU.** A thread in these loops spins until the lock is
  released, whatever the cost of a turn. A cheaper yield gives more turns, not less CPU. The estimate
  "about 0.1 to 0.2 core" of the layers table was therefore wrong: we would gain 2 system calls per yield, and
  a lock reacquisition faster by less than a microsecond.
- **User decision: we touch nothing.** Other games may read the return value of
  `SwitchToThread`. LKD's `thread_rusage` keeps returning 0 context switches, so `SwitchToThread`
  always answers FALSE under the engine.

### Measurements removed

At the user's request, no measurement runs at each frame in wsd anymore. The frame back detector
(GPU copy of 64 pixels and two CPU reads of 64 pixels per frame) is removed: the Neptune fix `22d94ed`
removed what it was looking for. To use it again, wsd must be taken back to `ff71f31`. The latency probe
stays, because it does nothing as long as no `wsdctl` input enables it.

## Voile release v26.3.0-9 (October 4, 02:00)

Content, for the Linux engine:
- ntsync;
- the file fixes;
- the frame back fix;
- wsd without its detector;
- FEXCore without its assertions (`e20b270`).

voile-lkd and Neptune are now submodules of Voile, like voile-wine. `build-linux.sh` compiles them; the
`lkd-*` and `neptune-*` archives are no longer used.

| Measurement | v26.3.0-9 |
|---|---|
| Archive | 400 MB (406.5 MB for the -8) |
| Rocket League, EAC | `bAntiCheatEnabled=(True)` on all the launches that reached the game |
| x86-64 witness programs, FEX without assertions | same exit codes as with assertions (21 out of 21) |

**Removal of the symbol tables: tried, then abandoned.** Voile already removed the debug info; it
tried to also remove the symbol tables:
- PE: `--strip-all`;
- Mach-O: `strip -x` for the libraries, `strip` for the programs;
- ELF: `--strip-all`.

The gain was 32 MB. At the user's request, Voile went back to `strip -S` and `--strip-debug`, as for
the -8. During the trials, two incidents:
- **First launch after the installation** (engine without symbols), therefore with a prefix update by
  wineboot:
  - Rocket League stopped at startup on corrupted data (`Bad name index 359464960/2678:
    Core.upk`);
  - then the EAC launcher crashed (list at pointer `0xffffffffffffd8f0`).
- **Launch with forced prefix update** (`.update-timestamp`), same engine: black screen. Two threads of the
  game were running at 99%, and a critical section had been blocked for more than 60 s.

The following launches all worked, without the symbols as with them: an engine with assertions, the engine without
assertions unstripped, then stripped, then the final package. The cause is not found. Both incidents fall on
a prefix update, when many Windows processes start together; ntsync is a lead.

What to do at the next incident:
- have `FEXDARWIN_DUMP_DIR` in `engine/fexdarwin.env`;
- touch `dump-request` during the hang;
- to reproduce, force the update (`printf '1\n' > <prefix>/.update-timestamp`).

**wsd bug on two displays.** wsd's `HELLO` gives the size of `NSScreen.mainScreen`, the display of the active
window. With the MacBook display on, Rocket League received 3024x1964 while its window was on the
2560x1440 display, and its frame overflowed. Fixed in `6a197ac`: wsd gives the display of the menu bar, which is
already the origin of its coordinates. Checked with Obelisk on the MacBook display: the game receives 2560x1440.

## Next measurements

- **System calls per thread and per type.** The profile gives the totals per thread and the types per process, but
  not both together. It is needed to know which thread makes the requests to wineserver and what bounds
  `LoadScriptPackages`.
- **wineserver requests by type** (point 4 above).
- **Uncapped frame time** (`NPT_WSD_PACING=off`, the game's fps limit at the maximum), to know the real margin
  and the part that saturates first.

## Reproduce

- Profile: `FEXDARWIN_PROFILE=1` (and `FEXDARWIN_HITCH_LOG=1`, optionally) in `engine/fexdarwin.env`; add
  `guest WINE_OBELISK_LATENCY=1` for the driver timestamps. The lines go to `~/Library/Logs/Obelisk/Epic/wine-<game>.log`.
- Native threads and GPU: `clang -O2 -o engine-top tools/dev/engine-top.c -framework IOKit -framework CoreFoundation`,
  then `engine-top 5`.
- Latency: in the menu, `wsdctl $TMPDIR/obelisk-engine-<pid>/wsd.sock tap 56` (Shift, enables the probe), then
  `tap 125`/`tap 126`. In Free Play, `tap 49` (ball camera). `wsd: probe:` lines in the log.
- Benchmarks: compile with `x86_64-w64-mingw32-clang -O2` (Voile's llvm-mingw). Run them through Obelisk's
  `crates/game-runtime/src/linux_engine/launch.sh`, under `run-with-timeout.sh`, then check
  `pgrep -x fexdarwin-run`.

## Pacing on the MacBook display and wineserver spinning idle (October 7)

Machine: the same, but on the built-in display (ProMotion, 120 Hz), on mains power. Voile runtime `v26.3.0-11`. Scene:
Free Play, Forbidden Temple, ball camera, car stopped, window 2560x1408, VSync unchecked. The launches are
done from Obelisk, driven through accessibility, and the game is driven with keyboard and mouse by `wsdctl`.

### Measurements

| Measurement | v26.3.0-11 | `latch` pacing | `latch` + epoll fix |
|---|---|---|---|
| Frames displayed, Free Play | 30.5 fps | 120 fps | 120 fps |
| Frames displayed, menu | 33 fps | 120 fps | 120 fps |
| Input → reacting frame submitted (12 attempts) | 62 to 98 ms, median 77 | 18 to 30 ms, median 20 | 17.5 to 23.6 ms, median 21 |
| Submission → display | 33 ms | 32 ms | 32 ms |
| Engine CPU (engine-top), Free Play | 0.55 core | 2.22 cores | 1.46 cores |
| wineserver | | 78% of a core | 8% of a core |
| Uncapped Free Play (`off` pacing, `UncappedFramerate=True`) | | 184 to 196 fps, 3.0 cores | 185 to 210 fps, 2.4 cores |
| Startup (`EULA sync response`) | 20.5 s | | 20.7 s |
| EAC (`bAntiCheatEnabled`) | True | True | True at each launch |

### Pacing: wait for the frame to be latched, plus its display

With the `after` pacing, nptunix waited for the `EV_PRESENTED` event of each frame, so the macOS composition
(3 to 4 refreshes on the built-in display, 25 to 33 ms) was part of each frame. On a 120 Hz display, the
game capped at 30 fps. The displayed intervals fell on 25 and 33.3 ms, multiples of the 8.33 ms tick: the
display link was indeed running at 120 Hz.

The new `latch` pacing (default) waits for `EV_RELEASE` of the sent frame, recognized by its serial number:
wsd copied it into a drawable at a tick, or replaced it. The game stays locked to the display's refresh rate with a
single frame queued, and the composition overlaps the following frames. `after`, `before` and `off` remain
available through `NPT_WSD_PACING`.

Checked: VSync enabled then disabled, switch to borderless then back to windowed, window minimized (about
16 fps, waits bounded to 100 ms) then restored (back to 118 fps in a few seconds).

Rejected trial: asking the display link for the display's maximum refresh rate (`preferredFrameRateRange`). No effect,
the display link was already running at 120 Hz.

### LKD epoll: a filter that watches only for closing spun in a loop

It is the wineserver epoll anomaly noted on October 3. A counter in `do_wait` showed about 300,000 idle turns
per second, always on a socket. wineserver sets `events` to 0 on a client socket that it does not want to
read right away. LKD still registered a level-triggered kqueue read filter, to see the
closing. As long as the socket kept unread data, kqueue reported it at each wait, `ep_collect` found
nothing requested, and `epoll_wait` started again immediately.

Fix: when the item requests neither `EPOLLIN` nor `EPOLLRDNORM` (`events` at 0, or `EPOLLRDHUP` alone), the read
filter is edge-triggered (`EV_CLEAR`). It wakes once per new data or at closing. When the
filter mode changes, LKD removes it then puts it back. Test `hangup_only_watch_does_not_spin` in `test_net_epoll`:
it fails before the fix (the CPU time of the wait) and passes after. The six network suites pass.

Two findings unrelated to this fix:
- LKD does not report `EPOLLHUP` on a socket whose peer has closed as long as it keeps unread data (same
  result before and after).
- `test_sig_futex` hangs in the `waitv` case (same result before and after).

### Profile of the render thread (uncapped Free Play)

A temporary sampler (suspension of each guest thread, host PC translated to guest RIP by
`RestoreRIPFromHostPC`, removed since) gives:
- game code 15 to 17%;
- Neptune's `d3d11.dll` 13 to 15%, 5% of it in the submission to the ring (lock, `sfence`, full barrier,
  atomic exchange at each command);
- ucrtbase's `memmove` and `memcmp` 5%;
- the game's busy-wait (`NtYieldExecution`: two `getrusage` and one `sched_yield`) about 30%;
- round trips with wineserver 6 to 14%: `NtCreateEvent`, then `get_inproc_sync` which fetches the
  ntsync descriptor of each new event;
- the rest in ntsync waits.

The main thread mostly waits for the render thread (ntsync) and makes socket ioctls (13%). SMC faults: 4 in 11 s,
JIT: 4 ms. In the render server, about 9% of the decoder goes into `dxmt::guid::make_guid_helper`:
`__uuidof` rereads the GUID text there at each `QueryInterface`.

### Rejected trials

| Trial | Result |
|---|---|
| `TSOEnabled=0` | The game crashes in `d3d11.dll` (Neptune's ring depends on x86 memory ordering). |
| User-interactive QoS for all guest threads | 189 to 211 fps uncapped (average +6%), but one launch out of six without EAC. |
| D3DMetal (x86_64 render server under Rosetta) | Correct frame, 119.4 fps, 212 fps uncapped (like DXMT), startup 22.5 s. First launch without EAC. |
| PE volatile metadata (TSO disabled outside volatile accesses) | None of the installed games has any. |

### EAC: the module mapping race

On all the launch logs of the day, EAC is absent exactly when "Wine module mapping" lasted
2.31 s (usually 0.37 to 0.41 s; a launch at 1.08 s stayed active). The two cases: a launch with the
user-interactive QoS, and the first D3DMetal launch, while Rosetta was translating the render server. Any
heavy load at launch time can therefore disable EAC. It is to be fixed for its own sake.

Follow-up (October 8). The two failures last 2.305 and 2.306 s: it is a fixed delay of about 2 s in the bootstrapper,
not just any slowness. The two failed launches also started the game much more slowly (preinitialization
of 5.4 s against 1.4 s with D3DMetal). A launch with the 18 cores saturated by 18 `yes` loops for 25 s from
the click: mapping in 0.386 s, EAC active. An ordinary CPU load is therefore not enough. No failure over more than 45
launches with the chosen configuration. What the bootstrapper waits for during these 2 s remains unknown.

## The pacing follows the game's VSync (October 8)

The `SyncInterval` of each `Present` now goes from Neptune (D3D11 and D3D12) to nptunix (Neptune `144f613`,
voile-wine `5ec9624`). A `Present` with VSync waits for wsd to latch the frame (`latch`), a `Present` without VSync
waits for nothing, as on Windows. `NPT_WSD_PACING` still forces a mode for all frames. Obelisk copies the
runtime's `d3d11.dll` into the prefix before each launch, so the driver and nptunix always have the same parameter
structure.

| Launch (Free Play) | Display | `SyncInterval` received | Frames submitted | Frames displayed | EAC |
|---|---|---|---|---|---|
| VSync unchecked, unlimited FPS | MacBook 120 Hz | 0 | 190 fps (menu 385) | 120 fps | True |
| VSync unchecked, limited FPS (`UncappedFramerate=False`) | MacBook 120 Hz | 0 | 194 fps | 120 fps | True |
| `UseVsync=True` + `UncappedFramerate=True` | 240 Hz, windowed and fullscreen | 0 on 20,270 and 15,062 frames | 179 to 187 fps | 173 to 179 fps | True |
| VSync checked in the game (`UncappedFramerate=False`) | 240 Hz | ≥ 1 (pacing wait 3.9 to 5.6 ms) | 176 fps | 176 fps | True |

Rocket League sends the VSync only if the FPS are not unlimited. `UncappedFramerate=False` limits nothing
here. Startup unchanged (20.7 s). In fullscreen, submission → display drops to 12.6 ms (24 ms in windowed).

To look into: on the external 240 Hz display (XG27AQWMG), wsd displays no more than 173 to 179 fps, with or without pacing,
even when the game submits more.

## Dead by Daylight reads /proc/self/status every frame (October 23)

Runtime Voile `v26.3.0-18`, Dead by Daylight (Epic, UE5, EAC) on the Linux engine, 240 Hz screen (XG27AQWMG). Tools:
`FEXDARWIN_PROFILE=1`, `engine-top`, and 8 thread dumps 4 s apart (`FEXDARWIN_DUMP_DIR`, then `touch dump-request`).

Before: in game, a frame every 25 to 28 ms (35 to 40 fps), GPU 8 to 13%. The `GameThread` used 85 to 90% of a core
and spent 6 to 8.7 s of every 10 s inside `read`, running, not waiting. System time of the game process: 7 to 10 s per
10 s.

Cause. 7 dumps of 8 caught the `GameThread` in `read` → `pf_generate_pid` → `lkd_mm_stats`: 5 in `resident_bytes`
(`mach_vm_region`, `mincore`), 2 in `vt_next` (the walk of the VMAs itself). The Wine frames under it are
`NtQueryInformationProcess(ProcessVmCounters)` → `fill_vm_counters`, which reads `/proc/self/status`: the game calls
`GetProcessMemoryInfo` every frame. `lkd_mm_stats` walked every VMA (`vt_next` descends from the root, so the walk is
O(n log n)) and asked the host for the residency of every host region, under the mmap lock. MEASURED in a native test:
19.5 ms to walk 40,000 host regions with `mach_vm_region`, 0.48 us for one `task_info(TASK_VM_INFO)`. The 100 ms RSS
cache never served, because each mmap or munmap of the game invalidated it. The lock also held the other threads: mmap
took 4.4 ms on average, munmap 1 ms, and every munmap walked all VMAs again for VmPeak (`hiwater_before_unmap`).

Fix (voile-lkd `fb0a9af`):

- RSS is the host process's resident memory (`TASK_VM_INFO`: `internal` as RssAnon, the rest as RssFile, RssShmem 0).
  It includes the engine's own memory (FEX's code cache, LKD), a few hundred MiB for a game of several GiB. The 100 ms
  cache stays, so `stat` and `status` read back to back agree.
- The VMA tree keeps the bytes of all VMAs and of the stack, exec and data ones at every insertion, removal and bound
  change, and around a change of prot or label in place (`vt_account`), as Linux keeps `total_vm`, `stack_vm`,
  `exec_vm` and `data_vm` (`vm_stat_account`). VmLck is `locked_bytes`. The VMA holding `start_stack` moves to the
  stack at read time. `vt_check` compares the counters with a walk; test `test_mm_misc` case
  `vm_counters_follow_the_tree`.

After, same kind of session: a frame every 16.7 ms (60 fps, the game's own limit: the `GameThread` now sleeps 6 to 7 s
per 10 s in `pselect6`), `GameThread` at 30 to 35% of a core, system time 3.5 to 4.3 s per 10 s, GPU 37 to 40%,
munmap about 10 us. The user confirmed in game that it runs much better.

Left as they are:

- Hitches of 0.4 to 1.5 s: 13 windows of 120 frames out of 42 hold a frame above 100 ms (27 of 47 before). Loading or
  shader compilation, not investigated yet.
- `RHIThread` and `GameThread` still spin in `NtYieldExecution` (`getrusage` + `sched_yield`, 2.5 to 3.3 million
  `getrusage` calls per 10 s on `RHIThread`), as decided for Rocket League.

Reading Wine's Unix frames in a dump: the printed offset is a file offset, and `ntdll.so` and `win32u.so` map their
code 0x4000 above it (LOAD segment), so add 0x4000 before looking the address up with `nm`.

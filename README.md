# voile-lkd

**The Linux engine of [Voile](https://github.com/Zagrios/voile).** It runs unmodified x86-64 Linux programs in native macOS arm64 processes, with no virtual machine and no Hypervisor.framework.

[Obelisk](https://github.com/Zagrios/obelisk) uses it for Windows games whose anti-cheat supports Linux, such as Easy Anti-Cheat in Rocket League and ELDEN RING. The game runs in voile-wine built for Linux, and the anti-cheat loads its Linux module and sees a real Linux kernel interface.

[docs/V1.md](docs/V1.md) records what works, the measurements behind it and the known limits.

## Contents

- [How it works](#how-it-works)
- [What is inside](#what-is-inside)
- [Building](#building)
- [Running a guest program](#running-a-guest-program)
- [Documentation](#documentation)
- [License](#license)
- [Related repositories](#related-repositories)

## How it works

Each guest process is one macOS host process, `fexdarwin-run`:

```text
Windows game ── voile-wine for Linux x86-64 ── glibc, from a Debian x86-64 root
    │
    ├─ x86-64 code ............ FEXCore translates it to arm64
    ├─ Linux system calls ..... LKD serves them on top of macOS
    ├─ windows, input, audio .. wsd: Cocoa windows, Metal presentation, CoreAudio
    ├─ Direct3D ............... Neptune's render server, then Metal
    └─ gamepads ............... obelisk-input, with SDL3
```

Apart from `guest/`, what runs inside the guest comes from elsewhere: voile-wine built for Linux x86-64, the Debian x86-64 root and Neptune's Direct3D DLLs. Voile builds and ships them with this engine.

## What is inside

| Folder | What it is |
|---|---|
| `lkd/` | **LKD**, Linux kernel on Darwin: a C library that implements about 375 Linux x86-64 system calls (memory, files, processes and threads, signals, futex, sockets, procfs, ptrace) on top of macOS. One host process per guest process. |
| `fexdarwin/` | The host program, `fexdarwin-run`: FEXCore translates the guest's x86-64 code to arm64, and every guest `SYSCALL` goes to LKD. It handles signals, thread kicks, self-modifying code, fork and exec, the safety watchdog, and a guest thread dump (`FEXDARWIN_DUMP_DIR`). |
| `fex/` | Submodule [Zagrios/FEX](https://github.com/Zagrios/FEX): FEX-Emu with its Darwin port. |
| `wsd/` | The window server: Cocoa windows, zero-copy Metal presentation of the frames the guest renders, keyboard and mouse events, and audio output and input (the default microphone) through CoreAudio. The guest's Wine drivers (`wineobelisk.drv`, `wineobeliskaudio.drv`, Neptune's `nptunix`) talk to it over a UNIX socket (`wsd/PROTOCOL.md`, `wsd/wsd_protocol.h`). `wsdctl` injects test input. |
| `input/` | `obelisk-input`, the gamepad service of both of Voile's Wines: a native arm64 program that reads the Mac's gamepads with SDL3, linked statically, and serves them to voile-wine's `winebus.sys` over a UNIX socket, rumble included (`input/obelisk_input_protocol.h`). |
| `guest/` | x86-64 Linux libraries for the guest, built with zig. `steam-hostpid.so` makes `getpid()` return the macOS pid of the host process when Valve's `steamclient.so` calls it (`/proc/<pid>/lkd_host_pid`): Obelisk's Steam host drops a client whose pid is not a live macOS process. |
| `tests/` | Witness programs: x86-64 ELF (`elf-x86_64`, freestanding), glibc (`glibc-x86_64`), Darwin facts, graphics, input, the EAC probe campaign. |
| `tools/dev/` | Development scripts that drive a whole session: authenticated Rocket League launch, offline Free Play, mouse stress. |
| `tools/epic-launch/` | Builds an Epic launch plan with Obelisk's `epic-kit`, for development only: Obelisk does this itself. |
| `docs/` | `V1.md`, and `journal/`, the research and integration journal. |

## Building

You need a Mac with Apple silicon, Xcode, cmake and ninja. zig is needed for `guest/`.

```sh
git clone --recursive https://github.com/Zagrios/voile-lkd.git
cd voile-lkd
brew install cmake ninja
./build.sh
```

| Result | Path |
|---|---|
| LKD | `lkd/build/lib/liblkd.a` |
| Host program | `fexdarwin/build/host/fexdarwin-run` |
| Window server | `wsd/bin/wsd`, `wsd/bin/wsdctl` |
| Gamepad service | `input/bin/obelisk-input`, which fetches SDL3 once |
| Guest library | `guest/bin/steam-hostpid.so`, with zig on `PATH` or `ZIG=<path to zig>` |

LKD builds in Release; `BUILD_TYPE=Debug` builds it unoptimized, for a debugger. `lkd/check.sh` builds LKD and runs its own tests.

## Running a guest program

```sh
FEXDARWIN_LKD=1 FEXDARWIN_ROOT=<x86-64 Debian root> FEXDARWIN_ROOT_RO=1 \
FEXDARWIN_BIND="/opt/wine=<wine install>:ro;/home/user=<home>" \
fexdarwin/scripts/run-with-timeout.sh 60 fexdarwin/build/host/fexdarwin-run \
  --env HOME=/home/user /opt/wine/bin/wine cmd.exe
```

> [!WARNING]
> Always start guests through `run-with-timeout.sh`, or any supervisor that sets `FEXDARWIN_SUPERVISOR_PID`. Every guest process ends itself within half a second once its supervisor is gone. Runaway guests once exhausted the kernel's VM map entries and panicked the Mac (see the safety section of `docs/V1.md`).

The scripts in `tools/dev/` and most of `tests/` expect the development workspace they were written in (`EXP`: the Debian root, the Wine build, a copy of the game, the evidence folders). They are kept as they ran for v1.

## Documentation

- [docs/V1.md](docs/V1.md): architecture, what works, measurements, how a session is assembled, known limits.
- `docs/journal/`: the research and integration journal. `01-integration-status.md` is the chronological log.
- [lkd/README.md](lkd/README.md): LKD's layout, conventions and module ownership.
- `wsd/PROTOCOL.md`: the window server's protocol.

## License

voile-lkd is licensed under the GNU General Public License v3.0: see `LICENSE`. The third-party works it includes or derives from keep their licenses, listed in `NOTICE`: HyperLinux (Apache-2.0), FEX-Emu (MIT) and the Linux uapi headers (GPL-2.0 WITH Linux-syscall-note, used as numeric ABI facts only).

## Related repositories

| Repository | Role |
|---|---|
| [Obelisk](https://github.com/Zagrios/obelisk) | The Mac app: Steam, Epic Games and GOG libraries, downloads and launches |
| [Voile](https://github.com/Zagrios/voile) | The Windows runtime Obelisk installs, which ships this engine |
| [voile-wine](https://github.com/Zagrios/voile-wine) | The Wine inside Voile, for both engines |
| **voile-lkd** | The Linux engine: x86-64 Linux programs in native macOS processes |
| [Neptune](https://github.com/Zagrios/neptune) | Direct3D for the Linux engine |
| [FEX](https://github.com/Zagrios/FEX) | FEX-Emu with its macOS port, which this engine translates x86-64 code with |

# Wine macOS: inventory for the Windows, Linux and macOS bridge

Date: 2026-10-02. Status: version 2, complete for the five points of the brief; the unknowns are listed in section 6. Written by the inventory agent on the Wine macOS side.

Conventions. MEASURED: output of a command or of a small program run today on this machine (M5 Max, macOS 27.0.1 build 26A434, Darwin 27.0.0 xnu-13432.1.9, 16 KiB host pages, Rosetta present). READ: source quote with `path:line`. INFERRED: inference to check. EXTERNAL: third-party claim read on the web, not checked locally.

Path abbreviations.
- `W` = `/Users/mathieu/Documents/Github/Zagrios/voile-wine` (branch `main`, HEAD `e82937c9`, `git describe` = `v26.3.0-2-14-ge82937c`, `VERSION` file = "Wine version 11.0", 22 Obelisk commits on top of `ff44e00d` "CrossOver 26.3.0").
- `V` = `/Users/mathieu/Documents/Github/Zagrios/voile` (HEAD `fb27a63`, pins voile-wine `e82937c9`).
- `RT` = `~/Library/Application Support/com.zagrios.obelisk/Voile/v26.3.0-4` (installed release, `runtime.json`: crossover 26.3.0, voile `6d2bbb44`, voile-wine `9861d8b4`, proton `5b89db94`, D3DMetal "Evaluation environment for Windows games 4.0 beta 2").
- `CUR` = `~/Library/Application Support/com.zagrios.obelisk/Voile/current` (development runtime installed by `install-dev.sh`, copy from 2026-09-26 19:26, without `runtime.json`, binaries not stripped).
- `FEX` = `build/experiments/rocket-hyperlinux-20260928/probes/fex-implementation-feasibility-20260929/source/FEX-0df84d3844bcdb87bb7d3f5b8fb0959cd009c038` (same commit as `FEX-Emu/FEX` HEAD on 2026-10-02, checked with `git ls-remote`).
- The probe sources and their raw outputs are in the session temporary directory (`.../scratchpad/wmb/`, volatile). The useful sources are reproduced in appendix A.

## 0. Summary for the architecture decision

1. The installed runtime is entirely x86_64 and runs under Rosetta. `lipo -archs` gives `x86_64` for `bin/wine`, `bin/wineserver`, the 34 Mach-O files of `lib/wine/x86_64-unix`, `libd3dshared.dylib`, `D3DMetal.framework` and its four dylibs. The 766 PE files of `x86_64-windows` are PE32+ x86-64 and the 815 of `i386-windows` are PE32 i386. There is no arm64, arm64ec or `xtajit` PE in the runtime (MEASURED, section 1).
2. Obelisk runs `Voile/current` before the pinned release (`crates/game-runtime/src/voile.rs:59-73`). On this machine the effective runtime is therefore the development build from 2026-09-26.
3. D3DMetal has no arm64 slice (MEASURED). A native arm64 process cannot load it: the Metal path of a "Wine arm64 plus FEX" design cannot reuse D3DMetal as is.
4. PE to Unix entry: the Wine DLLs call `callq *0x7ffe1000` (stub in `include/wine/asm.h:287-301`, bytes MEASURED in `ntdll.dll`), because `virtual_init_user_shared_data` forces `SystemCall = 1` (`virtual.c:4457`). No `syscall` instruction is executed by Wine's own code.
5. Raw `syscall`: under Rosetta, every class 0 number (so every Linux number and every NT identifier) raises SIGSYS (MEASURED on 6 cases). Wine macOS installs `sigsys_handler` (`signal_x86_64.c:3062-3065`), which forwards to the dispatcher. There is neither Syscall User Dispatch nor `prctl` in the tree. The Linux numbers (0 to about 450) overlap the NT identifiers of table 0 (259 services, 0x000 to 0x102): the Linux semantics can only be chosen by the caller's address or by a thread state. The real case is the EAC PE bootstrapper, which itself issues Linux `syscall`s (319 `memfd_create`, 1 `write`) from its image at `0x14004b9ff` (`docs/research/rocket-league-macos.md:54-57`).
6. The TEB is not the GS base on macOS: Wine copies `Tib.Self`, `ThreadLocalStoragePointer` and `Peb` into the pthread TSD (`gs:0x30`, `gs:0x58`, `gs:0x60`) and rewrites `localtime()` in memory. `ANNOUNCE.md:63-64` announces a GS swap in the dispatcher upstream, but the CrossOver code of this tree and the installed dispatcher (disassembled) do not do it.
7. Rosetta gives the guest real 4 KiB pages and accepts a 0x1000 `__PAGEZERO` (mappable low area, including 0x7ffe0000). A native arm64 process has 16 KiB pages, nothing below 4 GiB, is killed at exec if its `__PAGEZERO` is smaller than 4 GiB, and loses `x18` at each kernel entry (all MEASURED, section 3). Wine's current scheme (zerofill segments, USER_SHARED_DATA at 0x7ffe0000, TEB in `x18`) therefore does not carry over as is into an arm64 process.
8. voile-wine already contains the "Wine ARM64 plus emulator" plumbing: ARM64EC (`signal_arm64ec.c`, `load_arm64ec_module`), `xtajit64.dll` stub with 20 exports (16 `stdcall` and 4 `extern`), `Wow64\amd64` and `Wow64\x86` keys, `KiUserEmulationDispatcher`, simulation of 4 KiB pages on a 16 KiB host. The Voile build only compiles `i386,x86_64` (`V/scripts/build.sh:33`).
9. Wine's `xtajit64` interface is identical, name for name, to that of FEX 0df84d38's `libarm64ecfex.dll`. FEX's syscall goes through the `STATUS_EMULATION_SYSCALL` exception, which `signal_arm64ec.c:1282` can handle, and FEX puts the real TEB in `gs` (`FEX/Source/Windows/ARM64EC/Module.cpp:361-367`). No TSD hack is needed on the guest side.
10. Three candidates for the first crossing witness are described in section 5.3: (T1) a PE that loads a static ELF and whose Linux `syscall`s are routed by Wine to macOS, under Rosetta; (T2) a PE plus Unix Mach-O library pair compiled out of tree with the Voile recipe; (T3) a standalone arm64 host with FEXCore running the same PE and ELF loads, which serves as a double of T1.

## 1. Component matrix and architecture of the current macOS runtime

Sources: `lipo -archs`, `file`, `otool -L`, `otool -l`, `nm` on `RT` (commands in appendix B); PE headers read by a Python script; `CUR` gives the same architectures. Wine version: string `wine-11.0` in `ntdll.so`, `W/VERSION`, CrossOver 26.3.0 (`RT/runtime.json`).

### 1.1 Components

| # | Component | Path in `RT` | Format and architecture (MEASURED) | Loaded by | Process and execution architecture | Role |
|---|---|---|---|---|---|---|
| 1 | `wine` launcher | `bin/wine` (9808 B) | x86_64 Mach-O executable, 4 GiB `__PAGEZERO`, `__TEXT` at 0x100000000 | Obelisk (Rust, arm64) via `Command::new` (`wine.rs:242,406`) | Rosetta x86_64 | `W/tools/wine/wine.c:20-65`: `dlopen("ntdll.so")` then `__wine_main`; this process then re-executes the loader (row 2) |
| 2 | `wine` loader | `lib/wine/x86_64-unix/wine` (14704 B) | x86_64 Mach-O executable; `__PAGEZERO` 0x1000, `WINE_RESERVE` 0x1000+0x1fffff000, `WINE_TOP_DOWN` 0x7ff000000000+0x1ff0000, `__TEXT` 0x200000000 | `reexec_loader` (`W/dlls/ntdll/unix/loader.c:2658-2683`) then at each new Windows process by `exec_wineloader` (`loader.c:709-729`) | Rosetta | `W/loader/main.c`: reserves low memory before anything else, loads `ntdll.so`, calls `__wine_main` |
| 3 | `ntdll.so` | `lib/wine/x86_64-unix/ntdll.so` (623200 B; `CUR` 699888 B) | x86_64 Mach-O dylib; IOKit, CoreFoundation, CoreServices, libSystem | `dlopen` by rows 1 and 2 | Rosetta | Unix half of ntdll: syscall dispatcher, signals, virtual memory, wineserver protocol, msync, PE loading |
| 4 | `ntdll.dll` | `lib/wine/x86_64-windows/ntdll.dll`, `i386-windows/ntdll.dll` | PE32+ x86-64 ("Wine builtin DLL"), ImageBase 0x170000000; i386: PE32 | `load_ntdll()` then `load_wow64_ntdll()` (`loader.c:2371-2372`) | Rosetta (translated PE code) | syscall stubs, Rtl, Ldr, KiUser*Dispatcher |
| 5 | `win32u.so` | `lib/wine/x86_64-unix/win32u.so` (1896224 B) | x86_64 Mach-O dylib; CoreText, AppKit, CoreFoundation | `dlopen` on the first call of `win32u.dll` | Rosetta | Unix half of win32u; registers syscall table 1 (`W/dlls/win32u/syscall.c:103`) |
| 6 | `win32u.dll` | `lib/wine/x86_64-windows/win32u.dll` | PE32+ x86-64, ImageBase 0x180000000 | ntdll / user32 | Rosetta | `NtUser*` and `NtGdi*` stubs (identifiers 0x1000 and up) |
| 7 | `winemac.so` and `winemac.drv` | `lib/wine/x86_64-unix/winemac.so` (511936 B); `lib/wine/x86_64-windows/winemac.drv` | `.so`: x86_64 Mach-O; AppKit, Carbon, CoreVideo, Foundation, IOKit, Metal, OpenGL, QuartzCore, Security; `.drv`: PE32+ x86-64 | win32u (graphics driver) | Rosetta | macOS display driver: windows, input, OpenGL; sources `d3dmetal.c` and `d3dmetal_objc.m` for D3DMetal presentation (`W/dlls/winemac.drv/Makefile.in`) |
| 8 | `wineserver` | `bin/wineserver` (675120 B; `CUR` 845840 B) | x86_64 Mach-O executable; libSystem only | `exec_wineserver` (`loader.c:737-756`) or Obelisk (`wine.rs:251,513`) | separate Rosetta process, one per prefix | NT objects, registry; `W/server/mach.c` (bootstrap port, task ports, `mach_vm_read_overwrite`, `mach_vm_write`, debug state); `W/server/msync.c` |
| 9 | `libd3dshared.dylib` | `lib/external/libd3dshared.dylib` (241888 B) | x86_64 Mach-O dylib; libc++, Foundation, CoreGraphics, libobjc. Exports `___wine_unix_call_funcs`, `_register_non_native_code_region`, `_supports_non_native_code_regions`, `KeUserModeCallback`, `D3DMThreadInit`, `Win32DispatchInit`, `GetMacDRVFunctions`, `PatchUnixCallTable`, `PatchWin32DispatchFunctions` | `dlopen` by `init_non_native_support` if `CX_APPLEGPTK_LIBD3DSHARED_PATH` is set (`loader.c:1441-1473`; Obelisk sets it, `wine.rs:134-140`) and by the six links below | Rosetta | glues D3DMetal to Wine: looks up `NtCreateEvent`, `NtCreateSemaphore`, `ntdll_get_unix_file_name`, `macdrv_functions`; patches the Unix call tables |
| 10 | `d3d10.so`, `d3d11.so`, `d3d12.so`, `dxgi.so`, `nvapi64.so`, `nvngx-on-metalfx.so` | `lib/wine/x86_64-unix/` | six symbolic links to `../../external/libd3dshared.dylib` | `get_builtin_unix_funcs` (`virtual.c:837-860`) when the PE DLL of the same name queries `MemoryWineUnixFuncs` | Rosetta | Unix libraries of the D3DMetal wrappers |
| 11 | `D3DMetal.framework` | `lib/external/D3DMetal.framework/Versions/A/D3DMetal` (7578032 B) | x86_64-only Mach-O dylib (no arm64 slice); install name `/System/Library/Frameworks/D3DMetal.framework/...`; Metal, MetalFX, MetalKit, AppKit, CoreAnalytics (private) | `dlopen` by `libd3dshared` (strings `@rpath/D3DMetal.framework/D3DMetal`, `D3DMETAL_FRAMEWORK_PATH`) | Rosetta | Direct3D 10/11/12 to Metal translator. The system path `/System/Library/Frameworks/D3DMetal.framework` does not exist on this machine |
| 12 | `libmetalirconverter`, `libdxcompiler`, `libdxilconv`, `libdxccontainer` | `.../D3DMetal.framework/Versions/A/Resources/` | x86_64 Mach-O dylib (30.5 MB; 29 MB; 2.1 MB; 0.1 MB) | D3DMetal | Rosetta | DXIL to Metal IR conversion |
| 13 | D3DMetal PE wrappers | `lib/wine/x86_64-windows/d3d11.dll` (139264 B), `dxgi.dll` (122880), `d3d12.dll` (196608), `nvapi64.dll` (188416), `nvngx-on-metalfx.dll` (135632) | PE32+ x86-64; MEASURED imports: `NtQueryVirtualMemory` (and `GetProcAddress`, `LoadLibraryA`, `VirtualProtect`) | the game | Rosetta | call `NtQueryVirtualMemory(MemoryWineUnixFuncs)` then `__wine_unix_call` (ntdll export, `ntdll.spec:1773`, "CW HACK 22435", `W/dlls/ntdll/loader.c:4091-4098`) |
| 14 | `wow64.dll`, `wow64cpu.dll`, `wow64win.dll` | `lib/wine/x86_64-windows/` | PE32+ x86-64 | `init_wow64` (`W/dlls/ntdll/loader.c:5016-5050`) | Rosetta | new WoW64: a 64-bit process runs 32-bit code by CPU mode switch; `wow64cpu/cpu.c:55-70` contains a Rosetta workaround ("rosetta2_workaround") |
| 15 | Steam bridge | `lib/wine/x86_64-unix/lsteamclient.so` (2028928 B), `lsteamclient.dll`, `steam.exe`, `steam/steamclient.dll` (PE32 i386), `steam/steamclient64.dll` (PE32+ x86-64) | x86_64 Mach-O and PE | Obelisk / game | Rosetta | Proton bridge to the macOS Steam engine; the exports of Valve's `steamclient*.dll` are rewritten as `mov rax,imm64 ; jmp rax` (`loader.c:1139-1253`) |
| 16 | Third-party libraries | `lib/wine/x86_64-unix/libfreetype.6.dylib`, `libgnutls.30.dylib` | x86_64 Mach-O | `dlopen` by their leaf name (`V/scripts/build.sh:216-219,265-267`) | Rosetta | fonts; Windows TLS |
| 17 | Other Unix libraries | `avicap32, bcrypt, crypt32, ctapi32, dnsapi, dwrite, kerberos, localspl, mountmgr, msv1_0, netapi32, nsiproxy, odbc32, opencl, opengl32 (1193520 o), qcap, secur32, winebth, winebus, winecoreaudio, winedmo, wineps, winevulkan (645520 o), winscard, winspool, wpcap, ws2_32` | x86_64 Mach-O | their PE DLLs | Rosetta | Wine's Unix services. `V/scripts/build.sh:33-34` compiles `--without-vulkan`; the runtime does not contain MoltenVK |

Findings.
- A single execution model: everything is x86_64 and the PE code is run by Rosetta's translation in the same process as the Unix libraries. There is no architecture boundary between PE and Unix (MEASURED: everything is x86_64). This is what makes the `signal_x86_64.c` dispatcher possible.
- `cxcompatdb.so` is looked up at startup (`loader.c:2381-2394`) but is not shipped: warning only.
- The 766 PEs of `x86_64-windows` all carry the "Wine builtin DLL" signature; no PE machine `0xaa64` (ARM64) or `0xa641` (ARM64EC) exists. `wow64cpu.dll` is present but `xtajit.dll` and `xtajit64.dll` are not.
- `RT` and `CUR` have the same `libd3dshared.dylib` and `D3DMetal` (identical hashes). The Wine files differ (`CUR` is not stripped).

### 1.2 Processes and boundaries

- Obelisk (arm64) starts `bin/wineserver` and `bin/wine` with `WINEPREFIX`, `WINEDLLOVERRIDES`, `WINEDEBUG`, `WINEMSYNC=1`, `WINE_LARGE_ADDRESS_AWARE=1`, and, if `lib/external/libd3dshared.dylib` exists, `CX_APPLEGPTK_LIBD3DSHARED_PATH` and `CX_ACTIVE_GRAPHICS_BACKEND=d3dmetal` (`wine.rs:115-142`).
- Each Windows process is a separate Rosetta process, created by exec of the loader with `WINEPRELOADRESERVE=start-end` (the PE image range) and `WINESERVERSOCKET` (`loader.c:709-729`).
- wineserver and the Windows processes exchange through Unix sockets, through shared memory (USER_SHARED_DATA section mapped from a descriptor, `virtual.c:4400-4421`) and through Mach: each process sends its task port to the server registered in the bootstrap under the name `server_dir` (`W/dlls/ntdll/unix/server.c:1473-1519`, `W/server/mach.c:96-170`); msync replaces events and semaphores with Mach semaphores (`WINEMSYNC=1`, `W/dlls/ntdll/unix/msync.c`).
- The main thread of the process is reserved for AppKit: `apple_main_thread` (`loader.c:2556-2588`) parks it in a CFRunLoop loop and `start_main_thread` runs in a second pthread (`loader.c:2522-2545`).

## 2. Entry of an x86-64 PE thread into the Unix side of Wine macOS

### 2.1 Generation of the syscall stubs

- `W/tools/winebuild` does not generate the stubs in this version. `parser.c:72,259-305,495-502` only parses the `syscall` flag and `syscall=N` (range 0-0x3fff), `import.c:1040,1146` and `utils.c:675` ignore these entries in the import thunks. The file `W/dlls/ntdll/unix/syscall.c` cited by the brief does not exist; `W/dlls/win32u/syscall.c` exists and registers table 1.
- The body of the stubs comes from the `__ASM_SYSCALL_FUNC` macro (`W/include/wine/asm.h:225-301`), applied to the `ALL_SYSCALLS` list generated by `tools/make_specfiles` (`W/dlls/ntdll/ntsyscalls.h`, `W/dlls/win32u/win32syscalls.h`) in `W/dlls/ntdll/signal_x86_64.c:43-48` (PE side; the same file compiled for `arm64ec_x64` uses the `asm.h:268-280` variant).
- x86_64 form (`asm.h:281-301`): `mov r10,rcx ; mov eax,id ; test byte [0x7ffe0308],1 ; jne +3 ; syscall ; ret ; jmp +1 ; ret ; call [0x7ffe1000] ; ret`. MEASURED in `RT/lib/wine/x86_64-windows/ntdll.dll` (export `NtClose`, RVA 0x55930): `4c 8b d1 b8 0f 00 00 00 f6 04 25 08 03 fe 7f 01 75 03 0f 05 c3 eb 01 c3 ff 14 25 00 10 fe 7f c3`; `NtAllocateVirtualMemory`: identifier 0x18, same bytes; `win32u.dll`: `NtUserGetForegroundWindow` 0x13fa and `NtGdiBitBlt` 0x1097 (table 1).
- Numbering: bits 0-11 number, bits 12-13 table (0 ntdll, 1 win32u, 2 and 3 free). The dispatcher reads `(id >> 12) & 3` (`signal_x86_64.c:3338-3341`) in `KeServiceDescriptorTable[4]` (`loader.c:158-162`). `KeAddSystemServiceTable(funcs, counters, limit, args, index)` (`loader.c:787-799`) accepts indexes 0 to 3; only 1 is used (win32u).

### 2.2 Normal path: `callq *0x7ffe1000`

- `virtual_init_user_shared_data` writes `data->SystemCall = 1` (`virtual.c:4457`) into the shared USER_SHARED_DATA section. The stub's `testb $1,0x7ffe0308` test is therefore true for all of Wine's code, and execution goes through `jmp +1 ; call [0x7ffe1000]`.
- `signal_init_process` (`signal_x86_64.c:3007-3010`) maps a page at `user_shared_data + page_size` (0x7ffe1000) and writes `__wine_syscall_dispatcher` there. `NtSetInformationProcess(ProcessInstrumentationCallback)` swaps this pointer between `__wine_syscall_dispatcher` and `__wine_syscall_dispatcher_instrumentation` (`signal_x86_64.c:542-553`).
- The `__wine_syscall_dispatcher` pointer of the PE `ntdll.dll` is a variable (`W/dlls/ntdll/loader.c:80`, `ntdll.spec:1752`) filled in by the Unix library (`loader.c:2043`), like `__wine_unix_call_dispatcher` (`loader.c:2036,2051`) and `__wine_unixlib_handle` (`loader.c:2044`). These three names are the only binary links between the PE half and the Unix half of ntdll.

### 2.3 The dispatcher (`signal_x86_64.c:3226-3523`)

Sequence read, confirmed by the disassembly of the installed `ntdll.so` (`otool -tvV`, identical start: `movq %gs:0x30,%rcx ; movq 0x378(%rcx),%rcx` at `___wine_syscall_dispatcher` = +0x348d0):
1. `rcx` ← TEB (`gs:0x30`) then `rcx` ← `thread_data->syscall_frame` (TEB+0x378; on Linux it is `gs:0x378` directly, `signal_x86_64.c:3227-3234`).
2. Pops the return address into `frame->rip`, saves rflags, rbx, rdx, rsi, rdi, r12-r15, cs, rsp, ss, rbp, `syscall_id` (eax); `subq $0xb,frame->rip` (documented workaround for Legends of Runeterra, lines 3266-3268).
3. Saves the FPU/AVX state with `xsavec64`, `xsave64` or `fxsave64` depending on `0x7ffe0285`, `0x7ffe03d8`, `0x7ffe03ec` (lines 3269-3292).
4. Switches to the kernel stack (`movq %rcx,%rsp`, the kernel stack starts just below the frame), `ldmxcsr` of the Unix MXCSR (`TEB+0x33c`), computes the table `(id >> 8) & 0x30`, checks the `ServiceLimit` bound (otherwise `bad_syscall` returns `STATUS_INVALID_SYSTEM_SERVICE` 0xc000001c, lines 3519-3522), copies the stack arguments beyond 4 registers, loads `r10, rdx, r8, r9` into `rdi, rsi, rdx, rcx` and calls the implementation (SysV convention).
5. Return (`__wine_syscall_dispatcher_return`, lines 3369-3487): restores FPU according to `restore_flags`, then returns by `ret` (fast path), `iretq` (CONTEXT_CONTROL or INTEGER) or instrumentation callback.
- The `struct syscall_frame` frame (`signal_x86_64.c:463-492`, size 0x300, XSAVE at 0xc0) and `struct amd64_thread_data` (`signal_x86_64.c:498-518`, in `TEB.GdiTebBatch` from 0x2f0: dr0-dr7, `pthread_teb` 0x320, `frame_size` 0x328, `instrumentation_callback` 0x330, `fs` 0x338, `mxcsr` 0x33c) plus `struct ntdll_thread_data` (`unix_private.h:102-119`: `syscall_table` 0x370, `syscall_frame` 0x378, `syscall_trace` 0x380) form the internal ABI of the TEB. The offsets are fixed by `C_ASSERT`.

### 2.4 unixlib calls: `__wine_unix_call`

- A builtin PE DLL with a Unix library gets the function table of its `.so` through `NtQueryVirtualMemory(module, MemoryWineUnixFuncs)` (`virtual.c:6200-6212`), which does `dlopen(unix_path)` then `dlsym("__wine_unix_call_funcs")` (`virtual.c:837-860`). It then calls `__wine_unix_call_dispatcher(handle, code, args)` (`W/include/wine/unixlib.h:280-300`).
- `__wine_unix_call_dispatcher` (`signal_x86_64.c:3552-3654`) is a lightweight variant of the dispatcher: it saves the incomplete frame (`restore_flags = RESTORE_FLAGS_INCOMPLETE_FRAME_CONTEXT`), switches to the kernel stack and does `callq *(%r10,%rdx,8)` on the table. This path goes through neither a `syscall` instruction nor a signal. It is the one used by winemac, D3DMetal (via `ntdll.__wine_unix_call`) and lsteamclient.
- The internal ntdll table (`loader.c:1492-1506`) contains `load_so_dll`, `unwind_builtin_dll`, `wine_dbg_write`, `wine_server_call`, `wine_server_fd_to_handle`, `wine_server_handle_to_fd`, `wine_spawnvp`, `system_time_precise`, `steamclient_setup_trampolines` and, on x86_64 only, `pe_module_loaded` (which registers the PE code regions with `libd3dshared`, "CW HACK 22434", `loader.c:1419-1489`).

### 2.5 Unix return to PE

`load_ntdll_functions` (`loader.c:2011-2053`) resolves in the PE `ntdll.dll` `KiUserExceptionDispatcher`, `KiUserApcDispatcher`, `KiUserCallbackDispatcher`, `LdrInitializeThunk`, `RtlUserThreadStart`, `__wine_ctrl_routine`, `DbgUiRemoteBreakin`, `KiRaiseUserExceptionDispatcher` and, in ARM64EC, `KiUserEmulationDispatcher` (`loader.c:105-116`). Exceptions are delivered by building `exc_stack_layout` (`signal_x86_64.c:419-430`) on the user stack. D3DMetal calls `KeUserModeCallback` (exported by `libd3dshared`) to call back into PE code.

### 2.6 TEB, FS/GS and TLS on macOS

- READ (`init_syscall_frame`, `signal_x86_64.c:3099-3107`): on macOS there is no `arch_prctl`. Wine writes `teb->Tib.Self` at `gs:0x30`, `teb->ThreadLocalStoragePointer` at `gs:0x58` and `teb->Peb` at `gs:0x60`, that is into the pthread's TSD array, keeps the GS base of this pthread in `thread_data->pthread_teb` (`mac_thread_gsbase`, lines 2975-2988, through `thread_info(THREAD_IDENTIFIER_INFO)`) and copies it into `teb->Instrumentation[0]`. PE code therefore reads `gs:[0x30]`, `gs:[0x58]` and `gs:[0x60]` correctly; the other TEB offsets read directly through `gs:` (for example `gs:[0x48]` TID, `gs:[0x68]` LastError, `gs:[0x08]` and `gs:[0x10]` stack limits) land on other slots of the libpthread TSD. INFERRED: this is a known risk for programs that read the TEB without going through `gs:[0x30]`.
- READ (`loader.c:2375-2379`, `2316-2348`): the `gs:0x60` slot is used by libSystem's `localtime()`. `start_main_thread` rewrites the start of `localtime` into a `jmp *` to an address stored 8 bytes further (`my_localtime`) after an RWX `mprotect` of the libSystem text. `pthread_exit_wrapper` sets `gs:0x60` to NULL before the thread exits so that macOS does not free the PEB (`W/dlls/ntdll/unix/thread.c:1113-1124`).
- READ (`ANNOUNCE.md:63-64`, upstream text): "On macOS, the `%gs` register is swapped in the syscall dispatcher". MEASURED: the dispatcher of the installed `ntdll.so` contains no GS swap. INFERRED: CrossOver 26.3.0 keeps the TSD scheme, probably because a GS base swap costs one machdep system call per transition under Rosetta; not checked.
- Rosetta provides neither FS nor FSGSBASE to the guest (MEASURED, appendix A.4): `%fs:0` raises SIGSEGV at address 0, `wrfsbase`, `rdfsbase` and `wrgsbase` raise SIGILL, `CPUID` leaf 7 EBX bit 0 = 0. The TLS of a glibc ELF (based on `%fs`) therefore cannot run under Rosetta; only `%gs` (TSD) is usable. For 32-bit code, Wine uses the LDT (`i386_set_ldt`, `signal_x86_64.c:2898-2899`, `3019-3020`, `2944-2958`).
- FEX side (READ, `FEX/Source/Windows/ARM64EC/Module.cpp:361-367`): `State.gs_cached = NtCurrentTeb()` and `fs_cached = 0`. FEX carries the segment bases in its guest state, so `gs:[x]` reaches the real TEB and `%fs` can be any base (useful for ELF TLS).

### 2.7 Signals and Mach exceptions

- READ (`signal_x86_64.c:3043-3066`): `sigaction` with `SA_SIGINFO|SA_RESTART|SA_ONSTACK` and `sa_mask = server_block_set` for SIGINT, SIGFPE, SIGABRT, SIGQUIT, SIGUSR1 (suspension), SIGTRAP, SIGSEGV, SIGILL, SIGBUS and, on Apple only, SIGSYS. The x86 exception number comes from `__es.__trapno` in the context (`TRAP_sig`, line 327).
- No Mach exception port: `grep` for `task_set_exception_ports`, `thread_set_exception_ports` and `EXC_BAD_ACCESS` in `W/dlls/ntdll/unix/` finds nothing; `mach_port_allocate` only appears in `msync.c` (semaphores) and the Mach code of `server.c` is used to send the task port to the server. Faults are therefore handled through BSD signals, which Rosetta delivers to the guest.
- Hard-coded Rosetta specifics: `is_rosetta2` read through `sysctl.proc_translated` (`signal_x86_64.c:3026-3041`); incorrect MXCSR in signal contexts (CW Hack 24256, `signal_x86_64.c:1003-1008`, `2856-2875`); write faults reported as reads (`virtual.c:4556-4562`); faults on W|X and X pages (CW Hacks 24945 and 25719, `virtual.c:4564-4582`); rewriting of another process's code through `NtWriteVirtualMemory` not seen by Rosetta, hence a toggle of the executable bit (CW HACK 18947, `virtual.c:6836-6900`); emulation of `xgetbv` and of the CET NOPs (`signal_x86_64.c:1956-2035`, `2534-2539`); wineserver returns zeroed debug registers for a translated process (`W/server/mach.c:192-197`).
- Existing fake successes in Wine macOS: `NtSetContextThread` (`signal_x86_64.c:1115`, block at lines 1149-1154) and `set_thread_wow64_context` (line 1372, block at lines 1395-1400) turn a `STATUS_UNSUCCESSFUL` from the server into `STATUS_SUCCESS` when debug registers are requested (CW HACK 22131, message "Setting debug registers is not supported under Rosetta, faking success", present in the strings of the installed `ntdll.so`). This is not a modification of EAC, but it is a fabricated success answer to an API that anti-cheats use; to reread before any test with EAC (section 6).

### 2.8 Raw `syscall` with a Linux number: who decides

MEASURED (appendix A.1, program `rawsys`, x86_64 binary run under Rosetta, `proc_translated = 1`, one case per process, with handlers for SIGSYS, SIGILL, SIGSEGV, SIGBUS, SIGTRAP):

| Case | `rax` | Result |
|---|---|---|
| 0 | `0x2000014` (BSD class, getpid) | return = real pid, no signal |
| 1 | 39 (Linux getpid, class 0) | SIGSYS (12), `si_code = 0`, `si_addr = rip`, `rip` points just after the `0f 05` bytes |
| 2 | 0x18 (NT identifier, class 0) | SIGSYS, same form |
| 3 | 1 (Linux write, class 0) | SIGSYS, nothing is written |
| 4 | `0x4000001` (invalid class 4) | SIGSYS |
| 5 | `0x3000003` (machdep class 3, `thread_fast_set_cthread_self`) | TIMEOUT of the probe process (stopped by the driver after 8 s). INFERRED, not checked: machdep class 3 is `thread_fast_set_cthread_self`, which sets the GS base, here to 1, and the signal handler can no longer read the TSD |
| 6 | `0x1000014` (Mach class) | return `0x10000003`, no signal |
| 7 | 0x1018 (table 1 identifier, class 0) | SIGSYS |

Reading. Darwin or Rosetta decides, not Wine: the class of the number (bits 24-31) chooses the subsystem, and class 0 gives SIGSYS on macOS 27.0.1 (MEASURED). Wine's comment says "Only called on macOS 14 Sonoma and later" (`signal_x86_64.c:2831-2833`); the behavior of the intermediate versions has not been checked. The `rcx` and `r11` registers do not contain the return address or rflags in the delivered context (unrelated values in case 1): `sigsys_handler` rebuilds them itself (`frame->rcx = RIP`, `R11 = eflags`, lines 2842-2848).

What Wine does (READ, `signal_x86_64.c:2834-2876`): `frame->rip = RIP + 0xb` (the `ret` instruction at the end of the standard stub), `frame->eflags`, `RCX = frame`, `RIP = __wine_syscall_dispatcher_prolog_end_ptr`; the handler runs nothing itself, it sends the thread back to the dispatcher, which works outside the signal context, on the kernel stack. An NT identifier outside the table (`bad_syscall`) gives `STATUS_INVALID_SYSTEM_SERVICE`. There is no Syscall User Dispatch: `grep` for `SYSCALL_USER_DISPATCH`, `PR_SET_SYSCALL`, `SYS_USER_DISPATCH` in `W/dlls/ntdll`, `W/server`, `W/include`, `W/configure.ac` finds only the three SIGSYS lines above. Under Linux, this Wine therefore does not trap a raw `syscall` (only Wine's stubs go through `0x7ffe1000`).

Real case of the EAC bootstrapper. The local diagnosis (`docs/research/rocket-league-macos.md:47-61`) shows that the unmodified Windows bootstrapper, when it detects Wine, takes the `linux64` branch. Its PE image (base `0x140000000`) prepares Linux number 319 (`memfd_create`, address `0x14001cc82`), then 1 (`write`), builds `/proc/self/fd/%i`, executes the `syscall` instruction of a call bridge (`0x14004b9ff`), looks for `libc.so` in `/proc/%i/maps` (`0x14001c850`), validates an x86-64 ELF of shared library type and resolves `__libc_dlopen_mode` or `dlopen` then `dlsym` (`0x14001c470`) to load the Linux module into the same process. INFERRED from reading Wine's code (not executed): in unmodified Wine macOS, `syscall` 319 gives SIGSYS then the dispatcher, which compares 319 to the limit of table 0 (259 services, identifiers 0x000 to 0x102, `ARRAY_SIZE(syscalls)` of `loader.c:158-162`) and returns `STATUS_INVALID_SYSTEM_SERVICE` (`0xc000001c`) in `rax`; `syscall` 1 (`write`) would execute NT entry 0x0001, `NtWorkerFactoryWorkerReady`, which is a stub (`ntsyscalls.h:539`) and raises `EXCEPTION_WINE_STUB` (`loader.c:118-142`). In both cases the answer is not that of a Linux kernel. The crossing from PE to Linux semantics is therefore EAC's first obstacle and not an edge case: the caller is PE code, not only ELF.

### 2.9 Where to insert Linux semantics in a design that keeps Wine macOS

INFERRED, from the facts above.
1. Entry point: `sigsys_handler` (`signal_x86_64.c:2834`). It already receives every raw `syscall`, whether it comes from an ELF or, as in the EAC case, from PE code. The "NT or Linux" decision has to be made there, and the numbers overlap (NT table 0: 0x000-0x102; Linux x86-64: 0 to about 450), so the discriminant cannot be the number. Candidates: (a) the caller's address (`RIP_sig`) against a list of ranges registered by a unixlib call, a `NtSetInformationProcess` class or an environment variable; the list must be able to designate ELF ranges and also a specific PE function or image (the bootstrapper's `0x14004b9ff` bridge), without modifying the binary; (b) a per-thread personality flag, which requires entry and exit points that EAC's code does not provide; (c) the combination of (a) and (b). Choice (a) is deterministic and testable.
2. Execution: do not work inside the signal. Reuse Wine's pattern: the handler redirects `RIP` to a second assembly entry point (copy of the `__wine_syscall_dispatcher` prologue) that saves the frame, converts the Linux arguments (`rdi, rsi, rdx, r10, r8, r9`) to the internal convention (the NT dispatcher expects the first argument in `r10`) and calls a table of Linux services on the kernel stack. Less invasive variant: register a service table at index 2 with `KeAddSystemServiceTable` (`loader.c:787-799`) and rewrite `eax = 0x2000 | nr`; the argument conversion still has to be done.
3. Memory: the ELF segments must be views known to `virtual.c` (allocation through `NtAllocateVirtualMemory` or registration) so that `virtual_handle_fault` and the `pages_vprot` tracking stay consistent.
4. TLS: a glibc ELF needs `%fs`. Impossible under Rosetta (MEASURED); possible under FEX (virtual segment bases). A witness under Rosetta must therefore be a static ELF without glibc, or use `%gs`.
5. Linux signals and `clone`/`futex`/`mmap`: to translate to pthread, `os_sync_wait_on_address`, `mach_vm_*`; out of scope for the first witness.
6. NT `syscall`s from third-party code (anti-cheat, Chromium, protections) arrive through the same SIGSYS: the address test must let the other PE ranges through to the unchanged NT path.
7. FEX variant: the equivalent insertion point is `ECSyscallHandler::HandleSyscall` (`FEX/Source/Windows/ARM64EC/Module.cpp:520-536`), which receives the `CpuStateFrame` (so `rax` and the guest RIP) before raising `STATUS_EMULATION_SYSCALL`. It can route a Linux `syscall` to a macOS handler without a signal and without going through Wine's dispatcher.
8. The bootstrapper looks for an ELF libc in `/proc/%i/maps` and calls its `dlopen`: a synthetic `/proc/self/maps` is therefore needed that exposes an ELF image providing `dlopen`, `dlsym` or `__libc_dlopen_mode`, as well as a `memfd` and `/proc/self/fd/N`. This is not covered here; see `docs/research/rocket-league-eac-technologies-macos-2026-10-01.md`, section "Why a Linux graft into Wine Darwin would be a big port".

### 2.10 What, in `loader.c` and `signal_x86_64.c`, conflicts with FEXCore in the same process

Preliminary remark (INFERRED): a Rosetta process cannot run the output of an arm64 JIT. FEXCore in the Wine process therefore implies a Wine whose Unix half is arm64 (arm64 Mach-O), and all of `signal_x86_64.c` becomes unusable as is; it serves as the specification of what an equivalent must do. The conflicts are:
1. Ownership of signals. Wine installs SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS, SIGUSR1, SIGINT, SIGQUIT, SIGABRT for the whole process (`signal_x86_64.c:3043-3066`). FEXCore needs SIGSEGV/SIGBUS (self-modifying code detection, guest faults) and SIGILL. One handler per signal: chaining is required. The ARM64EC design avoids the conflict: FEX runs as a PE DLL, its faults arrive through `KiUserExceptionDispatcher` and `pResetToConsistentState` (`signal_arm64ec.c:56,1280-1290`).
2. TEB register. `include/winnt.h:2499` reads the TEB in `x18` and all of `signal_arm64.c` assumes `x18` (lines 852-877, 1548-1549, 1599, 1639, 1704-1752). MEASURED: macOS arm64 sets `x18` to zero (section 3.4).
3. Rewriting of libSystem: `hook(localtime, my_localtime)` (`loader.c:2316-2348,2375-2379`) is x86-64 code (bytes `ff 25`).
4. x86-64 assembly without an equivalent: dispatcher, `__restore_mxcsr_thunk`, `GPT_ABI_WRAPPER` (`unix_private.h:601-638`, `msthunk_NtClose`, `msthunk_NtCreateEvent`, `msthunk_NtCreateThreadEx`, etc.), `pe_module_loaded` (`loader.c:1503-1505`), x86 debug states (`x86_debug_state_t`) of `server/mach.c` (`#if defined(__i386__) || defined(__x86_64__)`, lines 179-262).
5. Reservations through zerofill segments (`loader/main.c:52-95`): an arm64 executable whose `__PAGEZERO` is smaller than 4 GiB is killed (MEASURED). The current loader, which is re-executed at each Windows process (`loader.c:709-729`), cannot stay as is.
6. `apple_main_thread` (`loader.c:2556-2588`) parks the main thread in CFRunLoop: compatible with FEX, but the Wine thread is a second pthread (the main thread's stack does not carry the guest code).
7. Page constants: `page_size = 0x1000` (`unix_private.h:152`), `host_page_size` constant 0x1000 outside aarch64 (`virtual.c:177-183`). An aarch64 build makes them dynamic (`virtual.c:3658-3662`) but several functions refuse partial frees not aligned on the host page (`virtual.c:2550-2555,2594-2607`).
8. Hardware debugging: wineserver returns zeros for the debug registers of translated processes; under FEX there would be no implementation.

## 3. Address space reservations and pages

### 3.1 Linking and reservations of Wine macOS

- `W/configure.ac:948`: `WINELOADER_LDFLAGS="-Wl,-segalign,0x1000,-pagezero_size,0x1000,-sectcreate,__TEXT,__info_plist,loader/wine_info.plist"`.
- `configure.ac:954-957`: if the linker knows `-Wl,-no_huge` (Xcode 15.3 and later), no preloader and `-Wl,-no_pie,-image_base,0x200000000,-no_huge,-no_fixup_chains,-segalign,0x1000,-segaddr,WINE_RESERVE,0x1000,-segaddr,WINE_TOP_DOWN,0x7ff000000000`. `configure.ac:962-977` describes the preloader fallback (not used here).
- `loader/main.c:59-72`: two zerofill sections `WINE_RESERVE` (0x1fffff000 bytes: from 0x1000 to 0x200000000, "low 8GB") and `WINE_TOP_DOWN` (0x1ff0000 bytes at 0x7ff000000000: top-down allocations and virtual heap). `init_reserved_areas` (lines 74-84) remaps them as `PROT_NONE|MAP_NORESERVE|MAP_FIXED`. The `wine_main_preload_info` table is read by `virtual_init` (`virtual.c:3647-3677`).
- MEASURED (`otool -l RT/lib/wine/x86_64-unix/wine`): `__PAGEZERO` 0x0 size 0x1000; `__TEXT` 0x200000000 size 0x2000; `__DATA` 0x200002000; `WINE_RESERVE` 0x1000 size 0x1fffff000; `WINE_TOP_DOWN` 0x7ff000000000 size 0x1ff0000. The `bin/wine` launcher, for its part, has a 4 GiB `__PAGEZERO` and none of these segments: this is why `__wine_main` re-executes the loader (`loader.c:2658-2683`).
- Other reservations (READ, `virtual.c:749-757`, only without preload information): 0x10000-0x68000000, 0x7f000000-0x7fff0000, 0x7ffffe000000-0x7fffffff0000. On macOS the `reserve_area` variant (`virtual.c:630-701`) scans the map with `mach_vm_region` and reserves the holes with `mach_vm_map(VM_FLAGS_FIXED, PROT_NONE)`. `anon_mmap_tryfixed` uses `mach_vm_map` for lack of `MAP_FIXED_NOREPLACE` (`virtual.c:602-616`).
- Upper limit: `get_host_addr_space_limit` (`virtual.c:2762-2785`) probes with `mmap` at 2^63 then halving. MEASURED (appendix A.3): same answer in native arm64 and under Rosetta, first power of two accepted 0x400000000000, hence a limit of about 0x7fffffff0000; fixed `mach_vm_allocate` succeeds up to 2^46 and fails at 2^47; `MACH_VM_MAX_ADDRESS` = 0x7ffffe000000 (arm64) and 0x7fffffe00000 (x86_64).

### 3.2 0x7ffe0000 area and dispatcher page

- `user_shared_data = 0x7ffe0000` (`virtual.c:206`); mapped from a section descriptor provided by the server (`virtual.c:4400-4421`, `MAP_SHARED|MAP_FIXED` of `page_size` bytes); initialized once (`virtual.c:4429-4473`) with `SystemCall = 1`, `NativeProcessorArchitecture` according to `native_machine`. The syscall stub reads `0x7ffe0308` hard-coded, the dispatcher reads `0x7ffe0285`, `0x7ffe03d8`, `0x7ffe03dc`, `0x7ffe03ec` (and `0x7ffe028a` in the `#ifdef __linux__` blocks only).
- The 0x7ffe1000 page contains the dispatcher pointer (section 2.2).
- MEASURED: under Rosetta with `__PAGEZERO` 0x1000, fixed `mach_vm_allocate` succeeds at 0x400000, 0x10000000, 0x7ffe0000 (1 and 4 pages) and 0xfffff000; in native arm64 the same addresses give `KERN_INVALID_ADDRESS`.

### 3.3 Page size in `virtual.c`

- READ: `page_shift = 12`, `page_mask = 0xfff`, `granularity_mask = 0xffff` (`virtual.c:173-175`); `page_size = 0x1000` constant (`unix_private.h:152`). `host_page_size` is the constant 0x1000, except `#ifdef __aarch64__` where it is `sysconf(_SC_PAGESIZE)` read at `virtual_init` (`virtual.c:177-183,3658-3662`).
- Voile compiles x86_64: `host_page_size` = 0x1000. Under Rosetta this holds end to end. MEASURED (appendix A.3, x86_64 program): `getpagesize()` = 4096, `sysconf(_SC_PAGESIZE)` = 4096, `vm_page_size` = 4096 but `vm_kernel_page_size` = 16384; two consecutive 4 KiB `mmap`s are 4096 apart; `mprotect(PROT_NONE)` on a 4 KiB page in the middle of a 64 KiB mapping succeeds. Rosetta therefore provides a real 4 KiB granularity to the guest, which Wine assumes everywhere.
- Simulation of 4 KiB on a 16 KiB host (aarch64 build, announced in `ANNOUNCE.md:78-82`: "works for simple applications ... Using a 4K-page kernel is strongly recommended"): a `pages_vprot` array keeps the protection per 4 KiB page; the host protection of a 16 KiB page is the OR of the protections of the sub-pages (`get_host_page_vprot`, `virtual.c:1095-1113`); stack guards, write tracking, views and `mprotect`s are rounded to `host_page_size` (`virtual.c:4518-4528,4591-4610`); the write tracking granularity becomes `host_page_size / page_size` (`virtual.c:367`). Explicit limits: a partial free that is not aligned on the host page fails with `STATUS_CONFLICTING_ADDRESSES` and an `ERR "unaligned partial free"` (`virtual.c:2550-2555,2594-2607`); there is no splitting of mappings, so two 4 KiB pages with incompatible protections in the same host page get the most permissive protection.
- ARM64EC: a map of one bit per 4 KiB page (`alloc_arm64ec_map`, `virtual.c:2789-2805`, flag `VPROT_ARM64EC` line 144).

### 3.4 Native arm64 versus Rosetta measurements (this machine)

| Measurement | native arm64 | x86_64 under Rosetta |
|---|---|---|
| Page size seen (`getpagesize`) | 16384 | 4096 |
| Binary's `__PAGEZERO` reduced to 0x1000 | the linker rounds it to 0x4000 and the process is killed (SIGKILL) at exec | accepted |
| Addresses 0x1000, 0x10000, 0x400000, 0x10000000, 0x7ffe0000, 0xfffff000 with default `__PAGEZERO` (4 GiB) | `KERN_INVALID_ADDRESS` | `KERN_INVALID_ADDRESS` |
| Same addresses with `__PAGEZERO` 0x1000 | not executable (killed) | 0x400000, 0x10000000, 0x7ffe0000, 0xfffff000: success; 0x1000 and 0x10000: `KERN_NO_SPACE` (already occupied, by what not identified) |
| `x18` after a real `svc #0x80`, a Mach trap, a data fault with a handler, a `usleep` | reset to 0 every time; the signal context contains the old value but `sigreturn` does not restore it; 309 to 320 losses over 20 million reads per thread during `usleep(1)` calls | not applicable |
| File `x18probe2` | 5 cases out of 5: `x18` = 0 right on return from the kernel | |

The sibling report `reports/fexcore-darwin-port.md` (lines 45-60) gets the same results with other programs (low area refused, reduced `__PAGEZERO` = exit code 137, `x18` lost). Consequences (INFERRED) for an arm64 design:
- USER_SHARED_DATA at 0x7ffe0000, the DOS area, 32-bit PE images or images linked below 4 GiB and `MEM_TOP_DOWN` allocations below 4 GiB cannot be hosted 1:1 in the space of a macOS arm64 process. A guest address window with an offset base is needed in FEXCore. EXTERNAL: the `willfaust/FEX` fork (PR #2) adds a "guest window" with reserved registers `x19` (base) and `x24` (temporary) for 32-bit guests on iOS, for the same reason.
- `x18` can hold neither the Windows ARM64 TEB nor JIT state. EXTERNAL: Madeira (iOS) finds that `x18` is not the TEB and reads as 0, gets the TEB through `TPIDRRO_EL0` and a TSD slot, with a binary rewriting pass after linking (`virtual_ios.c`, PR #60).
- 16 KiB pages require Wine's 4 KiB simulation and tracking of self-modifying code at 16 KiB granularity (EXTERNAL: `willfaust/FEX` PR #1).

## 4. arm64, arm64ec, wow64 and emulator hooks in voile-wine

### 4.1 What exists in the tree (READ)

| Element | Location | Content |
|---|---|---|
| x64 emulator stub for ARM64EC | `W/dlls/xtajit64/cpu.c`, `xtajit64.spec`, `Makefile.in` | 20 exports (`xtajit64.spec`: 16 `stdcall` and 4 `extern`: `DispatchJump`, `RetToEntryThunk`, `ExitToX64`, `BeginSimulation`); all simulation entries print "x64 emulation not implemented" and terminate the process (`cpu.c:39-77`); `UpdateProcessorInformation` forces `PROCESSOR_ARCHITECTURE_AMD64` (`cpu.c:235-240`) |
| Activation | `W/configure.ac:2413-2423` | `enable_xtajit64=${enable_xtajit64:-arm64ec}` when `PE_ARCHS` is not empty; `arm64ec` adds `x86_64` as an extra architecture (`configure.ac:397-402`), compiler `arm64ec-w64-mingw32-clang` (`configure.ac:422-424`) |
| Emulator selection | `W/dlls/ntdll/loader.c:4958-4992` (`load_arm64ec_module`) | reads the default value of `HKLM\Software\Microsoft\Wow64\amd64` (default `xtajit64.dll`), loads the DLL then calls `arm64ec_process_init` (`signal_arm64ec.c:180-230`) |
| Registry values | `W/loader/wine.inf.in:400-402,467,497-498` | `Wow64\amd64 = xtajit64.dll`, `Wow64\x86 = xtajit.dll`, KnownDLLs `xtajit`, `_xtajit`, `xtajit64` |
| Interface resolved by ntdll | `W/dlls/ntdll/signal_arm64ec.c:42-59,186-209` | `ExitToX64`, `DispatchJump`, `RetToEntryThunk` (variables `__os_arm64x_*`, lines 186-188) and 17 function pointers (`BTCpu64FlushInstructionCache`, `BTCpu64IsProcessorFeaturePresent`, `BTCpu64NotifyMemoryDirty`, `BTCpu64NotifyReadFile`, `BeginSimulation`, `FlushInstructionCacheHeavy`, `NotifyMapViewOfSection`, `NotifyMemoryAlloc`, `NotifyMemoryFree`, `NotifyMemoryProtect`, `NotifyUnmapViewOfSection`, `ProcessInit`, `ProcessTerm`, `ResetToConsistentState`, `ThreadInit`, `ThreadTerm`, `UpdateProcessorInformation`, lines 190-209) |
| Syscalls from the emulator | `signal_arm64ec.c:1280-1290` | `STATUS_EMULATION_SYSCALL` → `dispatch_syscall(arm_ctx)`; ARM64EC `KiUserExceptionDispatcher` with patch detection (`KiUserExceptionDispatcher_orig`) |
| Unix callbacks to the emulator | `W/dlls/ntdll/unix/loader.c:110,2038-2050` | `pKiUserEmulationDispatcher`, `__wine_unix_call_dispatcher_arm64ec` swapped with `__wine_unix_call_dispatcher` when `is_arm64ec()` (`unix_private.h:95-99`); `include/wine/unixlib.h:287-291` |
| Memory | `W/dlls/ntdll/unix/virtual.c:144,2789-2805` | `VPROT_ARM64EC`, ARM64EC map of one bit per page |
| WoW64 on ARM64 | `W/dlls/wow64/syscall.c:767` | the 32-bit CPU DLL is `xtajit.dll` if `native_machine == ARM64`, otherwise `wow64cpu.dll` |
| WoW64 x86_64 | `W/dlls/wow64cpu/cpu.c` | `BTCpu*` interface (`BTCpuSimulate`, `BTCpuProcessInit`, `BTCpuGetBopCode`, `__wine_get_unix_opcode`, `BTCpuTurboThunkControl`) with Rosetta workaround |
| Version | `W/ANNOUNCE.md` (Wine 11.0) | complete new WoW64; 4 KiB simulation on ARM64; GS swap on macOS (upstream) |

No FEX, box64, Hangover or `libarm64ecfex` symbol exists in the tree (`grep -ri` on `dlls`, `include`, `programs`, `loader`, `server`, `tools`, `configure.ac`: only `xtajit` and the words "FEX" in unrelated tests).

### 4.2 What does not exist in the installed runtime (MEASURED)

`RT/lib/wine` only contains `i386-windows`, `x86_64-windows` and `x86_64-unix`. No `aarch64-windows`, no `aarch64-unix`, no `xtajit64.dll`. `V/scripts/build.sh:33` compiles `--enable-archs=i386,x86_64` with `CC="clang -arch x86_64"`; the PE toolchain is universal llvm-mingw 20260922 (`V/sources.json:6-9`), which can in principle target `arm64ec-w64-mingw32` (EXTERNAL, see 4.4).

### 4.3 Comparison of the interface with FEX (READ on both sides)

The 20 exports of `W/dlls/xtajit64/xtajit64.spec` and those of `FEX/Source/Windows/ARM64EC/libarm64ecfex.def` (also 20 names, `DispatchJump`, `RetToEntryThunk`, `ExitToX64` and `BeginSimulation` marked `DATA`) are the same names (`BTCpu64FlushInstructionCache`, `BTCpu64IsProcessorFeaturePresent`, `BTCpu64NotifyMemoryDirty`, `BTCpu64NotifyReadFile`, `DispatchJump`, `RetToEntryThunk`, `ExitToX64`, `BeginSimulation`, `FlushInstructionCacheHeavy`, `NotifyMapViewOfSection`, `NotifyMemoryAlloc`, `NotifyMemoryFree`, `NotifyMemoryProtect`, `NotifyUnmapViewOfSection`, `ProcessInit`, `ProcessTerm`, `ResetToConsistentState`, `ThreadInit`, `ThreadTerm`, `UpdateProcessorInformation`). FEX's ARM64EC module:
- handles `syscall` on the guest side by raising `STATUS_EMULATION_SYSCALL` through `KiUserExceptionDispatcher` (`FEX/Source/Windows/ARM64EC/Module.cpp:516-536`);
- calls Wine's dispatcher through `__wine_syscall_dispatcher` for the native variants of `NtContinue`, `NtAllocateVirtualMemory`, `NtProtectVirtualMemory`, `NtRaiseException` (`Module.cpp:85-95,225-285`, `Module.S:106-130`);
- gets its Unix services through `GetProcAddress(ntdll, "__wine_unix_call_dispatcher")` then `MemoryWineLoadUnixLibByName` or, as a fallback, `MemoryWineUnixFuncs` on its own image (`FEX/Source/Windows/Common/FEXUnixLib.cpp:44-87`). voile-wine only defines `MemoryWineUnixFuncs` and `MemoryWineUnixWow64Funcs` (`W/include/winternl.h:2439-2440`): the fallback applies, a companion Unix library is needed (name expected by convention: `libarm64ecfex.so`);
- asks this Unix library for Linux services (`prctl(PR_SET_MEM_MODEL)`, `PR_ARM64_SET_UNALIGN_ATOMIC`, `PR_SET_VMA`, `shm_open`, `madvise`, `FEX/Source/Windows/UnixLib/FEXUnixLib.cpp:29-110`) that must be replaced on macOS.

### 4.4 External sources (consulted on 2026-10-02)

- Hangover, https://github.com/AndreRH/hangover, HEAD `56e385b7ff490edd281b288f420677093563560b` (raw README read with `curl`). Quotes: "we break out of emulation at the win32 syscall or wine unix call level for performance reasons", "everything Unix related is never emulated", "For 64-Bit we're using the ARM64EC ABI combined with an emulator (FEX)", "Current main focus is to run x86_64 and i386 Windows applications on ARM64 Linux". Variables: `HODLL64` (`libarm64ecfex.dll` by default, or `xtajit64.dll`) and `HODLL` (`wowbox64.dll` by default, `libwow64fex.dll`, `wow64cpu.dll`). The README contains none of the words macOS, Darwin or Apple (grep). Build: `docs/COMPILE.md`.
- FEX wiki, https://wiki.fex-emu.com/index.php/Development:ARM64EC (page read with `curl`): "Unlike Linux FEX, the Windows FEX modules purely handle CPU emulation, with syscall translation being done on either the wine side (WOW64) or ABI (ARM64EC). This is similarly the case for wine's unixcalls and as a result native ARM64 variants can be used for all .so libraries (Vulkan, wine, etc.) dropping the need for an x86 rootfs and FEX-side thunking."; "To actually use FEX create an empty registry key at HKLM\Software\Microsoft\Wow64\amd64 and set its value to libarm64ecfex.dll"; "wow64 replacement with FEX on Windows doesn't work and causes all 32-bit applications to hang"; FEX can only depend on `ntdll` at load time. Wine configure command: `./configure --enable-archs=arm64ec,aarch64,i386 --with-mingw=clang` (fork `https://github.com/bylaws/wine`, branch `upstream-arm64ec`, HEAD `a6844d10622fc1a973ec1f22fc4f78a0fcd6cb29`); FEX built twice with `-DCMAKE_TOOLCHAIN_FILE=../Data/CMake/toolchain_mingw.cmake` and `-DMINGW_TRIPLE=arm64ec-w64-mingw32` (64-bit) or `aarch64-w64-mingw32` (WOW64), llvm-mingw from `bylaws/llvm-mingw`.
- Wine 10.0 announcement, https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/ANNOUNCE.md (read with `curl`, tag `wine-10.0`, peeled commit `b073859675060c9211fcbccfd90e4e87520dc2c2`), ARM64 section: "The ARM64EC architecture is fully supported, with feature parity with the ARM64 support."; "The 64-bit x86 emulation interface is implemented. This takes advantage of the ARM64EC support to run all of the Wine code as native, with only the application's x86-64 code requiring emulation."; "No emulation library is provided with Wine at this point, but an external library that exports the emulation interface can be used, by specifying its name in the `HKLM\Software\Microsoft\Wow64\amd64` registry key. The FEX emulator implements this interface when built as ARM64EC."; "ARM64 support requires the system page size to be 4K, since that is what the Windows ABI specifies. Running on kernels with 16K or 64K pages is not supported at this point." The Wine 11.0 announcement (tag `wine-11.0`, peeled commit `db11d0fe6a169c457e23d007e20404643d067aa8`, local copy `W/ANNOUNCE.md:78-82`) adds the 4 KiB simulation. The `gitlab.winehq.org` and `www.winehq.org` pages refused automated requests (Anubis, 403).
- Madeira (iOS: Wine 11.4 in ARM64EC, FEX, DXMT), https://github.com/willfaust/Madeira, HEAD `ca3183ea3dfb0fd706aff1bea2abb871b5d27aec`; forks `https://github.com/willfaust/wine` (HEAD `abf22e09603014b8f55bb79bd1cf6badf96c34ca`) and `https://github.com/willfaust/FEX` (HEAD `26859e184ad90f0e811d7f8bbd943a4b1573a2c3`). Pull request pages read through a summarizing tool: a single Mach process with wineserver as a thread, JIT under a debugger; FEX #1 (self-modifying code tracking on 16 KiB pages, double mapping of the JIT, ARM64EC transitions that went through Mach exceptions), FEX #2 (32-bit guest window with base register `x19`), Madeira #60 (binary rewriting of `x18` to `TPIDRRO_EL0` plus a TSD slot, in `virtual_ios.c`). EXTERNAL, not reproduced here. It is a proof of existence on iOS, not on macOS.

## 5. Crossing options between Windows, Linux and macOS

### 5.1 Table

Legend: P = x86-64 PE code (Windows), E = x86-64 ELF code (Linux), M = macOS code (Mach-O, frameworks). "Rosetta" = current design, "FEX" = future arm64 host.

| # | Crossing | Mechanism (evidence) | Status | Advantages | Drawbacks |
|---|---|---|---|---|---|
| F1 | P to M, NT service | NT stub, `call [0x7ffe1000]`, dispatcher (`asm.h:287-301`, `signal_x86_64.c:3226`) | exists (Rosetta) | no trap, cost of a call, complete NT semantics, extensible tables (index 2 and 3) | x86-64 assembly; out of reach of an arm64 host (replaced by `signal_arm64.c`, based on `x18`) |
| F2 | P to M, raw `syscall` from third-party code | SIGSYS then `sigsys_handler` (`signal_x86_64.c:2834`) | exists (Rosetta; measured on macOS 27.0.1, the source mentions macOS 14 and later) | catches direct syscalls from anti-cheats and runtimes | one signal per call, MXCSR hacks (CW 24256), disappears under FEX where `syscall` is a point of the JIT |
| F3 | P to M, unixlib table | `NtQueryVirtualMemory(MemoryWineUnixFuncs)` then `__wine_unix_call` (`virtual.c:837-860,6200`, `signal_x86_64.c:3552`) | exists | no trap, per DLL, used by winemac, D3DMetal, lsteamclient | requires a builtin PE DLL with a companion `.so`; kernel stack and incomplete frame at each call |
| F4 | M to P, callbacks | `KiUserCallbackDispatcher`, `KiUserExceptionDispatcher`, `KiUserApcDispatcher`, `KeUserModeCallback` (`loader.c:2026-2041`) | exists | exact Windows model | user stack manipulated; different conventions between x86-64 and arm64ec |
| F5 | P to P, export redirection | rewriting `mov rax,imm64 ; jmp rax` or execution trap (`loader.c:1139-1253`, `WINESTEAMNOEXEC`) | exists (steamclient) | trivial | RWX mprotect on code; fragile with Rosetta (CW 18947) and with a JIT's code tracking |
| F6 | P to E, direct function call | MS x64 to SysV thunk in the same address space; `GPT_ABI_WRAPPER` model (`unix_private.h:601-638`, test of the caller's address) | to write | cost of a call; the thunk model already exists | different ABIs (registers, stack alignment, `xmm6-15` non-volatile on the MS side); ELF TLS absent under Rosetta |
| F7 | P or E to M, Linux `syscall` | SIGSYS (Rosetta, MEASURED) or JIT decoding (FEX, `HandleSyscall`) to a table of Linux services (section 2.9). The real caller is the EAC PE bootstrapper (319, 1, `/proc`), then the ELF module | to write | Linux semantics without a VM; single insertion point | numbers overlap with NT: routing by caller address mandatory; `clone`, `futex`, `mmap`, `ptrace`, `/proc` to translate; each call costs a signal under Rosetta |
| F8 | E to P, callbacks | SysV to MS thunk, stack prepared as for `KiUserCallbackDispatcher` | to write | | same ABIs as F6; exceptions: stack unwinding mixes DWARF (ELF) and `.pdata` (PE) |
| F9 | P/E (emulated) to M (native arm64), syscall of a FEX guest | `STATUS_EMULATION_SYSCALL` through `KiUserExceptionDispatcher` (`signal_arm64ec.c:1282`, `FEX Module.cpp:516-536`) | exists on the Wine and FEX side, never run on macOS | no Unix signal, real TEB in `gs`, `%fs` free | requires arm64 Wine on macOS: `x18`, low area, 16 KiB pages, `server/mach.c` x86 only |
| F10 | FEX guest to native arm64 (direct call) | `__os_arm64x_dispatch_call_no_redirect`, `DispatchJump`, `ExitToX64` (`xtajit64.spec`) | exists (interface) | native calls without emulating the libraries (Metal, Cocoa) | per-function thunks; data shared between ABIs |
| F11 | Windows process to wineserver | Unix socket, USD shared memory, Mach ports and msync (`server.c:1473-1519`, `server/mach.c`, `msync.c`) | exists | isolates the NT state | `mach_vm_*` on another process; fake debug registers under Rosetta; to port for arm64 |
| F12 | bridge through a helper process | a separate Rosetta or FEX process hosts E (or D3DMetal) and talks through shared memory and sockets | to write | works around the architecture constraints (D3DMetal x86_64 only, low area, `%fs`) | latency, synchronization, duplication of address space; EAC expects a single process |
| F13 | faults through Mach exceptions instead of signals | `thread_set_exception_ports` | not used by Wine | fine granularity, avoids the interaction with FEX's signals | not provided by Wine; EXTERNAL: Madeira uses it for ARM64EC transitions |
| F14 | Hypervisor.framework engine per process (HyperLinux) | read-only use of the sources for the ABI contracts | excluded by the brief | | not chosen |

### 5.2 Cross-cutting finding

- The only crossing already complete and exercised in production is F1+F3+F4 under Rosetta. Any arm64 design must replace the core of F1/F2 with F9 and rebuild F11.
- Linux and NT numbers overlap: F2/F7 only coexist through routing by address. Wine's dispatcher already contains the needed structure (service table per index, syscall frame); what is missing is the range and the argument conversion.
- ELF TLS (`%fs`) is impossible under Rosetta and natural under FEX. The ELFs of the initial tests must be without libc or use no TLS.

### 5.3 The three most promising candidates for the first witness

Common output contract: the PE writes, in order, a `PE` line, an `ELF` line (written by the ELF with a Linux `write`), a `MAC` line (data obtained from the macOS system, checkable from the outside) and a final `PE` line. The external validator compares the `MAC` value with a macOS command (`ps`, `sysctl`) and requires the order of the lines.

**T1. A PE that issues Linux `syscall`s then loads a static ELF that also issues them, routed to macOS (Rosetta, current runtime plus a `ntdll.so` patch)**
- Program: `crossing.exe` (llvm-mingw, x86_64). Step 1: like the EAC bootstrapper, it itself issues Linux `syscall`s from a call bridge function: `getpid` (39), `write` (1), `memfd_create` (319) followed by a `write` of the module into the memfd. Step 2: it reads a static x86-64 `ET_EXEC` ELF without libc (clang `--target=x86_64-linux-gnu -ffreestanding -nostdlib -static`, linked at a high address, without TLS), maps it through `VirtualAlloc` and `VirtualProtect`, prepares a stack and jumps to the entry. The ELF runs `write(1, ...)`, `getpid`, `uname`, then returns to the PE. Both sources of Linux `syscall`s, PE and ELF, are covered.
- Wine patch: ranges registered (bridge function of the test PE and segments of the ELF) through an environment variable or a new unixlib call; `sigsys_handler` routes these `syscall`s to a Linux entry point and leaves all the others to the NT path (section 2.9). A second PE `control.exe`, which issues a direct NT `syscall` (`NtClose` with its own stub bytes), must keep going through the NT path: this is the non-regression test of the routing.
- Evidence: `PE` line (`NtCurrentTeb()->ClientId`, PE machine, Windows pid), `ELF` line (written by the ELF with a Linux `write`; its Linux `getpid` returns the real macOS pid, different from the Windows pid), `MAC` line (`uname`/`sysctl` value returned by the macOS kernel through the Linux table, compared from the outside with `ps -p <pid>`). The three layers are distinct by construction: Windows pid, Unix pid, kernel value.
- Advantages: everything exists except a patch of a few hundred lines; exercises exactly the measured SIGSYS trap and the EAC scenario (the PE itself issues Linux `syscall`s); serves as a non-regression test for direct NT `syscall`s from third-party code. Limits: Rosetta, not FEX; ELF without glibc; no synthetic `/proc/self/maps`; a read-only "macOS" part.

**T2. A builtin PE plus Unix Mach-O library pair compiled out of tree with the Voile recipe**
- Program: `crossing.dll` (builtin PE) and `crossing.so` (x86_64 Mach-O) built like `lsteamclient` (`V/scripts/build.sh:297-324`: Wine Makefile reused with `UNIX_LIBS` and makedep); `crossing.exe` calls `WINE_UNIX_CALL`. On the Unix side, the function calls `sysctlbyname("machdep.cpu.brand_string")`, `sysctl.proc_translated`, `proc_pidpath`, `mach_absolute_time`, then (step 2) loads and runs the same ELF as T1 from the Unix library.
- Evidence: the `MAC` line shows values that only the macOS kernel provides to a translated process: `sysctl.proc_translated=1`, `hw.optional.arm64=1`, `machdep.cpu.brand_string=Apple M5 Max`, `hw.model=Mac17,7`, `kern.osversion=26A434` (MEASURED in appendix A.5: a Rosetta process sees these host values, while `hw.machine` and `uname -m` are `x86_64` there and `hw.cachelinesize` 64), while the `PE` line announces AMD64; the call goes through `NtQueryVirtualMemory(MemoryWineUnixFuncs)` then `__wine_unix_call_dispatcher`, checkable in `WINEDEBUG=+module,+syscall`.
- Advantages: no change to Wine, mechanism already used by winemac and D3DMetal, it is the best way to produce the macOS backdrop of T1. Limits: does not cover Linux semantics on its own.

**T3. A standalone arm64 host with FEXCore that runs the same PE and ELF loads (without Wine)**
- Program: `fexdarwin-run` (being built by the other team, `reports/fexcore-darwin-port.md`) loads two x86-64 blobs linked at free high addresses (measured: 0x110000000-0x180000000, 0x7000000000-0x7780000000, 0x7d80000000-0x7ffffe000000): a minimal PE whose service calls are `syscall`s with NT identifiers, and the same ELF as T1. The host translates each `syscall` according to the caller's range, writes through macOS `write(2)` and prints the same four lines.
- Evidence: output byte for byte identical to T1 (apart from the `MAC` lines if they depend on the pid); the `getpid` obtained is that of the arm64 host process, which proves that the ELF and the PE are run by the JIT in a native macOS process.
- Advantages: first contact of FEXCore with a guest `syscall` on macOS, without Wine; serves as a double of T1 (same load, two engines). Limits: no Wine, so no real TEB and no D3DMetal; the low area (USER_SHARED_DATA) is avoided by construction.

Recommended order: T2 first (the macOS backdrop), T1 next (Linux routing), T3 in parallel as soon as the FEXCore host runs, to compare the outputs.

## 6. Unknowns, risks and open questions

1. Not measured: cost of a SIGSYS round trip under Rosetta (latency per call); essential before sizing F2/F7.
2. Not measured: behavior of `thread_fast_set_cthread_self` (machdep class 3) to set the TEB as GS base under Rosetta; case 5 of the probe hung, probably because the GS base was 1 (not checked). An upstream-style GS swap may be possible under Rosetta, but neither its behavior nor its cost is known.
3. Not checked: whether the PE DLLs `d3d10.dll`, `d3d10core.dll` and `d3d12core.dll` are also D3DMetal wrappers (imports measured only for `d3d11`, `dxgi`, `d3d12`, `nvapi64`).
4. Not executed: no Wine, no game, no EAC was launched by this task (brief). The facts about the dispatcher come from reading and disassembly, not from a `WINEDEBUG=+syscall` trace.
5. EAC risk: Wine macOS already fabricates answers (debug registers accepted without effect under Rosetta, `signal_x86_64.c:1149-1154,1395-1400`; zeros returned by `server/mach.c:192-197`) and the Obelisk commit `bfd51d7f` ("Read cached debug registers when no breakpoint is enabled", taken from Proton) notes it in its message: "under Rosetta the server only ever answers zeroes"; it answers from a local copy of the registers as long as no hardware breakpoint is active, and it deliberately leaves out the server change 2105dac8b57e because "setting another thread's debug registers fails" under Rosetta. The brief forbids fabricated success answers for EAC: these paths are to be audited before any test.
6. `Voile/current` (development build) and `v26.3.0-4` differ; the architecture decision must state which one is the base. `current` has no `runtime.json`; its exact commit is not recorded (INFERRED: close to `e82937c9`).
7. The disagreement between `ANNOUNCE.md:63-64` (GS swap on macOS) and the CrossOver code is to clarify if the design wants to take the upstream fix.
8. The external sources of section 4.4 are GitHub and wiki pages read through a summarizing tool; Wine (gitlab and winehq.org) refused automated requests (403). The quotes from the 10.0 announcement come from the GitHub mirror.
9. Questions for the architect: (a) does the design target a native arm64 Wine (arm64 Mach-O) plus FEXCore in the process, or does it keep Rosetta for Wine and move ELF and FEX into a helper process? (b) which replacement for D3DMetal (x86_64 only) for Metal? (c) the TEB in `TPIDRRO_EL0`/TSD or in a reserved JIT register? (d) offset guest address window (so a modified FEXCore) or a Rosetta process for everything that requires the low area?

## Appendix A. Probes (useful sources and results)

All compiled with `clang -arch ... -isysroot $(xcrun --sdk macosx --show-sdk-path)` in the session temporary directory, run by a Python driver (one case per process, 8 s timeout).

### A.1 `rawsys.c` (x86_64, Rosetta): see the table in section 2.8

```c
static void on_signal(int sig, siginfo_t *si, void *ctx_) {
    ucontext_t *uc = ctx_; unsigned long long rip = uc->uc_mcontext->__ss.__rip;
    const unsigned char *p = (const unsigned char *)(rip - 2);
    fprintf(stderr, "sig=%d si_code=%d si_addr=%p rip=%llx bytes_before_rip=%02x%02x rax=%llx rcx=%llx r11=%llx\n",
            sig, si->si_code, si->si_addr, rip, p[0], p[1], uc->uc_mcontext->__ss.__rax,
            uc->uc_mcontext->__ss.__rcx, uc->uc_mcontext->__ss.__r11);
    uc->uc_mcontext->__ss.__rax = 0xdead0000ULL | (unsigned)sig;   /* resumes after the instruction */
}
__attribute__((noinline)) static long do_syscall(long nr, long a1, long a2, long a3, int *cf) {
    long ret; unsigned char c;
    __asm__ volatile("syscall\n\tsetc %1" : "=a"(ret), "=q"(c) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory", "cc");
    *cf = c; return ret;
}
```

Typical output (case 1): `HANDLER sig=12 si_code=0 si_addr=0x1007557d6 si_errno=0 rip=1007557d6 bytes_before_rip=0f05 rax=27 rcx=1007559d0 r11=20084b890`.

### A.2 `x18probe2.c` (native arm64)

```c
static inline unsigned long get_x18(void){unsigned long v;__asm__ volatile("mov %0, x18":"=r"(v));return v;}
static inline void set_x18(unsigned long v){__asm__ volatile("mov x18, %0"::"r"(v));}
static long raw_getpid(void){register long x16 __asm__("x16")=20;register long x0 __asm__("x0");
    __asm__ volatile("svc #0x80":"=r"(x0):"r"(x16):"memory","cc");return x0;}
/* set_x18(0x1122334455667788); raw_getpid(); get_x18() == 0 : the three tries, the Mach trap and the fault */
```

### A.3 `addrprobe.c` (arm64 and x86_64) and pages

Outputs: see section 3.4. x86_64 program, case 99 of `rawsys`: `proc_translated=1 getpagesize=4096 sysconf=4096 vm_page_size=4096 vm_kernel_page_size=16384`; `two 4K mmaps: a=0x10824d000 b=0x10824e000 diff=4096`; `mprotect(4K sub-page of 64K map, PROT_NONE) rc=0`.

### A.4 `fsgs.c` (x86_64, Rosetta)

Case 0: `%gs:0` readable. Case 1: `%fs:0` SIGSEGV `si_addr=0x0`. Cases 2, 3, 4: `wrfsbase`, `rdfsbase`, `wrgsbase` SIGILL. Case 5: `CPUID.7.0:EBX = 0`.

### A.5 `sysq.c` (same source compiled arm64 and x86_64)

| Key | native arm64 | x86_64 under Rosetta |
|---|---|---|
| `sysctl.proc_translated` | 0 | 1 |
| `hw.machine` and `uname.machine` | arm64 | x86_64 |
| `hw.cachelinesize` | 128 | 64 |
| `hw.optional.arm64` | 1 | 1 |
| `machdep.cpu.brand_string` | Apple M5 Max | Apple M5 Max |
| `hw.model` | Mac17,7 | Mac17,7 |
| `kern.osproductversion`, `kern.osversion`, `kern.osrelease` | 27.0.1, 26A434, 27.0.0 | identical |
| `hw.ncpu`, `hw.memsize` | 18, 38654705664 | identical |

## Appendix B. Runtime reading commands (no writes, no Wine execution)

- `lipo -archs`, `file`, `otool -L`, `otool -l`, `nm -gU`, `nm -u`, `otool -tvV -p ___wine_syscall_dispatcher` on the files of `RT` and `CUR`.
- `strings -a` on `bin/wine`, `lib/wine/x86_64-unix/wine`, `ntdll.so`, `libd3dshared.dylib`.
- Session Python scripts: reading of the PE headers (machine, magic, "Wine builtin DLL" signature) of `lib/wine/x86_64-windows` (766 files) and `i386-windows` (815), of the bytes of the export stubs and of the import tables.
- `git ls-remote` for the commit hashes of the external repositories; `git -C W log`, `describe`, `show --stat c6e20da3`.
- No session, token, registry or user data file was opened; only folder listings and `runtime.json` were read.

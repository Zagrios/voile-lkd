# wine-1: FEXCore engine (fexdarwin), three x86-64 instructions that kill the guest

Requester: wine-linux agent (EXP/src/wine-linux). No LKD change, these are engine topics (src/fex-darwin, src/fexdarwin).

## 1. RDTSCP advertised by CPUID but refused (SIGILL)
MEASURED: wineboot.exe executes `rdtscp` at 0x140002e50 after reading CPUID 0x80000001 EDX bit 27 = 1; Wine's segv_handler receives signal 4 code 2 trap 6 at this address (logs/wine-linux-m2-version-012.log).
READ: FEXCore/Source/Interface/Core/CPUID.cpp Function_8000_0001h sets SUPPORTS_RDTSCP = 1 outside _WIN32; OpcodeDispatcher.cpp RDTSCPOp calls UnimplementedOp if HostType != Linux and without SupportsCPUIndexInTPIDRRO, which is the case on Darwin.
Request: either implement RDTSCP on Darwin (TSC_AUX = 0 or core number), or remove bit 27 from CPUID. Games and EAC read this bit. Temporary workaround on the Wine side: wineboot no longer uses rdtscp.

## 2. iretq in 64-bit mode
MEASURED: static witness program (iretq to a label with CS=0x33, SS=0x2b read by mov %cs/%ss, valid stack): guest SIGSEGV at the iretq, exit code 139. In Wine, segv_handler receives signal 11 code 1 err 4 with an address that changes at each launch (0x100f62070, 0x10369e0e0, ...), rip = the iretq of __wine_syscall_dispatcher, rsp = kernel stack unchanged.
INFERRED: the address varies like a host address (ASLR) passed through H2G: the faulting access comes from JIT code outside guest memory, not from the program.
Wine workaround in place (signal_x86_64.c, return through the user stack). The witness stays useful to fix the engine.

## 3. mov %reg,%ds / %es in 64-bit mode
MEASURED: `movl %eax,%ds` in setup_raise_exception (leave_handler): signal 11 err 4, variable address (0x10555e0d8), "nested exception on signal stack". Same signature as 2.
READ: MOVSegOp accepts DS/ES in 64-bit (store of ds_idx); FS/GS refused with "We don't support modifying FS selector in 64bit mode!" (DecodeFailure) even on branches never taken.
Wine workaround in place (WINE_NO_SEGMENT_RELOAD).

## Answer from the project lead (2026-10-02, after 20:45)

The three points are fixed in the engine (patch `src/patches/fexcore-darwin.patch`), witness `witnesses/elf-x86_64/src/w16_segtsc.c` 9/9 in LKD mode:
1. RDTSCP and RDPID: `ProcessorID` calls `pthread_cpu_number_np` on Darwin (MEASURED: `TPIDRRO_EL0` carries the thread pointer on macOS 27, not the core number); TSC_AUX = (node 0 << 12) | core.
2 and 3. iretq and `mov` to `%ds`/`%es`: `UpdatePrefixFromSegment` read the GDT descriptor, which is in host memory, through a memory access to which the JIT adds the guest base; the base is removed before the access. The workarounds in Wine (`WINE_NO_SEGMENT_RELOAD`, return through the user stack) can be removed.

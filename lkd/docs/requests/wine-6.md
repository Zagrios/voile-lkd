# wine-6: the guest address space stops at 2^46; the EAC bootstrapper allocates at 0x7001_0000_0000 (Linux: 2^47)

Requester: wine-linux agent. Blocking for the EAC bootstrapper (cause of the "Failed to initialize Wine helper buffer" stop).

MEASURED:
- The dialog box that the bootstrapper shows in the 290x146 window (capture of the only wsd window: evidence/wine-linux/eac-splash/t2-win583.png) says "Easy Anti-Cheat Launch Error / Failed to initialize Wine helper buffer". Exit code 201 (evidence/eac-runs/20261002T215740, 215925, 220301).
- WINEDEBUG +virtual (evidence/eac-runs/20261002T220659/guest.log, thread 0024 = RocketLeague_EAC.exe), right before the error, ten calls:
  `NtAllocateVirtualMemory(GetCurrentProcess(), 0x700100000000, size 0x1000, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE)` then 0x700200000000 ... 0x700a00000000 (step of 2^40). All fail: Wine logs at startup `host addr space limit: 0x3fffffff0000` (mmap probe of virtual.c), so no address above 2^46 is available.
- Under Linux x86-64 (QEMU, GE-Proton, tests of September 30) user space goes up to 0x7fffffffffff (47 bits) and these addresses are free; Windows x64 too (128 TiB).
READ: Wine (virtual.c) does not limit user_space_limit to 2^46; it caps at the host_addr_space_limit measured by mmap. It is the guest space of fexdarwin/LKD (window of 2^46, see the lead's message on the 524288 entries) that sets the bound.
Request: a guest window of 2^47 (0x0000_0000_0000 .. 0x7fff_ffff_ffff), at the minimum that 0x7000_0000_0000 .. 0x7fff_0000_0000 be allocatable. Since mm's reservations now have a null maximum protection, the size of the window should no longer cost kernel entries (to be confirmed by the lead).
Witness: small Windows program that does VirtualAlloc((void*)0x700100000000, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE) and prints the result; under native Linux it succeeds.

Witness run (MEASURED, logs/wine-linux-highalloc-001.log, src/wine-linux/tests/highalloc.c, in our Wine under LKD): RWX VirtualAlloc at a fixed address succeeds at 0x100000000000, 0x200000000000, 0x300000000000 and fails (487 ERROR_INVALID_ADDRESS) at 0x400000000000 and beyond, including 0x700100000000 .. 0x700a00000000, the bootstrapper's addresses.

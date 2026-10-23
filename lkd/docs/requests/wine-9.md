# wine-9: FEXCore ASSERT "LookupCache: Failed to allocate PagePointer" at the clone3 of a new thread -> the game dies (SIGILL)

Requester: wine-linux agent. Blocking: Rocket League stops at the menu after about 135 s of play.

MEASURED (evidence/eac-runs/20261003T003343, guest.log lines 159-164):
```
[FEXCore ASSERT] LookupCache.cpp:42, LookupCache: Failed to allocate PagePointer
[fexdarwin] FATAL unhandled fault: signal 4 (si_code 2) fault address 0x102d34648
[fexdarwin]   host pc=0x102d34648 sp=0x706241a660 (outside JIT code: host C++)
[fexdarwin]   guest rip=0x7f8275edc529 rax=0x1b3 rbx=0x7f8275e5e620 rcx=0x7f8275edc529 ...
```
rax = 0x1b3 = 435 = clone3, rcx = rip: the game process creates a thread and the construction of the new thread's LookupCache (Core.cpp:429, make_unique<LookupCache>) fails: VirtualAlloc(TotalCacheSize) returns -1, ASSERT -> trap.
READ (LookupCache.cpp:27, Context.h:349, LookupCache.h:468): TotalCacheSize = VirtualMemSize/4096*8 + CODE_SIZE + MAX_L1_SIZE = 128 MiB + 128 MiB + L1, that is about 256 MiB of host reservation per guest thread.
INFERRED: either the LookupCaches of ended threads are not freed (Rocket League and Wine create and destroy many threads: workers, timers, threadpool), or the space where the host allocator places these reservations (host addresses 0x70_xxxx_xxxx) is full. Two previous runs of 150 s did not have the assertion; this one died at 134 s of play, after 1560 Present (menu at 60 fps, interval risen to 42 ms right before).
Request: check that the LookupCache is freed when a thread exits and the errno of the mmap that fails; a message with the errno and the number of live threads instead of the assertion would help.

Second occurrence (MEASURED, evidence/eac-runs/20261003T004000): same assertion, same guest rip 0x7f8275edc529, same clone3, but at 112 s, during loading (only 96 Present). It is therefore not linked to the play time.
READ (AllocatorHooks.h:128): on Darwin VirtualAlloc = mmap(NULL, about 260 MiB, RW, MAP_PRIVATE|MAP_ANONYMOUS); it is this host mmap that returns MAP_FAILED.
MEASURED: the three runs before 00:30 (20261002T233036, 20261003T002238, 20261003T002813, up to 150 s and 1680 Present) do not have the assertion; the two following ones (00:34 and 00:40) have it. LKD files modified in the meantime: src/lkd/src/net/{ifaces,socket,sockopt,sockmsg}.c, net_priv.h (wine-8 work); build-lead/host/fexdarwin-run rebuilt at 00:40. INFERRED: with visible network interfaces, the game or EAC starts more threads (or network threads), or the new code reserves host space; the free host address space for a contiguous 260 MiB mmap is exhausted or fragmented (the 2^47 guest window takes up most of the host space).
Consequence: the offline play test (Free Play, 60 s) is blocked.

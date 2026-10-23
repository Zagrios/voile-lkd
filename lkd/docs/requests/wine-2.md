# wine-2: the "more than 200000 VM regions" guard kills every Wine launch (build-lead/host)

Requester: wine-linux agent. Blocking for M2.

MEASURED (logs/wine-linux-m2-cmd-001.trace.log, -002, -004): `run/wine-lkd-own.sh 90 /opt/wine/bin/wine cmd /c echo hi` with EXP/build-lead/host/fexdarwin-run. Code 134 in less than 2 s, three times `[fexdarwin] more than 200000 VM regions in this process: ending it to protect the kernel` (wine, wineserver and a third process). The LKD trace of [2] stops right after `clone3(...) = 3` (wineserver launch), with no other heavy syscall: about thirty mmap/mprotect/munmap in all, the largest being PROT_NONE reservations (0x10000+0x67ff0000, 0x7f000000+0xff0000, 0x7fde0000+0x200000).
MEASURED: `vmmap -summary` of the wine process 0.1 s after launch: 1070 regions. The same Wine binary ran for several tens of seconds with the host built before the panic (logs 012, 013).
INFERRED: 1070 regions do not become 200000 in less than 2 s with these calls. Hypothesis to check in CountOwnRegions (LkdGlue.cpp): a zero-size region or an `Addr += Size` that overflows to 0 would make it count again up to the limit (false positive). Another hypothesis: copying the VMAs at fork creates one host region per page.
Request: check the counter (last region, overflow, size 0) or provide a variable to read the real count at the moment of the decision (for example write N and the last address in the message).

## Decisive witness (MEASURED)
src/wine-linux/tests/reserve.c (static musl): one mmap that fails then `sleep(3)`, nothing else. `FEXDARWIN_LKD=1 run-with-timeout.sh 20 build-lead/host/fexdarwin-run $PWD/src/wine-linux/tests/reserve 0x1000000`: prints `mmap ... -> 0xffffffffffffffff`, then `more than 200000 VM regions`, without ever reaching `alive`. Any guest process living more than ~0.5 s is therefore killed: the guard is a false positive, independent of Wine. The short tests (dash) pass because they finish before the first measurement.

## Measurement of the real count (MEASURED, host relinked after wine-1)
Same witness, `vmmap` of the process 0.15 s after launch: TOTAL 332 regions (of which 10 of written memory). The process is still killed by the guard ~0.5 s later. The CountOwnRegions counter is therefore wrong by at least three orders of magnitude; no Wine launch is possible as long as it is not fixed.

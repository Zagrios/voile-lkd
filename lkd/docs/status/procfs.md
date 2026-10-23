# Status of the procfs module (LKD)

Date: 2026-10-02. Milestone 2 reached: `/proc`, `/sys` and `/dev` are served by the vfs virtual mount, the three test programs pass (551 assertions), the other processes of the family are read through famsvc. procfs registers no syscall and holds no lock.

## What exists (src/procfs, 3060 lines)

| File | Content |
| --- | --- |
| `lkd_procfs.h` | API for the engine and the tests: `lkd_procfs_set_cpuid`, `lkd_procfs_set_backend`, `lkd_procfs_render_wire`, `lkd_procfs_dev_is_zero`, `lkd_procfs_ncpu` |
| `procfs_priv.h` | internal types (nodes, targets, inter-process requests) |
| `node.c` | resolution of the three trees (`pf_resolve`), attributes, listings, readlink; owner of the `/proc/<pid>` nodes (`task_dump_owner`) |
| `file.c` | `open` of the generated files (seq_file semantics: content produced at the first read and at any read at offset 0), file object, permissions |
| `pid.c` | all the files of `/proc/<pid>` and `task/<tid>`, access rules, `mem` object |
| `global.c` | cpuinfo, meminfo, uptime, loadavg, version, stat, filesystems, `/proc/sys` |
| `sysfs.c` | `/sys/devices/system/cpu` (online, possible, present, offline, kernel_max, cpuN/online, topology, cache), `clocksource0` |
| `dev.c` | `/dev`: null, zero, full, random, urandom, tty, console, ptmx, pts, fd, stdin, stdout, stderr |
| `remote.c` | famsvc opcode 49 (`LKD_FAMSVC_OP_TASK_STATUS`), default backend, object on a received descriptor |
| `init.c` | mounts `/proc` (proc), `/sys` (sysfs), `/dev` (devtmpfs) |
| `buf.c`, `NOTICE` | text buffer, attributions |

## Model

- A file of `/proc/<pid>` is rendered by the host process that carries the task, with its own structures (task, mm, sig, fs, ptrace). For another host process, the reader sends a famsvc request (`struct lkd_procfs_wire`) with the `ptrace_may_access` access verdict; the other process answers with the text. A zombie is rendered from the registry alone. `/proc/<pid>/maps` of another process goes through the mm service (`lkd_mm_remote_render_maps`), `mem` through `lkd_mm_remote_read/write`.
- `open("/proc/<autre>/fd/N")`: the other process reopens the description (`lkd_fdobj_reopen`) and sends back a host descriptor through SCM_RIGHTS; the reader wraps it in an object with a private offset (`pf_dupfile_new`), the other process's offset is never moved. Pipes and sockets: `-ENXIO` (fs rule).
- No file is a frozen snapshot: every read at offset 0 regenerates the content from the current state.
- No content is invented. A quantity that the host does not give is omitted, or is 0 where the concept does not exist under Darwin (list in "Gaps").

## Tests (MEASURED, private build, `tools/runtest.sh`)

| Program | Assertions | Cases | Content |
| --- | --- | --- | --- |
| `test_procfs_self` | 271 | 17 | self and thread-self, exe, cmdline/environ/auxv (partial reads), comm (read, write, escaping), status (all fields, values set by the test), Vm* counters (mmap, mlock), stat (52 fields, layout values), statm, maps/smaps, fd (memfd, pipe, getdents64 listing, independent reopening, fdinfo), cwd/root, mem, limits, syscall/wchan/cgroup/personality/mounts/net, owners and access by another reader (dumpable, uid), comparisons with the Linux samples |
| `test_procfs_other` | 104 | 4 | fake registry (listing order, zombie, ptrace verdicts, request sent to the other process); real processes: fork, read through famsvc of status, stat, cmdline, exe, cwd, root, limits, maps, fd/fdinfo, received memory descriptor, `mem` read and write, real TracerPid (PTRACE_TRACEME), zombie before wait4, disappearance after |
| `test_procfs_global` | 176 | 6 | cpuinfo (fake CPUID provider, each line), meminfo/uptime/loadavg measured against sysctl, version/stat/filesystems, `/proc/sys`, `/sys` CPU and cache tree, /dev |

In the complete build (`cmake --build -- -k 0` then `tools/run_tests.sh`), the three pass with the other modules; three consecutive runs without failure, no process or family directory left behind. Failures of other modules seen in the same build: `test_net_host`, `test_ptrace_requests`, `test_sig_futex` (timeout), `test_ptrace_smoke` (does not compile: `body_loop` defined twice), unrelated to procfs.

## Comparison with the Linux samples (explanations of the differences)

- stat: `rocket-hyperlinux-20260928/linux-reference/loader-trace/ge-eac-120s-threads.txt:17`. Same number of fields (52), same fields set to 0 (itrealvalue, kstkesp, kstkeip, nswap, cnswap). Differences: `flags` (4194560 on Linux: PF_RANDOMIZE and others; LKD only sets PF_EXITING, no ASLR), `tty_nr` 0 and `tty_pgrp` -1 (no controlling terminal tracked), `processor` (see gaps), `start_data`... come from the layout reported by the engine.
- maps: `qemu-neptune-graphics-20260930/game-query-fix-20260930T211549Z/outputs/game-process-maps.txt`. The name starts at offset 73 (748 lines measured out of 748), anonymous line ended by a space, `/memfd:name (deleted)`, `[heap]`, `[vdso]`. The test applies the same column checker to the recorded lines and to LKD's lines. Difference: LKD merges neighboring anonymous VMAs like Linux (mm).
- status: `linux-reference/pid1-control/pid1-before.txt` (kernel 7.0, x86-64). Same keys in the same order; missing in LKD: exactly `Seccomp` and `Seccomp_filters` (kernel modeled without CONFIG_SECCOMP, `task-2.md` point 9). Width of 8 columns for the Vm* fields, 16 hexadecimal digits for the signal sets: identical. The test checks these three points.

## Source of each field of `/proc/cpuinfo`

| Field | Source |
| --- | --- |
| number of blocks | `sysctl hw.logicalcpu` (18 on this Mac, MEASURED) |
| vendor_id, cpu family, model, stepping, fpu, cpuid level | CPUID 0 and 1 of the engine (FEX: `CPUID.cpp:429-448`) |
| model name | CPUID 0x80000002 to 4 per CPU (FEX: MIDR of the core, `CPUID.cpp:1121`) |
| cpu MHz, bogomips | CPUID 0x15: ecx*ebx/eax Hz; missing if eax is 0 (FEX divides by zero in that case) |
| cache size | largest cache of CPUID 4, in KB |
| flags | bits of CPUID 1, 6, 7, 0xD, 0x10, 0x80000001, 0x80000007, 0x80000008, 0x8000000A, 0x8000001F, same table as FEX; always present: `art rep_good nopl xtopology cpuid tsc_known_freq`; `hypervisor` follows CPUID 1 ecx bit 31 (shown, not hidden) |
| physical id, siblings, core id, cpu cores, apicid | flat topology: one package, N cores, one thread each (like FEX); identical to `/sys/devices/system/cpu/cpuN/topology` |
| clflush size, cache_alignment | CPUID 1 ebx[15:8] * 8 |
| address sizes | CPUID 0x80000008 eax |
| bugs, power management | empty |
| microcode | not printed (unknown value; FEX prints 0x0) |

FEX is an emulator and it shows: the model name is the one the engine gives, and `hypervisor` appears if the CPUID sets it.

## `/proc/<pid>/lkd_host_pid` (2026-10-10)

LKD-specific file, absent from Linux: the macOS PID of the host process that carries the process, followed by a newline (`72451\n`), mode 0444. The same file exists under `task/<tid>/` and therefore under `/proc/thread-self/`: all the threads of a process share its host process.
- Source: `lkd_proc_info.host_pid` of the family registry, without a famsvc request. Right after a fork, after an exec (same host process) and after the migration of a vfork child at its exec (new host process), the registry already has the right value.
- A zombie no longer has a host process: `-ESRCH`.
- The file does not appear in the listing of `/proc/<pid>`: it opens by its name only, so that the listing stays that of the modeled kernel (EAC reads these directories).
- Use: `guest/steam-hostpid.so` (repository root). Valve's `steamclient.so` announces `getpid()` to Obelisk's macOS Steam engine, which cuts the pipe if that PID is not a live macOS process.
- Tests: `test_procfs_other` case `host_pid_file` (fake registry, self, real fork seen from both sides, zombie, thread, absence from the listing); guest witness `tests/glibc-x86_64/hostpid.sh` (w25).

## Parent of another process: `PPid` read from the registry (2026-10-10)

- `status` (`PPid`) and `stat` (fields 4, 5, 6 and 38) of a process are rendered by its own host process. The rendering took its local copy of ppid, pgid, sid and of the exit signal, which the process only rereads from the registry when it calls `getppid`, `getpgrp`, etc. itself.
- Measured effect: an orphan that had not made that call still showed its dead parent. ppid probe of ELDEN RING report 07: `ping.exe` with `PPid 52`, the intermediate process of Wine's double fork, which had exited long before.
- `snap_get` now rereads these fields from the registry (`lkd_proc_refresh_ids`) before each rendering. A zombie was already rendered from the registry.
- Test: `test_procfs_other` case `orphan_ppid_follows_its_reaper`. FAILS without the reread (`PPid 4`, the dead intermediate process), PASSES with it.

## Files deliberately missing (ENOENT)

`/sys/devices/virtual/dmi/id/*`, `/sys/class`, `/sys/bus/pci`, `/proc/bus/pci/devices` (EAC probes, absence accepted), `/proc/<pid>/io` (no per-task I/O accounting), `map_files`, `ns`, `sched`, `stack`, `pagemap`, `oom_*`, `attr`, `loginuid`, `uid_map`, `/proc/sys/kernel/yama`, `/proc/net/tcp` and neighbors, `/proc/stat` lines `intr`, `ctxt`, `softirq`, `processes`. `/proc/<pid>/net/dev` only carries the header (no translated interface). If `inotify_init1` has no owner, `/proc/sys/fs/inotify` does not exist.

## Pseudo-terminals

SCM_RIGHTS (2026-10-10). null, zero, random and urandom go to another host process through their macOS twin (same behavior), opened at the first send with the guest access mode, and a twin received without a guest path becomes the Linux device again (`lkd_procfs_dev_adopt`, also for the runtime's standard input). Reads and writes stay in LKD: macOS refuses `write` on `/dev/urandom` (EPERM) and cannot wait on a device (`poll` POLLNVAL, `kevent` EINVAL). `/dev/full`, which has no twin, and a device opened O_PATH go out as a record (`LKD_FDREC_DEV`). A generated file and `/proc/<pid>/mem` go out as an `LKD_FDREC_PROCFS` record (canonical path, position): the receiver reopens it with its own permissions (`pf_open`) and resumes at the position. See `net.md`.

Not supported. `/dev/ptmx` exists with the Linux numbers (5:2, 0666) but `open` answers `-ENODEV` (kernel without a devpts driver); `/dev/pts` is an empty directory. `/dev/tty` and `/dev/console` open the host's controlling terminal (`/dev/tty`) and answer `-ENXIO` without a terminal; `/dev/console` is therefore not the system console.

## Known gaps (honest)

1. `VmPeak` and `VmHWM` are the current value (mm keeps no peak): lower bound. `procfs-2.md` point 2.
2. `THP_enabled` set to 1 (INFERRED). `VmPTE` 0 (no guest page table).
3. `cmdline` and `environ` are the copies taken at exec: a rewrite of argv by the program (setproctitle) does not appear. Linux rereads `arg_start..arg_end`.
4. `stat`: utime and stime of a thread other than the caller are 0 (per-thread view); the whole-process view uses `getrusage(RUSAGE_SELF)` of the host process, so a co-hosted process (vfork before exec) sees the host's times. `processor`: CPU of the calling thread, 0 for another thread. `flags`: see above. `minflt` and `majflt` are those of the host process.
5. `uptime`, `btime`: CLOCK_BOOTTIME clock of the task module (Darwin's CLOCK_MONOTONIC_RAW); `btime` = now - uptime. `kern.boottime` differs by 8 s on this Mac (MEASURED: 32695.43 against 32687.00 s) and is not used, so that `btime`, `/proc/uptime` and `clock_gettime(CLOCK_BOOTTIME)` agree.
6. `meminfo`: `Buffers`, `Dirty`, `Writeback`, `Slab`, `SReclaimable`, `SUnreclaim`, `KernelStack`, `PageTables`, `Shmem`, `Committed_AS`, `SwapCached` are 0 (no Darwin equivalent or no accounting). `MemAvailable` = free + inactive + speculative + purgeable, capped at `MemTotal`. `Cached` = file-backed pages, `AnonPages` = internal pages. `CommitLimit` = half of the memory + swap (`overcommit_memory` is 1: mm counts nothing).
7. `/proc/stat`: per-CPU ticks from `host_processor_info` (100 Hz); `iowait`, `irq`, `softirq`, `steal`, `guest` are 0 because they do not exist under Darwin.
8. `/proc/sys`: writes refused (`-EACCES` for a non-root, logged `-EPERM` for root); the parameters are fixed. `pid_max` = `LKD_PID_MAX + 1` (larger than Linux's ceiling, it is the real limit of the family counter). `randomize_va_space` 0 (no ASLR). `overcommit_memory` 1. inotify limits 128 / 65536 / 16384 (not provided by net, `procfs-4.md` point 3).
9. Another host process whose famsvc service does not answer within the timeout (5 s) gives `-EIO`; a vanished service `-ESRCH` (reading a dead process returns `-ESRCH`, like Linux, and the directory disappears at `wait4`).
10. The `S` state of another host process comes from the registry (task: `R` except during wait4, nanosleep); `State:` read in the process itself is exact for its own threads. See `task-1.md` point 4.
11. The `/proc` listings limit the registry to 1024 processes (size of task's registry); each entry rereads the list (O(n) per entry, not measured).
12. `execve("/proc/self/exe")` (the link itself, not its text) is not checked with the engine (`procfs-3.md` point 3).
13. `/proc/<pid>/fd/N`: the mode of the link follows the access mode of the description; for another host process, it comes from the target process's `fdinfo`.
14. Memory of a dead process: `mem` answers 0 bytes (the service's `-ESRCH` is turned into end of file), like `mm_access` returning NULL.

## Performance

Not measured (decision: no benchmark before the first real launch). The famsvc service costs p50 10.3 us per round trip (MEASURED by sig); a status read of another process does one round trip.

## Written requests

`docs/requests/procfs-1.md` (core: include path, `lkd_task_uname`), `procfs-2.md` (mm: /dev/zero, peaks), `procfs-3.md` (engine: CPUID provider, layout after exec), `procfs-4.md` (fs, net, ptrace, task).

## Possible next steps

`/proc/<pid>/io` from `proc_pid_rusage` (bytes read and written from the disk), `/proc/net/tcp`/`udp`/`unix` with the net module, `smaps_rollup`, `/proc/<pid>/children`, reading the live argv, measuring the cost of listing `/proc`.

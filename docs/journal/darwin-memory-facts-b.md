# macOS facts on JIT, faults and signals (groups C and D)

Last update: 2026-10-02 (draft 3: parts C and D measured except SMC and fault in the handler). The sections marked `to be completed` have no measurement yet.

This report separates what was MEASURED (C programs written and run on this machine, journals in `evidence/darwin-memory/`) from what is READ (source code, SDK headers) and from what is DEDUCED. Working root: `build/experiments/rocket-wine-fex-darwin-20261002`, written `$EXP`.

## Measurement machine

Measured: macOS 27.0.1 (build 26A434), kernel `xnu-13432.1.9~1/RELEASE_ARM64_T6050`, Apple M5 Max, 18 logical cores, 16 KiB host pages (`hw.pagesize` = 16384), 36 GiB of memory, SIP enabled, Apple clang 21.0.0, SDK macOS 27.0. No `boot-args` variable is readable.

## Progress state

| Item | State |
| --- | --- |
| `witnesses/darwin-memory/src/common.h` left by the previous attempt | inspected, reused (copy in `cd_common.h`) |
| No `c_*` or `d_*` file existed at the start of this resumption | observed |
| C programs (JIT): `c_jit_basic.c`, `c_jit_threads.c`, `c_jit_cost.c` | written, run (`plain` variant), all variants to be rerun |
| D programs (faults and signals) | to be completed |
| Scripts `build-cd.sh` and `run-cd.sh` | written |

## C. JIT

Measurements of draft 2 (`plain` variant: ad hoc signature without hardened runtime or entitlement). The full journals are `evidence/darwin-memory/c_jit_basic.<variant>.log`, `c_jit_threads.<variant>.log` and `c_jit_cost.<variant>.log`. Details to be completed in the final version.

### C.1 MAP_JIT and per-thread write toggle (measured)

- An ad hoc signed binary without any particular entitlement and without hardened runtime gets `MAP_JIT` (`c_jit_basic.plain`, test `C02.mapjit`). With the hardened runtime (`-o runtime`) and without the `com.apple.security.cs.allow-jit` entitlement, `mmap(MAP_JIT)` fails with `EINVAL` (`rt` variant). With the hardened runtime and `allow-jit` (`rtjit` variant), it succeeds.
- `MAP_JIT|MAP_SHARED` and `MAP_JIT` without `MAP_ANON` give `EINVAL`. `MAP_JIT|MAP_FIXED` gives `EINVAL`. A 4 KiB `MAP_JIT` region is rounded to 16 KiB.
- The default state of each thread (the main one and any created thread, whether the parent is in write mode or not) is execute mode: a write into the `MAP_JIT` page raises `SIGBUS` (`si_code` = `BUS_ADRALN`, ESR EC=0x24, DFSC=0x0f, permission fault), a read and a call succeed (`c_jit_threads` T01).
- `pthread_jit_write_protect_np(0)`: write and read possible, execution raises `SIGBUS` with `si_addr` = `pc`. `pthread_jit_write_protect_np(1)`: read and execution possible, write raises `SIGBUS`. The state is specific to the calling thread (T01.isolation, T02, T05).
- Cost of a toggle: 11.6 ns per call (11.7 ns even if the state does not change), 23 ns for a pair. `pthread_jit_write_with_callback_np` costs 22.7 ns. A complete patch (toggle, one store, toggle) costs 23 ns, and 102 ns with `sys_icache_invalidate` of 4 bytes (`c_jit_cost` K01, K02, K04).

### C.2 Instruction cache coherence (measured)

- Without invalidation, a thread that rewrites an instruction word then calls it runs the old version 199,900 times out of 200,000. `dsb ish; isb` alone and `dc cvau` alone are not enough (same rate). `ic ivau` followed by `dsb ish; isb` is enough, without `dc cvau`. `sys_icache_invalidate` and `__builtin___clear_cache` are enough (0 stale executions) (`C03.same-thread`).
- Between threads: a writer that does toggle, write, toggle, `sys_icache_invalidate`, then publishes through a release store produces no stale execution in three readers without `isb` (T02, 60,000 rewrites per configuration, 128-byte lines shared between the executed code and the rewritten code). Without invalidation: 69,149 stale executions out of 69,250.
- Patching a `b` in place while another thread loops: with invalidation the reader leaves the loop 125 ns (median) after the store, 250 ns at p99 (T03). Without invalidation, the median is 6.8 ms and 9 trials out of 300 did not leave the loop within 30 ms.
- An instruction word alternated 11 million times between two values is read whole by two concurrent readers (11.9 million executions, 0 intermediate values, 0 faults) (T06).
- A literal pointer `ldr x16, lit; blr x16` patched by an aligned 64-bit store in write mode is seen by the reader in 42 ns (median), without cache maintenance (T04).
- A thread that toggles and writes continuously does not slow down four threads that run code (0.72 ns per call with or without writer, same 16 KiB page included) (T05).
- `CTR_EL0` is not readable from user space (`SIGILL`); `hw.cachelinesize` is 128, L1I 128 KiB, L1D 64 KiB. `sys_icache_invalidate` costs 82 ns up to 128 bytes, 652 ns for 16 KiB, 35 µs for 1 MiB (about 25 to 30 GB/s) (K03). It works in write mode as in execute mode, and raises `SIGSEGV` on an unmapped range.

### C.3 RWX mappings without MAP_JIT (measured)

- Anonymous `mmap(RWX)` without `MAP_JIT`: `EACCES` in all variants. `mprotect(RWX)` on any anonymous mapping: `EACCES`.
- `plain` variant (without hardened runtime): `mmap(RW)`, write, `mprotect(RX)`, call: succeeds. `rtjit` variant (hardened runtime + `allow-jit`): the same sequence is killed by `SIGKILL` at execution. `rtjitx` variant (`allow-jit` + `allow-unsigned-executable-memory`): succeeds.
- Unsigned file mapped `PROT_EXEC`: `EPERM`.
- The pair `mprotect(RX)` + `mprotect(RW)` costs 540 ns for 16 KiB, 650 ns for 128 KiB, 1.6 µs for 1 MiB, 680 ns for 16 KiB with 8 busy threads, and acts on the whole process (K05).

### C.4 Sizes and number of regions (measured)

- A single RWX `MAP_JIT` reservation of 1 GiB, 4 GiB, 64 GiB, 1 TiB, 127 TiB succeeds in 1 to 3 µs without touching a page; 128 TiB gives `ENOMEM`. The first and last pages are usable.
- 100,000 `MAP_JIT` regions of 16 KiB (0.5 µs each, no merging of VM entries), 20,000 of 1 MiB and 4,096 of 1 GiB were created without failure (limit set by the test, not by the kernel).
- A `MAP_JIT` region created with protection `---`, `r--`, `rw-` or `r-x` can be promoted by `mprotect` to any protection, including RWX. Once RWX (created that way or promoted), any new `mprotect` gives `EACCES`: the promotion is one-way. A region created RWX refuses any `mprotect`, including to RWX. `mach_vm_protect(set_maximum)` gives `KERN_PROTECTION_FAILURE`.
- An address hint is honored for `MAP_JIT` as for ordinary `mmap` when the address is free (0x120000000, 0x7000000000, 0x10000000000 honored; occupied addresses are moved).
- `MAP_FIXED|MAP_ANON` without `MAP_JIT` over `MAP_JIT` pages succeeds, but the pages permanently lose the JIT status (`mprotect(RWX)`: `EACCES`, `MAP_FIXED|MAP_JIT`: `EINVAL`). `munmap` of a hole then `mmap(MAP_JIT)` at the same address (hint) succeeds. `mach_vm_remap` of a `MAP_JIT` region gives `KERN_PROTECTION_FAILURE`: no double mapping.
- `madvise` on `MAP_JIT` pages: `MADV_ZERO` gives `EPERM` (it succeeds on ordinary anonymous memory), `MADV_DONTNEED`, `MADV_FREE`, `MADV_FREE_REUSABLE` return 0 without zeroing (the content 0xa5 stays readable), `MADV_PAGEOUT` gives `ENOTSUP`.
- Cost of the first touch of a fresh 16 KiB `MAP_JIT` page (write, invalidation, first call): 2.26 µs, versus 259 ns for an already resident page and 615 ns for an ordinary anonymous page (K06). `munmap` of an untouched `MAP_JIT` reservation: 1.8 µs for 4 MiB, 4.0 µs for 1 GiB.

### C.5 Control transfers (measured)

- JIT code calls a host C function through `ldr x16, lit; blr x16` and returns; `bl` between two neighboring JIT regions (16 KiB apart) works; `ldr x16, lit; br x16` to a region 443 GiB away works; 1,000 host-to-JIT calls through a function pointer work.
- A `blr` to a JIT address without a landing `bti` instruction runs without fault (FEAT_BTI is present, but not enforced on the `MAP_JIT` pages of a plain arm64 binary).
- An indirect call of a JIT stub costs 0.48 to 0.56 ns, versus 0.47 ns for a host function (K08).

## D. Faults and signals

Measurements of draft 3 (`plain` variant), journals `evidence/darwin-memory/d_*.plain.log`. The parts "cost of a fault round trip" (SMC) and "fault in the handler" are to be completed.

### D.1 Fault classification (measured, `d_faults`)

- Unmapped page (read, write, jump) and non-canonical addresses: `SIGSEGV`, `si_code` = `SEGV_ACCERR` (2) and never `SEGV_MAPERR`, `si_addr` = FAR = faulting address, ESR EC=0x24 (data) or 0x20 (instruction), DFSC=0x04 to 0x07 (translation).
- Page mapped but `PROT_NONE`, read-only on write, `PROT_READ|PROT_WRITE` on execution, MAP_JIT in execute mode (write) or in write mode (call), file beyond EOF, read-only shared file on write: `SIGBUS`, `si_code` = `BUS_ADRALN` (1) in all cases, `si_addr` = FAR. The ESR distinguishes: DFSC=0x0f (permission) for a write-protected or non-executable page, DFSC=0x07 (translation) for `PROT_NONE` and the truncated file.
- So on Darwin: unmapped = `SIGSEGV`, mapped with insufficient protection = `SIGBUS`. The `si_code` does not allow distinguishing the cases; the context's ESR (`uc_mcontext->__es.__esr`) does. The context's `__exception` field is always 0.
- Unaligned accesses: `ldr`, `ldp`, `ldr q`, `ldar` and the 32- and 64-bit exclusives inside an aligned 16-byte window raise no fault. An `ldar`, `stlr`, `ldaxr`, `ldadd`, `swp`, `cas` that crosses a 16-byte boundary raises `SIGBUS`/`BUS_ADRALN`, ESR DFSC=0x21 (alignment), `si_addr` = start address of the access. FEAT_LSE2 is present (`hw.optional.arm.FEAT_LSE2` = 1).
- An unaligned `pc` (2 mod 4): `SIGBUS`, ESR EC=0x22, `si_addr` = target. An unaligned `sp` used as base: `SIGBUS`, ESR EC=0x26, `si_addr` = 0.
- Access straddling a page boundary whose second page is inaccessible: the fault address (`si_addr` and FAR) is the first address of the second page, not the start of the access; no partial write took place (the 4 bytes of the first page stayed intact in the three variants).
- `brk #0`: `SIGTRAP`, `si_code` = 0, `si_addr` = pc of the instruction, ESR EC=0x3c. `udf`, `mrs ttbr0_el1`, `hvc`, `msr daifset`: `SIGILL`/`ILL_ILLTRP` (2), ESR EC=0x00 except `msr` (EC=0x18).
- The FPCR floating-point exception enable bits (IOE, DZE, OFE, UFE, IXE, IDE) read back as written and the exceptions are really raised: `1.0/0.0` with DZE gives `SIGILL`/`ILL_ILLTRP`, ESR EC=0x2c, `si_addr` = faulting instruction. Integer division by zero raises nothing (result 0).
- Top-byte-ignore is active for data accesses: a pointer whose top byte is 0xab accesses the same page.
- Stack overflow: main thread without alternate stack, dies by `SIGSEGV`; `pthread` thread without alternate stack, dies by `SIGILL` (the kernel cannot build the frame). With `sigaltstack` and `SA_ONSTACK` the handler runs: `SIGSEGV` (`SEGV_ACCERR`) on the main thread, `SIGBUS` (`BUS_ADRALN`) on a `pthread` thread (guard page mapped `PROT_NONE`).

### D.2 arm64 context (measured, `d_context`)

- `sizeof(siginfo_t)` = 104, `ucontext_t` = 56 bytes (fields `uc_onstack` 0, `uc_sigmask` 4, `uc_stack` 8, `uc_link` 32, `uc_mcsize` 40, `uc_mcontext` 48), the 816-byte `mcontext` follows immediately (`uc_mcontext` points at `uc + 56`). `sigset_t` is 4 bytes and `NSIG` is 32.
- In `struct __darwin_mcontext64`: `__es` (16 bytes: `__far` 0, `__esr` 8, `__exception` 12), `__ss` at 16 (x0 at 16, x28 at 240, fp 248, lr 256, sp 264, pc 272, cpsr 280, 272 bytes), `__ns` at 288 (v0 288, v31 784, fpsr 800, fpcr 804, 528 bytes).
- Read: the handler sees exactly x0 to x28, fp, lr, sp, v0 to v31, NZCV, fpsr and fpcr set before a `brk`, on the thread's stack as on an alternate stack. Write: x0 to x28, fp, lr, v0 to v31, NZCV, fpsr, fpcr, sp and pc modified by the handler are those that the resumed code observes after `sigreturn` (sp changed to another stack, pc to another function).
- Exception: **x18 is reset to 0 by the kernel**. The value written in the context is not restored (read back as 0 after `sigreturn`); x18 is also reset to 0 at every system call return (2,000,000 `svc #0x80` out of 2,000,000, and libSystem's `getpid()`) and during a loop without signal (1,500 to 5,000 changes per second, interrupts and scheduling). x18 is therefore unusable by JIT code.
- Signal frame: about 1,120 bytes above the stack (siginfo at top-1120, ucontext at top-1016); the minimal accepted alternate stack is `MINSIGSTKSZ` = 32,768 (`ENOMEM` below), `SIGSTKSZ` = 131,072. A created thread does not inherit the alternate stack of its creator (`SS_DISABLE`).

### D.3 Thread interruption (measured, `d_thread_signals`)

Durations are in microseconds; they are measured with `cntvct_el0` normalized by the process's `CNTFRQ_EL0` (see D.6).

- `pthread_kill` to a thread that loops in a `MAP_JIT` region: handler entry 3.1 µs (median), 3.6 µs (p90), 9.6 µs (p99); the handler sees a pc in the loop (4000 out of 4000); the `pthread_kill` call itself lasts 0.71 µs. The handler redirects the pc to a stub in the same region, in another `MAP_JIT` region, or in the host's text: 12,000 correct returns out of 12,000; thread back in C after `sigreturn`: 3.4 µs (median), 10 µs (p99).
- Blocked thread: `read` on a pipe, `os_sync_wait_on_address`, `usleep`, `nanosleep` all return with `EINTR` (500 out of 500, without `SA_RESTART`). Handler entry: 6.8 µs, 16.6 µs, 11.3 µs, 15.2 µs (medians).
- Stop and resume by signal (the handler waits on `os_sync_wait_on_address`): `pthread_kill` to parked handler 3.4 µs (median), 28.8 µs (p99); wakeup to thread running again 5.9 µs; full cycle 9.5 µs (median), 62 µs (p99). The thread is really stopped in 3000 cycles out of 3000.
- Stop through Mach (`thread_suspend`, `thread_get_state`, `thread_resume`) without any particular entitlement: `thread_suspend` returns in 6.9 µs, `thread_get_state` 1.2 µs, `thread_resume` to resumption 5.3 µs, cycle 13.6 µs (median); the pc read is in the JIT loop in 3000 cases out of 3000. Redirection of the pc to a JIT stub through `thread_set_state`: 2000 successes out of 2000, 12.4 µs from the suspension to the return to C.
- Masks: a signal directed to the process (`kill(getpid(), ...)`) is always delivered to the main thread if it does not block it (200 out of 200), otherwise to the first unblocked thread. Ten sends of the same signal while it is blocked give a single handler call. A thread inherits the mask of its creator. In a handler, the delivered signal and the signals of `sa_mask` are blocked; on return, the mask is that of `uc_sigmask` at the end of the handler: a change of `uc_sigmask` is applied, `pthread_sigmask` calls made in the handler are overwritten.
- There are no real-time signals (`SIGRTMIN` absent), no `sigqueue`, `sigtimedwait`, `sigwaitinfo`, `signalfd`, `timer_create`, `tgkill`.

### D.4 JIT write mode and signals (measured, `d_jit_signal_state`)

- On entry to a handler the thread is always in execute mode, whatever its mode at the time of the interruption (asynchronous signal, `pthread_kill` on itself or `brk`): a store into a JIT page raises `SIGBUS`, a JIT stub call succeeds.
- After `sigreturn`: if the thread was in write mode when the signal arrived, it is in write mode, whatever the handler did. If it was in execute mode, it keeps the mode that the handler left it in (so in write mode if the handler called `pthread_jit_write_protect_np(0)` and did not undo it).
- Measured consequence: a handler that redirects the pc to JIT code fails (`SIGBUS` at resumption) if the thread was in write mode, or if the handler left write mode; it succeeds if the thread was in execute mode and the handler changed nothing.
- A synchronous fault raised by code running in a JIT page: the handler starts in execute mode; a write mode left by the handler persists after `sigreturn`.

### D.5 To be completed

- Fault and resume round trip (self-modifying code detection through page protection), fault in the handler.

### D.6 Time counter (measured)

- `CNTFRQ_EL0` is 1,000,000,000 in a process launched by `exec` or `posix_spawn` (the counter advances 1.00 tick per ns), and 24,000,000 in a process created by `fork` without `exec` (0.02 tick per ns). `mach_absolute_time` advances at 24 MHz (ratio 125/3) in both cases. No raw `cntvct_el0` value must be compared between these two types of process.

## Implications for FEXCore on Darwin

to be completed

## What is not testable here

to be completed

## Files

to be completed

# Feedback on the engine witnesses w15, w02, w11, w14, w07 (signal action read as 0)

Requester: sig. Recipient: lead (engine and witnesses, which sig does not modify). Date: 2026-10-02.

## Cause of "lkd_signal_next = -1" on w15_async (MEASURED)

Temporary debug trace in `rt_sigaction` (removed since) on the lead's reproduction: the action read in guest memory is `handler = 0x0, flags = 0x4000004` for SIGUSR1 and SIGUSR2, on the action object shared by the two threads (same pointer seen in `complete_signal` for tid 3). SIG_DFL for SIGUSR1 kills the process: status 138. CLONE_SIGHAND sharing works.

The value 0 comes from the binary: the witnesses in `witnesses/elf-x86_64/bin` (built at 15:17) are static PIEs (`e_type = ET_DYN`, segments starting at 0, `PT_DYNAMIC`, no `PT_INTERP`) with `R_X86_64_RELATIVE` relocations that are never applied (read with a small Python ELF reader):

| Witness | RELATIVE relocations | Example |
| --- | --- | --- |
| w15_async | 2 | 0x99a0 <- base+0x5270 (handler), 0x99b0 <- base+0x4640 (restorer) |
| w02_int | 4 | 0x27dc0.. |
| w11_badjump | 2 | 0x9090, 0x90a0 |
| w14_overflow | 3 | 0x8df0.. |
| w07_fault | 1 | |
| w10_threads | 0 | |

`rt.h` has no self-relocation and Linux does not relocate a static PIE (that is the role of `_dlstart`/rcrt1.o, absent with `-nostdlib`). The `struct ksigaction` templates initialized with `(u64)handler` are copied from .data with the field at 0. Under Linux the same binary would have the same defect. Possible fix on the witness side: link as ET_EXEC (`-no-pie`, as `build.sh` plans with `--image-base`), or add a self-relocation to `rt.h`.

## What passes once the binaries are relocated (MEASURED, copies relocated for base 0x2aaaaaaaa000 in my working directory, host built against the current tree)

- w02_int: PASS 9524/9524.
- w14_overflow: PASS 7/7 (stack overflow caught on the alternate stack).
- w10_threads: PASS 13/13.
- w07_fault: all cases up to "register edit" pass (SEGV_MAPERR/ACCERR, si_addr, trapno, err, cr2, mask in the handler, saved mask, return, xmm5, register editing).

## What remains on the engine side (MEASURED, INFERRED for the cause)

1. Execution of a non-executable or unmapped address: FEXCore loops on `[FEXCore E] NoExec instruction in entry block: 3FFFF7FF0000` (w07, case "non-exec fetch") and `... : 9` (w11) without calling `lkd_fault`. Expected: `lkd_fault(t, SIGSEGV, SEGV_ACCERR, target, 0x15, 14)` for a non-executable page, `SEGV_MAPERR` with si_addr = 9 for w11. The log grows by hundreds of MiB per minute (w07: 657 MiB in 60 s).
2. w15_async: delivery is correct (`lkd_signal_next = 1`, the handler runs on the right thread, SI_TKILL and SI_USER correct), but the register invariants fail intermittently: first try "spinner registers intact" false, second try "main thread registers intact" and "spinner made progress" false. INFERRED: the context saved at a kick in the middle of translated code is not exact (reconstruction at an instruction boundary, or rcx/r11/xmm in `FrameBuild`/`FrameRestore`). LKD does not touch the registers: `frame_build` and `frame_restore` are engine operations, LKD provides only mask, siginfo and alternate stack.

## Non-regression test added on the LKD side

`tests/test_sig_more.c`, case `clone_thread_shares_handler`: task created by `lkd_task_create_main`, `rt_sigaction(SIGUSR1)`, real `clone` (task module) with the flags of w15, `tgkill` to the new thread, `lkd_signal_next(child) == 1` and the frame carries the handler.

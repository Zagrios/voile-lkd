# Request task-4 to the mm module: address space reset in time proportional to the VMAs

Date: 2026-10-02. Author: task module, following the hang reported by the project lead (`/bin/sh -c '/bin/true; echo status=$?'` under the real engine).

## Finding (MEASURED)

1. Under the real engine (window of 2^46 bytes), `sample` of the host process of the vfork child (guest pid 3) during the hang: `do_execve -> lkd_exec_commit -> lkd_mm_exec_commit -> lkd_mm_reset_for_exec -> mm_unmap_locked -> mm_host_cleanup -> page_has_vma -> vt_first_end_after`, state `R` (it is computing, it is not blocked). The child therefore never exits and the shell rightly waits in `wait4`. With `LKD_TRACE=1`, the child finishes, but the shell then stays in its own `exit_group` (`lkd_mm_task_exit -> lkd_mm_put -> mm_destroy`, same walk). The witness `w02_int` launched by the sig agent showed the same stack (`lkd_mm_task_exit -> lkd_mm_put`).
2. Cause (READ, `src/mm/mm.c`, `mm_host_cleanup`): `mm_unmap_locked(mm, g_lo, g_hi)` calls `mm_host_cleanup` on the whole window, whose inner loop `while (q < pe && !page_has_vma(mm, q)) q += mm_hps;` advances by one 16 KiB host page at a time: 2^46 / 2^14 = 2^32 iterations, each with a tree lookup.
3. Fix tried in a COPY of the tree (nothing was modified in `src/mm`): jump directly to the page of the next VMA:
   ```c
   mm_vma *nv = vt_first_end_after(&mm->vt, p);
   uint64_t q = nv ? HP_DOWN(nv->start) : pe;
   if (q > pe) q = pe;
   if (q <= p) q = p + mm_hps;
   ```
   With this copy, the engine runs `/bin/sh -c '/bin/true; echo status=$?'` in 1 s, with or without `LKD_TRACE`, prints `status=0` and ends with `exit_group(0)`.
4. Non-regression test added: `tests/test_task_bigwin.c` (vfork, execve, wait4 then exit_group with a window of 2^46 bytes at 8 TiB). Current tree: 20,114 ms, FAIL (bound 5 s). Fixed copy: 524 ms, PASS.

## Request

Make `mm_host_cleanup` (and any walk of the whole window: `mm_destroy`, `lkd_mm_reset_for_exec`) proportional to the number of VMAs and of host pages really in use, for example with the jump above. `mm_host_reset` on a large empty interval stays a single call.

Note: `lkd_mm_window_size()` is not defined by mm (core's replacement returns 0); the test uses `lkd_guest_base != 0` to know whether the window is reserved.


## Handled on 2026-10-02 (integration maintainer)

Fixed by mm (`mm_host_cleanup` in O(VMA)). MEASURED: `test_task_bigwin` passes in less than a second in `build-integration`.

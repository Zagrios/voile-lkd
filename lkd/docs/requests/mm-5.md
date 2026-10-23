# Request mm-5 (core): fault recovery in LKD copies must go through mm for a watched page

Context: write-protected pages of translated code (`docs/status/mm.md`, `src/mm/lkd_mm_watch.h`). `lkd_user_write` already goes through `lkd_mm_check_range`, which mm has strengthened: it unprotects the watched page before the copy. A race remains: another engine thread can translate code and watch the page again between the check and the copy. The copy then faults in `lkd_uc_copy`.

## MEASURED (scratch, without touching src/core)

Copy onto a watched page without a prior check (no task bound):

- current `src/core/usermem.c`: `lkd_user_write` returns -14 (EFAULT), 0 invalidations, page still watched.
- With the fix below: `lkd_user_write` returns 0, 1 invalidation, page unprotected, the value is written.

Important finding for the engine (MEASURED, ESR 0x92000046): a store to a read-only page that is not resident yet gives a TRANSLATION fault (DFSC 0x06), not a permission fault (0x0f). No filter on DFSC must be applied: the correct filter is "data abort (EC 0x24) with WnR = 1".

## Exact fix (src/core/usermem.c)

```diff
--- a/src/core/usermem.c
+++ b/src/core/usermem.c
@@ -17,6 +17,7 @@
 
 #include "lkd_internal.h"
 #include "mm.h"
+int lkd_mm_code_write_fault(uint64_t gva); /* mm.h after the patch; weak 0 in stubs_mm.c */
 
 uint64_t lkd_guest_base;
 
@@ -74,6 +75,16 @@
     ucontext_t *uc = ctx;
     uintptr_t pc = (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
     if (pc >= (uintptr_t)lkd_uc_copy_begin && pc < (uintptr_t)lkd_uc_copy_end) {
+        /* A store into a page whose write permission mm removed because it holds translated code
+         * (src/mm/watch.c): mm drops the translations and restores the permission; return without
+         * touching the context so the store is executed again. Only data aborts with WnR set (a store).
+         * The fault status is NOT filtered: macOS reports a store to a non-resident read-only page as a
+         * translation fault (DFSC 0x06, MESURE), a resident one as a permission fault (0x0f). */
+        uint32_t esr = uc->uc_mcontext->__es.__esr;
+        uint32_t ec = esr >> 26;
+        if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6)) &&
+            lkd_mm_code_write_fault(lkd_h2g((const void *)(uintptr_t)uc->uc_mcontext->__es.__far)) == 1)
+            return 1;
         void *lr = (void *)arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
         arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, lr);
         uc->uc_mcontext->__ss.__x[0] = (uint64_t)-1;
```

## Other requested changes

1. `src/include/mm.h`: add `int lkd_mm_code_write_fault(uint64_t gva);` (then remove the fix's local declaration) and `#include "../../src/mm/lkd_mm_watch.h"` is not needed: the engine can include `src/mm/lkd_mm_watch.h`, self-contained and C++-compatible.
2. `src/core/stubs_mm.c`: `LKD_WEAK int lkd_mm_code_write_fault(uint64_t gva) { (void)gva; return 0; }` and likewise `lkd_mm_watch_code` (returns 0) for the tests of the other modules. mm defines them strong in `src/mm/watch.c`.
3. Locks: `lkd_mm_code_write_fault` takes the watch lock (recursive mutex, never held during guest code, during `ops->invalidate_code` or during an `lkd_user_*` copy) and the VMA read lock. The signal handler therefore calls a mutex: acceptable because the interrupted code is guest code or `lkd_uc_copy` (never code that holds these locks). `ops->invalidate_code` is called from the handler: it must take no lock that interrupted guest code could hold.


## Handled on 2026-10-02 (integration maintainer)

Fix present in `src/core/usermem.c` (`lkd_fault_try_recover`) and declarations present. Verified by `test_mm_watch` and `test_core_usermem`.

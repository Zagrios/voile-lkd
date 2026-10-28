/* mm.h: guest address space, VMA table, user memory validation. OWNER: mm.
 * Core provides weak placeholders (src/core/stubs_mm.c): lkd_mm_check_range allows everything, the
 * rest returns -ENOSYS. lkd_user_* (core) validate through lkd_mm_check_range, so unit tests of other
 * modules can use host memory as guest memory with lkd_guest_base == 0.
 *
 * Memory model (decision doc section 3): the guest sees 4 KiB pages and a window of the host address
 * space (lkd_guest_base .. +2^46). The host page is 16 KiB, so the host protection of a 16 KiB page is
 * the union of the protections of its four 4 KiB sub-pages and the VMA table keeps the exact view. A
 * guest access that the host allows but the VMA table forbids (PROT_NONE sub-page inside an accessible
 * host page) is caught by lkd_mm_check_range for kernel-side accesses; for guest code it is a
 * documented fidelity limit.
 */
#ifndef LKD_MM_H
#define LKD_MM_H

#include "task.h"

struct lkd_fdobj;
struct lkd_mm; /* private to mm; one per address space, refcounted, shared by CLONE_VM threads */
typedef struct lkd_mm lkd_mm;

/* Identity of a futex word (see futex.h). */
struct lkd_futex_key { uint8_t kind; uint64_t a; uint64_t b; };
enum { LKD_FK_PRIVATE = 1, LKD_FK_SHARED = 2 };

/* One VMA as /proc/<pid>/maps shows it. */
struct lkd_vma_info {
    uint64_t start, end;           /* guest addresses, 4 KiB aligned, end exclusive */
    uint32_t prot;                 /* LX_PROT_READ|WRITE|EXEC */
    uint32_t flags;                /* LX_MAP_SHARED or LX_MAP_PRIVATE, plus LX_MAP_GROWSDOWN etc. */
    uint64_t file_offset;          /* byte offset in the file */
    uint32_t dev_major, dev_minor; /* Linux device numbers of the file, 0 for anonymous */
    uint64_t ino;
    char name[256];                /* path, "[heap]", "[stack]", "[vdso]", "[vvar]", "/memfd:x", or "" */
    uint8_t deleted;               /* file unlinked: render " (deleted)" */
};

/* ---- The guest window ------------------------------------------------------------------------- */

/* lkd_mm_init() reserves the guest window once (PROT_NONE, MAP_NORESERVE, lkd_config.guest_window_bytes,
 * default 2^46, at lkd_config.guest_window_base or a base the module picks from the measured free map,
 * with a fallback search) and sets lkd_guest_base. The window is never released. In a fork child the
 * reservation is inherited at the same address. Returns the window size in bytes (0 before init). */
uint64_t lkd_mm_window_size(void);

/* ---- Address space objects -------------------------------------------------------------------- */

lkd_mm *lkd_mm_create(void);                 /* empty address space; the first one reserves the window */
lkd_mm *lkd_mm_get(lkd_mm *mm);
void lkd_mm_put(lkd_mm *mm);
/* Clone path: CLONE_VM returns `parent` with a reference. Without CLONE_VM the child process was
 * created by host fork() and inherits the private memory copy-on-write: this call is then made in the
 * child by lkd_mm_fork_child and returns the (re-identified) mm. */
lkd_mm *lkd_mm_clone(lkd_mm *parent, uint64_t clone_flags);
uint64_t lkd_mm_identity(const lkd_mm *mm);  /* unique per address space within the family (futex keys) */
/* Exec: unmap everything except the window reservation, reset brk, VMA table empty. */
void lkd_mm_reset_for_exec(lkd_mm *mm);

/* ---- Mapping (used by the mmap syscalls and by the engine's ELF loader / exec) ------------------ */

/* Map len bytes at addr (hint or fixed per flags: LX_MAP_*; prot: LX_PROT_*). file may be NULL for
 * anonymous memory; offset is the byte offset in the file. name overrides the maps label when non-NULL.
 * Returns the mapped address, or -LX_E* (-LX_ENOMEM, -LX_EEXIST for FIXED_NOREPLACE, -LX_EACCES,
 * -LX_ENODEV, -LX_EINVAL...). */
int64_t lkd_mm_map(lkd_mm *mm, uint64_t addr, uint64_t len, int prot, uint32_t flags,
                   struct lkd_fdobj *file, uint64_t offset, const char *name);
int lkd_mm_unmap(lkd_mm *mm, uint64_t addr, uint64_t len);                 /* 0 or -errno */
int lkd_mm_protect(lkd_mm *mm, uint64_t addr, uint64_t len, int prot);     /* 0 or -errno */
int64_t lkd_mm_remap(lkd_mm *mm, uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint32_t flags, uint64_t new_addr);
int64_t lkd_mm_brk(lkd_mm *mm, uint64_t new_brk);                          /* returns the new break */
/* VMA containing addr, or the first one above it when `next` is nonzero. Returns 0 and fills *out, or
 * -LX_ENOENT. */
int lkd_mm_find(lkd_mm *mm, uint64_t addr, int next, struct lkd_vma_info *out);
/* lkd_mm_find without waiting: -LX_EBUSY when the VMA table is being changed. For a report written from a fatal
 * signal handler, where the faulting thread may hold the table's lock itself. */
int lkd_mm_find_nowait(lkd_mm *mm, uint64_t addr, struct lkd_vma_info *out);
/* Is any writable shared mapping of this file/memfd present? (fs asks for F_SEAL_WRITE: EBUSY.) */
int lkd_mm_has_writable_shared_mapping(lkd_mm *mm, struct lkd_fdobj *file);
/* fs wrote [off, off + len) of the file through its descriptor (write, pwrite...): drop the translated code
 * of every view of those bytes mapped in this process (request wine-7, aliased code). ANY, no LKD lock. */
void lkd_mm_file_written(struct lkd_fdobj *file, uint64_t off, uint64_t len);
/* Register or replace the [vdso]/[vvar]/[stack] pseudo VMAs by name (engine/loader use). */
int lkd_mm_set_vma_name(lkd_mm *mm, uint64_t addr, uint64_t len, const char *name);

/* ---- Validation (hot path of every lkd_user_*) ------------------------------------------------- */

/* Is [gva, gva+len) mapped with the needed access in the VMA table (readable, or writable when
 * need_write)? Returns 0 or -LX_EFAULT. ANY thread, takes only the VMA leaf lock (LKD_RANK_VMA) in read
 * mode, never blocks otherwise, safe under any other lock. Advisory against concurrent munmap: the host
 * fault recovery of lkd_user_* is the real safety net. The core default (weak) returns 0 for any range;
 * the mm module's strong definition replaces it. Uses the current task's mm (lkd_current()). */
int lkd_mm_check_range(uint64_t gva, size_t len, int need_write);

/* Translated-code pages (src/mm/lkd_mm_watch.h has the full contract): a store fault on a write-protected page
 * that holds translated code. Returns 1 when mm dropped the translations and restored the permission (retry the
 * store), 0 for a real guest fault. Core calls it from the usercopy fault recovery; the engine from its handler. */
int lkd_mm_code_write_fault(uint64_t gva);
/* Guarded host pages (lkd_mm_watch.h has the contract). */
int lkd_mm_guard_fault(uint64_t gva, int write, uint64_t *page_out);
int lkd_mm_guard_read(uint64_t gva, void *dst, unsigned len);
int lkd_mm_guard_aliases(uint64_t gva, const void *alias[4], const uint64_t **gen, uint64_t *gen_now);
void lkd_mm_guard_close(uint64_t page);
void lkd_mm_guard_close_all(void);

/* ---- /proc support ---------------------------------------------------------------------------- */

/* /proc/<pid>/maps of an address space of THIS process: malloc'ed text (caller frees), length in
 * *len_out. Linux format: "%08lx-%08lx rwxp %08llx %02x:%02x %lu" then padding to column 73 and the path,
 * exactly as fs/proc/task_mmu.c show_map_vma prints (" (deleted)" after an unlinked file). */
char *lkd_mm_render_maps(lkd_mm *mm, size_t *len_out);
char *lkd_mm_render_smaps(lkd_mm *mm, size_t *len_out);
/* Loader facts after exec (request mm-2): start of the heap (brk; without it brk reads the layout of
 * lkd_proc_set_layout, else answers 0 like an absent heap), the stack address that names [stack], the top
 * of the top-down mmap search (default: window top - 128 MiB). */
void lkd_mm_set_brk_start(lkd_mm *mm, uint64_t start_brk);
void lkd_mm_set_stack_start(lkd_mm *mm, uint64_t start_stack);
void lkd_mm_set_mmap_base(lkd_mm *mm, uint64_t base);
/* Guest address range of the window, [lo, hi): lo is 0 in production, hi is the guest TASK_SIZE (2^46 by
 * default, smaller than Linux's 0x7ffffffff000: request mm-2 point 3). */
uint64_t lkd_mm_window_guest_lo(void);
uint64_t lkd_mm_window_guest_hi(void);

/* Counters for /proc/<pid>/status and statm (Vm* of the guest's VMAs; RSS of the whole host process, see
 * host_rss in mm.c). */
struct lkd_mm_stats {
    uint64_t total_vm, locked_vm, data_vm, stack_vm, exec_vm, rss_anon, rss_file, rss_shmem, pte_bytes, nmaps;
    /* high-water marks for VmPeak and VmHWM (request procfs-2), never below the current values.
     * hiwater_vm is exact (recorded before every unmap, like update_hiwater_vm); hiwater_rss is recorded
     * at every stats read and before every unmap, so a short-lived resident peak between two can be
     * missed (lower bound). Both restart at exec. */
    uint64_t hiwater_vm, hiwater_rss;
};
int lkd_mm_stats(lkd_mm *mm, struct lkd_mm_stats *out);

/* ---- Cross-process memory service (ptrace PEEK/POKE, /proc/<pid>/mem, process_vm_*) --------------- */

/* Read/write the guest memory of another process of the family. The transport (Mach task port of a child,
 * or a request to a helper thread of the target process) is the mm module's choice. After a write the
 * target's ops->invalidate_code is called for the range when it intersects executable VMAs, before the
 * call returns. Return bytes transferred (may be short at the first unmapped byte) or -errno
 * (-LX_ESRCH, -LX_EPERM, -LX_EFAULT). May block; permissions are the caller's job (ptrace.h). */
int64_t lkd_mm_remote_read(int32_t pid, uint64_t gva, void *dst, size_t n);
int64_t lkd_mm_remote_write(int32_t pid, uint64_t gva, const void *src, size_t n);
/* maps of another process of the family (malloc'ed, caller frees) or NULL with *err = -errno. */
char *lkd_mm_remote_render_maps(int32_t pid, size_t *len_out, int *err);

/* ---- Futex keys ------------------------------------------------------------------------------- */

/* Key of the aligned 4-byte word at gva for the futex implementation (sig module). Shared mappings
 * (MAP_SHARED, memfd, SysV shm) give LKD_FK_SHARED with a backing id that is equal in every process of
 * the family; everything else LKD_FK_PRIVATE with the mm identity. private_hint (FUTEX_PRIVATE_FLAG)
 * allows skipping the VMA lookup. Returns 0, -LX_EFAULT, -LX_EINVAL (misaligned). */
int lkd_mm_futex_key(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key);

/* ---- Fork ------------------------------------------------------------------------------------- */
/* lkd_mm_fork_prepare/parent/child are the standard module hooks (lkd_internal.h): prepare takes the
 * mmap lock and the VMA lock, the host fork copies private anonymous memory copy-on-write, shared
 * mappings stay shared (shared anonymous memory is backed by a shared memory object), parent releases,
 * child releases and re-identifies the mm. */

#endif /* LKD_MM_H */

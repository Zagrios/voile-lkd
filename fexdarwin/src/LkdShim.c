// SPDX-License-Identifier: MIT
// The only engine file that uses LKD internal headers (src/lkd/src/include). It answers memory-map questions of the
// engine from LKD's VMA table until LKD exports an equivalent public call (request src/lkd/docs/requests/engine-1.md).
#include <lkd/lkd.h>

#include "mm.h"
#include "task.h"
#include "../mm/lkd_mm_watch.h"

// VMA of the current task containing gva: 0 with [start, end) and LX_PROT_* bits, or -LX_ENOENT if unmapped.
int fexdarwin_lkd_vma_at(uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot) {
  lkd_task* t = lkd_current();
  if (!t || !t->mm) return -LX_ENOENT;
  struct lkd_vma_info v;
  int r = lkd_mm_find(t->mm, gva, 0, &v);
  if (r) return r;
  *start = v.start;
  *end = v.end;
  *prot = v.prot;
  return 0;
}

// VMA of task t containing gva, with its name ("" for anonymous) and file offset: for the thread dump, from any host thread.
// With nowait, -LX_EBUSY instead of waiting for the VMA table (the fatal fault report).
static int vma_describe(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot, uint64_t* off,
                        char* name, unsigned long name_len, int nowait) {
  if (!t || !t->mm) return -LX_ENOENT;
  struct lkd_vma_info v;
  int r = nowait ? lkd_mm_find_nowait(t->mm, gva, &v) : lkd_mm_find(t->mm, gva, 0, &v);
  if (r) return r;
  *start = v.start;
  *end = v.end;
  *prot = v.prot;
  *off = v.file_offset;
  if (name_len) {
    unsigned long i = 0;
    for (; i + 1 < name_len && v.name[i]; i++) name[i] = v.name[i];
    name[i] = 0;
  }
  return 0;
}
int fexdarwin_lkd_vma_describe(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot, uint64_t* off,
                               char* name, unsigned long name_len) {
  return vma_describe(t, gva, start, end, prot, off, name, name_len, 0);
}
int fexdarwin_lkd_vma_describe_nowait(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot, uint64_t* off,
                                      char* name, unsigned long name_len) {
  return vma_describe(t, gva, start, end, prot, off, name, name_len, 1);
}

// comm of task t (profile): the owner may rename it meanwhile, so the copy is only as exact as a log needs.
void fexdarwin_lkd_task_comm(lkd_task* t, char* out, unsigned long len) {
  unsigned long i = 0;
  for (; t && i + 1 < len && i < LKD_COMM_LEN && t->comm[i]; i++) out[i] = t->comm[i];
  if (len) out[i] = 0;
}

// fs module (src/lkd/src/fs/lkd_fs.h): the host stdin, stdout and stderr become guest fds 0, 1, 2 of the first task.
int lkd_fs_install_stdio(lkd_task* t);
int fexdarwin_lkd_install_stdio(lkd_task* t) {
  return lkd_fs_install_stdio(t);
}

// mm module (src/lkd/src/mm/lkd_mm_ext.h, requested for mm.h in docs/requests/mm-2.md): loader facts of the new image.
void lkd_mm_set_brk_start(lkd_mm* mm, uint64_t start_brk);
void lkd_mm_set_stack_start(lkd_mm* mm, uint64_t start_stack);
int fexdarwin_lkd_set_image_layout(lkd_task* t, uint64_t brk_start, uint64_t stack_start) {
  if (!t || !t->mm) return -LX_ESRCH;
  lkd_mm_set_brk_start(t->mm, brk_start);
  lkd_mm_set_stack_start(t->mm, stack_start);
  return 0;
}

// task module: layout of the new image (start_code, end_code, start_data, end_data, start_brk, start_stack, arg_start,
// arg_end, env_start, env_end, entry) and a copy of the auxiliary vector.
void fexdarwin_lkd_set_proc_layout(lkd_task* t, const uint64_t v[11], const void* auxv, size_t auxv_len) {
  struct lkd_proc_layout l = {
    .start_code = v[0], .end_code = v[1], .start_data = v[2], .end_data = v[3], .start_brk = v[4], .start_stack = v[5],
    .arg_start = v[6], .arg_end = v[7], .env_start = v[8], .env_end = v[9], .entry = v[10],
    .auxv = (uint8_t*)auxv, .auxv_len = auxv_len,
  };
  lkd_proc_set_layout(t, &l);
}

// vfs (src/lkd/src/include/vfs.h) and fs: composition of the guest file system from host directories.
#include "vfs.h"
void lkd_fs_set_root_ro(int ro);
int fexdarwin_lkd_bind(const char* guest_path, const char* host_dir, int ro) {
  return lkd_vfs_mount_bind(guest_path, host_dir, ro);
}
void fexdarwin_lkd_root_ro(int ro) {
  lkd_fs_set_root_ro(ro);
}

// Translated-code pages (mm module, src/mm/lkd_mm_watch.h): write-protect the pages FEXCore translated from, and take
// the first store to such a page. Wrapped here because LKD headers are C and the engine files are C++.
int fexdarwin_lkd_watch_code(uint64_t gva, uint64_t len) {
  return lkd_mm_watch_code(gva, len);
}
int fexdarwin_lkd_code_write_fault(uint64_t gva) {
  return lkd_mm_code_write_fault(gva);
}
int fexdarwin_lkd_code_watched(uint64_t gva) {
  return lkd_mm_code_watched(gva);
}
// Guarded host pages (lkd_mm_watch.h): PROT_NONE on the host, decided per 4 KiB of the guest's protection.
int fexdarwin_lkd_guard_fault(uint64_t gva, int write, uint64_t* page) {
  return lkd_mm_guard_fault(gva, write, page);
}
int fexdarwin_lkd_guard_read(uint64_t gva, void* dst, unsigned len) {
  return lkd_mm_guard_read(gva, dst, len);
}
int fexdarwin_lkd_guard_aliases(uint64_t gva, const void* alias[4], const uint64_t** gen, uint64_t* gen_now) {
  return lkd_mm_guard_aliases(gva, alias, gen, gen_now);
}
void fexdarwin_lkd_guard_close(uint64_t page) {
  lkd_mm_guard_close(page);
}
void fexdarwin_lkd_guard_close_all(void) {
  lkd_mm_guard_close_all();
}

// procfs (src/lkd/src/procfs/lkd_procfs.h): /proc/cpuinfo and /sys cache files are rendered from the CPUID the guest
// itself executes, which only the engine can answer. fexdarwin_cpuid is defined in LkdGlue.cpp.
#include "../procfs/lkd_procfs.h"
extern int fexdarwin_cpuid(uint32_t leaf, uint32_t subleaf, int cpu, uint32_t out[4]);
static int cpuid_trampoline(uint32_t leaf, uint32_t subleaf, int cpu, struct lkd_cpuid_regs* out) {
  uint32_t r[4];
  if (fexdarwin_cpuid(leaf, subleaf, cpu, r) != 0) return -1;
  out->eax = r[0]; out->ebx = r[1]; out->ecx = r[2]; out->edx = r[3];
  return 0;
}
void fexdarwin_lkd_register_cpuid(void) {
  lkd_procfs_set_cpuid(cpuid_trampoline);
}

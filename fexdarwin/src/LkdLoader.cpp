// SPDX-License-Identifier: MIT
// ELF loader for LKD mode. Like the Linux binfmt_elf loader it maps the executable and its PT_INTERP (ld.so) from files of
// the guest root, builds the initial stack (argc, argv, envp, auxv) and sets the entry registers. It only uses LKD system
// calls on behalf of the task (openat, pread64, mmap, mprotect, munmap, close), so the VMA table, /proc/self/maps and
// file identities are LKD's own. Guest pages are 4 KiB (AT_PAGESZ = 4096).
#include "Host.h"
#include "LkdGlue.h"
#include "LkdLoader.h"

#include <FEXCore/Core/X86Enums.h>
#include <lkd/syscall_nr.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace X = FEXCore::X86State;

uint64_t LkdStackTop = 0x3fff'fffe'0000ULL;

// Kernel-internal call of LKD (src/lkd/src/include/lkd_internal.h): runs a syscall handler without the task
// bookkeeping, ptrace stops and tracing. The loader is kernel code: a tracer of the task must not see its mmap calls
// (an execve under PTRACE_SYSCALL shows the execve entry and exit only).
extern "C" int64_t lkd_dispatch(lkd_task* t, const lkd_sysreq* rq);

extern "C" int fexdarwin_lkd_set_image_layout(lkd_task* t, uint64_t brk_start, uint64_t stack_start);
extern "C" void fexdarwin_lkd_set_proc_layout(lkd_task* t, const uint64_t v[11], const void* auxv, size_t auxv_len);

namespace {
struct Elf64Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff; uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Elf64Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
constexpr uint32_t PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6, PT_GNU_STACK = 0x6474e551;
constexpr uint64_t Page = 4096;
constexpr uint64_t PageDown(uint64_t V) { return V & ~(Page - 1); }
constexpr uint64_t PageUp(uint64_t V) { return (V + Page - 1) & ~(Page - 1); }

struct Loader {
  lkd_task* T;
  uint64_t Scratch {}; // one guest page for path strings
  std::string Err;

  int64_t Sys(uint64_t Nr, uint64_t A0 = 0, uint64_t A1 = 0, uint64_t A2 = 0, uint64_t A3 = 0, uint64_t A4 = 0, uint64_t A5 = 0) {
    lkd_sysreq Rq {Nr, {A0, A1, A2, A3, A4, A5}};
    return lkd_dispatch(T, &Rq);
  }

  int64_t Open(const std::string& GuestPath) {
    if (GuestPath.size() + 1 > Page) return -LX_ENAMETOOLONG;
    if (lkd_user_write(Scratch, GuestPath.c_str(), GuestPath.size() + 1) != 0) return -LX_EFAULT;
    return Sys(LKD_NR_openat, static_cast<uint64_t>(-100), Scratch, 0 /*O_RDONLY*/, 0);
  }

  bool PreadAll(int64_t Fd, void* Dst, uint64_t Len, uint64_t Off) {
    // pread64 into the scratch page, then copy out, in page-sized chunks
    uint64_t Done = 0;
    while (Done < Len) {
      const uint64_t N = std::min(Len - Done, Page);
      const int64_t R = Sys(LKD_NR_pread64, Fd, Scratch, N, Off + Done);
      if (R <= 0) return false;
      if (lkd_user_read(Scratch, static_cast<uint8_t*>(Dst) + Done, R) != 0) return false;
      Done += R;
    }
    return true;
  }

  struct Image {
    uint64_t Bias {}, Entry {}, PhdrAddr {}, PhNum {}, LoadLow {}, LoadHigh {};
    uint64_t StartCode {~0ULL}, EndCode {}, StartData {}, EndData {}; // as load_elf_binary computes them
    std::string Interp;
  };

  // Map one ELF file. Base is used for ET_DYN.
  bool Map(const std::string& GuestPath, uint64_t Base, Image* Out) {
    const int64_t Fd = Open(GuestPath);
    if (Fd < 0) { Err = GuestPath + ": open failed " + std::to_string(Fd); return false; }
    Elf64Ehdr Eh {};
    if (!PreadAll(Fd, &Eh, sizeof Eh, 0) || memcmp(Eh.ident, "\177ELF", 4) != 0 || Eh.ident[4] != 2 || Eh.machine != 62) {
      Err = GuestPath + ": not an x86-64 ELF64 file";
      Sys(LKD_NR_close, Fd);
      return false;
    }
    std::vector<Elf64Phdr> Ph(Eh.phnum);
    if (!PreadAll(Fd, Ph.data(), Eh.phnum * sizeof(Elf64Phdr), Eh.phoff)) { Err = GuestPath + ": cannot read program headers"; return false; }
    uint64_t Lo = ~0ULL, Hi = 0;
    for (auto& P : Ph) {
      if (P.type == PT_LOAD) { Lo = std::min(Lo, PageDown(P.vaddr)); Hi = std::max(Hi, PageUp(P.vaddr + P.memsz)); }
      if (P.type == PT_INTERP) {
        std::string S(P.filesz, '\0');
        if (!PreadAll(Fd, S.data(), P.filesz, P.offset)) { Err = "cannot read PT_INTERP"; return false; }
        Out->Interp = S.c_str();
      }
    }
    const bool Dyn = Eh.type == 3;
    uint64_t Bias = 0;
    if (Dyn) {
      // Reserve the whole image first (like load_elf_binary's total_mapping_size), then map segments with MAP_FIXED.
      const int64_t R = Sys(LKD_NR_mmap, Base, Hi - Lo, 0 /*PROT_NONE*/, 0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, static_cast<uint64_t>(-1), 0);
      if (R < 0 && R > -4096) { Err = GuestPath + ": reserve failed " + std::to_string(R); return false; }
      Bias = static_cast<uint64_t>(R) - Lo;
    }
    for (auto& P : Ph) {
      if (P.type != PT_LOAD) continue;
      const int Prot = ((P.flags & 4) ? 1 : 0) | ((P.flags & 2) ? 2 : 0) | ((P.flags & 1) ? 4 : 0);
      const uint64_t SegStart = PageDown(P.vaddr + Bias);
      const uint64_t FileEnd = P.vaddr + Bias + P.filesz;
      const uint64_t MemEnd = P.vaddr + Bias + P.memsz;
      const uint64_t PgOff = PageDown(P.offset);
      if (P.filesz) {
        const int64_t R = Sys(LKD_NR_mmap, SegStart, PageUp(FileEnd) - SegStart, Prot | 2 /*writable while loading*/, 0x12 /*MAP_PRIVATE|MAP_FIXED*/, Fd, PgOff);
        if (R < 0 && R > -4096) { Err = GuestPath + ": segment mmap failed " + std::to_string(R); return false; }
        // zero the tail of the last file page (bss start)
        if (MemEnd > FileEnd && PageUp(FileEnd) > FileEnd) {
          if (lkd_user_zero(FileEnd, PageUp(FileEnd) - FileEnd) != 0) { Err = "bss zeroing failed"; return false; }
        }
      }
      // A segment without file data (pure bss) still owns the page that holds it: Linux maps it anonymously (elf_load
      // calls vm_brk_flags from the page start when p_filesz is 0), even when the bss fits in one page.
      if (PageUp(MemEnd) > PageUp(FileEnd) || (!P.filesz && MemEnd > SegStart)) {
        const uint64_t A = P.filesz ? std::max(PageUp(FileEnd), SegStart) : SegStart;
        const int64_t R = Sys(LKD_NR_mmap, A, PageUp(MemEnd) - A, Prot, 0x32 /*MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS*/, static_cast<uint64_t>(-1), 0);
        if (R < 0 && R > -4096) { Err = GuestPath + ": bss mmap failed " + std::to_string(R); return false; }
      }
      if (P.filesz && !(Prot & 2)) Sys(LKD_NR_mprotect, SegStart, PageUp(FileEnd) - SegStart, Prot);
      Out->StartCode = std::min(Out->StartCode, P.vaddr + Bias);
      if ((P.flags & 1) && FileEnd > Out->EndCode) Out->EndCode = FileEnd;
      if (P.vaddr + Bias > Out->StartData) Out->StartData = P.vaddr + Bias;
      if (FileEnd > Out->EndData) Out->EndData = FileEnd;
    }
    Sys(LKD_NR_close, Fd);
    Out->Bias = Bias;
    Out->Entry = Eh.entry + Bias;
    Out->PhNum = Eh.phnum;
    Out->LoadLow = Lo + Bias;
    Out->LoadHigh = Hi + Bias;
    Out->PhdrAddr = 0;
    for (auto& P : Ph) {
      if (P.type == PT_PHDR) Out->PhdrAddr = P.vaddr + Bias;
    }
    if (!Out->PhdrAddr) {
      for (auto& P : Ph)
        if (P.type == PT_LOAD && Eh.phoff >= P.offset && Eh.phoff < P.offset + P.filesz) Out->PhdrAddr = P.vaddr + Bias + (Eh.phoff - P.offset);
    }
    return true;
  }
};
} // namespace

bool LkdLoadProgram(GuestThread* GT, const LkdLoadRequest& Req, std::string* Err) {
  Loader L {GT->Task};
  // scratch page and stack
  L.Scratch = static_cast<uint64_t>(L.Sys(LKD_NR_mmap, 0, Page, 3, 0x22, static_cast<uint64_t>(-1), 0));
  if (static_cast<int64_t>(L.Scratch) < 0) { *Err = "scratch mmap failed"; return false; }

  if (!Req.Cwd.empty()) {
    if (Req.Cwd.size() + 1 > Page || lkd_user_write(L.Scratch, Req.Cwd.c_str(), Req.Cwd.size() + 1) != 0) { *Err = "cwd too long"; return false; }
    const int64_t R = L.Sys(LKD_NR_chdir, L.Scratch);
    if (R < 0) { *Err = "chdir " + Req.Cwd + " failed " + std::to_string(R); return false; }
  }

  Loader::Image Exe, Interp;
  // ELF_ET_DYN_BASE of x86-64 Linux (2/3 of TASK_SIZE).
  if (!L.Map(Req.GuestPath, 0x5555'5555'4000ULL, &Exe)) { *Err = L.Err; return false; }
  uint64_t Entry = Exe.Entry, InterpBase = 0;
  if (!Exe.Interp.empty()) {
    // The interpreter goes where a plain mmap puts it (top-down below the mmap base), as load_elf_interp does.
    if (!L.Map(Exe.Interp, 0, &Interp)) { *Err = L.Err; return false; }
    Entry = Interp.Entry;
    InterpBase = Interp.LoadLow;
  }
  // brk starts at the end of the executable's bss, as load_elf_binary sets mm->start_brk (no randomization).
  fexdarwin_lkd_set_image_layout(GT->Task, Exe.LoadHigh, 0);
  L.Sys(LKD_NR_brk, 0);

  // Stack: 8 MiB below StackTop, PROT_READ|WRITE, then the Linux initial stack layout.
  const uint64_t StackSize = 8ULL << 20;
  // MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS | MAP_STACK.
  const int64_t SB = L.Sys(LKD_NR_mmap, Req.StackTop - StackSize, StackSize, 3, 0x32 | 0x20000, static_cast<uint64_t>(-1), 0);
  if (SB < 0 && SB > -4096) { *Err = "stack mmap failed " + std::to_string(SB); return false; }

  // Initial stack as Linux builds it (fs/exec.c copy_strings, fs/binfmt_elf.c create_elf_tables): from the top, one
  // null word, the execfn string, then the envp and argv strings in ascending order (arg_start..arg_end and
  // env_start..env_end are contiguous, /proc/<pid>/cmdline and environ read them), then the platform string, AT_RANDOM
  // bytes, alignment, and argc/argv/envp/auxv.
  uint64_t Sp = Req.StackTop - 8;
  auto PutBytes = [&](uint64_t At, const void* P, size_t N) { lkd_user_write(At, P, N); };
  const std::string ExecFnS = Req.ExecFn.empty() ? Req.GuestPath : Req.ExecFn;
  Sp -= ExecFnS.size() + 1;
  const uint64_t ExecFn = Sp;
  PutBytes(ExecFn, ExecFnS.c_str(), ExecFnS.size() + 1);
  auto PlaceStrings = [&](const std::vector<std::string>& V, std::vector<uint64_t>* Ptrs) -> std::pair<uint64_t, uint64_t> {
    size_t Total = 0;
    for (auto& S : V) Total += S.size() + 1;
    Sp -= Total;
    uint64_t At = Sp;
    for (auto& S : V) {
      Ptrs->push_back(At);
      PutBytes(At, S.c_str(), S.size() + 1);
      At += S.size() + 1;
    }
    return {Sp, Sp + Total};
  };
  std::vector<uint64_t> ArgP, EnvP;
  const auto EnvRange = PlaceStrings(Req.Envp, &EnvP);
  const auto ArgRange = PlaceStrings(Req.Argv, &ArgP);
  Sp -= 7;
  const uint64_t Platform = Sp;
  PutBytes(Platform, "x86_64", 7);
  uint8_t Rnd[16];
  arc4random_buf(Rnd, sizeof Rnd);
  Sp -= sizeof Rnd;
  const uint64_t Random = Sp;
  PutBytes(Random, Rnd, sizeof Rnd);
  Sp &= ~15ULL;
  std::vector<uint64_t> Aux = {
    3 /*AT_PHDR*/, Exe.PhdrAddr, 4 /*AT_PHENT*/, 56, 5 /*AT_PHNUM*/, Exe.PhNum, 6 /*AT_PAGESZ*/, Page,
    7 /*AT_BASE*/, InterpBase, 8 /*AT_FLAGS*/, 0, 9 /*AT_ENTRY*/, Exe.Entry, 11 /*AT_UID*/, Req.Uid, 12 /*AT_EUID*/, Req.Uid,
    13 /*AT_GID*/, Req.Gid, 14 /*AT_EGID*/, Req.Gid, 15 /*AT_PLATFORM*/, Platform, 16 /*AT_HWCAP*/, Req.HwCap,
    17 /*AT_CLKTCK*/, 100, 23 /*AT_SECURE*/, 0, 25 /*AT_RANDOM*/, Random, 26 /*AT_HWCAP2*/, 0, 31 /*AT_EXECFN*/, ExecFn,
    51 /*AT_MINSIGSTKSZ*/, 3632, 0 /*AT_NULL*/, 0};
  const size_t Words = 1 + ArgP.size() + 1 + EnvP.size() + 1 + Aux.size();
  if (Words & 1) Sp -= 8; // keep rsp 16-byte aligned at entry
  std::vector<uint64_t> Block;
  Block.push_back(ArgP.size());
  for (auto P : ArgP) Block.push_back(P);
  Block.push_back(0);
  for (auto P : EnvP) Block.push_back(P);
  Block.push_back(0);
  Block.insert(Block.end(), Aux.begin(), Aux.end());
  Sp -= Block.size() * 8;
  lkd_user_write(Sp, Block.data(), Block.size() * 8);

  // Layout facts for /proc/<pid>/stat, auxv and PR_GET_AUXV.
  const uint64_t Layout[11] = {Exe.StartCode, Exe.EndCode, Exe.StartData, Exe.EndData, Exe.LoadHigh, Sp,
                               ArgRange.first, ArgRange.second, EnvRange.first, EnvRange.second, Exe.Entry};
  fexdarwin_lkd_set_proc_layout(GT->Task, Layout, Aux.data(), Aux.size() * 8);

  L.Sys(LKD_NR_munmap, L.Scratch, Page);
  // mm->start_stack: the VMA holding it is reported as [stack] in /proc/<pid>/maps.
  fexdarwin_lkd_set_image_layout(GT->Task, Exe.LoadHigh, Sp);
  GT->StartRip = Entry;
  GT->StartRsp = Sp;
  GT->StartRax = 0;
  return true;
}

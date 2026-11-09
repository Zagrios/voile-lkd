// SPDX-License-Identifier: MIT
// int $0x80: Linux's 32-bit system call entry, which a kernel with IA32 emulation also serves for 64-bit code. Wine's
// new WoW64 makes one call through it in every thread that runs 32-bit code: set_thread_area, which gives the thread's
// 32-bit TEB a TLS entry of the GDT for %fs (alloc_fs_sel, dlls/ntdll/unix/signal_x86_64.c). FEXCore raises #GP for an
// int it does not handle; DarwinSignalDelegator::DeliverGuestSignal takes the one of set_thread_area and
// get_thread_area out of the fault path and the kick safe point calls HandleIa32Syscall. Any other int $0x80 keeps its
// #GP and SIGSEGV, as on a kernel without IA32 emulation: Windows code gets the access violation Windows gives it.
// The TLS entries live in the thread's GDT (GuestThread::Gdt), where FEXCore reads the descriptor of a selector that a
// segment register loads. The calls follow arch/x86/kernel/tls.c.
#include "Host.h"

#include <lkd/abi_x64.h>
#include <lkd/lkd.h>

#include <FEXCore/Core/X86Enums.h>

#include <cstdint>
#include <cstring>

extern "C" void fexdarwin_lkd_guard_close_all(void);

namespace {
using FEXCore::Core::CPUState;
namespace X = FEXCore::X86State;

constexpr uint32_t IA32_NR_set_thread_area = 243, IA32_NR_get_thread_area = 244;
// arch/x86/include/asm/segment.h (64-bit): GDT entries 12 to 14 are the three TLS entries of a thread.
constexpr int GDT_ENTRY_TLS_MIN = 12, GDT_ENTRY_TLS_MAX = 14;

// LDT_empty and LDT_zero (asm/desc.h): either one clears the entry. Neither looks at lm or at the padding above it.
bool ClearsEntry(const lx_user_desc& D) {
  const uint32_t F = D.flags & (LX_USER_DESC_LM - 1);
  return D.base_addr == 0 && D.limit == 0 &&
         (F == 0 || F == (LX_USER_DESC_READ_EXEC_ONLY | LX_USER_DESC_SEG_NOT_PRESENT));
}

// tls_desc_okay: a descriptor that clears the entry, or a present 32-bit data or expand-down data segment (no 16-bit
// segment, no code).
bool TlsDescOkay(const lx_user_desc& D) {
  if (ClearsEntry(D)) return true;
  return (D.flags & LX_USER_DESC_SEG_32BIT) && LX_USER_DESC_CONTENTS(D.flags) <= 1 &&
         !(D.flags & LX_USER_DESC_SEG_NOT_PRESENT);
}

bool IsFree(const CPUState::gdt_segment& S) {
  static constexpr CPUState::gdt_segment Zero {};
  return memcmp(&S, &Zero, sizeof S) == 0;
}

// fill_ldt (asm/desc.h): a present user data descriptor of privilege 3, accessed. lm is never set: a TLS selector
// loaded into CS must not give 64-bit code.
void FillDescriptor(CPUState::gdt_segment* S, const lx_user_desc& D) {
  *S = {};
  CPUState::SetGDTBase(S, D.base_addr);
  CPUState::SetGDTLimit(S, D.limit & 0xF'FFFF);
  S->Type = ((D.flags & LX_USER_DESC_READ_EXEC_ONLY) ? 0 : 2) | (LX_USER_DESC_CONTENTS(D.flags) << 2) | 1;
  S->S = 1;
  S->DPL = 3;
  S->P = 1;
  S->AVL = (D.flags & LX_USER_DESC_USEABLE) ? 1 : 0;
  S->L = 0;
  S->D = (D.flags & LX_USER_DESC_SEG_32BIT) ? 1 : 0;
  S->G = (D.flags & LX_USER_DESC_LIMIT_IN_PAGES) ? 1 : 0;
}

// do_set_thread_area reloads DS, ES, FS and GS when they hold the changed entry's selector, so their bases follow the
// new descriptor. A cleared entry no longer loads: the kernel's fixup leaves the null selector and base 0 in the
// register (EX_TYPE_CLEAR_FS, asm_load_gs_index), loading __USER_DS first on processors whose null load keeps the base.
void ReloadSegments(CPUState& St, int Idx, const CPUState::gdt_segment& S, bool Cleared) {
  const uint16_t Selector = static_cast<uint16_t>((Idx << 3) | 3);
  const uint32_t Base = Cleared ? 0 : CPUState::CalculateGDTBase(S);
  auto Reload = [&](uint16_t& Sel, auto& Cached) {
    if (Sel != Selector) return;
    if (Cleared) Sel = 0;
    Cached = Base; // zero-extended for FS and GS, as a load in 64-bit mode does
  };
  Reload(St.ds_idx, St.ds_cached);
  Reload(St.es_idx, St.es_cached);
  Reload(St.fs_idx, St.fs_cached);
  Reload(St.gs_idx, St.gs_cached);
}

// do_set_thread_area for the calling thread: the descriptor is checked before an entry is allocated and its number
// written back.
int64_t SetThreadArea(GuestThread* T, uint32_t Ptr) {
  lx_user_desc D;
  if (lkd_user_read(Ptr, &D, sizeof D) != 0) return -LX_EFAULT;
  if (!TlsDescOkay(D)) return -LX_EINVAL;
  int Idx = static_cast<int32_t>(D.entry_number);
  if (Idx == -1) {
    for (int I = GDT_ENTRY_TLS_MIN; I <= GDT_ENTRY_TLS_MAX && Idx == -1; ++I) {
      if (IsFree(T->Gdt[I])) Idx = I;
    }
    if (Idx == -1) return -LX_ESRCH;
    const uint32_t Entry = static_cast<uint32_t>(Idx);
    if (lkd_user_write(Ptr + offsetof(lx_user_desc, entry_number), &Entry, sizeof Entry) != 0) return -LX_EFAULT;
  }
  if (Idx < GDT_ENTRY_TLS_MIN || Idx > GDT_ENTRY_TLS_MAX) return -LX_EINVAL;
  const bool Cleared = ClearsEntry(D);
  if (Cleared) {
    T->Gdt[Idx] = {};
  } else {
    FillDescriptor(&T->Gdt[Idx], D);
  }
  ReloadSegments(T->State->CurrentFrame->State, Idx, T->Gdt[Idx], Cleared);
  return 0;
}

// do_get_thread_area and fill_user_desc: one TLS entry as set_thread_area takes it (an empty entry reads back as
// read_exec_only and seg_not_present).
int64_t GetThreadArea(GuestThread* T, uint32_t Ptr) {
  lx_user_desc D;
  if (lkd_user_read(Ptr + offsetof(lx_user_desc, entry_number), &D.entry_number, sizeof D.entry_number) != 0) {
    return -LX_EFAULT;
  }
  const int Idx = static_cast<int32_t>(D.entry_number);
  if (Idx < GDT_ENTRY_TLS_MIN || Idx > GDT_ENTRY_TLS_MAX) return -LX_EINVAL;
  const auto& S = T->Gdt[Idx];
  D.base_addr = CPUState::CalculateGDTBase(S);
  D.limit = CPUState::CalculateGDTLimit(S);
  D.flags = (S.D ? LX_USER_DESC_SEG_32BIT : 0) | (static_cast<uint32_t>(S.Type >> 2) << 1) |
            ((S.Type & 2) ? 0 : LX_USER_DESC_READ_EXEC_ONLY) | (S.G ? LX_USER_DESC_LIMIT_IN_PAGES : 0) |
            (S.P ? 0 : LX_USER_DESC_SEG_NOT_PRESENT) | (S.AVL ? LX_USER_DESC_USEABLE : 0) | (S.L ? LX_USER_DESC_LM : 0);
  if (lkd_user_write(Ptr, &D, sizeof D) != 0) return -LX_EFAULT;
  return 0;
}
} // namespace

void ClearIa32TlsEntries(GuestThread* T) {
  for (int I = GDT_ENTRY_TLS_MIN; I <= GDT_ENTRY_TLS_MAX; ++I) T->Gdt[I] = {};
}

// The 32-bit ABI: number in eax (the upper half of rax is ignored), arguments in ebx, ecx, edx, esi, edi, ebp.
bool ServesIa32Syscall(const CPUState& St) {
  const uint32_t Nr = static_cast<uint32_t>(St.gregs[X::REG_RAX]);
  return Nr == IA32_NR_set_thread_area || Nr == IA32_NR_get_thread_area;
}

void HandleIa32Syscall(GuestThread* T) {
  auto& St = T->State->CurrentFrame->State;
  const uint32_t Nr = static_cast<uint32_t>(St.gregs[X::REG_RAX]);
  const uint32_t A0 = static_cast<uint32_t>(St.gregs[X::REG_RBX]);
  const int64_t Ret = Nr == IA32_NR_set_thread_area ? SetThreadArea(T, A0) : GetThreadArea(T, A0);
  // The copies above may have opened guarded host pages, which LKD closes at the end of each of its system calls.
  if (!T->GuardStepping) fexdarwin_lkd_guard_close_all();
  if (G->Opt.Trace) {
    TraceLog("[fexdarwin] tid %d int $0x80 nr %u (%#x) = %lld\n", T->Tid, Nr, A0, static_cast<long long>(Ret));
  }
  St.gregs[X::REG_RAX] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Ret))); // sign-extended
  St.rip += 2; // past cd 80: FEXCore reports the #GP at the instruction
}

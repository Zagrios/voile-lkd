// SPDX-License-Identifier: MIT
// int $0x80: Linux's 32-bit system call entry, which a kernel with IA32 emulation also serves for 64-bit code. Wine's
// new WoW64 makes one call through it in every thread that runs 32-bit code: set_thread_area, which gives the thread's
// 32-bit TEB a TLS entry of the GDT for %fs (alloc_fs_sel, dlls/ntdll/unix/signal_x86_64.c). FEXCore raises #GP for an
// int it does not handle; DarwinSignalDelegator::DeliverGuestSignal recognizes this one and the kick safe point calls
// HandleIa32Syscall. The TLS entries live in the thread's GDT (GuestThread::Gdt), where FEXCore reads a selector's base
// when code loads it. Every other 32-bit call fails with ENOSYS, as in a kernel built without it.
#include "Host.h"

#include <lkd/abi_x64.h>
#include <lkd/lkd.h>

#include <FEXCore/Core/X86Enums.h>

#include <cstdint>
#include <cstring>

namespace {
using FEXCore::Core::CPUState;
namespace X = FEXCore::X86State;

constexpr uint32_t IA32_NR_set_thread_area = 243, IA32_NR_get_thread_area = 244;
// arch/x86/include/asm/segment.h (64-bit): GDT entries 12 to 14 are the three TLS entries of a thread.
constexpr int GDT_ENTRY_TLS_MIN = 12, GDT_ENTRY_TLS_ENTRIES = 3;

// struct user_desc (asm/ldt.h). Flags: seg_32bit:1, contents:2, read_exec_only:1, limit_in_pages:1, seg_not_present:1,
// useable:1, lm:1.
struct UserDesc {
  uint32_t EntryNumber;
  uint32_t BaseAddr;
  uint32_t Limit;
  uint32_t Flags;
};
constexpr uint32_t SEG_32BIT = 1u << 0, READ_EXEC_ONLY = 1u << 3, LIMIT_IN_PAGES = 1u << 4, SEG_NOT_PRESENT = 1u << 5,
                   USEABLE = 1u << 6, LM = 1u << 7;
uint32_t Contents(const UserDesc& D) { return (D.Flags >> 1) & 3; }

// LDT_empty and LDT_zero (asm/desc.h): either clears the entry.
bool ClearsEntry(const UserDesc& D) {
  const bool Zero = D.BaseAddr == 0 && D.Limit == 0 && D.Flags == 0;
  const bool Empty = D.BaseAddr == 0 && D.Limit == 0 && (D.Flags & ~LM) == (READ_EXEC_ONLY | SEG_NOT_PRESENT);
  return Zero || Empty;
}

bool IsFree(const CPUState::gdt_segment& S) {
  static constexpr CPUState::gdt_segment Zero {};
  return memcmp(&S, &Zero, sizeof S) == 0;
}

// fill_ldt (asm/desc.h): a present user data or code descriptor of privilege 3, accessed.
void FillDescriptor(CPUState::gdt_segment* S, const UserDesc& D) {
  *S = {};
  CPUState::SetGDTBase(S, D.BaseAddr);
  CPUState::SetGDTLimit(S, D.Limit & 0xF'FFFF);
  S->Type = ((D.Flags & READ_EXEC_ONLY) ? 0 : 2) | (Contents(D) << 2) | 1;
  S->S = 1;
  S->DPL = 3;
  S->P = (D.Flags & SEG_NOT_PRESENT) ? 0 : 1;
  S->AVL = (D.Flags & USEABLE) ? 1 : 0;
  S->L = (D.Flags & LM) ? 1 : 0;
  S->D = (D.Flags & SEG_32BIT) ? 1 : 0;
  S->G = (D.Flags & LIMIT_IN_PAGES) ? 1 : 0;
}

// do_set_thread_area (arch/x86/kernel/tls.c), for the calling thread.
int64_t SetThreadArea(GuestThread* T, uint32_t Ptr) {
  UserDesc D;
  if (lkd_user_read(Ptr, &D, sizeof D) != 0) return -LX_EFAULT;
  int Idx = static_cast<int32_t>(D.EntryNumber);
  if (Idx == -1) {
    for (int I = GDT_ENTRY_TLS_MIN; I < GDT_ENTRY_TLS_MIN + GDT_ENTRY_TLS_ENTRIES && Idx == -1; ++I) {
      if (IsFree(T->Gdt[I])) Idx = I;
    }
    if (Idx == -1) return -LX_ESRCH;
    D.EntryNumber = static_cast<uint32_t>(Idx);
    if (lkd_user_write(Ptr, &D.EntryNumber, sizeof D.EntryNumber) != 0) return -LX_EFAULT;
  }
  if (Idx < GDT_ENTRY_TLS_MIN || Idx >= GDT_ENTRY_TLS_MIN + GDT_ENTRY_TLS_ENTRIES) return -LX_EINVAL;
  if (ClearsEntry(D)) {
    T->Gdt[Idx] = {};
    return 0;
  }
  // tls_desc_okay: a 32-bit data or expand-down segment only (no 16-bit segment, no code).
  if (!(D.Flags & SEG_32BIT) || Contents(D) > 1) return -LX_EINVAL;
  FillDescriptor(&T->Gdt[Idx], D);
  return 0;
}

// do_get_thread_area: the inverse of SetThreadArea for one TLS entry.
int64_t GetThreadArea(GuestThread* T, uint32_t Ptr) {
  UserDesc D;
  if (lkd_user_read(Ptr, &D.EntryNumber, sizeof D.EntryNumber) != 0) return -LX_EFAULT;
  const int Idx = static_cast<int32_t>(D.EntryNumber);
  if (Idx < GDT_ENTRY_TLS_MIN || Idx >= GDT_ENTRY_TLS_MIN + GDT_ENTRY_TLS_ENTRIES) return -LX_EINVAL;
  const auto& S = T->Gdt[Idx];
  D.BaseAddr = CPUState::CalculateGDTBase(S);
  D.Limit = S.Limit0 | (S.Limit1 << 16);
  D.Flags = 0;
  if (IsFree(S)) {
    D.Flags = READ_EXEC_ONLY | SEG_NOT_PRESENT;
  } else {
    D.Flags |= S.D ? SEG_32BIT : 0;
    D.Flags |= ((S.Type >> 2) & 3) << 1;
    D.Flags |= (S.Type & 2) ? 0 : READ_EXEC_ONLY;
    D.Flags |= S.G ? LIMIT_IN_PAGES : 0;
    D.Flags |= S.P ? 0 : SEG_NOT_PRESENT;
    D.Flags |= S.AVL ? USEABLE : 0;
    D.Flags |= S.L ? LM : 0;
  }
  if (lkd_user_write(Ptr, &D, sizeof D) != 0) return -LX_EFAULT;
  return 0;
}
} // namespace

void ClearIa32TlsEntries(GuestThread* T) {
  for (int I = GDT_ENTRY_TLS_MIN; I < GDT_ENTRY_TLS_MIN + GDT_ENTRY_TLS_ENTRIES; ++I) T->Gdt[I] = {};
}

void HandleIa32Syscall(GuestThread* T) {
  auto& St = T->State->CurrentFrame->State;
  // The 32-bit ABI: number in eax, arguments in ebx, ecx, edx, esi, edi, ebp; the result in eax, sign-extended.
  const uint32_t Nr = static_cast<uint32_t>(St.gregs[X::REG_RAX]);
  const uint32_t A0 = static_cast<uint32_t>(St.gregs[X::REG_RBX]);
  int64_t Ret;
  switch (Nr) {
  case IA32_NR_set_thread_area: Ret = SetThreadArea(T, A0); break;
  case IA32_NR_get_thread_area: Ret = GetThreadArea(T, A0); break;
  default: Ret = -LX_ENOSYS; break;
  }
  if (G->Opt.Trace) {
    TraceLog("[fexdarwin] tid %d int $0x80 nr %u (%#x) = %lld\n", T->Tid, Nr, A0, static_cast<long long>(Ret));
  }
  St.gregs[X::REG_RAX] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(Ret)));
  St.rip += 2; // past cd 80: FEXCore reports the #GP at the instruction
}

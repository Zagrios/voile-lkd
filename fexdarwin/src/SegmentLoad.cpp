// SPDX-License-Identifier: MIT
// mov to %fs in 64-bit code. Wine's new WoW64 loads the thread's 32-bit TEB selector into %fs from 64-bit code each
// time it returns to 32-bit code: at the end of every system call and Unix call the 32-bit side makes
// (__wine_syscall_dispatcher, __wine_unix_call_dispatcher), in leave_handler and in wow64cpu's BTCpuSimulate
// (dlls/ntdll/unix/signal_x86_64.c, dlls/wow64cpu/cpu.c). On x86-64 the load sets the FS base to the descriptor's base,
// which the 32-bit code then reads through %fs. FEXCore rejects a segment load into %fs in 64-bit mode as an undefined
// instruction (#UD); DarwinSignalDelegator::DeliverGuestSignal holds such a #UD for the kick safe point, which calls
// EmulateFsLoad: it performs the load the way the processor does and moves RIP past it, or returns false and the
// thread gets its SIGILL.
#include "Host.h"

#include <lkd/lkd.h>

#include <FEXCore/Core/X86Enums.h>

#include <cstdint>
#include <cstring>

namespace {
using FEXCore::Core::CPUState;
namespace X = FEXCore::X86State;

// The general-purpose registers in ModRM/SIB order (rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8-r15).
constexpr int GprOrder[16] = {X::REG_RAX, X::REG_RCX, X::REG_RDX, X::REG_RBX, X::REG_RSP, X::REG_RBP, X::REG_RSI,
                              X::REG_RDI, X::REG_R8,  X::REG_R9,  X::REG_R10, X::REG_R11, X::REG_R12, X::REG_R13,
                              X::REG_R14, X::REG_R15};

// Decodes 8e /4 (mov r/m16, %fs) at RIP: its length and the selector it loads. Prefixes: operand size (ignored, as by
// the processor for a segment load), address size, and the segment overrides of ds, es, ss and cs, which change
// nothing in 64-bit mode; an fs or gs override, lock or rep makes it decline.
bool DecodeFsLoad(const CPUState& St, uint16_t* Selector, uint64_t* Length) {
  uint8_t B[15] {};
  if (lkd_user_read(St.rip, B, sizeof B) != 0) return false;
  size_t I = 0;
  bool Addr32 = false;
  for (; I < sizeof B; ++I) {
    const uint8_t P = B[I];
    if (P == 0x66 || P == 0x2e || P == 0x36 || P == 0x3e || P == 0x26) continue;
    if (P == 0x67) { Addr32 = true; continue; }
    break;
  }
  uint8_t Rex = 0;
  if (I < sizeof B && (B[I] & 0xf0) == 0x40) Rex = B[I++];
  if (I + 2 > sizeof B || B[I] != 0x8e) return false;
  const uint8_t ModRM = B[I + 1];
  I += 2;
  const uint8_t Mod = ModRM >> 6, Reg = (ModRM >> 3) & 7, Rm = ModRM & 7;
  if (Reg != 4) return false; // %fs only
  if (Mod == 3) {
    *Selector = static_cast<uint16_t>(St.gregs[GprOrder[Rm | ((Rex & 1) << 3)]]);
    *Length = I;
    return true;
  }
  uint64_t Ea = 0;
  bool RipRelative = false;
  if (Rm == 4) { // SIB
    if (I >= sizeof B) return false;
    const uint8_t Sib = B[I++];
    const uint8_t Scale = Sib >> 6, Index = ((Sib >> 3) & 7) | ((Rex & 2) << 2), Base = (Sib & 7) | ((Rex & 1) << 3);
    if (Index != 4) Ea += St.gregs[GprOrder[Index]] << Scale;
    if ((Base & 7) != 5 || Mod != 0) {
      Ea += St.gregs[GprOrder[Base]];
    } else { // no base register: a disp32 follows
      if (I + 4 > sizeof B) return false;
      int32_t D;
      memcpy(&D, &B[I], 4);
      I += 4;
      Ea += static_cast<int64_t>(D);
    }
  } else if (Rm == 5 && Mod == 0) {
    RipRelative = true;
  } else {
    Ea += St.gregs[GprOrder[Rm | ((Rex & 1) << 3)]];
  }
  if (Mod == 1) {
    if (I + 1 > sizeof B) return false;
    Ea += static_cast<int64_t>(static_cast<int8_t>(B[I]));
    I += 1;
  } else if (Mod == 2 || RipRelative) {
    if (I + 4 > sizeof B) return false;
    int32_t D;
    memcpy(&D, &B[I], 4);
    I += 4;
    Ea += static_cast<int64_t>(D);
  }
  if (RipRelative) Ea += St.rip + I; // relative to the next instruction
  if (Addr32) Ea &= 0xffff'ffffu;
  if (lkd_user_read(Ea, Selector, sizeof *Selector) != 0) return false;
  *Length = I;
  return true;
}
} // namespace

bool EmulateFsLoad(GuestThread* T) {
  auto& St = T->State->CurrentFrame->State;
  if ((St.cs_idx | 3) == GUEST_USER32_CS) return false; // 32-bit code: FEXCore loads %fs itself
  uint16_t Selector;
  uint64_t Length;
  if (!DecodeFsLoad(St, &Selector, &Length)) return false;
  // A null selector loads without a fault and, as on Intel processors, leaves the base as it was.
  if ((Selector & ~3) != 0) {
    // FEXCore reads both tables from GuestThread::Gdt; a selector outside it or a not-present descriptor faults on a
    // processor (#GP, #NP): leave those to the SIGILL rather than invent a fault.
    if ((Selector >> 3) >= sizeof T->Gdt / sizeof T->Gdt[0]) return false;
    const auto& D = *CPUState::GetSegmentFromIndex(St, Selector);
    if (!D.P || !D.S) return false;
    St.fs_cached = CPUState::CalculateGDTBase(D);
  }
  St.fs_idx = Selector;
  St.rip += Length;
  if (G->Opt.Trace) {
    TraceLog("[fexdarwin] tid %d mov %#x to %%fs in 64-bit code: base %#llx\n", T->Tid, Selector,
             static_cast<unsigned long long>(St.fs_cached));
  }
  return true;
}

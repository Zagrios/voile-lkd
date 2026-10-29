// SPDX-License-Identifier: MIT
// Delivery of synchronous faults to x86-64 Linux guest signal handlers, and rt_sigreturn.
//
// The frame the guest sees is the one the Linux kernel builds (struct rt_sigframe: return address, ucontext, siginfo, with
// the FP state in an XSAVE sized area above it), so unmodified C libraries and test programs can read and modify it.
// Only synchronous faults are handled: the host thread is resumed at the top of the dispatcher loop with the host stack
// reset, so there is no interrupted translated code to come back to.
#include "DarwinContext.h"
#include <FEXCore/Utils/GuestBase.h>
#include "DarwinSignalDelegator.h"
#include "GuestSignalAbi.h"
#include "Host.h"
#include "LkdGlue.h"

#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/FPState.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace Linux;
namespace X = FEXCore::X86State;

namespace {
void Fatal(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
void Fatal(const char* Fmt, ...) {
  char Buf[512];
  va_list Ap;
  va_start(Ap, Fmt);
  int N = vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  if (N > 0) {
    ssize_t R = ::write(2, Buf, std::min<size_t>(N, sizeof Buf - 1));
    (void)R;
  }
}

uint16_t BuildFsw(const FEXCore::Core::CPUState& St) {
  return static_cast<uint16_t>((St.flags[X::X87FLAG_TOP_LOC] << 11) | (St.flags[X::X87FLAG_C0_LOC] << 8) | (St.flags[X::X87FLAG_C1_LOC] << 9) |
                               (St.flags[X::X87FLAG_C2_LOC] << 10) | (St.flags[X::X87FLAG_C3_LOC] << 14) | St.flags[X::X87FLAG_IE_LOC]);
}

} // namespace

void SaveGuestFpState(FEXCore::Core::InternalThreadState* Th, XState* Xs) {
  auto& St = Th->CurrentFrame->State;
  FxSave& Fx = Xs->fx;
  memset(Xs, 0, sizeof(*Xs));
  Fx.fcw = St.FCW;
  Fx.fsw = BuildFsw(St);
  Fx.ftw = St.AbridgedFTW;
  Fx.mxcsr = St.mxcsr;
  Fx.mxcsr_mask = 0xffc0;
  const uint16_t Top = St.flags[X::X87FLAG_TOP_LOC];
  for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_MMS; ++i) {
    memcpy(Fx.st[i], &St.mm[(i + Top) % 8], sizeof(St.mm[0]));
  }
  G->CTX->ReconstructXMMRegisters(Th, reinterpret_cast<__uint128_t*>(Fx.xmm), reinterpret_cast<__uint128_t*>(Xs->ymmh));
  Fx.magic1 = FP_XSTATE_MAGIC1;
  Fx.extended_size = sizeof(XState);
  Fx.xfeatures = 7; // x87, SSE, AVX
  Fx.xstate_size = sizeof(XState);
  Xs->xstate_bv = 7;
  Xs->magic2 = FP_XSTATE_MAGIC2;
}

void RestoreGuestFpState(FEXCore::Core::InternalThreadState* Th, const XState* Xs) {
  auto& St = Th->CurrentFrame->State;
  const FxSave& Fx = Xs->fx;
  G->CTX->SetXMMRegistersFromState(Th, reinterpret_cast<const __uint128_t*>(Fx.xmm), reinterpret_cast<const __uint128_t*>(Xs->ymmh));
  St.mxcsr = Fx.mxcsr & 0xffc0;
  St.FCW = Fx.fcw;
  St.AbridgedFTW = Fx.ftw;
  St.flags[X::X87FLAG_IE_LOC] = Fx.fsw & 1;
  St.flags[X::X87FLAG_C0_LOC] = (Fx.fsw >> 8) & 1;
  St.flags[X::X87FLAG_C1_LOC] = (Fx.fsw >> 9) & 1;
  St.flags[X::X87FLAG_C2_LOC] = (Fx.fsw >> 10) & 1;
  St.flags[X::X87FLAG_C3_LOC] = (Fx.fsw >> 14) & 1;
  St.flags[X::X87FLAG_TOP_LOC] = (Fx.fsw >> 11) & 7;
  const uint16_t Top = St.flags[X::X87FLAG_TOP_LOC];
  for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_MMS; ++i) {
    memcpy(&St.mm[(i + Top) % 8], Fx.st[i], sizeof(St.mm[0]));
  }
}

void ResetGuestFpForHandler(FEXCore::Core::InternalThreadState* Th) {
  auto& St = Th->CurrentFrame->State;
  memset(St.xmm.avx.data, 0, sizeof(St.xmm));
  memset(St.avx_high, 0, sizeof(St.avx_high));
  memset(St.mm, 0, sizeof(St.mm));
  St.FCW = 0x37f;
  St.AbridgedFTW = 0;
  St.mxcsr = 0x1f80;
  for (int i : {X::X87FLAG_IE_LOC, X::X87FLAG_C0_LOC, X::X87FLAG_C1_LOC, X::X87FLAG_C2_LOC, X::X87FLAG_C3_LOC, X::X87FLAG_TOP_LOC}) St.flags[i] = 0;
}

bool DarwinSignalDelegator::DeliverGuestSignal(GuestThread* T, ucontext_t* Uc, const GuestFault& F, bool StateIsSpilled) {
  auto* Th = T->State;
  auto* Frame = Th->CurrentFrame;
  auto& St = Frame->State;
  if (!StateIsSpilled) {
    SpillSRA(Th, Uc, Frame->InSyscallInfo & 0xFFFF);
  }

  if (LkdEnabled()) {
    // LKD owns the signal state: queue the forced signal, then let the kick trampoline run lkd_signal_next on the thread
    // stack (handler frame, or death of the process for a blocked, ignored or default action).
    T->FaultErr = F.Err;
    T->FaultTrapNo = F.TrapNo;
    T->FaultAddr = F.Addr;
    lkd_fault(T->Task, F.Signal, F.SiCode, F.Addr, F.Err, F.TrapNo);
    Frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;
    Frame->InSyscallInfo = 0;
    DarwinCtx::SetSp(Uc, Frame->ReturningStackLocation);
    DarwinCtx::SetPc(Uc, reinterpret_cast<uint64_t>(&fexdarwin_kick_trampoline));
    DarwinCtx::SetReg(Uc, 0, reinterpret_cast<uint64_t>(T));
    DarwinCtx::SetReg(Uc, 28, reinterpret_cast<uint64_t>(Frame));
    return true;
  }

  KernelSigaction Act;
  {
    std::lock_guard<std::mutex> lk(G->SigLock);
    Act = G->SigActions[F.Signal];
  }
  const bool Blocked = (T->SigMask >> (F.Signal - 1)) & 1;
  if (Act.Handler == SIG_DFL_ || Act.Handler == SIG_IGN_ || Blocked) {
    return false; // the kernel kills the process for a synchronous signal it cannot deliver
  }

  const uint32_t EFlags = GuestEflags(Th);

  // Stack selection: the alternate stack only when the handler asked for it and the thread is not already on it.
  const uint64_t InterruptedSp = St.gregs[X::REG_RSP];
  uint64_t Sp = InterruptedSp;
  bool OnAlt = T->AltStack.Size && InterruptedSp > T->AltStack.Sp && InterruptedSp <= T->AltStack.Sp + T->AltStack.Size;
  if ((Act.Flags & SA_ONSTACK_) && !(T->AltStack.Flags & SS_DISABLE_) && T->AltStack.Size && !OnAlt) {
    Sp = T->AltStack.Sp + T->AltStack.Size;
  }
  Sp -= 128; // red zone
  Sp -= sizeof(XState);
  Sp &= ~63ULL;
  const uint64_t FpAddr = Sp;
  Sp -= sizeof(RtSigFrame);
  Sp = (Sp & ~15ULL) - 8; // the return address push makes rsp + 8 16-byte aligned at handler entry
  const uint64_t FrameAddr = Sp;
  const uint64_t FrameEnd = FpAddr + sizeof(XState);
  if (!G->Mem.Check(FrameAddr, FrameEnd - FrameAddr, PROT_WRITE_)) {
    Fatal("[fexdarwin] cannot build the signal frame at %#llx (stack not writable): the kernel would kill the process\n", (unsigned long long)FrameAddr);
    return false;
  }
  PrepareGuestWrite(T, FrameAddr, FrameEnd - FrameAddr);

  auto* Fr = static_cast<RtSigFrame*>(FEXCore::G2H(FrameAddr));
  memset(Fr, 0, sizeof(*Fr));
  Fr->pretcode = (Act.Flags & SA_RESTORER_) ? Act.Restorer : 0;
  Fr->uc.uc_flags = UC_FP_XSTATE | UC_SIGCONTEXT_SS | UC_STRICT_RESTORE_SS;
  Fr->uc.uc_stack = T->AltStack;
  if (OnAlt) Fr->uc.uc_stack.Flags |= SS_ONSTACK_;
  SigContext64& Sc = Fr->uc.uc_mcontext;
  Sc.r8 = St.gregs[X::REG_R8]; Sc.r9 = St.gregs[X::REG_R9]; Sc.r10 = St.gregs[X::REG_R10]; Sc.r11 = St.gregs[X::REG_R11];
  Sc.r12 = St.gregs[X::REG_R12]; Sc.r13 = St.gregs[X::REG_R13]; Sc.r14 = St.gregs[X::REG_R14]; Sc.r15 = St.gregs[X::REG_R15];
  Sc.rdi = St.gregs[X::REG_RDI]; Sc.rsi = St.gregs[X::REG_RSI]; Sc.rbp = St.gregs[X::REG_RBP]; Sc.rbx = St.gregs[X::REG_RBX];
  Sc.rdx = St.gregs[X::REG_RDX]; Sc.rax = St.gregs[X::REG_RAX]; Sc.rcx = St.gregs[X::REG_RCX]; Sc.rsp = InterruptedSp;
  Sc.rip = St.rip;
  Sc.eflags = EFlags;
  Sc.cs = St.cs_idx; Sc.gs = St.gs_idx; Sc.fs = St.fs_idx; Sc.ss = St.ss_idx;
  Sc.err = F.Err;
  Sc.trapno = F.TrapNo;
  Sc.oldmask = 0;
  Sc.cr2 = F.TrapNo == TRAP_PF ? F.Addr : 0;
  Sc.fpstate = FpAddr;
  Fr->uc.uc_sigmask = T->SigMask;
  SaveGuestFpState(Th, static_cast<XState*>(FEXCore::G2H(FpAddr)));

  Fr->info.si_signo = F.Signal;
  Fr->info.si_errno = 0;
  Fr->info.si_code = F.SiCode;
  Fr->info.si_addr = F.Addr;

  // Guest state for the handler: arguments in rdi, rsi, rdx, rax cleared (varargs convention), rip at the handler.
  St.rip = Act.Handler;
  St.gregs[X::REG_RSP] = FrameAddr;
  St.gregs[X::REG_RAX] = 0;
  St.gregs[X::REG_RDI] = F.Signal;
  St.gregs[X::REG_RSI] = FrameAddr + offsetof(RtSigFrame, info);
  St.gregs[X::REG_RDX] = FrameAddr + offsetof(RtSigFrame, uc);
  // Linux clears DF, RF and TF when entering a handler and starts it with a clean FPU.
  St.flags[X::RFLAG_DF_RAW_LOC] = 1;
  St.flags[X::RFLAG_RF_LOC] = 0;
  St.flags[X::RFLAG_TF_RAW_LOC] = 0;
  memset(St.xmm.avx.data, 0, sizeof(St.xmm));
  memset(St.avx_high, 0, sizeof(St.avx_high));
  memset(St.mm, 0, sizeof(St.mm));
  St.FCW = 0x37f;
  St.AbridgedFTW = 0;
  St.mxcsr = 0x1f80;
  for (int i : {X::X87FLAG_IE_LOC, X::X87FLAG_C0_LOC, X::X87FLAG_C1_LOC, X::X87FLAG_C2_LOC, X::X87FLAG_C3_LOC, X::X87FLAG_TOP_LOC}) St.flags[i] = 0;

  // Signal mask while the handler runs.
  uint64_t NewMask = T->SigMask | Act.Mask;
  if (!(Act.Flags & SA_NODEFER_)) NewMask |= 1ULL << (F.Signal - 1);
  T->SigMask = NewMask & ~((1ULL << (L_SIGKILL - 1)) | (1ULL << (L_SIGSTOP - 1)));
  if (Act.Flags & SA_RESETHAND_) {
    std::lock_guard<std::mutex> lk(G->SigLock);
    G->SigActions[F.Signal].Handler = SIG_DFL_;
  }
  Frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;

  if (G->Opt.TraceSignals) {
    Fatal("[fexdarwin] guest signal %d (si_code %d, si_addr %#llx, trap %llu) delivered to handler %#llx, frame %#llx, interrupted rip %#llx\n",
          F.Signal, F.SiCode, (unsigned long long)F.Addr, (unsigned long long)F.TrapNo, (unsigned long long)Act.Handler,
          (unsigned long long)FrameAddr, (unsigned long long)Sc.rip);
  }

  // Resume the host thread at the top of the dispatcher loop with a clean host stack. x1 = 0 means "not single stepping".
  DarwinCtx::SetSp(Uc, Frame->ReturningStackLocation);
  DarwinCtx::SetPc(Uc, Config.AbsoluteLoopTopAddressFillSRA);
  DarwinCtx::SetReg(Uc, 1, 0);
  DarwinCtx::SetReg(Uc, 28, reinterpret_cast<uint64_t>(Frame));
  return true;
}

int64_t GuestRtSigreturn(GuestThread* T) {
  auto* Th = T->State;
  auto* Frame = Th->CurrentFrame;
  auto& St = Frame->State;
  // The handler returned into the restorer, popping the return address: the frame starts 8 bytes below rsp.
  const uint64_t FrameAddr = St.gregs[X::REG_RSP] - sizeof(uint64_t);
  if (!G->Mem.Check(FrameAddr, sizeof(RtSigFrame), PROT_READ_)) {
    Fatal("[fexdarwin] rt_sigreturn with an unreadable frame at %#llx: the kernel delivers SIGSEGV\n", (unsigned long long)FrameAddr);
    _exit(128 + L_SIGSEGV);
  }
  auto* Fr = static_cast<RtSigFrame*>(FEXCore::G2H(FrameAddr));
  const SigContext64& Sc = Fr->uc.uc_mcontext;
  if (!G->Mem.Check(Sc.fpstate, sizeof(XState), PROT_READ_)) {
    Fatal("[fexdarwin] rt_sigreturn with an unreadable FP state at %#llx\n", (unsigned long long)Sc.fpstate);
    _exit(128 + L_SIGSEGV);
  }
  St.gregs[X::REG_R8] = Sc.r8; St.gregs[X::REG_R9] = Sc.r9; St.gregs[X::REG_R10] = Sc.r10; St.gregs[X::REG_R11] = Sc.r11;
  St.gregs[X::REG_R12] = Sc.r12; St.gregs[X::REG_R13] = Sc.r13; St.gregs[X::REG_R14] = Sc.r14; St.gregs[X::REG_R15] = Sc.r15;
  St.gregs[X::REG_RDI] = Sc.rdi; St.gregs[X::REG_RSI] = Sc.rsi; St.gregs[X::REG_RBP] = Sc.rbp; St.gregs[X::REG_RBX] = Sc.rbx;
  St.gregs[X::REG_RDX] = Sc.rdx; St.gregs[X::REG_RAX] = Sc.rax; St.gregs[X::REG_RCX] = Sc.rcx; St.gregs[X::REG_RSP] = Sc.rsp;
  St.rip = Sc.rip;
  G->CTX->SetFlagsFromCompactedEFLAGS(Th, static_cast<uint32_t>(Sc.eflags));
  RestoreGuestFpState(Th, static_cast<const XState*>(FEXCore::G2H(Sc.fpstate)));
  T->SigMask = Fr->uc.uc_sigmask & ~((1ULL << (L_SIGKILL - 1)) | (1ULL << (L_SIGSTOP - 1)));
  T->SyscallSetsRip = true;
  if (G->Opt.TraceSignals) {
    Fatal("[fexdarwin] rt_sigreturn: back to rip %#llx rsp %#llx\n", (unsigned long long)Sc.rip, (unsigned long long)Sc.rsp);
  }
  return static_cast<int64_t>(Sc.rax);
}

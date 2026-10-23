// SPDX-License-Identifier: MIT
#include "DarwinSignalDelegator.h"
#include "CodeValidation.h"
#include "CrashReport.h"
#include "HitchLog.h"
#include "LkdGlue.h"
#include <FEXCore/Utils/GuestBase.h>
#include "DarwinContext.h"
#include "Host.h"
#include "DarwinSyscallHandler.h"
#include "GuestSignalAbi.h"

#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

thread_local GuestThread* CurrentGuestThread = nullptr;

extern "C" int fexdarwin_lkd_vma_at(uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot);
extern "C" int fexdarwin_lkd_code_write_fault(uint64_t gva);
extern "C" int fexdarwin_lkd_code_watched(uint64_t gva);

namespace {
// Protection of the guest page holding Addr: negative when unmapped. In LKD mode the VMA table of LKD is the authority.
int GuestProtAt(uint64_t Addr) {
  if (LkdEnabled()) {
    uint64_t S, E;
    uint32_t P;
    return fexdarwin_lkd_vma_at(Addr, &S, &E, &P) == 0 ? static_cast<int>(P) : -1;
  }
  return G->Mem.ProtAt(Addr);
}
} // namespace

namespace {
constexpr size_t HostAltStackSize = 256 * 1024;

void WriteErr(const char* Buf, size_t Len) {
  while (Len) {
    ssize_t R = ::write(2, Buf, Len);
    if (R <= 0) return;
    Buf += R;
    Len -= R;
  }
}
void Say(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* Fmt, ...) {
  char Buf[512];
  va_list Ap;
  va_start(Ap, Fmt);
  int N = vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  if (N > 0) WriteErr(Buf, std::min<size_t>(N, sizeof Buf - 1));
}
} // namespace

void DarwinSignalDelegator::Install() {
  struct sigaction Sa {};
  Sa.sa_sigaction = &DarwinSignalDelegator::Handler;
  sigemptyset(&Sa.sa_mask);
  // SA_NODEFER is not set: a handler does not take its own signal again. macOS then replays a fault of that signal
  // inside the handler forever, so HandleFault lets SIGBUS through where its own stores may fault. SA_ONSTACK keeps the
  // handler off the (possibly overflowed) guest-thread stack.
  Sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (int S : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE}) {
    if (sigaction(S, &Sa, nullptr) != 0) {
      fprintf(stderr, "[fexdarwin] sigaction(%d) failed: %s\n", S, strerror(errno));
    }
  }
}

void DarwinSignalDelegator::RegisterThread(GuestThread* T) {
  CurrentGuestThread = T;
  T->HostAltStackSize = HostAltStackSize;
  T->HostAltStack = ::mmap(nullptr, HostAltStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (T->HostAltStack == MAP_FAILED) {
    // No host memory left: the handlers then run on the thread's own stack.
    T->HostAltStack = nullptr;
    fprintf(stderr, "[fexdarwin] no host memory for the signal stack of tid %d\n", T->Tid);
    return;
  }
  stack_t Ss {};
  Ss.ss_sp = T->HostAltStack;
  Ss.ss_size = HostAltStackSize;
  Ss.ss_flags = 0;
  if (sigaltstack(&Ss, nullptr) != 0) {
    fprintf(stderr, "[fexdarwin] sigaltstack failed: %s\n", strerror(errno));
  }
}

void DarwinSignalDelegator::UnregisterThread(GuestThread* T) {
  stack_t Ss {};
  Ss.ss_flags = SS_DISABLE;
  sigaltstack(&Ss, nullptr);
  if (T->HostAltStack) {
    ::munmap(T->HostAltStack, T->HostAltStackSize);
    T->HostAltStack = nullptr;
  }
  CurrentGuestThread = nullptr;
}

void DarwinSignalDelegator::SpillSRA(FEXCore::Core::InternalThreadState* Thread, ucontext_t* Uc, uint32_t IgnoreMask) {
  auto& State = Thread->CurrentFrame->State;
  State.rip = G->CTX->RestoreRIPFromHostPC(Thread, DarwinCtx::GetPc(Uc));
  for (size_t i = 0; i < Config.SRAGPRCount; i++) {
    const uint8_t Idx = Config.SRAGPRMapping[i];
    if (IgnoreMask & (1U << Idx)) {
      continue; // already spilled
    }
    State.gregs[i] = DarwinCtx::GetReg(Uc, Idx);
  }
  const bool SupportsAVX = G->Features.SupportsAVX;
  const bool SupportsSVE256 = G->Features.SupportsSVE256;
  for (size_t i = 0; i < Config.SRAFPRCount; i++) {
    __uint128_t Fpr = DarwinCtx::GetFpr(Uc, Config.SRAFPRMapping[i]);
    if (SupportsAVX && SupportsSVE256) {
      memcpy(&State.xmm.avx.data[i][0], &Fpr, sizeof(Fpr));
    } else {
      memcpy(&State.xmm.sse.data[i][0], &Fpr, sizeof(Fpr));
    }
  }
  const uint32_t EFlags = G->CTX->ReconstructCompactedEFLAGS(Thread, true, DarwinCtx::GPRs(Uc), DarwinCtx::GetPState(Uc));
  G->CTX->SetFlagsFromCompactedEFLAGS(Thread, EFlags);
}

void DarwinSignalDelegator::InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* CallingThread, uint64_t Start, uint64_t Len,
                                                     FEXCore::Context::CodeRangeInvalidationFn After) {
  std::lock_guard<std::mutex> ThreadsLk(G->ThreadsLock);
  auto Lk = FEXCore::GuardSignalDeferringSectionWithFallback(G->CTX->GetCodeInvalidationMutex(), CallingThread);
  G->CTX->InvalidateCodeBuffersCodeRange(Start, Len);
  for (GuestThread* T : G->Threads) {
    G->CTX->InvalidateThreadCachedCodeRange(T->State, Start, Len);
  }
  if (After) {
    After(Start, Len);
  }
}

bool DarwinSignalDelegator::RedirectFromJit(GuestThread* T, ucontext_t* Uc, uint64_t Target) {
  auto* Th = T->State;
  auto* Frame = Th->CurrentFrame;
  if (!G->CTX->IsAddressInCodeBuffer(Th, DarwinCtx::GetPc(Uc))) {
    return false; // dispatcher or host C++: the guest state is not in the host registers
  }
  if (Frame->State.DeferredSignalRefCount.Load() != 0 || Frame->InSyscallInfo != 0) {
    return false;
  }
  // Only at the first host instruction of a guest instruction: anywhere else the translation of an x86 instruction may be
  // half done (a register already written, flags not yet), and FEX's RIP reconstruction ("as close as FEX can get")
  // would restart the instruction after the handler and apply it twice. The caller retries until the thread is at a
  // boundary (the kick retry thread resends the host signal every millisecond).
  uint64_t BoundaryRip;
  if (!G->CTX->IsHostPCAtGuestInstructionBoundary(Th, DarwinCtx::GetPc(Uc), &BoundaryRip)) {
    return false;
  }
  SpillSRA(Th, Uc, 0);
  Frame->State.rip = BoundaryRip;
  // The interrupted block is abandoned: restart from the dispatcher's own stack position, like a synchronous fault.
  DarwinCtx::SetSp(Uc, Frame->ReturningStackLocation);
  DarwinCtx::SetReg(Uc, 0, reinterpret_cast<uint64_t>(T));
  DarwinCtx::SetPc(Uc, Target);
  return true;
}

void DarwinSignalDelegator::Handler(int Sig, siginfo_t* Info, void* Uctx) {
  // LKD's fault-safe user copies run on guest threads and on host-only threads: let LKD recover its own faults first
  // (contract of lkd_fault_try_recover in lkd.h; our handler is installed after lkd_init).
  if ((Sig == SIGSEGV || Sig == SIGBUS) && LkdEnabled() && lkd_fault_try_recover(Uctx)) {
    return;
  }
  auto* Self = G->Signals.get();
  auto* Uc = static_cast<ucontext_t*>(Uctx);
  GuestThread* T = CurrentGuestThread;
  if (!T) {
    Say("[fexdarwin] fatal: signal %d at pc=%#llx addr=%p on a thread that is not a guest thread\n", Sig,
        (unsigned long long)DarwinCtx::GetPc(Uc), Info->si_addr);
    CrashReport::Host(Uc, reinterpret_cast<uint64_t>(Info->si_addr), nullptr);
    _exit(128 + Sig);
  }
  if (!Self->HandleFault(T, Sig, Info, Uc)) {
    Self->Crash(T, Sig, Info, Uc, "unhandled fault");
  }
}

bool DarwinSignalDelegator::HandleSmcWrite(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc) {
  const uint64_t Fault = FEXCore::H2G(Info->si_addr);
  auto* Th = T->State;
  uint64_t PageBase;
  if (LkdEnabled()) {
    // Any fault status: macOS reports a store to a non-resident read-only page as a translation fault (MESURE), so the
    // filter is "data abort with WnR set" (request mm-5).
    const uint32_t Esr = DarwinCtx::GetEsr(Uc);
    const uint32_t Ec = Esr >> 26;
    if (!((Ec == 0x24 || Ec == 0x25) && ((Esr >> 6) & 1))) return false;
    // mm drops the translations of the host page through ops->invalidate_code, restores the write permission and tells
    // us to retry the store.
    HitchLog::Scope Timed(HitchLog::Kind::SmcFault);
    HitchLog::NoteSmcPage(Fault);
    PageBase = Fault & ~(FEXCore::Utils::FEX_HOST_PAGE_SIZE - 1);
    // A page whose code keeps being rewritten leaves write tracking (CodeValidation.h): translations made from now on
    // check their bytes, and this invalidation drops the ones made before the mark. LKD's own invalidation below is not
    // enough: another thread may have unwatched the page in between, and a translation that started before the mark
    // may end after it without watching the page.
    if (fexdarwin_lkd_code_watched(Fault) && CodeValidation::NoteWriteFault(PageBase)) {
      InvalidateGuestCodeRange(Th, PageBase, FEXCore::Utils::FEX_HOST_PAGE_SIZE);
      if (HitchLog::Enabled() || G->Opt.TraceSignals) {
        Say("[fexdarwin] pid %d: host page %#llx keeps being rewritten while its code runs; its translations now check their bytes\n",
            static_cast<int>(getpid()), (unsigned long long)PageBase);
      }
    }
    if (fexdarwin_lkd_code_write_fault(Fault) != 1) return false;
  } else {
    if (!G->Mem.IsSmcPage(Fault)) {
      return false;
    }
    PageBase = GuestMemory::RoundDown(Fault);
    // Invalidate every translation that came from this host page, then make the page writable again while the locks are
    // held (same order as ThreadManager::InvalidateGuestCodeRange with its callback in FEX/Linux).
    InvalidateGuestCodeRange(Th, PageBase, GuestMemory::PageSize, [&](uint64_t Start, uint64_t Len) { G->Mem.ClearSmcPages(Start, Len); });
  }
  if (G->Opt.TraceSignals) {
    Say("[fexdarwin] SMC write fault at %#llx (page %#llx), translations dropped\n", (unsigned long long)Fault, (unsigned long long)PageBase);
  }

  auto* CTX = G->CTX.get();
  const uint64_t PC = DarwinCtx::GetPc(Uc);
  if (CTX->IsAddressInCodeBuffer(Th, PC) && !CTX->IsCurrentBlockSingleInst(Th) && CTX->IsAddressInCurrentBlock(Th, PageBase, GuestMemory::PageSize)) {
    // The write may have changed the code of the block that is running right now. Rebuild the guest state and restart at
    // the faulting instruction as a one-instruction block so the change takes effect immediately.
    SpillSRA(Th, Uc, Th->CurrentFrame->InSyscallInfo & 0xFFFF);
    DarwinCtx::SetPc(Uc, Config.AbsoluteLoopTopAddressFillSRA);
    DarwinCtx::SetReg(Uc, 1, 1); // ENTRY_FILL_SRA_SINGLE_INST_REG (TMP2 = x1): force a single step
  }
  return true;
}

// Faults that belong to the guest program: memory accesses from translated code that the guest's own memory map forbids,
// and the exceptions FEX raises itself (ud2, int3, hlt, ...) through the dispatcher stubs.
bool DarwinSignalDelegator::HandleGuestFault(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc) {
  auto* Th = T->State;
  auto* Frame = Th->CurrentFrame;
  const uint64_t PC = DarwinCtx::GetPc(Uc);
  const bool InDispatcher = PC >= Config.DispatcherBegin && PC < Config.DispatcherEnd;
  GuestFault F {};

  if (InDispatcher && Frame->SynchronousFaultData.FaultToTopAndGeneratedException) {
    // Break op: FEX stored the reason in the frame and spilled the guest registers before the host exception instruction.
    const auto& D = Frame->SynchronousFaultData;
    F.Signal = D.Signal; // Core::FAULT_* are the host numbers of SIGSEGV, SIGILL and SIGTRAP, which equal the Linux ones
    F.SiCode = D.si_code;
    F.TrapNo = D.TrapNo;
    F.Err = D.err_code;
    F.Addr = (D.TrapNo == Linux::TRAP_PF) ? Frame->State.rip : Frame->State.rip;
    if (D.TrapNo == Linux::TRAP_PF && (D.err_code & Linux::PF_INSTR)) {
      F.Addr = Frame->State.rip; // an instruction fetch fault reports the target address
      // FEX reports every instruction fetch fault as a protection fault. Linux distinguishes: an unmapped target is
      // SEGV_MAPERR with the present bit clear (error code 0x14), a mapped non-executable one SEGV_ACCERR with 0x15.
      if (GuestProtAt(F.Addr) < 0) {
        F.SiCode = Linux::SEGV_MAPERR_;
        F.Err &= ~static_cast<uint64_t>(Linux::PF_PROT);
      } else {
        F.SiCode = Linux::SEGV_ACCERR_;
        F.Err |= Linux::PF_PROT;
      }
    }
    return DeliverGuestSignal(T, Uc, F, true);
  }

  if ((Sig == SIGSEGV || Sig == SIGBUS) && G->CTX->IsAddressInCodeBuffer(Th, PC)) {
    const uint64_t Addr = FEXCore::H2G(Info->si_addr);
    const int Prot = GuestProtAt(Addr);
    const uint32_t Esr = DarwinCtx::GetEsr(Uc);
    const bool Write = (Esr >> 6) & 1; // WnR of a data abort syndrome
    F.Signal = Linux::L_SIGSEGV;
    F.SiCode = Prot < 0 ? Linux::SEGV_MAPERR_ : Linux::SEGV_ACCERR_;
    F.Addr = Addr;
    F.TrapNo = Linux::TRAP_PF;
    F.Err = Linux::PF_USER | (Prot >= 0 ? Linux::PF_PROT : 0) | (Write ? Linux::PF_WRITE : 0);
    return DeliverGuestSignal(T, Uc, F, false);
  }
  return false;
}

bool DarwinSignalDelegator::HandleFault(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc) {
  auto* Th = T->State;
  const uint64_t PC = DarwinCtx::GetPc(Uc);
  const uint64_t Addr = reinterpret_cast<uint64_t>(Info->si_addr);

  if (Sig == SIGSEGV || Sig == SIGBUS) {
    // 1. The JIT output buffer has a guard page. Running into it means the block needs a bigger buffer: restart the
    //    compilation through the saved jump buffer (same as the Linux frontend).
    if (Th->JITGuardPage && Addr >= Th->JITGuardPage && Addr < Th->JITGuardPage + FEXCore::Utils::FEX_HOST_PAGE_SIZE) {
      FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(Th->RestartJump, Th->JITGuardOverflowArgument, DarwinCtx::GPRs(Uc),
                                                      DarwinCtx::Fprs(Uc), reinterpret_cast<uint64_t*>(&DarwinCtx::SS(Uc).__pc));
      return true;
    }

    // 2. The call-ret shadow stack is bracketed by guard pages. Reset it, as FEX does on Linux.
    {
      const uint64_t Base = reinterpret_cast<uint64_t>(Th->CallRetStackBase);
      const uint64_t AllocBase = Base - FEXCore::Utils::FEX_HOST_PAGE_SIZE;
      const uint64_t AllocEnd = Base + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + FEXCore::Utils::FEX_HOST_PAGE_SIZE;
      if (Base && Addr >= AllocBase && Addr < AllocEnd) {
        DarwinCtx::SetReg(Uc, 25, Base + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4);
        return true;
      }
    }

    // 3. Unaligned atomic access in JIT code (alignment fault): back-patch the access, as FEX does on Linux. The handler
    //    stores the atomic's result itself, and a store to a page watched for code faults there. Linux reports that
    //    fault as SIGSEGV, which reaches step 4 from inside this handler. macOS reports it as SIGBUS, the signal being
    //    handled: blocked, it would replay the store forever (Hunt: Showdown's Easy Anti-Cheat froze on its splash).
    //    SIGBUS is let through while the access is emulated.
    if (Sig == SIGBUS && G->CTX->IsAddressInCodeBuffer(Th, PC) && DarwinCtx::IsDataAlignmentFault(Uc)) {
      const auto Type = FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier;
      sigset_t Bus, Blocked;
      sigemptyset(&Bus);
      sigaddset(&Bus, SIGBUS);
      pthread_sigmask(SIG_UNBLOCK, &Bus, &Blocked);
      auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(Th, Type, PC, DarwinCtx::GPRs(Uc));
      pthread_sigmask(SIG_SETMASK, &Blocked, nullptr);
      if (Result) {
        DarwinCtx::SetPc(Uc, PC + *Result);
        return true;
      }
    }

    // 4. Write to a page that holds translated code (self-modifying code detection).
    if (HandleSmcWrite(T, Sig, Info, Uc)) {
      return true;
    }
  }

  // 5. Everything else that happens in translated code is the guest program's own fault.
  return HandleGuestFault(T, Sig, Info, Uc);
}

namespace {
// Code regions of the faulting thread for the crash report (set by Crash, read by CodeRegionOf).
uint64_t CrashDispatcherBegin, CrashDispatcherEnd;
const char* CodeRegionOf(uint64_t Addr) {
  if (Addr >= CrashDispatcherBegin && Addr < CrashDispatcherEnd) return "FEX dispatcher";
  GuestThread* T = CurrentGuestThread;
  return T && G->CTX->IsAddressInCodeBuffer(T->State, Addr) ? "JIT code" : nullptr;
}
} // namespace

void DarwinSignalDelegator::Crash(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc, const char* Why) {
  auto* Th = T->State;
  const uint64_t PC = DarwinCtx::GetPc(Uc);
  const bool InJit = G->CTX->IsAddressInCodeBuffer(Th, PC);
  const bool InDispatcher = PC >= Config.DispatcherBegin && PC < Config.DispatcherEnd;
  Say("\n[fexdarwin] FATAL %s: signal %d (si_code %d) fault address %p\n", Why, Sig, Info->si_code, Info->si_addr);
  Say("[fexdarwin]   host pc=%#llx sp=%#llx %s\n", (unsigned long long)PC, (unsigned long long)DarwinCtx::GetSp(Uc),
      InJit ? "(inside JIT code)" : InDispatcher ? "(inside the FEX dispatcher)" : "(outside JIT code: host C++)");
  if (InJit) {
    // Raw instruction words around the faulting host PC (decode with llvm-mc --disassemble -triple=arm64).
    const uint32_t* W = reinterpret_cast<const uint32_t*>(DarwinCtx::GetPc(Uc));
    Say("[fexdarwin]   host code words around pc:");
    for (int i = -12; i <= 2; i++) Say(" %s0x%08x", i == 0 ? ">" : "", W[i]);
    Say("\n[fexdarwin]   x0..x3 %#llx %#llx %#llx %#llx x24 %#llx x28 %#llx\n", (unsigned long long)DarwinCtx::SS(Uc).__x[0],
        (unsigned long long)DarwinCtx::SS(Uc).__x[1], (unsigned long long)DarwinCtx::SS(Uc).__x[2], (unsigned long long)DarwinCtx::SS(Uc).__x[3],
        (unsigned long long)DarwinCtx::SS(Uc).__x[24], (unsigned long long)DarwinCtx::SS(Uc).__x[28]);
    SpillSRA(Th, Uc, Th->CurrentFrame->InSyscallInfo & 0xFFFF);
  }
  auto& St = Th->CurrentFrame->State;
  Say("[fexdarwin]   guest rip=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx\n", (unsigned long long)St.rip, (unsigned long long)St.gregs[0],
      (unsigned long long)St.gregs[3], (unsigned long long)St.gregs[1], (unsigned long long)St.gregs[2]);
  Say("[fexdarwin]   guest rsi=%#llx rdi=%#llx rbp=%#llx rsp=%#llx r8=%#llx r9=%#llx\n", (unsigned long long)St.gregs[6],
      (unsigned long long)St.gregs[7], (unsigned long long)St.gregs[5], (unsigned long long)St.gregs[4], (unsigned long long)St.gregs[8],
      (unsigned long long)St.gregs[9]);
  CrashDispatcherBegin = Config.DispatcherBegin;
  CrashDispatcherEnd = Config.DispatcherEnd;
  CrashReport::Host(Uc, reinterpret_cast<uint64_t>(Info->si_addr), CodeRegionOf);
  CrashReport::Guest(T);
  _exit(128 + Sig);
}

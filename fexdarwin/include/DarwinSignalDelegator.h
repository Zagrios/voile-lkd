// SPDX-License-Identifier: MIT
#pragma once

#include "Host.h"

#include <FEXCore/Core/SignalDelegator.h>
#include <csignal>

class DarwinSignalDelegator final : public FEXCore::SignalDelegator {
public:
  // Installs the process-wide handlers for the synchronous signals FEXCore depends on.
  void Install();
  // Per-thread setup: alternate signal stack. Call on the host thread that will run the guest thread.
  void RegisterThread(GuestThread* T);
  void UnregisterThread(GuestThread* T);

  // Copies the statically allocated registers and flags out of a host signal context into the thread state, so the guest
  // state in Thread->CurrentFrame->State is exact at the faulting instruction (port of SignalDelegator::SpillSRA).
  void SpillSRA(FEXCore::Core::InternalThreadState* Thread, ucontext_t* Uc, uint32_t IgnoreMask);

  // Invalidate the translations of [Start, Start + Len) in every thread, then run After while the locks are held.
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* CallingThread, uint64_t Start, uint64_t Len,
                                FEXCore::Context::CodeRangeInvalidationFn After = {});

  // A synchronous fault in guest code, expressed the way the Linux kernel reports it to the guest.
  struct GuestFault {
    int Signal;       // Linux signal number
    int SiCode;       // Linux si_code
    uint64_t Addr;    // si_addr
    uint64_t TrapNo;  // x86 trap number stored in sigcontext
    uint64_t Err;     // x86 error code stored in sigcontext
  };
  // Builds an x86-64 rt_sigframe on the guest stack and redirects the thread to the guest handler. The host thread is
  // resumed at the top of the FEX dispatcher loop with the host stack reset (synchronous faults never return to the
  // interrupted translated code). Returns false when the guest has no usable handler for the signal.
  bool DeliverGuestSignal(GuestThread* T, ucontext_t* Uc, const GuestFault& F, bool StateIsSpilled);

  // LKD kick: when the thread was interrupted inside translated code, outside any deferred-signal section and outside a
  // syscall sequence, spill the guest registers (exact guest state at the interrupted instruction, as FEX does for
  // asynchronous signals on Linux) and resume the host thread at Target with x0 = T. Returns false (nothing changed)
  // in every other case; the caller retries later.
  bool RedirectFromJit(GuestThread* T, ucontext_t* Uc, uint64_t Target);
  uint64_t LoopTopFillSRA() const {
    return Config.AbsoluteLoopTopAddressFillSRA;
  }

private:
  static void Handler(int Sig, siginfo_t* Info, void* Uc);
  bool HandleFault(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc);
  bool HandleSmcWrite(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc);
  bool HandleGuestFault(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc);
  bool BeginGuardStep(GuestThread* T, ucontext_t* Uc, uint64_t Page);
  void EndGuardStep(GuestThread* T, ucontext_t* Uc);
  bool ResolveGuardedFault(GuestThread* T, ucontext_t* Uc, uint64_t Addr, bool Write, bool InJit);
  [[noreturn]] void Crash(GuestThread* T, int Sig, siginfo_t* Info, ucontext_t* Uc, const char* Why);
};

// Ends a guard step (DarwinSignalDelegator::BeginGuardStep) before its trap: closes the pages it opened and clears the
// trap flag it set. Run before a guest signal frame is built in the middle of a step, so the guest never sees that trap
// flag; the instruction faults again after the handler and is stepped anew. No-op when the thread is not stepping.
void CancelGuardStep(GuestThread* T);

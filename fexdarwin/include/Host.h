// SPDX-License-Identifier: MIT
// Shared state of the fexdarwin host: one FEXCore context, the guest address space, the threads.
#pragma once

#include "GuestMemory.h"
#include "LinuxAbi.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <vector>

class DarwinSyscallHandler;
class DarwinSignalDelegator;

struct GuestThread {
  FEXCore::Core::InternalThreadState* State {};
  pthread_t PThread {};
  int32_t Tid {};
  bool IsMain {};
  uint64_t ClearTidAddr {};  // set_tid_address / CLONE_CHILD_CLEARTID
  uint64_t SigMask {};       // blocked guest signals (bit n-1 is signal n)
  Linux::StackT AltStack {}; // guest sigaltstack
  FEXCore::Core::CPUState::gdt_segment Gdt[32] {};
  void* HostAltStack {};
  size_t HostAltStackSize {};
  void* CallRetAlloc {};
  std::atomic<bool> Exited {false};
  int ExitCode {};
  uint64_t StartRip {};
  uint64_t StartRsp {};
  uint64_t StartRax {};         // value of rax at thread start (0 for the child of a clone)
  uint64_t StartFsBase {};
  bool InSignalFrame {};
  bool SyscallSetsRip {};      // set by a system call that loads the guest RIP itself (rt_sigreturn)
  // LKD mode (FEXDARWIN_LKD=1)
  struct lkd_task* Task {};    // the LKD task of this guest thread
  uint64_t OrigRax {};         // syscall number of the current or last syscall (ptrace orig_rax)
  uint64_t SysArgs[6] {};      // its arguments (thread dump)
  std::atomic<bool> Stopped {false};       // parked in a ptrace stop: registers are in memory
  std::atomic<bool> KickRequested {false};
  // A guarded host page (LKD mm) opened for one instruction that reads it: DarwinSignalDelegator::BeginGuardStep.
  uint64_t GuardPages[4] {};
  int GuardN {};
  bool GuardStepping {};
  bool Compiling {}; // between PreCompile and PostCompile: host C++ faults on guest memory are the decoder's
  // Guest 4 KiB page of the faults retried because the guest map allows the access while the host page does not yet
  // (a protection change in flight on another thread): DarwinSignalDelegator::ResolveGuardedFault.
  uint64_t RaceAddr {};
  uint64_t RaceStart {}, RaceLast {}; // CLOCK_UPTIME_RAW ns of the first and the latest retry of this episode
  unsigned RaceCount {};
  std::atomic<bool> InLkd {false};         // inside lkd_syscall/lkd_signal_next: LKD checks pending signals on its way out
  uint64_t FaultErr {}, FaultTrapNo {}, FaultAddr {}; // sigcontext err/trapno/cr2 of the pending synchronous fault
  // Profile (FEXDARWIN_PROFILE): system calls of this thread, in all and per number. SysNr holds the counts of each
  // number, then their nanoseconds inside LKD; the thread allocates it on its first profiled call.
  std::atomic<uint64_t> SysCount {0}, SysNs {0};
  std::atomic<std::atomic<uint64_t>*> SysNr {nullptr};
  ~GuestThread() { delete[] SysNr.load(); }
};

// The guest thread attached to the calling host thread (null on host-only threads).
extern thread_local GuestThread* CurrentGuestThread;

struct HostOptions {
  bool Trace = false;           // FEXDARWIN_TRACE=1 or --trace
  bool TraceSignals = false;
  bool ShowFeatures = false;
  bool NoSmc = false;           // --smc=none
  std::string SmcMode = "mtrack"; // none | mtrack | full
  std::vector<std::string> ExtraConfig; // --config NAME=VALUE passed to FEXCore::Config::Set
};

struct Host {
  HostOptions Opt;
  GuestMemory Mem;
  FEXCore::HostFeatures Features {};
  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  std::unique_ptr<DarwinSyscallHandler> Syscalls;
  std::unique_ptr<DarwinSignalDelegator> Signals;

  std::mutex ThreadsLock;
  std::vector<GuestThread*> Threads;
  std::atomic<int32_t> NextTid {0};
  int32_t Pid {};

  std::mutex SigLock;
  Linux::KernelSigaction SigActions[65] {};

  uint64_t StackTop {};
  uint64_t StackSize {};
};

extern Host* G;

// The guest's RFLAGS as the Linux kernel shows them (ptrace registers, signal frames): the defined bits of x86-64
// RFLAGS, with reserved bit 1 set. ReconstructCompactedEFLAGS also ORs in the bytes where FEXCore keeps its NZCV copy
// (flags[24..27]), so bits 24-31 can come out set (bit 31 in tracers and signal handlers), which no x86 CPU does.
inline uint32_t GuestEflags(FEXCore::Core::InternalThreadState* Th) {
  // CF, 1, PF, AF, ZF, SF, TF, IF, DF, OF, IOPL, NT, RF, VM, AC, VIF, VIP, ID
  constexpr uint32_t Defined = 0x3f7fd7;
  return (G->CTX->ReconstructCompactedEFLAGS(Th, false, nullptr, 0) & Defined) | 0x2;
}

// Linux x86-64 user selectors (arch/x86/include/asm/segment.h): 32-bit code, data and 64-bit code are GDT entries 4, 5
// and 6, with RPL 3.
constexpr uint16_t GUEST_USER32_CS = (4 << 3) | 3, GUEST_USER_DS = (5 << 3) | 3, GUEST_USER_CS = (6 << 3) | 3;

// Loads the guest's CS and SS on the way back to user mode. FEXCore decodes the code that follows in the mode of CS's
// descriptor: 32-bit code under GUEST_USER32_CS, 64-bit code under any other selector, as Windows forces its user code
// segment into a context whose CS names none (Wine copies the CONTEXT's SegCs into the signal frame). SS is always
// GUEST_USER_DS, the only stack segment ring 3 can load.
inline void LoadGuestCodeSegments(FEXCore::Core::CPUState& St, uint16_t Cs) {
  using FEXCore::Core::CPUState;
  St.cs_idx = (Cs | 3) == GUEST_USER32_CS ? GUEST_USER32_CS : GUEST_USER_CS;
  St.ss_idx = GUEST_USER_DS;
  St.cs_cached = CPUState::CalculateGDTBase(*CPUState::GetSegmentFromIndex(St, St.cs_idx));
  St.ss_cached = 0;
}

// ---- thread management (GuestThread.cpp) ----
// Creates the FEXCore thread state for a new guest thread. The caller fills StartRip/StartRsp/... then calls RunGuestThread
// on the host thread that will execute it.
// Returns null when the host has no memory left for the thread.
GuestThread* CreateGuestThread(GuestThread* Parent);
// Frees a thread made by CreateGuestThread that never ran (its host thread could not be created).
void DestroyGuestThread(GuestThread* T);
// Executes the guest thread on the calling host thread until the guest exits it. Never returns for the main thread.
void RunGuestThread(GuestThread* T);
void ForEachThread(void (*Fn)(GuestThread*, void*), void* Arg);
// Terminate the whole process (exit_group semantics).
[[noreturn]] void ExitProcess(int Code);

// rt_sigreturn: restores the guest context from the signal frame the guest stack pointer designates. Returns the restored
// RAX. Sets T->SyscallSetsRip because the guest RIP is loaded from the frame.
int64_t GuestRtSigreturn(GuestThread* T);
// Drop the translations of SMC-protected pages in [P, P + L) before the host writes there for the guest.
void PrepareGuestWrite(GuestThread* T, uint64_t P, uint64_t L);

// ---- tracing ----
void TraceLog(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));

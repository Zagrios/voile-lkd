// SPDX-License-Identifier: MIT
#include "DarwinSignalDelegator.h"
#include "DarwinSyscallHandler.h"
#include "HitchLog.h"
#include "Host.h"

#include <FEXCore/Core/X86Enums.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

Host* G = nullptr;

GuestThread* CreateGuestThread(GuestThread* Parent) {
  using FEXCore::Core::CPUState;
  // Each thread reserves about 280 MiB of host address space (FEXCore's lookup cache, call-ret stack, host stack). When
  // the host has none left, the thread is refused (clone gives EAGAIN, as Linux does without memory for a new task)
  // instead of running with caches FEXCore could not allocate.
  constexpr size_t Guard = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
  constexpr size_t Total = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * Guard;
  void* Alloc = ::mmap(nullptr, Total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Alloc == MAP_FAILED) {
    fprintf(stderr, "[fexdarwin] no host memory for the call-ret stack of a new thread: %s\n", strerror(errno));
    return nullptr;
  }
  // A child copies the CPU state of its parent (clone semantics), the first thread starts from the reset state.
  const CPUState* Init = Parent ? &Parent->State->CurrentFrame->State : nullptr;
  auto* State = G->CTX->CreateThread(Init);
  if (!State) {
    fprintf(stderr, "[fexdarwin] no host address space for the FEXCore caches of a new thread\n");
    ::munmap(Alloc, Total);
    return nullptr;
  }
  auto* T = new GuestThread();
  T->Tid = Parent ? ++G->NextTid + G->Pid : G->Pid;
  T->IsMain = Parent == nullptr;
  T->State = State;
  T->State->FrontendPtr = T;
  HitchLog::Attach(T->State);
  auto* Frame = T->State->CurrentFrame;

  // Call-ret shadow stack with a guard page on each side (the guard size is one *host* page: mprotect granularity).
  T->CallRetAlloc = Alloc;
  uint8_t* Base = static_cast<uint8_t*>(Alloc) + Guard;
  ::mprotect(Base, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE);
  T->State->CallRetStackBase = Base;
  Frame->State.callret_sp = reinterpret_cast<uint64_t>(Base) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;

  // GDT: the flat 64-bit and 32-bit code segments of Linux. A far transfer to GUEST_USER32_CS runs 32-bit code, as
  // Windows code does with its own 0x23, which is Linux's too.
  Frame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = &T->Gdt[0];
  Frame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = &T->Gdt[0];
  if (Parent) {
    memcpy(T->Gdt, Parent->Gdt, sizeof(T->Gdt));
    T->SigMask = Parent->SigMask;
  } else {
    // FEX selects the GDT entry with Selector >> 3, so the RPL bits are safe.
    for (uint16_t Selector : {GUEST_USER32_CS, GUEST_USER_CS}) {
      auto* Seg = CPUState::GetSegmentFromIndex(Frame->State, Selector);
      CPUState::SetGDTBase(Seg, 0);
      CPUState::SetGDTLimit(Seg, 0xF'FFFFU);
      Seg->L = Selector == GUEST_USER_CS; // long mode
      Seg->D = Selector == GUEST_USER32_CS;
    }
    LoadGuestCodeSegments(Frame->State, GUEST_USER_CS);
  }

  {
    std::lock_guard<std::mutex> lk(G->ThreadsLock);
    G->Threads.push_back(T);
  }
  return T;
}

void DestroyGuestThread(GuestThread* T) {
  {
    std::lock_guard<std::mutex> lk(G->ThreadsLock);
    std::erase(G->Threads, T);
  }
  G->CTX->DestroyThread(T->State);
  if (T->CallRetAlloc) {
    ::munmap(T->CallRetAlloc, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * FEXCore::Utils::FEX_HOST_PAGE_SIZE);
  }
  delete T;
}

void ForEachThread(void (*Fn)(GuestThread*, void*), void* Arg) {
  std::lock_guard<std::mutex> lk(G->ThreadsLock);
  for (auto* T : G->Threads) Fn(T, Arg);
}

void RunGuestThread(GuestThread* T) {
  namespace X = FEXCore::X86State;
  G->Signals->RegisterThread(T);
  auto& St = T->State->CurrentFrame->State;
  St.rip = T->StartRip;
  St.gregs[X::REG_RSP] = T->StartRsp;
  St.gregs[X::REG_RAX] = T->StartRax;
  if (T->StartFsBase) St.fs_cached = T->StartFsBase;
  G->CTX->ExecuteThread(T->State);
  // ExecuteThread returns when the thread is told to stop. The process-wide exit paths never get here.
}

[[noreturn]] void ExitProcess(int Code) {
  HitchLog::LogProfileNow("exit");
  // Diagnostics: FEXDARWIN_EXIT_PAUSE=<seconds> keeps the host process alive before it ends, so its address space can be
  // inspected with vmmap. The supervisor watchdog still ends it if the run is torn down.
  if (const char* P = getenv("FEXDARWIN_EXIT_PAUSE"); P && *P) sleep(static_cast<unsigned>(atoi(P)));
  _exit(Code);
}

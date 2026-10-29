// SPDX-License-Identifier: MIT
#include "Sampler.h"
#include "FaultStats.h"
#include "Host.h"

#include "Interface/Core/CPUBackend.h"
#include "Utils/variable_length_integer.h"

#include <FEXCore/Core/CodeInvalidation.h>
#include <FEXCore/Utils/GuestBase.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

extern "C" const struct mach_header_64 _mh_execute_header;
extern "C" int fexdarwin_lkd_vma_describe_nowait(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot,
                                                 uint64_t* off, char* name, unsigned long name_len);
extern "C" void fexdarwin_lkd_task_comm(lkd_task* t, char* out, unsigned long len);

namespace Sampler {
namespace {
using JITCodeHeader = FEXCore::CPU::CPUBackend::JITCodeHeader;
using JITCodeTail = FEXCore::CPU::CPUBackend::JITCodeTail;

enum Kind : uint8_t {
  KindJit = 1,     // host PC in the thread's current block: Rip is the guest instruction
  KindSyscall = 2, // inside LKD: Sys is the guest system call, Rip where the guest made it
  KindCompile = 3, // FEXCore is translating: Rip is the block being translated (the thread's RIP)
  KindHost = 4,    // anywhere else (dispatcher, FEXCore helpers, fault handling): Rip is the current block's entry
};

// One sample, written as is to sample-<pid>.bin (little endian, 64 bytes; the text file says "rec 64").
struct Rec {
  uint64_t Ns;        // CLOCK_UPTIME_RAW
  uint64_t HostPc;
  uint64_t Rip;
  uint64_t BlockRip;  // entry of the current block (0 when the thread has none)
  uint32_t BlockSize; // its guest size in bytes
  uint32_t Insn;      // the host instruction word at HostPc (0 when unreadable)
  int32_t Tid;
  uint16_t Sys;
  uint8_t Kind;
  uint8_t Pad;
  uint64_t HostBegin; // host address of the current block's code (its JIT header)
  uint32_t HostSize;  // its host size in bytes
  uint32_t Pad2;
};
static_assert(sizeof(Rec) == 64);

bool ReadHost(uint64_t Addr, void* Out, size_t Len) {
  mach_vm_size_t Got = 0;
  return mach_vm_read_overwrite(mach_task_self(), Addr, Len, reinterpret_cast<mach_vm_address_t>(Out), &Got) == KERN_SUCCESS &&
         Got == Len;
}

// What a suspended thread is doing. Reads only memory and registers: no lock, no allocation (the thread may hold the
// malloc lock or an engine lock while it is suspended).
void Classify(GuestThread* T, uint64_t Pc, Rec& R) {
  const auto* Frame = T->State->CurrentFrame;
  R.Rip = Frame->State.rip;
  const uint64_t Begin = Frame->State.InlineJITBlockHeader;
  JITCodeHeader H {};
  JITCodeTail Tail {};
  const bool HaveBlock = Begin && ReadHost(Begin, &H, sizeof H) && ReadHost(Begin + H.OffsetToBlockTail, &Tail, sizeof Tail);
  if (HaveBlock) {
    R.BlockRip = Tail.RIP;
    R.BlockSize = static_cast<uint32_t>(std::min<size_t>(Tail.GuestSize, UINT32_MAX));
    R.HostBegin = Begin;
    R.HostSize = static_cast<uint32_t>(std::min<size_t>(Tail.Size, UINT32_MAX));
  }
  if (T->InLkd.load(std::memory_order_relaxed)) {
    R.Kind = KindSyscall;
    R.Sys = static_cast<uint16_t>(T->OrigRax);
    return;
  }
  if (T->Compiling) {
    R.Kind = KindCompile;
    return;
  }
  if (!HaveBlock || Pc < Begin || Pc >= Begin + Tail.Size) {
    R.Kind = KindHost;
    if (HaveBlock) R.Rip = Tail.RIP;
    return;
  }
  // Same walk as ContextImpl::RestoreRIPFromHostPC, through copies of the entries.
  uint8_t Entries[4096];
  const size_t Len = std::min<size_t>(sizeof Entries, static_cast<size_t>(Tail.NumberOfRIPEntries) * 16);
  const uint64_t EntriesAt = Begin + H.OffsetToBlockTail + Tail.OffsetToRIPEntries;
  size_t Got = 0;
  // The entries may end near the end of a mapping: read what is there.
  for (size_t Step = Len; Step >= 16 && !Got; Step /= 2) {
    if (ReadHost(EntriesAt, Entries, Step)) Got = Step;
  }
  uint64_t HostAt = Begin, RipAt = Tail.RIP;
  const uint8_t* P = Entries;
  for (uint32_t I = 0; I < Tail.NumberOfRIPEntries && P + 16 <= Entries + Got; ++I) {
    const auto Off = FEXCore::Utils::vl64pair::Decode(P);
    P += Off.Size;
    if (Pc < HostAt + Off.IntegerARMPC) break;
    HostAt += Off.IntegerARMPC;
    RipAt += Off.IntegerX86RIP;
  }
  R.Kind = KindJit;
  R.Rip = RipAt;
}

struct Target {
  GuestThread* T;
  thread_t Port;
};

class State {
public:
  void Run();

private:
  void Tick();
  void Flush(bool Final);
  void WriteText(FILE* F);

  std::string Dir;
  uint64_t PeriodUs = 1000;
  std::vector<Rec> Recs;
  std::vector<Target> Targets;
  std::unordered_map<int32_t, std::string> Comms;
  std::unordered_set<uint64_t> DescribedPages; // guest 4 KiB pages named in the text file
  std::unordered_map<uint64_t, uint32_t> BlockSizes;
  std::unordered_set<uint64_t> DumpedBlocks;
  std::unordered_map<uint64_t, uint64_t> BlockHits; // since the last flush
  struct HostBlock {
    uint64_t Rip;
    uint32_t Size;
    uint64_t Hits;
  };
  std::unordered_map<uint64_t, HostBlock> HostHits; // by host begin, since the last flush
  std::unordered_set<uint64_t> DumpedHost;
  uint64_t LastFlush = 0;
  uint64_t Ticks = 0, Suspends = 0, TickNs = 0;
  unsigned SeenExec = 0;
};

std::atomic<unsigned> ExecCount {0};
std::atomic<bool> Running {false};
std::atomic<int> Finishing {0}; // 1: ExitProcess asks for the last flush, 2: done

uint64_t NowNs() {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void State::Tick() {
  const uint64_t T0 = NowNs();
  Targets.clear();
  if (!G->ThreadsLock.try_lock()) return;
  // The lock stays held while threads are suspended: none of them can be destroyed under the sampler, and none waits for
  // it while suspended (a thread waiting for the lock is not on a CPU, so it is not suspended).
  for (GuestThread* T : G->Threads) {
    if (!T->PThread || !T->State || !T->State->CurrentFrame) continue;
    const thread_t Port = pthread_mach_thread_np(T->PThread);
    if (Port == MACH_PORT_NULL) continue;
    thread_basic_info_data_t Info {};
    mach_msg_type_number_t C = THREAD_BASIC_INFO_COUNT;
    if (thread_info(Port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&Info), &C) != KERN_SUCCESS) continue;
    if (Info.run_state != TH_STATE_RUNNING) continue;
    Targets.push_back({T, Port});
  }
  if (Recs.capacity() - Recs.size() < Targets.size()) {
    G->ThreadsLock.unlock();
    return; // full until the next flush
  }
  for (const Target& X : Targets) {
    if (thread_suspend(X.Port) != KERN_SUCCESS) continue;
    Suspends++;
    arm_thread_state64_t St {};
    mach_msg_type_number_t C = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(X.Port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&St), &C) == KERN_SUCCESS) {
      Rec R {};
      R.Ns = NowNs();
      R.Tid = X.T->Tid;
      R.HostPc = arm_thread_state64_get_pc(St);
      Classify(X.T, R.HostPc, R);
      ReadHost(R.HostPc, &R.Insn, sizeof R.Insn);
      Recs.push_back(R); // capacity checked above: no allocation
    }
    thread_resume(X.Port);
  }
  G->ThreadsLock.unlock();
  Ticks++;
  TickNs += NowNs() - T0;
}

void State::WriteText(FILE* F) {
  // Thread names (guest comm), for the threads of the samples written so far.
  if (G->ThreadsLock.try_lock()) {
    for (GuestThread* T : G->Threads) {
      if (!T->Task) continue;
      char Comm[32] = "";
      fexdarwin_lkd_task_comm(T->Task, Comm, sizeof Comm);
      auto [It, New] = Comms.try_emplace(T->Tid, Comm);
      if (New || It->second != Comm) {
        It->second = Comm;
        fprintf(F, "thread %d %s\n", T->Tid, Comm);
      }
    }
    G->ThreadsLock.unlock();
  }
  // Guest pages of the samples: their mapping (module name and file offset), once per page. They are looked up through
  // the task of a thread in G->Threads while ThreadsLock is held (a thread leaves the list before its task is released),
  // and nothing is allocated meanwhile: a guest thread may wait for that lock while it holds the malloc lock.
  std::vector<uint64_t> Pages;
  for (const Rec& R : Recs) {
    for (uint64_t V : {R.Rip, R.BlockRip}) {
      if (V && !DescribedPages.count(V & ~0xfffull)) Pages.push_back(V & ~0xfffull);
    }
  }
  std::sort(Pages.begin(), Pages.end());
  Pages.erase(std::unique(Pages.begin(), Pages.end()), Pages.end());
  struct Mapping {
    uint64_t S, E, Off;
    char Name[512];
  };
  std::vector<Mapping> Mappings;
  Mappings.reserve(Pages.size());
  if (!Pages.empty() && G->ThreadsLock.try_lock()) {
    lkd_task* Task = nullptr;
    for (GuestThread* T : G->Threads) {
      if (T->Task) {
        Task = T->Task;
        break;
      }
    }
    uint64_t Covered = 0; // the pages are sorted: those of the mapping just found are named by its line
    for (uint64_t Page : Pages) {
      if (!Task || Page < Covered) continue;
      Mapping M;
      uint32_t Prot;
      // A page whose mapping cannot be read without waiting is retried at the next flush.
      if (fexdarwin_lkd_vma_describe_nowait(Task, Page, &M.S, &M.E, &Prot, &M.Off, M.Name, sizeof M.Name) != 0) continue;
      Covered = std::min(M.E, M.S + (64ull << 20));
      Mappings.push_back(M);
    }
    G->ThreadsLock.unlock();
  }
  for (const Mapping& M : Mappings) {
    fprintf(F, "vma %#llx %#llx %#llx %s\n", (unsigned long long)M.S, (unsigned long long)M.E, (unsigned long long)M.Off,
            M.Name[0] ? M.Name : "[anon]");
    // The whole mapping is described by this line.
    for (uint64_t P = M.S & ~0xfffull; P < M.E && P < M.S + (64ull << 20); P += 0x1000) DescribedPages.insert(P);
  }
  // The guest code of the hottest blocks of this period, once per block, for disassembly.
  std::vector<std::pair<uint64_t, uint64_t>> Hot(BlockHits.begin(), BlockHits.end());
  std::sort(Hot.begin(), Hot.end(), [](auto& A, auto& B) { return A.second > B.second; });
  int Dumped = 0;
  for (auto& [Rip, Hits] : Hot) {
    if (Dumped >= 256 || Hits < 5) break;
    if (!DumpedBlocks.insert(Rip).second) continue;
    const uint32_t Size = std::min<uint32_t>(BlockSizes[Rip], 4096);
    if (!Size) continue;
    std::vector<uint8_t> Code(Size);
    if (!ReadHost(FEXCore::GuestBase + Rip, Code.data(), Size)) continue;
    fprintf(F, "code %#llx ", (unsigned long long)Rip);
    for (uint8_t B : Code) fprintf(F, "%02x", B);
    fputc('\n', F);
    Dumped++;
  }
  BlockHits.clear();
  // The host code FEX made for the hottest blocks (a block translated again has a new host copy).
  std::vector<std::pair<uint64_t, HostBlock>> HotHost(HostHits.begin(), HostHits.end());
  std::sort(HotHost.begin(), HotHost.end(), [](auto& A, auto& B) { return A.second.Hits > B.second.Hits; });
  int DumpedH = 0;
  for (auto& [Begin, B] : HotHost) {
    if (DumpedH >= 256 || B.Hits < 5) break;
    if (!DumpedHost.insert(Begin).second) continue;
    const uint32_t Size = std::min<uint32_t>(B.Size, 16384);
    std::vector<uint8_t> Code(Size);
    if (!Size || !ReadHost(Begin, Code.data(), Size)) continue;
    fprintf(F, "hostcode %#llx %#llx ", (unsigned long long)B.Rip, (unsigned long long)Begin);
    for (uint8_t X : Code) fprintf(F, "%02x", X);
    fputc('\n', F);
    DumpedH++;
  }
  HostHits.clear();
}

void State::Flush(bool Final) {
  for (const Rec& R : Recs) {
    if (R.BlockRip) {
      BlockSizes[R.BlockRip] = R.BlockSize;
      if (R.Kind == KindJit) {
        BlockHits[R.BlockRip]++;
        auto& H = HostHits[R.HostBegin];
        H.Rip = R.BlockRip;
        H.Size = R.HostSize;
        H.Hits++;
      }
    }
  }
  char Path[1024];
  snprintf(Path, sizeof Path, "%s/sample-%d.bin", Dir.c_str(), static_cast<int>(getpid()));
  if (FILE* F = fopen(Path, "ab")) {
    fwrite(Recs.data(), sizeof(Rec), Recs.size(), F);
    fclose(F);
  }
  snprintf(Path, sizeof Path, "%s/sample-%d.txt", Dir.c_str(), static_cast<int>(getpid()));
  if (FILE* F = fopen(Path, "a")) {
    if (!LastFlush) {
      fprintf(F, "fexdarwin-run %#llx period_us %llu guest_pid %d rec 64\n", (unsigned long long)reinterpret_cast<uint64_t>(&_mh_execute_header),
              (unsigned long long)PeriodUs, G->Pid);
    }
    WriteText(F);
    fprintf(F, "faults");
    for (unsigned K = 0; K < FaultStats::Count; K++) fprintf(F, " %s %llu", FaultStats::Name(K), (unsigned long long)FaultStats::Counts[K].load());
    fputc('\n', F);
    if (Final || (Ticks / std::max<uint64_t>(1, 1000000 / PeriodUs)) % 30 == 0) {
      // The busiest guarded-fault sites: host pc, path, count, last guest address (the report names the guest RIP).
      std::vector<std::tuple<uint32_t, uint64_t, uint32_t, uint64_t>> Top;
      for (const auto& X : FaultStats::Sites) {
        if (const uint64_t Pc = X.Pc.load(std::memory_order_relaxed)) Top.emplace_back(X.Count.load(), Pc, X.Kind.load(), X.Addr.load());
      }
      std::sort(Top.begin(), Top.end(), [](auto& A, auto& B) { return std::get<0>(A) > std::get<0>(B); });
      for (size_t I = 0; I < Top.size() && I < 40; I++) {
        fprintf(F, "faultsite %#llx %s %u %#llx\n", (unsigned long long)std::get<1>(Top[I]), FaultStats::Name(std::get<2>(Top[I])),
                std::get<0>(Top[I]), (unsigned long long)std::get<3>(Top[I]));
      }
    }
    const auto& Inv = FEXCore::Context::CodeInvalidations;
    fprintf(F, "flush %llu samples %zu ticks %llu suspends %llu tick_us_avg %.1f invalidations %llu units recorded %llu skipped %llu revived %llu%s\n",
            (unsigned long long)NowNs(), Recs.size(), (unsigned long long)Ticks, (unsigned long long)Suspends, Ticks ? TickNs / 1e3 / Ticks : 0.0,
            (unsigned long long)Inv.Requests.load(), (unsigned long long)Inv.UnitsRecorded.load(), (unsigned long long)Inv.UnitsSkipped.load(),
            (unsigned long long)Inv.UnitsRevived.load(), Final ? " final" : "");
    fclose(F);
  }
  Recs.clear();
  LastFlush = NowNs();
}

void State::Run() {
  pthread_setname_np("fexdarwin-sampler");
  PeriodUs = std::max<uint64_t>(100, strtoull(getenv("FEXDARWIN_SAMPLE"), nullptr, 10));
  const char* D = getenv("FEXDARWIN_SAMPLE_DIR");
  if (!D) D = getenv("FEXDARWIN_DUMP_DIR");
  Dir = D ? D : "/tmp";
  Recs.reserve(1 << 18);
  Targets.reserve(1024);
  // Only processes that run guest code for a while are worth a file: the first flush comes after 1 s.
  LastFlush = 0;
  uint64_t Start = NowNs(), Next = Start;
  for (;;) {
    Next += PeriodUs * 1000;
    const uint64_t Now = NowNs();
    if (Next > Now) {
      usleep(static_cast<useconds_t>((Next - Now) / 1000));
    } else {
      Next = Now; // late: no catch-up burst
    }
    if (const unsigned X = ExecCount.load(); X != SeenExec) {
      // A new image replaced the address space: what the text file named so far belongs to the old one.
      if (!Recs.empty()) Flush(false);
      SeenExec = X;
      DescribedPages.clear();
      DumpedBlocks.clear();
      BlockSizes.clear();
      char Path[1024];
      snprintf(Path, sizeof Path, "%s/sample-%d.txt", Dir.c_str(), static_cast<int>(getpid()));
      if (FILE* F = fopen(Path, "a")) {
        fprintf(F, "exec %llu\n", (unsigned long long)NowNs());
        fclose(F);
      }
    }
    if (Finishing.load() == 1) {
      Flush(true);
      Finishing.store(2);
      for (;;) pause();
    }
    Tick();
    const uint64_t Since = LastFlush ? LastFlush : Start;
    if (NowNs() - Since > 1'000'000'000ull || Recs.size() + 1024 > Recs.capacity()) {
      if (!Recs.empty()) Flush(false);
      else if (!LastFlush) Start = NowNs();
    }
  }
}
} // namespace

void NoteExec() {
  ExecCount.fetch_add(1);
}

void Finish() {
  if (!Running.load()) return;
  int Expected = 0;
  if (!Finishing.compare_exchange_strong(Expected, 1)) return;
  for (int I = 0; I < 200 && Finishing.load() != 2; I++) usleep(5000);
}

void Start() {
  const char* E = getenv("FEXDARWIN_SAMPLE");
  if (!E || !*E || !strcmp(E, "0")) return;
  Running.store(true);
  Finishing.store(0);
  FaultStats::Enabled.store(true, std::memory_order_relaxed);
  pthread_t Th;
  // Leaked on purpose: the thread runs until the process ends (a host fork child starts its own).
  if (pthread_create(&Th, nullptr, [](void*) -> void* { (new State)->Run(); return nullptr; }, nullptr) == 0) pthread_detach(Th);
}
} // namespace Sampler

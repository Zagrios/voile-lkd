// SPDX-License-Identifier: MIT
// Engine side of the LKD contract (src/lkd/include/lkd/lkd.h): FEXCore provides the CPU, LKD the Linux kernel.
// Enabled with FEXDARWIN_LKD=1; the older built-in syscall table of DarwinSyscallHandler stays the default until LKD
// can run the same witnesses.
#include "CodeValidation.h"
#include "GuardedLoadPatch.h"
#include "CrashReport.h"
#include "DarwinSignalDelegator.h"
#include "HitchLog.h"
#include "GuestSignalAbi.h"
#include "Host.h"
#include "LkdGlue.h"
#include "LkdLoader.h"

#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/GuestBase.h>
#include <FEXCore/Utils/JITWriteScope.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace X = FEXCore::X86State;
using FEXCore::Core::CPUState;

// The kick handler resumes a thread taken out of translated code here, with x0 = its GuestThread, x28 = its FEX state
// frame (callee-saved, untouched) and the host stack pointer of the interrupted block. The C part delivers the signals and
// returns the dispatcher entry that refills the static registers; x1 = 0 asks that entry for a normal block.
extern "C" void fexdarwin_kick_trampoline();
extern "C" uint64_t fexdarwin_kick_safepoint(GuestThread* T);
asm(R"(
  .text
  .p2align 2
  .globl _fexdarwin_kick_trampoline
_fexdarwin_kick_trampoline:
  bl _fexdarwin_kick_safepoint
  mov x16, x0
  mov x1, #0
  br x16
)");

namespace {
constexpr int KickSignal = SIGUSR2;

GuestThread* GT(lkd_task* t) {
  return static_cast<GuestThread*>(lkd_task_engine_data(t));
}

// ---- registers ----
int GetRegs(lkd_task* t, lkd_regs* R) {
  auto* T = GT(t);
  if (!T) return -LX_ESRCH;
  if (T != CurrentGuestThread && !T->Stopped.load()) return -LX_ESRCH; // a running task's registers are not in memory
  auto* Th = T->State;
  const auto& St = Th->CurrentFrame->State;
  R->r15 = St.gregs[X::REG_R15]; R->r14 = St.gregs[X::REG_R14]; R->r13 = St.gregs[X::REG_R13]; R->r12 = St.gregs[X::REG_R12];
  R->rbp = St.gregs[X::REG_RBP]; R->rbx = St.gregs[X::REG_RBX]; R->r11 = St.gregs[X::REG_R11]; R->r10 = St.gregs[X::REG_R10];
  R->r9 = St.gregs[X::REG_R9]; R->r8 = St.gregs[X::REG_R8]; R->rax = St.gregs[X::REG_RAX]; R->rcx = St.gregs[X::REG_RCX];
  R->rdx = St.gregs[X::REG_RDX]; R->rsi = St.gregs[X::REG_RSI]; R->rdi = St.gregs[X::REG_RDI];
  R->orig_rax = T->OrigRax;
  R->rip = St.rip;
  R->cs = St.cs_idx;
  R->eflags = GuestEflags(Th);
  R->rsp = St.gregs[X::REG_RSP];
  R->ss = St.ss_idx;
  R->fs_base = St.fs_cached;
  R->gs_base = St.gs_cached;
  R->ds = St.ds_idx; R->es = St.es_idx; R->fs = St.fs_idx; R->gs = St.gs_idx;
  return 0;
}

int SetRegs(lkd_task* t, const lkd_regs* R) {
  auto* T = GT(t);
  if (!T) return -LX_ESRCH;
  if (T != CurrentGuestThread && !T->Stopped.load()) return -LX_ESRCH;
  auto* Th = T->State;
  auto& St = Th->CurrentFrame->State;
  St.gregs[X::REG_R15] = R->r15; St.gregs[X::REG_R14] = R->r14; St.gregs[X::REG_R13] = R->r13; St.gregs[X::REG_R12] = R->r12;
  St.gregs[X::REG_RBP] = R->rbp; St.gregs[X::REG_RBX] = R->rbx; St.gregs[X::REG_R11] = R->r11; St.gregs[X::REG_R10] = R->r10;
  St.gregs[X::REG_R9] = R->r9; St.gregs[X::REG_R8] = R->r8; St.gregs[X::REG_RAX] = R->rax; St.gregs[X::REG_RCX] = R->rcx;
  St.gregs[X::REG_RDX] = R->rdx; St.gregs[X::REG_RSI] = R->rsi; St.gregs[X::REG_RDI] = R->rdi;
  T->OrigRax = R->orig_rax;
  if (St.rip != R->rip) T->SyscallSetsRip = true;
  St.rip = R->rip;
  St.gregs[X::REG_RSP] = R->rsp;
  G->CTX->SetFlagsFromCompactedEFLAGS(Th, static_cast<uint32_t>(R->eflags));
  St.fs_cached = R->fs_base;
  St.gs_cached = R->gs_base;
  // Linux only accepts the user selectors here; keep what the guest can legally hold.
  St.cs_idx = static_cast<uint16_t>(R->cs); St.ss_idx = static_cast<uint16_t>(R->ss);
  St.ds_idx = static_cast<uint16_t>(R->ds); St.es_idx = static_cast<uint16_t>(R->es);
  St.fs_idx = static_cast<uint16_t>(R->fs); St.gs_idx = static_cast<uint16_t>(R->gs);
  return 0;
}

int GetFp(lkd_task* t, int Kind, void* Buf, size_t* Len) {
  auto* T = GT(t);
  if (!T) return -LX_ESRCH;
  Linux::XState Xs;
  SaveGuestFpState(T->State, &Xs);
  const size_t Full = Kind == LKD_FP_FXSAVE ? sizeof(Linux::FxSave) : Kind == LKD_FP_XSAVE ? sizeof(Linux::XState) : 0;
  if (!Full) return -LX_EINVAL;
  memcpy(Buf, &Xs, std::min(*Len, Full));
  *Len = Full;
  return 0;
}

int SetFp(lkd_task* t, int Kind, const void* Buf, size_t Len) {
  auto* T = GT(t);
  if (!T) return -LX_ESRCH;
  Linux::XState Xs;
  SaveGuestFpState(T->State, &Xs); // keep the parts the caller does not provide
  const size_t Full = Kind == LKD_FP_FXSAVE ? sizeof(Linux::FxSave) : Kind == LKD_FP_XSAVE ? sizeof(Linux::XState) : 0;
  if (!Full) return -LX_EINVAL;
  memcpy(&Xs, Buf, std::min(Len, Full));
  RestoreGuestFpState(T->State, &Xs);
  return 0;
}

// ---- kick ----
// A kicked thread is in one of four places:
//   - blocked in a host system call inside LKD: the signal makes it return EINTR and LKD re-checks on its way out;
//   - inside LKD otherwise (InLkd): LkdHandleSyscall checks lkd_signal_pending before resuming guest code;
//   - in translated code: the handler spills the guest registers and resumes the host thread at the kick trampoline,
//     which runs lkd_signal_next on the ordinary thread stack (it may block in a ptrace or job-control stop) and then
//     re-enters the dispatcher loop;
//   - in the dispatcher, in a deferred-signal section or in host C++: the guest registers are not exact there, so the
//     retry thread sends the kick again 1 ms later until the thread is in one of the three places above.
std::atomic<uint32_t> KickEpoch {0};

// Guest threads that ended, waiting for their host thread to be joined and their FEX state freed. Each thread owns a
// FEX lookup cache (about 260 MiB of reserved address space), JIT buffers, a call-ret stack and an 8 MiB host stack; a
// game that creates and ends threads all the time exhausted the host address space when they were never freed
// (request wine-9: "LookupCache: Failed to allocate PagePointer" after 112-134 s).
std::mutex DeadLock;
std::vector<std::pair<GuestThread*, pthread_t>> DeadThreads;

void ReapDeadThreads() {
  std::vector<std::pair<GuestThread*, pthread_t>> Dead;
  {
    std::lock_guard<std::mutex> Lk(DeadLock);
    Dead.swap(DeadThreads);
  }
  for (auto& [T, Host] : Dead) {
    pthread_join(Host, nullptr); // the thread is past pthread_exit or about to be: returns at once
    G->CTX->DestroyThread(T->State);
    if (T->CallRetAlloc) {
      ::munmap(T->CallRetAlloc, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * FEXCore::Utils::FEX_HOST_PAGE_SIZE);
    }
    delete T;
  }
}

[[noreturn]] void EndGuestThread(GuestThread* T) {
  pthread_t Host;
  {
    std::lock_guard<std::mutex> Lk(G->ThreadsLock);
    std::erase(G->Threads, T);
    Host = T->PThread;
    T->PThread = nullptr; // no more kicks: the host thread is going away
  }
  int Status = 0;
  G->Signals->UnregisterThread(T);
  if (lkd_task_release(T->Task, &Status)) {
    ExitProcess(Status);
  }
  // The main guest thread is joined by RunWithLkd itself; every other thread is reaped by a later thread creation.
  if (!T->IsMain && Host) {
    std::lock_guard<std::mutex> Lk(DeadLock);
    DeadThreads.emplace_back(T, Host);
  }
  pthread_exit(nullptr);
}

// Loops lkd_signal_next until nothing is left. The guest registers must be current in the thread state.
void DeliverPendingSignals(GuestThread* T) {
  T->KickRequested.store(false);
  while (lkd_signal_pending(T->Task)) {
    const int R = lkd_signal_next(T->Task);
    if (G->Opt.TraceSignals) fprintf(stderr, "[fexdarwin] tid %d lkd_signal_next = %d, rip now %#llx\n", T->Tid, R,
                                     (unsigned long long)T->State->CurrentFrame->State.rip);
    if (R == 0) break;
    if (R < 0) EndGuestThread(T);
  }
}

void KickHandler(int, siginfo_t*, void* Uctx) {
  GuestThread* T = CurrentGuestThread;
  if (!T || !T->Task || T->InLkd.load()) return;
  if (!lkd_signal_pending(T->Task)) {
    T->KickRequested.store(false);
    return;
  }
  G->Signals->RedirectFromJit(T, static_cast<ucontext_t*>(Uctx), reinterpret_cast<uint64_t>(&fexdarwin_kick_trampoline));
}

void Kick(lkd_task* t) {
  auto* T = GT(t);
  if (!T || T == CurrentGuestThread) return;
  pthread_t P = T->PThread;
  if (!P) return;
  T->KickRequested.store(true);
  pthread_kill(P, KickSignal);
  KickEpoch.fetch_add(1);
  os_sync_wake_by_address_any(&KickEpoch, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// ---- supervisor watchdog (machine safety) ----
// Every host process of a guest family follows one supervisor process (FEXDARWIN_SUPERVISOR_PID, set by
// run-with-timeout.sh or by the first process of the family) and ends itself as soon as that process is gone. Guest
// processes are host fork()s; without this, a run whose wrapper was killed leaves its processes running (2026-10-02: 332
// leaked fexdarwin-run processes exhausted the kernel VM map entry zone and panicked the Mac).
// Number of VM regions of this process, counted up to Limit (each region is at least one kernel VM map entry; the
// kernel zone of map entries is global and its exhaustion panics the machine).
uint64_t CountOwnRegions(uint64_t Limit) {
  mach_vm_address_t Addr = 0;
  uint64_t N = 0;
  while (N < Limit) {
    mach_vm_size_t Size = 0;
    natural_t Depth = 0;
    vm_region_submap_short_info_data_64_t Info;
    mach_msg_type_number_t Count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &Addr, &Size, &Depth, reinterpret_cast<vm_region_recurse_info_t>(&Info), &Count) !=
        KERN_SUCCESS) {
      break;
    }
    ++N;
    Addr += Size;
  }
  return N;
}

// ---- thread dump (diagnostics) ----
// FEXDARWIN_DUMP_DIR=<host dir>: when <dir>/dump-request appears or changes, every guest process of the family writes
// <dir>/dump-<host pid>-<n>.txt: first the backtrace of every host thread (CrashReport::HostThreads), then per guest
// thread its tid, the system call it is in (number and arguments) or "running", its registers and the stack words that
// point into executable mappings (module name and offset). Read-only for the guest: memory is read with
// mach_vm_read_overwrite (no fault), nothing is changed, nothing is sent to the guest. It never waits for a lock: a
// process frozen by an engine lock held forever (ELDEN RING's world loading) still writes its dump.
extern "C" int fexdarwin_lkd_vma_describe_nowait(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot,
                                                 uint64_t* off, char* name, unsigned long name_len);

static bool ReadGuest(uint64_t Gva, void* Out, size_t Len) {
  mach_vm_size_t Got = 0;
  return mach_vm_read_overwrite(mach_task_self(), FEXCore::GuestBase + Gva, Len, reinterpret_cast<mach_vm_address_t>(Out), &Got) ==
           KERN_SUCCESS && Got == Len;
}

static void DescribeCode(FILE* F, lkd_task* Task, uint64_t V) {
  uint64_t S, E, Off;
  uint32_t Prot;
  char Name[256];
  if (const int R = fexdarwin_lkd_vma_describe_nowait(Task, V, &S, &E, &Prot, &Off, Name, sizeof Name); R != 0) {
    fprintf(F, "%#llx ? (%d)", (unsigned long long)V, R);
    return;
  }
  fprintf(F, "%#llx %s+%#llx (vma %#llx%s)", (unsigned long long)V, Name[0] ? Name : "[anon]", (unsigned long long)(V - S + Off),
          (unsigned long long)S, (Prot & 4) ? " x" : "");
}

// ThreadsLock within about a second, or false.
static bool TryLockThreads() {
  for (int I = 0; I < 100; I++) {
    if (G->ThreadsLock.try_lock()) return true;
    usleep(10 * 1000);
  }
  return false;
}

static void DumpThreads(const char* Dir, unsigned Seq) {
  char Path[1024];
  snprintf(Path, sizeof Path, "%s/dump-%d-%u.txt", Dir, static_cast<int>(getpid()), Seq);
  FILE* F = fopen(Path, "a"); // appends: HostThreads writes to the same file through its descriptor
  if (!F) return;
  std::vector<CrashReport::ThreadLabel> Labels;
  const bool Listed = TryLockThreads();
  if (Listed) {
    for (GuestThread* T : G->Threads) {
      uint64_t Id = 0;
      if (T->PThread && pthread_threadid_np(T->PThread, &Id) == 0) Labels.push_back({Id, T->Tid});
    }
    G->ThreadsLock.unlock();
  }
  fprintf(F, "pid %d (guest %d)%s\n\n", static_cast<int>(getpid()), G->Pid, Listed ? "" : ", ThreadsLock busy for 1 s");
  fflush(F);
  CrashReport::HostThreads(fileno(F), Labels.data(), Labels.size());
  if (!TryLockThreads()) {
    fprintf(F, "\nguest threads not listed: ThreadsLock busy for 1 s\n");
    fclose(F);
    return;
  }
  fprintf(F, "\n%zu guest threads\n", G->Threads.size());
  for (GuestThread* T : G->Threads) {
    if (!T->State || !T->State->CurrentFrame) continue;
    const auto& St = T->State->CurrentFrame->State;
    const bool In = T->InLkd.load();
    fprintf(F, "\n== tid %d %s", T->Tid, In ? "in syscall" : "running (registers may be stale)");
    if (In)
      fprintf(F, " nr %llu (%#llx, %#llx, %#llx, %#llx, %#llx, %#llx)", (unsigned long long)T->OrigRax,
              (unsigned long long)T->SysArgs[0], (unsigned long long)T->SysArgs[1], (unsigned long long)T->SysArgs[2],
              (unsigned long long)T->SysArgs[3], (unsigned long long)T->SysArgs[4], (unsigned long long)T->SysArgs[5]);
    if (In && T->OrigRax == 202) { // futex: the word and, for a glibc mutex, its owner tid at +8
      uint32_t W[4] = {};
      if (ReadGuest(T->SysArgs[0], W, sizeof W))
        fprintf(F, "\n   futex word %#x, next %#x, owner(+8) %u, +12 %#x", W[0], W[1], W[2], W[3]);
    }
    fprintf(F, "\n   rip ");
    DescribeCode(F, T->Task, St.rip);
    const uint64_t Rsp = St.gregs[FEXCore::X86State::REG_RSP];
    fprintf(F, "\n   rsp %#llx rbp %#llx rax %#llx rbx %#llx rcx %#llx rdx %#llx rsi %#llx rdi %#llx\n", (unsigned long long)Rsp,
            (unsigned long long)St.gregs[FEXCore::X86State::REG_RBP], (unsigned long long)St.gregs[FEXCore::X86State::REG_RAX],
            (unsigned long long)St.gregs[FEXCore::X86State::REG_RBX], (unsigned long long)St.gregs[FEXCore::X86State::REG_RCX],
            (unsigned long long)St.gregs[FEXCore::X86State::REG_RDX], (unsigned long long)St.gregs[FEXCore::X86State::REG_RSI],
            (unsigned long long)St.gregs[FEXCore::X86State::REG_RDI]);
    // Stack words pointing into executable mappings: candidate return addresses, innermost first.
    uint64_t S, E, Off;
    uint32_t Prot;
    char Name[256];
    if (fexdarwin_lkd_vma_describe_nowait(T->Task, Rsp, &S, &E, &Prot, &Off, Name, sizeof Name) != 0) continue;
    uint64_t End = E < Rsp + 64 * 1024 ? E : Rsp + 64 * 1024;
    static uint64_t Buf[8192];
    const size_t Len = static_cast<size_t>((End - (Rsp & ~7ull)) & ~7ull);
    if (!Len || !ReadGuest(Rsp & ~7ull, Buf, Len)) continue;
    unsigned Hits = 0;
    for (size_t I = 0; I < Len / 8 && Hits < 48; I++) {
      const uint64_t V = Buf[I];
      if (V < 0x10000 || V >= (1ull << 47)) continue;
      uint64_t VS, VE, VO;
      uint32_t VP;
      char VN[2];
      if (fexdarwin_lkd_vma_describe_nowait(T->Task, V, &VS, &VE, &VP, &VO, VN, sizeof VN) != 0 || !(VP & 4)) continue;
      fprintf(F, "   [rsp+%#zx] ", I * 8);
      DescribeCode(F, T->Task, V);
      fputc('\n', F);
      Hits++;
    }
  }
  G->ThreadsLock.unlock();
  fclose(F);
}

static void MaybeDumpThreads() {
  static const char* Dir = getenv("FEXDARWIN_DUMP_DIR");
  static struct timespec Last {};
  static unsigned Seq;
  if (!Dir || !G) return;
  char Req[1024];
  snprintf(Req, sizeof Req, "%s/dump-request", Dir);
  struct stat Sb;
  if (stat(Req, &Sb) != 0) return;
  if (Sb.st_mtimespec.tv_sec == Last.tv_sec && Sb.st_mtimespec.tv_nsec == Last.tv_nsec) return;
  Last = Sb.st_mtimespec;
  DumpThreads(Dir, ++Seq);
}

void* SupervisorWatchThread(void*) {
  pthread_setname_np("fexdarwin-supervisor-watch");
  const char* E = getenv("FEXDARWIN_SUPERVISOR_PID");
  const pid_t Sup = E ? static_cast<pid_t>(strtol(E, nullptr, 10)) : 0;
  // A guest process normally has a few hundred regions (measured: dash about 400, Wine processes about 400, wineserver
  // about 460). 200000 is far above that and far below what panicked the Mac (177 million entries in total).
  constexpr uint64_t MaxRegions = 200000;
  for (unsigned Tick = 0;; ++Tick) {
    usleep(getenv("FEXDARWIN_REGION_LOG") ? 50 * 1000 : 500 * 1000);
    MaybeDumpThreads();
    static const bool LogRegions = getenv("FEXDARWIN_REGION_LOG") != nullptr;
    if (LogRegions || (Tick & 3) == 0) {
      const uint64_t N = CountOwnRegions(MaxRegions + 1);
      if (LogRegions) {
        char B[96];
        int L = snprintf(B, sizeof B, "[fexdarwin] pid %d: %llu VM regions\n", static_cast<int>(getpid()), (unsigned long long)N);
        ssize_t R = write(2, B, L);
        (void)R;
      }
      if (N > MaxRegions) {
        static const char Msg[] = "[fexdarwin] more than 200000 VM regions in this process: ending it to protect the kernel\n";
        ssize_t R = write(2, Msg, sizeof Msg - 1);
        (void)R;
        _exit(134);
      }
    }
    if (Sup > 1 && kill(Sup, 0) != 0 && errno == ESRCH) {
      static const char Msg[] = "[fexdarwin] supervisor process is gone: ending this guest process\n";
      ssize_t R = write(2, Msg, sizeof Msg - 1);
      (void)R;
      _exit(137);
    }
  }
  return nullptr;
}
void StartSupervisorWatch() {
  pthread_t W;
  if (pthread_create(&W, nullptr, SupervisorWatchThread, nullptr) == 0) pthread_detach(W);
}

void* KickRetryThread(void*);
void StartKickRetryThread() {
  pthread_t Retry;
  if (pthread_create(&Retry, nullptr, KickRetryThread, nullptr) == 0) pthread_detach(Retry);
}

void* KickRetryThread(void*) {
  pthread_setname_np("lkd-kick-retry");
  uint32_t Seen = 0;
  for (;;) {
    const uint32_t E = KickEpoch.load();
    if (E == Seen) {
      os_sync_wait_on_address(&KickEpoch, E, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_NONE);
      continue;
    }
    Seen = E;
    for (bool Any = true; Any;) {
      usleep(1000);
      Any = false;
      std::lock_guard<std::mutex> Lk(G->ThreadsLock);
      for (GuestThread* T : G->Threads) {
        if (T->KickRequested.load() && !T->InLkd.load() && T->PThread) {
          Any = true;
          pthread_kill(T->PThread, KickSignal);
        }
      }
    }
  }
  return nullptr;
}

// ---- threads ----
void* SpawnedThreadMain(void* P) {
  auto* T = static_cast<GuestThread*>(P);
  lkd_set_current(T->Task);
  RunGuestThread(T);
  return nullptr;
}

int SpawnThread(lkd_task* Child, const lkd_regs* Start) {
  GuestThread* Parent = CurrentGuestThread;
  ReapDeadThreads(); // first: the threads that ended give their host memory back to this one
  auto* C = CreateGuestThread(Parent);
  if (!C) return -LX_EAGAIN;
  C->Task = Child;
  C->Tid = lkd_task_tid(Child);
  lkd_task_set_engine_data(Child, C);
  auto& St = C->State->CurrentFrame->State;
  St.gregs[X::REG_R15] = Start->r15; St.gregs[X::REG_R14] = Start->r14; St.gregs[X::REG_R13] = Start->r13; St.gregs[X::REG_R12] = Start->r12;
  St.gregs[X::REG_RBP] = Start->rbp; St.gregs[X::REG_RBX] = Start->rbx; St.gregs[X::REG_R11] = Start->r11; St.gregs[X::REG_R10] = Start->r10;
  St.gregs[X::REG_R9] = Start->r9; St.gregs[X::REG_R8] = Start->r8; St.gregs[X::REG_RCX] = Start->rcx;
  St.gregs[X::REG_RDX] = Start->rdx; St.gregs[X::REG_RSI] = Start->rsi; St.gregs[X::REG_RDI] = Start->rdi;
  C->StartRip = Start->rip;
  C->StartRsp = Start->rsp;
  C->StartRax = Start->rax;
  C->StartFsBase = Start->fs_base;
  St.gs_cached = Start->gs_base;
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, 8 << 20);
  const int E = pthread_create(&C->PThread, &Attr, SpawnedThreadMain, C);
  pthread_attr_destroy(&Attr);
  if (E != 0) {
    // LKD drops the task on this error: forget it and free what was made for it.
    lkd_task_set_engine_data(Child, nullptr);
    DestroyGuestThread(C);
    return -LX_EAGAIN;
  }
  return 0;
}

void InvalidateCode(uint64_t Gva, uint64_t Len) {
  // [any] in the LKD contract: also called from LKD's own host threads (the mm service thread that performs a tracer's
  // POKE). Without a guest thread, FEX's deferred-signal guard falls back to masking host signals.
  GuestThread* T = CurrentGuestThread;
  HitchLog::Scope Timed(HitchLog::Kind::Invalidate, Len);
  G->Signals->InvalidateGuestCodeRange(T ? T->State : nullptr, Gva, Len);
}

// ---- signal frames ----
int FrameBuild(lkd_task* t, const lkd_delivery* D) {
  auto* T = GT(t);
  CancelGuardStep(T);
  auto* Th = T->State;
  auto& St = Th->CurrentFrame->State;
  const uint32_t EFlags = GuestEflags(Th);
  const uint64_t InterruptedSp = St.gregs[X::REG_RSP];
  uint64_t Sp = D->use_altstack ? D->altstack.ss_sp + D->altstack.ss_size : InterruptedSp - 128; // red zone
  Sp -= sizeof(Linux::XState);
  Sp &= ~63ULL;
  const uint64_t FpAddr = Sp;
  Sp -= sizeof(Linux::RtSigFrame);
  Sp = (Sp & ~15ULL) - 8; // rsp + 8 is 16-byte aligned at handler entry
  const uint64_t FrameAddr = Sp;
  const uint64_t FrameLen = FpAddr + sizeof(Linux::XState) - FrameAddr;

  Linux::RtSigFrame Fr {};
  Fr.pretcode = (D->flags & LX_SA_RESTORER) ? D->restorer : 0;
  Fr.uc.uc_flags = Linux::UC_FP_XSTATE | Linux::UC_SIGCONTEXT_SS | Linux::UC_STRICT_RESTORE_SS;
  Fr.uc.uc_stack.Sp = D->altstack.ss_sp;
  Fr.uc.uc_stack.Flags = D->altstack.ss_flags;
  Fr.uc.uc_stack.Size = D->altstack.ss_size;
  auto& Sc = Fr.uc.uc_mcontext;
  Sc.r8 = St.gregs[X::REG_R8]; Sc.r9 = St.gregs[X::REG_R9]; Sc.r10 = St.gregs[X::REG_R10]; Sc.r11 = St.gregs[X::REG_R11];
  Sc.r12 = St.gregs[X::REG_R12]; Sc.r13 = St.gregs[X::REG_R13]; Sc.r14 = St.gregs[X::REG_R14]; Sc.r15 = St.gregs[X::REG_R15];
  Sc.rdi = St.gregs[X::REG_RDI]; Sc.rsi = St.gregs[X::REG_RSI]; Sc.rbp = St.gregs[X::REG_RBP]; Sc.rbx = St.gregs[X::REG_RBX];
  Sc.rdx = St.gregs[X::REG_RDX]; Sc.rax = St.gregs[X::REG_RAX]; Sc.rcx = St.gregs[X::REG_RCX]; Sc.rsp = InterruptedSp;
  Sc.rip = St.rip;
  Sc.eflags = EFlags;
  Sc.cs = St.cs_idx; Sc.gs = St.gs_idx; Sc.fs = St.fs_idx; Sc.ss = St.ss_idx;
  Sc.err = T->FaultErr;
  Sc.trapno = T->FaultTrapNo;
  Sc.cr2 = T->FaultTrapNo == Linux::TRAP_PF ? T->FaultAddr : 0;
  Sc.fpstate = FpAddr;
  Fr.uc.uc_sigmask = D->old_mask;
  static_assert(sizeof(Fr.info) == sizeof(D->info), "siginfo size");
  memcpy(&Fr.info, &D->info, sizeof(Fr.info));

  Linux::XState Xs;
  SaveGuestFpState(Th, &Xs);
  if (lkd_user_write(FpAddr, &Xs, sizeof Xs) != 0 || lkd_user_write(FrameAddr, &Fr, sizeof Fr) != 0) {
    return -LX_EFAULT;
  }
  (void)FrameLen;

  St.rip = D->handler;
  St.gregs[X::REG_RSP] = FrameAddr;
  St.gregs[X::REG_RAX] = 0;
  St.gregs[X::REG_RDI] = static_cast<uint64_t>(D->signo);
  St.gregs[X::REG_RSI] = FrameAddr + offsetof(Linux::RtSigFrame, info);
  St.gregs[X::REG_RDX] = FrameAddr + offsetof(Linux::RtSigFrame, uc);
  St.flags[X::RFLAG_DF_RAW_LOC] = 1; // DF clear (FEX stores DF as +1/-1)
  St.flags[X::RFLAG_RF_LOC] = 0;
  St.flags[X::RFLAG_TF_RAW_LOC] = 0;
  ResetGuestFpForHandler(Th);
  T->SyscallSetsRip = true;
  T->FaultErr = T->FaultTrapNo = T->FaultAddr = 0;
  return 0;
}

int FrameRestore(lkd_task* t, uint64_t* SavedMask, struct lx_stack_t* SavedStack) {
  auto* T = GT(t);
  auto* Th = T->State;
  auto& St = Th->CurrentFrame->State;
  const uint64_t FrameAddr = St.gregs[X::REG_RSP] - sizeof(uint64_t); // the handler's ret popped pretcode
  Linux::RtSigFrame Fr;
  if (lkd_user_read(FrameAddr, &Fr, sizeof Fr) != 0) return -LX_EFAULT;
  const auto& Sc = Fr.uc.uc_mcontext;
  Linux::XState Xs;
  if (Sc.fpstate && lkd_user_read(Sc.fpstate, &Xs, sizeof Xs) != 0) return -LX_EFAULT;
  St.gregs[X::REG_R8] = Sc.r8; St.gregs[X::REG_R9] = Sc.r9; St.gregs[X::REG_R10] = Sc.r10; St.gregs[X::REG_R11] = Sc.r11;
  St.gregs[X::REG_R12] = Sc.r12; St.gregs[X::REG_R13] = Sc.r13; St.gregs[X::REG_R14] = Sc.r14; St.gregs[X::REG_R15] = Sc.r15;
  St.gregs[X::REG_RDI] = Sc.rdi; St.gregs[X::REG_RSI] = Sc.rsi; St.gregs[X::REG_RBP] = Sc.rbp; St.gregs[X::REG_RBX] = Sc.rbx;
  St.gregs[X::REG_RDX] = Sc.rdx; St.gregs[X::REG_RAX] = Sc.rax; St.gregs[X::REG_RCX] = Sc.rcx; St.gregs[X::REG_RSP] = Sc.rsp;
  St.rip = Sc.rip;
  G->CTX->SetFlagsFromCompactedEFLAGS(Th, static_cast<uint32_t>(Sc.eflags));
  if (Sc.fpstate) RestoreGuestFpState(Th, &Xs);
  *SavedMask = Fr.uc.uc_sigmask;
  SavedStack->ss_sp = Fr.uc.uc_stack.Sp;
  SavedStack->ss_flags = Fr.uc.uc_stack.Flags;
  SavedStack->ss_size = Fr.uc.uc_stack.Size;
  T->SyscallSetsRip = true;
  return 0;
}

// execve after the point of no return: LKD has already torn down the address space and the other threads. Load the new
// image in this host process and give the thread the register state of a fresh Linux process.
int ExecImage(lkd_task* t, const struct lkd_exec_image* Img) {
  HitchLog::LogProfileNow("exec");
  auto* T = GT(t);
  auto* Th = T->State;
  // Nothing translated from the old image may run again.
  G->Signals->InvalidateGuestCodeRange(Th, 0, 1ULL << 47);
  CodeValidation::Reset();
  GuardedLoadPatch::Reset();

  LkdLoadRequest Req;
  Req.GuestPath = Img->guest_path;
  Req.ExecFn = Img->original_path ? Img->original_path : Img->guest_path;
  for (int i = 0; i < Img->argc && Img->argv[i]; i++) Req.Argv.emplace_back(Img->argv[i]);
  for (char** E = Img->envp; E && *E; E++) Req.Envp.emplace_back(*E);
  Req.StackTop = LkdStackTop;
  Req.HwCap = G->CTX->RunCPUIDFunction(1, 0).edx;
  std::string Err;
  if (!LkdLoadProgram(T, Req, &Err)) {
    // Past the point of no return Linux kills the process with SIGSEGV; LKD does the same when we fail here.
    fprintf(stderr, "[fexdarwin] execve %s: %s\n", Img->guest_path, Err.c_str());
    return -LX_ENOEXEC;
  }

  auto& St = Th->CurrentFrame->State;
  for (auto& R : St.gregs) R = 0;
  St.rip = T->StartRip;
  St.gregs[X::REG_RSP] = T->StartRsp;
  St.fs_cached = 0;
  St.gs_cached = 0;
  G->CTX->SetFlagsFromCompactedEFLAGS(Th, 0x202);
  ResetGuestFpForHandler(Th); // FCW 0x37f, MXCSR 0x1f80, clean x87/SSE/AVX registers
  St.callret_sp = reinterpret_cast<uint64_t>(Th->CallRetStackBase) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;
  T->FaultErr = T->FaultTrapNo = T->FaultAddr = 0;
  T->SyscallSetsRip = true;
  return 0;
}

// Host fork keeps only the calling thread: hold the engine locks across it so the child never inherits one in use.
void ForkPrepare(lkd_task* t) {
  G->ThreadsLock.lock();
  G->CTX->LockBeforeFork(GT(t)->State);
}
void ForkParent(lkd_task* t, pid_t) {
  G->CTX->UnlockAfterFork(GT(t)->State, false);
  G->ThreadsLock.unlock();
}
void ForkChild(lkd_task* t) {
  // macOS starts the child's thread with the MAP_JIT write state this thread had at its last context switch, not the
  // one it has now. Switched out inside a JITWriteScope and back in execute mode since, the thread forked a child that
  // could not execute the dispatcher (SIGBUS on its first instruction, request wine-5, witness w33): set it again here.
  pthread_jit_write_protect_np(FEXCore::Utils::JITWriteScope::IsOpen() ? 0 : 1);
  auto* T = GT(t);
  G->CTX->UnlockAfterFork(T->State, true);
  // task has already switched the identity: this host thread is now the only thread of the child process.
  T->Tid = lkd_task_tid(t);
  T->PThread = pthread_self();
  T->KickRequested.store(false);
  G->Threads.assign(1, T);
  G->ThreadsLock.unlock();
  {
    // The parent's dead threads have no host thread in this process: drop the list without joining (leaked here).
    std::lock_guard<std::mutex> Lk(DeadLock);
    DeadThreads.clear();
  }
  StartKickRetryThread();
  StartSupervisorWatch(); // threads do not survive a host fork
  HitchLog::AfterFork();
}

// Syscall user dispatch: a syscall instruction of Windows code that Wine's SIGSYS handler would send to its system call
// dispatcher. THE FINALS (its packer) runs `syscall` with EAX = -1, about 50,000 times per second on the game thread:
// no service has that number, and Wine's dispatcher answers STATUS_INVALID_SYSTEM_SERVICE (bad_syscall in
// dlls/ntdll/unix/signal_x86_64.c). The answer is given here, without the signal frame, the handler and the
// dispatcher's register and XSAVE round trip, with the registers that path leaves: RAX the status, R11 the flags, the
// others untouched but RCX and RDX. Without an instrumentation callback, RCX is the return address and RDX 0 (the
// restore flags). With one (ProcessInstrumentationCallback, which that packer installs), Wine returns into the callback
// (iretq): RIP the callback, R10 the return address, RCX Wine's syscall frame, RDX RESTORE_FLAGS_INSTRUMENTATION.
// amd64_thread_data() in the TEB holds a pointer to Wine's callback variable (+ 0x330) and the syscall frame (+ 0x378).
// Not when the trap flag is set: Wine then returns through its CONTEXT_CONTROL path.
int SudShortcut(lkd_task* t, uint64_t Nr) {
  if (static_cast<uint32_t>(Nr) != 0xffffffffu) return 0;
  auto* Th = GT(t)->State;
  auto& St = Th->CurrentFrame->State;
  if (St.flags[X::RFLAG_TF_RAW_LOC]) return 0;
  const uint64_t Teb = St.gs_cached;
  uint64_t CallbackPtr = 0, Callback = 0, SyscallFrame = 0;
  if (lkd_user_read(Teb + 0x330, &CallbackPtr, sizeof CallbackPtr) != 0 ||
      (CallbackPtr && lkd_user_read(CallbackPtr, &Callback, sizeof Callback) != 0) ||
      (Callback && lkd_user_read(Teb + 0x378, &SyscallFrame, sizeof SyscallFrame) != 0)) {
    return 0;
  }
  const uint64_t Return = St.rip; // already after the syscall instruction
  St.gregs[X::REG_RAX] = 0xc000001cull; // STATUS_INVALID_SYSTEM_SERVICE
  St.gregs[X::REG_R11] = GuestEflags(Th);
  if (Callback) {
    St.gregs[X::REG_R10] = Return;
    St.gregs[X::REG_RCX] = SyscallFrame;
    St.gregs[X::REG_RDX] = 0x10000; // RESTORE_FLAGS_INSTRUMENTATION
    St.rip = Callback;
  } else {
    St.gregs[X::REG_RCX] = Return;
    St.gregs[X::REG_RDX] = 0;
  }
  return 1;
}

const lkd_engine_ops Ops = {
  .size = sizeof(lkd_engine_ops),
  .get_regs = GetRegs,
  .set_regs = SetRegs,
  .get_fp = GetFp,
  .set_fp = SetFp,
  .kick = Kick,
  .spawn_thread = SpawnThread,
  .invalidate_code = InvalidateCode,
  .frame_build = FrameBuild,
  .frame_restore = FrameRestore,
  .exec_image = ExecImage,
  .fork_prepare = ForkPrepare,
  .fork_parent = ForkParent,
  .fork_child = ForkChild,
  .set_single_step = nullptr,
  .sud_shortcut = SudShortcut,
};
} // namespace

extern "C" uint64_t fexdarwin_kick_safepoint(GuestThread* T) {
  T->InLkd.store(true);
  // Not inside a system call: ptrace and the signal frame must see orig_rax = -1 (request ptrace-1).
  T->OrigRax = static_cast<uint64_t>(-1);
  if (G->Opt.TraceSignals) {
    fprintf(stderr, "[fexdarwin] tid %d stopped in translated code at rip %#llx for LKD signal delivery\n", T->Tid,
            (unsigned long long)T->State->CurrentFrame->State.rip);
  }
  DeliverPendingSignals(T);
  T->InLkd.store(false);
  return G->Signals->LoopTopFillSRA();
}

bool LkdEnabled() {
  static const bool On = [] {
    const char* E = getenv("FEXDARWIN_LKD");
    return E && *E == '1';
  }();
  return On;
}

int LkdSetup() {
  struct sigaction Sa {};
  Sa.sa_sigaction = KickHandler;
  Sa.sa_flags = SA_SIGINFO | SA_ONSTACK; // no SA_RESTART: blocking calls must see EINTR
  sigemptyset(&Sa.sa_mask);
  sigaction(KickSignal, &Sa, nullptr);
  // The first process of a family becomes the supervisor when no wrapper set one.
  if (!getenv("FEXDARWIN_SUPERVISOR_PID")) {
    char B[32];
    snprintf(B, sizeof B, "%d", static_cast<int>(getpid()));
    setenv("FEXDARWIN_SUPERVISOR_PID", B, 1);
  }
  StartSupervisorWatch(); // the first process watches itself only through the region guard (kill(self) succeeds)
  StartKickRetryThread();
  // A host pipe or socket the engine keeps must never kill the runtime; LKD sets F_SETNOSIGPIPE on guest ones.
  signal(SIGPIPE, SIG_IGN);
  return lkd_init(&Ops);
}

// Syscall entry in LKD mode. Returns with the guest state updated; the caller does not touch RIP afterwards.
void LkdHandleSyscall(GuestThread* T, FEXCore::Core::CpuStateFrame* Frame) {
  auto& St = Frame->State;
  lkd_sysreq Rq;
  Rq.nr = St.gregs[X::REG_RAX];
  Rq.a[0] = St.gregs[X::REG_RDI]; Rq.a[1] = St.gregs[X::REG_RSI]; Rq.a[2] = St.gregs[X::REG_RDX];
  Rq.a[3] = St.gregs[X::REG_R10]; Rq.a[4] = St.gregs[X::REG_R8]; Rq.a[5] = St.gregs[X::REG_R9];
  T->OrigRax = Rq.nr;
  for (int I = 0; I < 6; I++) T->SysArgs[I] = Rq.a[I];
  T->SyscallSetsRip = false;
  T->InLkd.store(true);
  // Linux reports RIP after the syscall instruction while the call runs (ptrace, signal frames).
  St.rip += 2;
  int64_t Rax = 0;
  const uint64_t T0 = HitchLog::ProfileEnabled() ? HitchLog::NowNs() : 0;
  const lkd_disp D = lkd_syscall(T->Task, &Rq, &Rax);
  if (T0) {
    HitchLog::NoteSyscall(T, Rq.nr, HitchLog::NowNs() - T0);
    if (D == LKD_RETURN && Rax == -2) HitchLog::NoteMissingPath(Rq.nr, Rq.a);
  }
  switch (D) {
  case LKD_RETURN: St.gregs[X::REG_RAX] = static_cast<uint64_t>(Rax); break;
  case LKD_CONTEXT_REPLACED: break;
  case LKD_RESTART:
    St.rip -= 2;
    St.gregs[X::REG_RAX] = static_cast<uint64_t>(Rax);
    break;
  case LKD_EXIT_THREAD:
  case LKD_EXIT_GROUP: EndGuestThread(T);
  }
  // Deliver pending signals at this safe point; the frame saves the post-syscall context.
  DeliverPendingSignals(T);
  T->InLkd.store(false);
  T->SyscallSetsRip = true; // RIP is final, the generic path must not add 2 again
}

// CPUID provider for procfs: the leaves a Linux guest reads for /proc/cpuinfo and the cache topology.
extern "C" int fexdarwin_cpuid(uint32_t Leaf, uint32_t Subleaf, int Cpu, uint32_t Out[4]) {
  if (!G || !G->CTX) return -1;
  // The brand string of a (hybrid) model is per CPU in FEX.
  const bool PerCpu = Leaf >= 0x80000002 && Leaf <= 0x80000004;
  const auto R = PerCpu ? G->CTX->RunCPUIDFunctionName(Leaf, Subleaf, static_cast<uint32_t>(Cpu < 0 ? 0 : Cpu)) :
                          G->CTX->RunCPUIDFunction(Leaf, Subleaf);
  Out[0] = R.eax; Out[1] = R.ebx; Out[2] = R.ecx; Out[3] = R.edx;
  return 0;
}

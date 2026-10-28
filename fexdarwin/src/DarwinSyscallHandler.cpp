// SPDX-License-Identifier: MIT
#include "DarwinSyscallHandler.h"
#include "CodeValidation.h"
#include <FEXCore/Utils/GuestBase.h>
#include "DarwinSignalDelegator.h"
#include "LkdGlue.h"

#include <FEXCore/Core/X86Enums.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <os/os_sync_wait_on_address.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <climits>

using namespace Linux;

namespace {
constexpr int64_t NegErr(int LinuxErrno) {
  return -static_cast<int64_t>(LinuxErrno);
}
int64_t FromHost(int64_t R) {
  return R < 0 ? NegErr(FromDarwinErrno(errno)) : R;
}

// Guest memory access helpers. The guest pointer is the host pointer (identity mapping) but it is validated first so a
// bad guest pointer produces -EFAULT like on Linux instead of crashing the host.
bool ReadableRange(uint64_t P, uint64_t L) {
  return G->Mem.Check(P, L, PROT_READ_);
}
bool WritableRange(uint64_t P, uint64_t L) {
  return G->Mem.Check(P, L, PROT_WRITE_);
}
bool GuestCString(uint64_t P, std::string* Out, size_t Max = 4096) {
  Out->clear();
  for (size_t I = 0; I < Max; ++I) {
    if (!ReadableRange(P + I, 1)) return false;
    char C = *static_cast<const char*>(FEXCore::G2H(P + I));
    if (!C) return true;
    Out->push_back(C);
  }
  return false; // too long
}

int HostDirFd(int64_t LinuxDirFd) {
  return LinuxDirFd == AT_FDCWD_ ? AT_FDCWD : static_cast<int>(LinuxDirFd);
}

int64_t DoStat(GuestThread* T, uint64_t StatBuf, int Rc, const struct stat& St) {
  if (Rc < 0) return NegErr(FromDarwinErrno(errno));
  if (!WritableRange(StatBuf, sizeof(Stat))) return NegErr(L_EFAULT);
  PrepareGuestWrite(T, StatBuf, sizeof(Stat));
  ConvertStat(St, static_cast<Stat*>(FEXCore::G2H(StatBuf)));
  return 0;
}

constexpr uint64_t SupportedSyncSignals = (1ULL << (L_SIGSEGV - 1)) | (1ULL << (L_SIGBUS - 1)) | (1ULL << (L_SIGILL - 1)) |
                                          (1ULL << (L_SIGFPE - 1)) | (1ULL << (L_SIGTRAP - 1));
} // namespace

// Pages that translated code depends on are write-protected on the host. The kernel (this process) is about to write
// into guest memory, so drop the translations of those pages and make them writable first.
void PrepareGuestWrite(GuestThread* T, uint64_t P, uint64_t L) {
  if (G->Opt.SmcMode != "mtrack" || L == 0) return;
  std::set<uint64_t> Pages;
  G->Mem.SmcPagesIn(P, L, &Pages);
  if (Pages.empty()) return;
  const uint64_t Lo = *Pages.begin();
  const uint64_t Hi = *Pages.rbegin() + GuestMemory::PageSize;
  G->Signals->InvalidateGuestCodeRange(T->State, Lo, Hi - Lo, [](uint64_t S, uint64_t N) { G->Mem.ClearSmcPages(S, N); });
}

void TraceLog(const char* Fmt, ...) {
  char Buf[768];
  va_list Ap;
  va_start(Ap, Fmt);
  int N = vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  if (N > 0) {
    N = std::min<int>(N, sizeof Buf - 2);
    Buf[N++] = '\n';
    ssize_t R = ::write(2, Buf, N);
    (void)R;
  }
}

const char* LinuxSyscallName(uint64_t Nr) {
  switch (Nr) {
  case 0: return "read";
  case 1: return "write";
  case 2: return "open";
  case 3: return "close";
  case 4: return "stat";
  case 5: return "fstat";
  case 6: return "lstat";
  case 8: return "lseek";
  case 9: return "mmap";
  case 10: return "mprotect";
  case 11: return "munmap";
  case 12: return "brk";
  case 13: return "rt_sigaction";
  case 14: return "rt_sigprocmask";
  case 15: return "rt_sigreturn";
  case 16: return "ioctl";
  case 17: return "pread64";
  case 18: return "pwrite64";
  case 19: return "readv";
  case 20: return "writev";
  case 21: return "access";
  case 24: return "sched_yield";
  case 25: return "mremap";
  case 28: return "madvise";
  case 32: return "dup";
  case 33: return "dup2";
  case 35: return "nanosleep";
  case 39: return "getpid";
  case 56: return "clone";
  case 57: return "fork";
  case 59: return "execve";
  case 60: return "exit";
  case 62: return "kill";
  case 63: return "uname";
  case 72: return "fcntl";
  case 79: return "getcwd";
  case 89: return "readlink";
  case 96: return "gettimeofday";
  case 97: return "getrlimit";
  case 102: return "getuid";
  case 104: return "getgid";
  case 107: return "geteuid";
  case 108: return "getegid";
  case 110: return "getppid";
  case 131: return "sigaltstack";
  case 158: return "arch_prctl";
  case 186: return "gettid";
  case 201: return "time";
  case 202: return "futex";
  case 218: return "set_tid_address";
  case 228: return "clock_gettime";
  case 229: return "clock_getres";
  case 230: return "clock_nanosleep";
  case 231: return "exit_group";
  case 234: return "tgkill";
  case 257: return "openat";
  case 262: return "newfstatat";
  case 273: return "set_robust_list";
  case 302: return "prlimit64";
  case 318: return "getrandom";
  case 334: return "rseq";
  default: return "?";
  }
}

void DarwinSyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) {
  namespace X = FEXCore::X86State;
  auto* T = static_cast<GuestThread*>(Frame->Thread->FrontendPtr);
  if (LkdEnabled()) {
    LkdHandleSyscall(T, Frame);
    return;
  }
  auto& R = Frame->State.gregs;
  const uint64_t Nr = R[X::REG_RAX];
  const uint64_t A[6] = {R[X::REG_RDI], R[X::REG_RSI], R[X::REG_RDX], R[X::REG_R10], R[X::REG_R8], R[X::REG_R9]};
  SyscallCount.fetch_add(1, std::memory_order_relaxed);

  T->SyscallSetsRip = false;
  const int64_t Ret = Dispatch(T, Nr, A);
  R[X::REG_RAX] = static_cast<uint64_t>(Ret);
  if (G->Opt.Trace) {
    if (Ret == NegErr(L_ENOSYS)) {
      TraceLog("[syscall] %s(%llu) (%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) = -ENOSYS  <-- NOT IMPLEMENTED", LinuxSyscallName(Nr),
               (unsigned long long)Nr, (unsigned long long)A[0], (unsigned long long)A[1], (unsigned long long)A[2], (unsigned long long)A[3],
               (unsigned long long)A[4], (unsigned long long)A[5]);
    } else {
      TraceLog("[syscall] %s(%llu) (%#llx, %#llx, %#llx, %#llx, %#llx, %#llx) = %lld", LinuxSyscallName(Nr), (unsigned long long)Nr,
               (unsigned long long)A[0], (unsigned long long)A[1], (unsigned long long)A[2], (unsigned long long)A[3], (unsigned long long)A[4],
               (unsigned long long)A[5], (long long)Ret);
    }
  }
  // Skip the two bytes of the `syscall` instruction (RCX and R11 were already written by the translated instruction),
  // unless the call loaded RIP itself (rt_sigreturn).
  if (!T->SyscallSetsRip) {
    Frame->State.rip += 2;
  }
}

extern "C" int fexdarwin_lkd_vma_at(uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot);
extern "C" int fexdarwin_lkd_watch_code(uint64_t gva, uint64_t len);
extern "C" void fexdarwin_lkd_guard_close_all(void);

// Host faults on guest memory from now to PostCompile are the decoder's (DarwinSignalDelegator::HandleFault, step 7).
void DarwinSyscallHandler::PreCompile() {
  if (CurrentGuestThread) CurrentGuestThread->Compiling = true;
}

// The decoder has read the guest's code: the guarded host pages it had to open go back to PROT_NONE. Not during a guard
// step: the stepped instruction's block is compiled after BeginGuardStep opened its page, and CancelGuardStep closes
// them all at the trap.
void DarwinSyscallHandler::PostCompile() {
  if (CurrentGuestThread) CurrentGuestThread->Compiling = false;
  if (CurrentGuestThread && CurrentGuestThread->GuardStepping) return;
  fexdarwin_lkd_guard_close_all();
}

void DarwinSyscallHandler::MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) {
  if (LkdEnabled()) {
    // mm removes the write permission of the host pages that hold translated code; the first store to one comes back
    // through the fault handler (HandleSmcWrite). Pages that left write tracking stay writable: their translations
    // check their own bytes (NeedsCodeValidation).
    if (G->Opt.SmcMode == "mtrack" && !CodeValidation::Validated(Start, Length)) fexdarwin_lkd_watch_code(Start, Length);
    return;
  }
  if (G->Opt.SmcMode == "mtrack") {
    G->Mem.MarkSmc(Start, Length);
  }
}

bool DarwinSyscallHandler::NeedsCodeValidation(uint64_t Start, uint64_t Length) {
  return LkdEnabled() && CodeValidation::Validated(Start, Length);
}

void DarwinSyscallHandler::InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  G->Signals->InvalidateGuestCodeRange(Thread, Start, Length);
}

FEXCore::HLE::ExecutableRangeInfo DarwinSyscallHandler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) {
  if (LkdEnabled()) {
    uint64_t Start, End;
    uint32_t Prot;
    if (fexdarwin_lkd_vma_at(Address, &Start, &End, &Prot) != 0 || !(Prot & LX_PROT_EXEC)) return {0, 0, false};
    return {Start, End - Start, (Prot & LX_PROT_WRITE) != 0};
  }
  auto R = G->Mem.QueryExec(Address);
  return {R.Base, R.Size, R.Writable};
}

void DarwinSyscallHandler::SleepThread(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame*) {
  // The pause/stop machinery that calls this is not used by the host yet.
  TraceLog("[fexdarwin] SleepThread called, not supported");
}


namespace {
// ---- threads (clone with CLONE_VM | CLONE_THREAD) ----
constexpr uint64_t CLONE_VM_ = 0x100, CLONE_FS_ = 0x200, CLONE_FILES_ = 0x400, CLONE_SIGHAND_ = 0x800, CLONE_THREAD_ = 0x10000,
                   CLONE_SETTLS_ = 0x80000, CLONE_PARENT_SETTID_ = 0x100000, CLONE_CHILD_CLEARTID_ = 0x200000,
                   CLONE_CHILD_SETTID_ = 0x1000000;

void* GuestThreadEntry(void* P) {
  RunGuestThread(static_cast<GuestThread*>(P));
  return nullptr;
}

int64_t CloneThread(GuestThread* T, const uint64_t A[6]) {
  namespace X = FEXCore::X86State;
  const uint64_t Flags = A[0];
  const uint64_t Needed = CLONE_VM_ | CLONE_FS_ | CLONE_FILES_ | CLONE_SIGHAND_ | CLONE_THREAD_;
  if ((Flags & Needed) != Needed) {
    // fork-style clones belong to the LKD process layer; this prototype host does not implement them.
    return NegErr(L_ENOSYS);
  }
  auto& Parent = T->State->CurrentFrame->State;
  auto* C = CreateGuestThread(T);
  if (!C) return NegErr(L_EAGAIN);
  C->StartRip = Parent.rip + 2; // the child resumes after the syscall instruction
  C->StartRsp = A[1] ? A[1] : Parent.gregs[X::REG_RSP];
  C->StartRax = 0;
  if (Flags & CLONE_SETTLS_) C->StartFsBase = A[4];
  if (Flags & CLONE_PARENT_SETTID_) *static_cast<int32_t*>(FEXCore::G2H(A[2])) = C->Tid;
  if (Flags & CLONE_CHILD_SETTID_) *static_cast<int32_t*>(FEXCore::G2H(A[3])) = C->Tid;
  if (Flags & CLONE_CHILD_CLEARTID_) C->ClearTidAddr = A[3];
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, 8 << 20);
  const int E = pthread_create(&C->PThread, &Attr, GuestThreadEntry, C);
  pthread_attr_destroy(&Attr);
  if (E != 0) return NegErr(L_EAGAIN);
  return C->Tid;
}

int64_t FutexWake(uint64_t Uaddr, uint64_t Count) {
  void* Addr = FEXCore::G2H(Uaddr);
  if (Count >= INT_MAX) {
    return os_sync_wake_by_address_all(Addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE) == 0 ? 1 : 0;
  }
  int64_t Woken = 0;
  while (static_cast<uint64_t>(Woken) < Count && os_sync_wake_by_address_any(Addr, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE) == 0) {
    Woken++;
  }
  return Woken;
}

int64_t Futex(const uint64_t A[6]) {
  const int Cmd = A[1] & 0x7f; // drop FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME
  const bool Realtime = A[1] & 256;
  switch (Cmd) {
  case 0:   // FUTEX_WAIT, relative timeout
  case 9: { // FUTEX_WAIT_BITSET, absolute timeout
    if (Cmd == 9 && static_cast<uint32_t>(A[5]) == 0) return NegErr(L_EINVAL);
    auto* Addr = static_cast<uint32_t*>(FEXCore::G2H(A[0]));
    const uint32_t Expected = static_cast<uint32_t>(A[2]);
    if (__atomic_load_n(Addr, __ATOMIC_SEQ_CST) != Expected) return NegErr(L_EAGAIN);
    int R;
    if (A[3]) {
      const auto* Ts = static_cast<const int64_t*>(FEXCore::G2H(A[3]));
      int64_t Ns = Ts[0] * 1000000000LL + Ts[1];
      if (Cmd == 9) {
        struct timespec Now;
        clock_gettime(Realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &Now);
        Ns -= Now.tv_sec * 1000000000LL + Now.tv_nsec;
      }
      if (Ns <= 0) return NegErr(L_ETIMEDOUT);
      R = os_sync_wait_on_address_with_timeout(Addr, Expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                               static_cast<uint64_t>(Ns));
    } else {
      R = os_sync_wait_on_address(Addr, Expected, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    }
    if (R >= 0) return 0;
    if (errno == ETIMEDOUT) return NegErr(L_ETIMEDOUT);
    if (errno == EINTR) return NegErr(L_EINTR);
    return NegErr(L_EAGAIN);
  }
  case 1:  // FUTEX_WAKE
  case 10: // FUTEX_WAKE_BITSET
    return FutexWake(A[0], static_cast<uint32_t>(A[2]));
  default: return NegErr(L_ENOSYS);
  }
}

[[noreturn]] void ExitGuestThread(GuestThread* T, int Code) {
  bool Last;
  {
    std::lock_guard<std::mutex> lk(G->ThreadsLock);
    Last = G->Threads.size() == 1;
    if (!Last) G->Threads.erase(std::find(G->Threads.begin(), G->Threads.end(), T));
  }
  if (Last) ExitProcess(Code);
  if (T->ClearTidAddr) {
    __atomic_store_n(static_cast<int32_t*>(FEXCore::G2H(T->ClearTidAddr)), 0, __ATOMIC_SEQ_CST);
    FutexWake(T->ClearTidAddr, 1);
  }
  T->ExitCode = Code;
  T->Exited = true;
  G->Signals->UnregisterThread(T);
  pthread_exit(nullptr);
}
} // namespace

int64_t DarwinSyscallHandler::Dispatch(GuestThread* T, uint64_t Nr, const uint64_t A[6]) {
  switch (Nr) {
  case 0: { // read(fd, buf, count)
    const uint64_t Count = std::min<uint64_t>(A[2], 0x7ffff000);
    if (!WritableRange(A[1], Count)) return NegErr(L_EFAULT);
    PrepareGuestWrite(T, A[1], Count);
    return FromHost(::read(static_cast<int>(A[0]), static_cast<void*>(FEXCore::G2H(A[1])), Count));
  }
  case 1: { // write(fd, buf, count)
    const uint64_t Count = std::min<uint64_t>(A[2], 0x7ffff000);
    if (!ReadableRange(A[1], Count)) return NegErr(L_EFAULT);
    return FromHost(::write(static_cast<int>(A[0]), static_cast<const void*>(FEXCore::G2H(A[1])), Count));
  }
  case 2: { // open(path, flags, mode)
    std::string P;
    if (!GuestCString(A[0], &P)) return NegErr(L_EFAULT);
    return FromHost(::open(P.c_str(), ConvertOpenFlags(static_cast<int>(A[1])), static_cast<mode_t>(A[2])));
  }
  case 257: { // openat(dirfd, path, flags, mode)
    std::string P;
    if (!GuestCString(A[1], &P)) return NegErr(L_EFAULT);
    return FromHost(::openat(HostDirFd(static_cast<int64_t>(A[0])), P.c_str(), ConvertOpenFlags(static_cast<int>(A[2])), static_cast<mode_t>(A[3])));
  }
  case 3: // close(fd)
    return FromHost(::close(static_cast<int>(A[0])));
  case 4:
  case 6: { // stat / lstat(path, buf)
    std::string P;
    if (!GuestCString(A[0], &P)) return NegErr(L_EFAULT);
    struct stat St {};
    int Rc = Nr == 4 ? ::stat(P.c_str(), &St) : ::lstat(P.c_str(), &St);
    return DoStat(T, A[1], Rc, St);
  }
  case 5: { // fstat(fd, buf)
    struct stat St {};
    int Rc = ::fstat(static_cast<int>(A[0]), &St);
    return DoStat(T, A[1], Rc, St);
  }
  case 262: { // newfstatat(dirfd, path, buf, flags)
    std::string P;
    if (!GuestCString(A[1], &P)) return NegErr(L_EFAULT);
    struct stat St {};
    int Rc;
    const int Flags = static_cast<int>(A[3]);
    if ((Flags & AT_EMPTY_PATH_) && P.empty()) {
      Rc = ::fstat(static_cast<int>(A[0]), &St);
    } else {
      Rc = ::fstatat(HostDirFd(static_cast<int64_t>(A[0])), P.c_str(), &St, (Flags & AT_SYMLINK_NOFOLLOW_) ? AT_SYMLINK_NOFOLLOW : 0);
    }
    return DoStat(T, A[2], Rc, St);
  }
  case 8: // lseek(fd, off, whence)
    return FromHost(::lseek(static_cast<int>(A[0]), static_cast<off_t>(A[1]), static_cast<int>(A[2])));
  case 17: { // pread64(fd, buf, count, off)
    if (!WritableRange(A[1], A[2])) return NegErr(L_EFAULT);
    PrepareGuestWrite(T, A[1], A[2]);
    return FromHost(::pread(static_cast<int>(A[0]), static_cast<void*>(FEXCore::G2H(A[1])), A[2], static_cast<off_t>(A[3])));
  }
  case 18: { // pwrite64(fd, buf, count, off)
    if (!ReadableRange(A[1], A[2])) return NegErr(L_EFAULT);
    return FromHost(::pwrite(static_cast<int>(A[0]), static_cast<const void*>(FEXCore::G2H(A[1])), A[2], static_cast<off_t>(A[3])));
  }
  case 19:
  case 20: { // readv / writev(fd, iov, iovcnt)
    const uint64_t Cnt = A[2];
    if (Cnt > 1024) return NegErr(L_EINVAL);
    if (!ReadableRange(A[1], Cnt * sizeof(Iovec))) return NegErr(L_EFAULT);
    struct iovec Host[1024];
    auto* Guest = static_cast<const Iovec*>(FEXCore::G2H(A[1]));
    for (uint64_t I = 0; I < Cnt; ++I) {
      const bool Ok = Nr == 19 ? WritableRange(Guest[I].Base, Guest[I].Len) : ReadableRange(Guest[I].Base, Guest[I].Len);
      if (!Ok) return NegErr(L_EFAULT);
      if (Nr == 19) PrepareGuestWrite(T, Guest[I].Base, Guest[I].Len);
      Host[I].iov_base = FEXCore::G2H(Guest[I].Base);
      Host[I].iov_len = Guest[I].Len;
    }
    return FromHost(Nr == 19 ? ::readv(static_cast<int>(A[0]), Host, static_cast<int>(Cnt)) : ::writev(static_cast<int>(A[0]), Host, static_cast<int>(Cnt)));
  }
  case 21: { // access(path, mode)
    std::string P;
    if (!GuestCString(A[0], &P)) return NegErr(L_EFAULT);
    return FromHost(::access(P.c_str(), static_cast<int>(A[1])));
  }
  case 9: { // mmap(addr, len, prot, flags, fd, off)
    const int Flags = static_cast<int>(A[3]);
    const int64_t Fd = static_cast<int64_t>(A[4]);
    int64_t R = G->Mem.Mmap(A[0], A[1], static_cast<int>(A[2]), Flags, static_cast<int>(Fd), static_cast<int64_t>(A[5]), "mmap");
    if (R >= 0 && (Flags & (MAP_FIXED_ | MAP_FIXED_NOREPLACE_))) {
      G->Signals->InvalidateGuestCodeRange(T->State, R, GuestMemory::RoundUp(A[1]));
    }
    return R;
  }
  case 10: { // mprotect(addr, len, prot)
    int64_t R = G->Mem.Mprotect(A[0], A[1], static_cast<int>(A[2]));
    if (R == 0) G->Signals->InvalidateGuestCodeRange(T->State, A[0], GuestMemory::RoundUp(A[1]));
    return R;
  }
  case 11: { // munmap(addr, len)
    int64_t R = G->Mem.Munmap(A[0], A[1]);
    if (R == 0) G->Signals->InvalidateGuestCodeRange(T->State, A[0], GuestMemory::RoundUp(A[1]));
    return R;
  }
  case 12: // brk(addr)
    return static_cast<int64_t>(G->Mem.Brk(A[0]));
  case 28: { // madvise(addr, len, advice)
    const int Advice = static_cast<int>(A[2]);
    if (A[0] & GuestMemory::PageMask) return NegErr(L_EINVAL);
    if (!G->Mem.Check(A[0], GuestMemory::RoundUp(A[1]), 0)) return NegErr(L_ENOMEM);
    switch (Advice) {
    case 0: case 1: case 2: case 3: // NORMAL, RANDOM, SEQUENTIAL, WILLNEED: hints
      return 0;
    case 4: // MADV_DONTNEED: private anonymous pages become zero again
      if (!WritableRange(A[0], A[1])) return NegErr(L_EINVAL);
      PrepareGuestWrite(T, A[0], A[1]);
      if (::madvise(static_cast<void*>(FEXCore::G2H(A[0])), GuestMemory::RoundUp(A[1]), MADV_ZERO) != 0) return NegErr(L_EINVAL);
      return 0;
    default: return NegErr(L_ENOSYS);
    }
  }
  case 13: { // rt_sigaction(sig, act, oact, sigsetsize)
    const int Sig = static_cast<int>(A[0]);
    if (A[3] != 8 || Sig < 1 || Sig > 64 || Sig == L_SIGKILL || Sig == L_SIGSTOP) return NegErr(L_EINVAL);
    if (A[1] && !ReadableRange(A[1], sizeof(KernelSigaction))) return NegErr(L_EFAULT);
    if (A[2] && !WritableRange(A[2], sizeof(KernelSigaction))) return NegErr(L_EFAULT);
    std::lock_guard<std::mutex> lk(G->SigLock);
    if (A[1]) {
      KernelSigaction New = *static_cast<const KernelSigaction*>(FEXCore::G2H(A[1]));
      const bool RealHandler = New.Handler != SIG_DFL_ && New.Handler != SIG_IGN_;
      if (RealHandler && !((SupportedSyncSignals >> (Sig - 1)) & 1)) {
        // Asynchronous signal delivery to guest handlers is not implemented: refuse visibly instead of pretending.
        return NegErr(L_ENOSYS);
      }
      if (A[2]) *static_cast<KernelSigaction*>(FEXCore::G2H(A[2])) = G->SigActions[Sig];
      G->SigActions[Sig] = New;
    } else if (A[2]) {
      *static_cast<KernelSigaction*>(FEXCore::G2H(A[2])) = G->SigActions[Sig];
    }
    return 0;
  }
  case 14: { // rt_sigprocmask(how, set, oldset, sigsetsize)
    if (A[3] != 8) return NegErr(L_EINVAL);
    if (A[1] && !ReadableRange(A[1], 8)) return NegErr(L_EFAULT);
    if (A[2] && !WritableRange(A[2], 8)) return NegErr(L_EFAULT);
    const uint64_t Old = T->SigMask;
    if (A[1]) {
      uint64_t Set = *static_cast<const uint64_t*>(FEXCore::G2H(A[1]));
      Set &= ~((1ULL << (L_SIGKILL - 1)) | (1ULL << (L_SIGSTOP - 1)));
      switch (static_cast<int>(A[0])) {
      case SIG_BLOCK_: T->SigMask |= Set; break;
      case SIG_UNBLOCK_: T->SigMask &= ~Set; break;
      case SIG_SETMASK_: T->SigMask = Set; break;
      default: return NegErr(L_EINVAL);
      }
    }
    if (A[2]) *static_cast<uint64_t*>(FEXCore::G2H(A[2])) = Old;
    return 0;
  }
  case 131: { // sigaltstack(ss, old_ss)
    if (A[0] && !ReadableRange(A[0], sizeof(StackT))) return NegErr(L_EFAULT);
    if (A[1] && !WritableRange(A[1], sizeof(StackT))) return NegErr(L_EFAULT);
    const StackT Old = T->AltStack;
    if (A[0]) {
      const StackT New = *static_cast<const StackT*>(FEXCore::G2H(A[0]));
      if (New.Flags & ~SS_DISABLE_) return NegErr(L_EINVAL);
      if (!(New.Flags & SS_DISABLE_) && New.Size < 2048) return NegErr(L_ENOMEM);
      T->AltStack = New;
    }
    if (A[1]) *static_cast<StackT*>(FEXCore::G2H(A[1])) = Old;
    return 0;
  }
  case 158: { // arch_prctl(code, addr)
    auto& St = T->State->CurrentFrame->State;
    switch (static_cast<int>(A[0])) {
    case ARCH_SET_FS: St.fs_cached = A[1]; return 0;
    case ARCH_SET_GS: St.gs_cached = A[1]; return 0;
    case ARCH_GET_FS:
      if (!WritableRange(A[1], 8)) return NegErr(L_EFAULT);
      *static_cast<uint64_t*>(FEXCore::G2H(A[1])) = St.fs_cached;
      return 0;
    case ARCH_GET_GS:
      if (!WritableRange(A[1], 8)) return NegErr(L_EFAULT);
      *static_cast<uint64_t*>(FEXCore::G2H(A[1])) = St.gs_cached;
      return 0;
    default: return NegErr(L_EINVAL);
    }
  }
  case 218: // set_tid_address(tidptr)
    T->ClearTidAddr = A[0];
    return T->Tid;
  case 186: return T->Tid;       // gettid
  case 39: return G->Pid;        // getpid
  case 110: return ::getppid();  // getppid
  case 102: return ::getuid();
  case 104: return ::getgid();
  case 107: return ::geteuid();
  case 108: return ::getegid();
  case 24: ::sched_yield(); return 0;
  case 228: { // clock_gettime(clk, ts)
    const int Clk = ConvertClock(static_cast<int>(A[0]));
    if (Clk < 0) return NegErr(L_EINVAL);
    if (!WritableRange(A[1], sizeof(Timespec))) return NegErr(L_EFAULT);
    struct timespec Ts;
    if (::clock_gettime(static_cast<clockid_t>(Clk), &Ts) != 0) return NegErr(FromDarwinErrno(errno));
    auto* Out = static_cast<Timespec*>(FEXCore::G2H(A[1]));
    Out->tv_sec = Ts.tv_sec;
    Out->tv_nsec = Ts.tv_nsec;
    return 0;
  }
  case 229: { // clock_getres(clk, ts)
    const int Clk = ConvertClock(static_cast<int>(A[0]));
    if (Clk < 0) return NegErr(L_EINVAL);
    if (A[1]) {
      if (!WritableRange(A[1], sizeof(Timespec))) return NegErr(L_EFAULT);
      struct timespec Ts;
      if (::clock_getres(static_cast<clockid_t>(Clk), &Ts) != 0) return NegErr(FromDarwinErrno(errno));
      auto* Out = static_cast<Timespec*>(FEXCore::G2H(A[1]));
      Out->tv_sec = Ts.tv_sec;
      Out->tv_nsec = Ts.tv_nsec;
    }
    return 0;
  }
  case 96: { // gettimeofday(tv, tz)
    if (A[0]) {
      if (!WritableRange(A[0], sizeof(Timeval))) return NegErr(L_EFAULT);
      struct timeval Tv;
      ::gettimeofday(&Tv, nullptr);
      auto* Out = static_cast<Timeval*>(FEXCore::G2H(A[0]));
      Out->tv_sec = Tv.tv_sec;
      Out->tv_usec = Tv.tv_usec;
    }
    if (A[1]) {
      if (!WritableRange(A[1], 8)) return NegErr(L_EFAULT);
      memset(static_cast<void*>(FEXCore::G2H(A[1])), 0, 8); // struct timezone {minuteswest=0, dsttime=0}
    }
    return 0;
  }
  case 201: { // time(tloc)
    const int64_t Now = ::time(nullptr);
    if (A[0]) {
      if (!WritableRange(A[0], 8)) return NegErr(L_EFAULT);
      *static_cast<int64_t*>(FEXCore::G2H(A[0])) = Now;
    }
    return Now;
  }
  case 35: { // nanosleep(req, rem)
    if (!ReadableRange(A[0], sizeof(Timespec))) return NegErr(L_EFAULT);
    if (A[1] && !WritableRange(A[1], sizeof(Timespec))) return NegErr(L_EFAULT);
    const auto* Req = static_cast<const Timespec*>(FEXCore::G2H(A[0]));
    if (Req->tv_sec < 0 || Req->tv_nsec < 0 || Req->tv_nsec >= 1000000000) return NegErr(L_EINVAL);
    struct timespec In {static_cast<time_t>(Req->tv_sec), static_cast<long>(Req->tv_nsec)}, Rem {};
    int Rc = ::nanosleep(&In, &Rem);
    if (Rc != 0) {
      if (A[1] && errno == EINTR) {
        auto* Out = static_cast<Timespec*>(FEXCore::G2H(A[1]));
        Out->tv_sec = Rem.tv_sec;
        Out->tv_nsec = Rem.tv_nsec;
      }
      return NegErr(FromDarwinErrno(errno));
    }
    return 0;
  }
  case 63: { // uname(buf)
    if (!WritableRange(A[0], sizeof(Utsname))) return NegErr(L_EFAULT);
    auto* U = static_cast<Utsname*>(FEXCore::G2H(A[0]));
    memset(U, 0, sizeof *U);
    strcpy(U->sysname, "Linux");
    char Host[65] = {};
    ::gethostname(Host, sizeof Host - 1);
    strncpy(U->nodename, Host, sizeof U->nodename - 1);
    strcpy(U->release, "6.8.0-fexdarwin");
    strcpy(U->version, "#1 SMP FEXCore on macOS arm64");
    strcpy(U->machine, "x86_64");
    strcpy(U->domainname, "(none)");
    return 0;
  }
  case 318: { // getrandom(buf, len, flags)
    if (!WritableRange(A[0], A[1])) return NegErr(L_EFAULT);
    PrepareGuestWrite(T, A[0], A[1]);
    uint64_t Done = 0;
    while (Done < A[1]) {
      const size_t Chunk = std::min<uint64_t>(256, A[1] - Done);
      if (::getentropy(static_cast<void*>(FEXCore::G2H(A[0] + Done)), Chunk) != 0) return NegErr(L_EIO);
      Done += Chunk;
    }
    return static_cast<int64_t>(A[1]);
  }
  case 15: // rt_sigreturn()
    return GuestRtSigreturn(T);
  case 56: // clone(flags, newsp, ptid, ctid, tls)
    return CloneThread(T, A);
  case 202: // futex(uaddr, op, val, timeout, uaddr2, val3)
    return Futex(A);
  case 60: // exit(code): ends the calling thread only
    ExitGuestThread(T, static_cast<int>(A[0] & 0xff));
  case 231: // exit_group(code)
    ExitProcess(static_cast<int>(A[0] & 0xff));
  default:
    return NegErr(L_ENOSYS);
  }
}

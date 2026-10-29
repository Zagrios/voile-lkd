// SPDX-License-Identifier: MIT
// fexdarwin-run [--env K=V ...] [options] ELF [args...]
// Runs a freestanding static x86-64 Linux ELF on macOS arm64 through FEXCore (JIT).
#include "DarwinHostFeatures.h"
#include <FEXCore/Utils/GuestBase.h>
#include "DarwinSignalDelegator.h"
#include "DarwinSyscallHandler.h"
#include "ElfLoader.h"
#include "Host.h"
#include "LkdGlue.h"
#include "LkdLoader.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Threads.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <fstream>
#include <vector>

namespace {
// Guest address space layout (identity mapping, see README and the report for why it sits this high).
// Without a guest base (FEXDARWIN_GUEST_BASE unset or 0) guest address == host address and the guest window sits high.
// With a guest base, the guest window starts at 64 KiB like on Linux (mmap_min_addr) and the host window is GuestBase + guest.
uint64_t WindowBase = 0x6000'0000'0000ULL;
uint64_t WindowSize = 0x1000'0000'0000ULL; // 16 TiB
constexpr uint64_t DynBase = 0x6000'0000'0000ULL + 0x1'0000'0000ULL;
constexpr uint64_t StackBytes = 8ULL << 20;
constexpr uint64_t BrkMaxBytes = 1ULL << 30;

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[FEXCore %s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
void AssertHandler(const char* Message) {
  fprintf(stderr, "[FEXCore ASSERT] %s\n", Message);
  fflush(nullptr);
}

// FEXCore::Threads over pthread for FEXCore's own helper threads.
class PThreadWrapper final : public FEXCore::Threads::Thread {
public:
  pthread_t T {};
  bool Joinable = true;
  bool joinable() override { return Joinable; }
  bool join(void** Ret) override {
    Joinable = false;
    return pthread_join(T, Ret) == 0;
  }
  bool detach() override {
    Joinable = false;
    return pthread_detach(T) == 0;
  }
  bool IsSelf() override { return pthread_equal(T, pthread_self()); }
};
fextl::unique_ptr<FEXCore::Threads::Thread> CreateFexThread(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags, const char* Name) {
  auto W = fextl::make_unique<PThreadWrapper>();
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, 8 << 20);
  pthread_create(&W->T, &Attr, Func, Arg);
  pthread_attr_destroy(&Attr);
  return W;
}
void CleanupAfterFork() {}

void Usage() {
  fprintf(stderr,
          "usage: fexdarwin-run [options] [--env K=V ...] ELF [args...]\n"
          "  --env K=V          add an environment variable for the guest (default: empty environment)\n"
          "  --inherit-env      pass the host environment to the guest\n"
          "  --trace            log every system call (same as FEXDARWIN_TRACE=1)\n"
          "  --trace-signals    log fault handling decisions\n"
          "  --features         print the detected host features and exit\n"
          "  --smc=MODE         self-modifying code detection: mtrack (default), full or none\n"
          "  --fex-config K=V   set a FEXCore config option (for example TSOENABLED=0)\n"
          "  --stats            print translation and syscall statistics when the guest exits\n");
}

struct Args {
  std::vector<std::string> Env;
  bool InheritEnv = false;
  bool Stats = false;
  std::string ElfPath;
  std::vector<std::string> GuestArgv;
  std::string Cwd = "/"; // --cwd: guest working directory of the first process (LKD mode)
};

// Development overrides without changing the launcher: fexdarwin.env next to the executable. "NAME=VALUE" sets a host
// variable for this process and every guest process it forks (FEXDARWIN_DUMP_DIR, LKD_TRACE...) unless it is set
// already; "guest NAME=VALUE" sets or replaces a variable of the guest environment. # starts a comment.
void LoadEnvFile(Args& A) {
  char Exe[PATH_MAX];
  uint32_t Size = sizeof Exe;
  if (_NSGetExecutablePath(Exe, &Size) != 0) return;
  std::string Path = Exe;
  Path = Path.substr(0, Path.rfind('/') + 1) + "fexdarwin.env";
  std::ifstream F(Path);
  for (std::string Line; std::getline(F, Line);) {
    if (Line.empty() || Line[0] == '#') continue;
    bool Guest = Line.rfind("guest ", 0) == 0;
    if (Guest) Line = Line.substr(6);
    size_t Eq = Line.find('=');
    if (Eq == std::string::npos || Eq == 0) continue;
    std::string Name = Line.substr(0, Eq + 1);
    if (!Guest) {
      setenv(Line.substr(0, Eq).c_str(), Line.substr(Eq + 1).c_str(), 0);
      continue;
    }
    bool Replaced = false;
    for (auto& E : A.Env)
      if (E.rfind(Name, 0) == 0) { E = Line; Replaced = true; }
    if (!Replaced) A.Env.push_back(Line);
  }
}

uint64_t PushString(uint64_t& Cur, const std::string& S) {
  Cur -= S.size() + 1;
  memcpy(FEXCore::G2H(Cur), S.c_str(), S.size() + 1);
  return Cur;
}

// Builds argc/argv/envp/auxv on the guest stack exactly like the Linux kernel. Returns the initial rsp.
uint64_t BuildInitialStack(uint64_t StackTop, const LoadedElf& Elf, const std::vector<std::string>& Argv, const std::vector<std::string>& Envp,
                           uint64_t HwCap) {
  using namespace Linux;
  uint64_t Cur = StackTop;
  Cur -= 8; // end marker
  memset(FEXCore::G2H(Cur), 0, 8);
  const uint64_t ExecFn = PushString(Cur, Elf.Path);
  std::vector<uint64_t> EnvPtrs, ArgPtrs;
  for (auto It = Envp.rbegin(); It != Envp.rend(); ++It) EnvPtrs.insert(EnvPtrs.begin(), PushString(Cur, *It));
  for (auto It = Argv.rbegin(); It != Argv.rend(); ++It) ArgPtrs.insert(ArgPtrs.begin(), PushString(Cur, *It));
  const uint64_t Platform = PushString(Cur, "x86_64");
  Cur &= ~15ULL;
  Cur -= 16;
  const uint64_t Random = Cur;
  if (getentropy(FEXCore::G2H(Random), 16) != 0) memset(FEXCore::G2H(Random), 0x5a, 16);

  const uint64_t Auxv[][2] = {
    {AT_PHDR_, Elf.PhdrAddr},
    {AT_PHENT_, Elf.PhEnt},
    {AT_PHNUM_, Elf.PhNum},
    {AT_PAGESZ_, GuestMemory::PageSize},
    {AT_BASE_, 0},
    {AT_FLAGS_, 0},
    {AT_ENTRY_, Elf.Entry},
    {AT_UID_, getuid()},
    {AT_EUID_, geteuid()},
    {AT_GID_, getgid()},
    {AT_EGID_, getegid()},
    {AT_PLATFORM_, Platform},
    {AT_HWCAP_, HwCap},
    {AT_CLKTCK_, 100},
    {AT_SECURE_, 0},
    {AT_RANDOM_, Random},
    {AT_HWCAP2_, 0},
    {AT_EXECFN_, ExecFn},
    {AT_NULL_, 0},
  };
  const size_t NAux = sizeof(Auxv) / sizeof(Auxv[0]);
  const size_t Words = 1 + Argv.size() + 1 + Envp.size() + 1 + NAux * 2;
  Cur -= Words * 8;
  Cur &= ~15ULL;
  const uint64_t Sp = Cur;
  uint64_t* P = static_cast<uint64_t*>(FEXCore::G2H(Cur));
  *P++ = Argv.size();
  for (auto A : ArgPtrs) *P++ = A;
  *P++ = 0;
  for (auto E : EnvPtrs) *P++ = E;
  *P++ = 0;
  for (size_t I = 0; I < NAux; ++I) {
    *P++ = Auxv[I][0];
    *P++ = Auxv[I][1];
  }
  return Sp;
}

struct ThreadStart {
  GuestThread* T;
};
void* GuestThreadEntry(void* Arg) {
  auto* T = static_cast<GuestThread*>(Arg);
  if (T->Task) lkd_set_current(T->Task);
  RunGuestThread(T);
  return nullptr;
}

// LKD mode. FEXDARWIN_ROOT is the host directory used as the guest root file system; the ELF path is a guest path.
extern "C" int fexdarwin_lkd_install_stdio(lkd_task* t);
extern "C" int fexdarwin_lkd_bind(const char* guest_path, const char* host_dir, int ro);
extern "C" void fexdarwin_lkd_root_ro(int ro);
extern "C" void fexdarwin_lkd_register_cpuid(void);

int RunWithLkd(const Args& A, const HostOptions& Opt) {
  lkd_config Cfg {};
  Cfg.size = sizeof Cfg;
  const char* Root = getenv("FEXDARWIN_ROOT");
  Cfg.guest_root = Root && *Root ? Root : "/";
  // A nonzero window base selects LKD's offset mode (guest address g at host lkd_guest_base + g); 0 would be the identity
  // mode of the unit tests. 8 TiB is the base all witnesses were qualified with.
  // Guest window from 1 TiB to the top of the arm64 user address space (0x7ffffe000000): guest addresses 0 ..
  // 0x7efffe000000, close to the 2^47 of x86-64 Linux and Windows (the EAC bootstrapper maps fixed addresses at
  // 0x7001'0000'0000 and up; a 2^46 window made it fail). Reserved with maximum protection none, it is one kernel map
  // entry whatever its size (see src/lkd/src/mm/window.c).
  // Everything of the host itself lives below the window: macOS 27 keeps [0xfc0000000, 0x7000000000) for itself
  // (VM_MEMORY_GUARD), so the host has [0x7000000000, base). The first free address (0x7d80000000) left it 54 GiB, of
  // which libmalloc reserves 12 or 24 GiB, and each guest thread takes about 280 MiB (FEXCore's lookup cache, call-ret
  // stack, host stack): ELDEN RING's 110th thread found no room (MESURE 2026-10-10). From 1 TiB the host has 576 GiB,
  // enough for LKD's 512 threads per process. Host mappings placed by ASLR can reach into the range, so the base is also
  // past the end of the highest occupied region below the top, rounded up to 1 GiB.
  {
    constexpr uint64_t Top = 0x7fff'fe00'0000ULL;
    uint64_t Base = 0x100'0000'0000ULL;
    mach_vm_address_t A = Base;
    for (;;) {
      mach_vm_size_t Sz = 0;
      vm_region_basic_info_data_64_t Inf;
      mach_msg_type_number_t C = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t O;
      if (mach_vm_region(mach_task_self(), &A, &Sz, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&Inf), &C, &O) != KERN_SUCCESS ||
          A >= Top) {
        break;
      }
      Base = std::max<uint64_t>(Base, (A + Sz + (1ULL << 30) - 1) & ~((1ULL << 30) - 1));
      A += Sz;
    }
    // FEXDARWIN_GUEST_BASE=<address> (diagnostics): the window from that address to the top instead.
    if (const char* GB = getenv("FEXDARWIN_GUEST_BASE"); GB && *GB) {
      const uint64_t Want = strtoull(GB, nullptr, 0);
      if (Want && Want < Top) Base = Want;
    }
    Cfg.guest_window_base = Base;
    Cfg.guest_window_bytes = Top - Base;
  }
  if (int E = lkd_init_config(&Cfg); E != 0) { fprintf(stderr, "[fexdarwin] lkd_init_config failed %d\n", E); return 70; }
  if (int E = LkdSetup(); E != 0) { fprintf(stderr, "[fexdarwin] lkd_init failed %d\n", E); return 70; }
  // FEXDARWIN_ROOT_RO=1 keeps the guest root read-only (a shared reference tree); FEXDARWIN_BIND adds host directories
  // at guest paths: "guest=host[:ro]" entries separated by ';'.
  if (const char* RO = getenv("FEXDARWIN_ROOT_RO"); RO && *RO == '1') fexdarwin_lkd_root_ro(1);
  if (const char* B = getenv("FEXDARWIN_BIND"); B && *B) {
    std::string All = B;
    size_t Pos = 0;
    while (Pos <= All.size()) {
      size_t End = All.find(';', Pos);
      if (End == std::string::npos) End = All.size();
      std::string Item = All.substr(Pos, End - Pos);
      Pos = End + 1;
      if (Item.empty()) continue;
      const size_t Eq = Item.find('=');
      if (Eq == std::string::npos) { fprintf(stderr, "[fexdarwin] bad FEXDARWIN_BIND entry %s\n", Item.c_str()); return 2; }
      std::string Guest = Item.substr(0, Eq), Hostp = Item.substr(Eq + 1);
      int Ro = 0;
      if (Hostp.size() > 3 && Hostp.compare(Hostp.size() - 3, 3, ":ro") == 0) { Ro = 1; Hostp.resize(Hostp.size() - 3); }
      if (int E = fexdarwin_lkd_bind(Guest.c_str(), Hostp.c_str(), Ro); E != 0) {
        fprintf(stderr, "[fexdarwin] bind %s -> %s failed %d\n", Guest.c_str(), Hostp.c_str(), E);
        return 2;
      }
      if (Opt.Trace) fprintf(stderr, "[fexdarwin] bind %s -> %s%s\n", Guest.c_str(), Hostp.c_str(), Ro ? " (ro)" : "");
    }
  }

  std::vector<std::string> Envp = A.Env;
  std::vector<const char*> ArgvC, EnvC;
  for (auto& S : A.GuestArgv) ArgvC.push_back(S.c_str());
  ArgvC.push_back(nullptr);
  for (auto& S : Envp) EnvC.push_back(S.c_str());
  EnvC.push_back(nullptr);
  lkd_main_spec Spec {};
  Spec.size = sizeof Spec;
  Spec.exe_path = A.ElfPath.c_str();
  Spec.argv = ArgvC.data();
  Spec.envp = EnvC.data();
  Spec.cwd = A.Cwd.c_str();
  lkd_task* Task = lkd_task_create_main(&Spec);
  if (!Task) { fprintf(stderr, "[fexdarwin] lkd_task_create_main failed: %s\n", strerror(errno)); return 70; }
  // The guest window is reserved with the first address space (lkd_mm_create), so the base is known only now. FEXCore
  // must be created after this point: the dispatcher embeds the base as a constant when InitCore generates it.
  FEXCore::GuestBase = lkd_guest_base;
  LkdStackTop = Cfg.guest_window_bytes - (64ULL << 20);
  if (Opt.Trace) fprintf(stderr, "[fexdarwin] LKD mode, guest root %s, guest base %#llx\n", Cfg.guest_root, (unsigned long long)lkd_guest_base);

  G->CTX = FEXCore::Context::Context::CreateNewContext(G->Features);
  G->Syscalls = std::make_unique<DarwinSyscallHandler>();
  G->Signals = std::make_unique<DarwinSignalDelegator>();
  G->CTX->SetSignalDelegator(G->Signals.get());
  G->CTX->SetSyscallHandler(G->Syscalls.get());
  if (!G->CTX->InitCore()) { fprintf(stderr, "[fexdarwin] FEXCore InitCore failed\n"); return 70; }
  G->Signals->Install();
  fexdarwin_lkd_register_cpuid();

  GuestThread* Main = CreateGuestThread(nullptr);
  if (!Main) { fprintf(stderr, "[fexdarwin] no host memory for the first guest thread\n"); return 70; }
  Main->Task = Task;
  Main->Tid = lkd_task_tid(Task);
  lkd_task_set_engine_data(Task, Main);
  lkd_set_current(Task);
  if (int E = fexdarwin_lkd_install_stdio(Task); E != 0) { fprintf(stderr, "[fexdarwin] guest stdio setup failed %d\n", E); return 70; }

  LkdLoadRequest Req;
  Req.GuestPath = A.ElfPath;
  Req.Argv = A.GuestArgv;
  Req.Envp = Envp;
  Req.StackTop = LkdStackTop;
  Req.Cwd = A.Cwd;
  Req.HwCap = G->CTX->RunCPUIDFunction(1, 0).edx;
  std::string Err;
  if (!LkdLoadProgram(Main, Req, &Err)) { fprintf(stderr, "[fexdarwin] %s\n", Err.c_str()); return 126; }
  if (Opt.Trace) fprintf(stderr, "[fexdarwin] entry=%#llx rsp=%#llx\n", (unsigned long long)Main->StartRip, (unsigned long long)Main->StartRsp);

  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, 256ULL << 20);
  pthread_t Th;
  if (pthread_create(&Th, &Attr, GuestThreadEntry, Main) != 0) { fprintf(stderr, "[fexdarwin] pthread_create failed\n"); return 70; }
  Main->PThread = Th;
  pthread_join(Th, nullptr);
  // The guest main thread ended with exit() while other threads live on: Linux keeps the process running. The process
  // ends when the last thread is released (ExitProcess from that thread).
  for (;;) pause();
}
} // namespace

int main(int argc, char** argv) {
  Args A;
  HostOptions Opt;
  Opt.Trace = getenv("FEXDARWIN_TRACE") && strcmp(getenv("FEXDARWIN_TRACE"), "0") != 0;
  bool OnlyFeatures = false;
  int I = 1;
  for (; I < argc; ++I) {
    std::string S = argv[I];
    if (S == "--") { ++I; break; }
    if (S == "--env" && I + 1 < argc) A.Env.push_back(argv[++I]);
    else if (S == "--inherit-env") A.InheritEnv = true;
    else if (S == "--cwd" && I + 1 < argc) A.Cwd = argv[++I];
    else if (S == "--trace") Opt.Trace = true;
    else if (S == "--trace-signals") Opt.TraceSignals = true;
    else if (S == "--features") OnlyFeatures = true;
    else if (S == "--stats") A.Stats = true;
    else if (S.rfind("--smc=", 0) == 0) Opt.SmcMode = S.substr(6);
    else if (S == "--fex-config" && I + 1 < argc) Opt.ExtraConfig.push_back(argv[++I]);
    else if (S == "-h" || S == "--help") { Usage(); return 0; }
    else if (!S.empty() && S[0] == '-') { fprintf(stderr, "unknown option %s\n", S.c_str()); Usage(); return 2; }
    else break;
  }
  LoadEnvFile(A);
  Opt.Trace = Opt.Trace || (getenv("FEXDARWIN_TRACE") && strcmp(getenv("FEXDARWIN_TRACE"), "0") != 0);
  if (Opt.SmcMode != "none" && Opt.SmcMode != "mtrack" && Opt.SmcMode != "full") {
    fprintf(stderr, "--smc must be none, mtrack or full\n");
    return 2;
  }
  if (!OnlyFeatures) {
    if (I >= argc) { Usage(); return 2; }
    A.ElfPath = argv[I++];
    A.GuestArgv.push_back(A.ElfPath);
    for (; I < argc; ++I) A.GuestArgv.push_back(argv[I]);
  }

  auto T0 = std::chrono::steady_clock::now();
  G = new Host();
  G->Opt = Opt;
  G->Pid = getpid();

  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  FEXCore::Threads::Thread::SetInternalPointers({.CreateThread = CreateFexThread, .CleanupAfterFork = CleanupAfterFork});

  std::string FeatureReport;
  G->Features = GetDarwinHostFeatures(&FeatureReport);
  if (OnlyFeatures || Opt.Trace) {
    fprintf(stderr, "[fexdarwin] host features (sysctl hw.optional.arm.*):\n%s", FeatureReport.c_str());
  }
  if (OnlyFeatures) return 0;

  // ---- FEXCore configuration ----
  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, Opt.SmcMode == "none" ? "0" : Opt.SmcMode == "mtrack" ? "1" : "2");
  {
    static const std::pair<const char*, FEXCore::Config::ConfigOption> Lookup[] = {
#define OPT_BASE(type, group, enum, json, default) {#enum, FEXCore::Config::ConfigOption::CONFIG_##enum},
#include <FEXCore/Config/ConfigValues.inl>
    };
    for (auto& KV : Opt.ExtraConfig) {
      auto Eq = KV.find('=');
      if (Eq == std::string::npos) { fprintf(stderr, "bad --fex-config %s\n", KV.c_str()); return 2; }
      std::string Name = KV.substr(0, Eq), Val = KV.substr(Eq + 1);
      bool Found = false;
      for (auto& L : Lookup) {
        if (Name == L.first) { FEXCore::Config::Set(L.second, Val); Found = true; break; }
      }
      if (!Found) { fprintf(stderr, "unknown FEXCore option %s\n", Name.c_str()); return 2; }
    }
  }

  // ---- LKD mode: the Linux kernel layer owns the guest address space, files, processes and signals ----
  if (LkdEnabled()) {
    return RunWithLkd(A, Opt);
  }

  // ---- guest address space ----
  if (const char* GB = getenv("FEXDARWIN_GUEST_BASE"); GB && *GB) {
    FEXCore::GuestBase = strtoull(GB, nullptr, 0);
  }
  if (FEXCore::GuestBase) {
    WindowBase = 0x10000;
    WindowSize = 0x7000'0000'0000ULL - WindowBase;
  }
  std::string Err;
  if (!G->Mem.Reserve(WindowBase, WindowSize, &Err)) {
    fprintf(stderr, "[fexdarwin] %s\n", Err.c_str());
    return 70;
  }

  // ---- FEXCore context ----
  G->CTX = FEXCore::Context::Context::CreateNewContext(G->Features);
  G->Syscalls = std::make_unique<DarwinSyscallHandler>();
  G->Signals = std::make_unique<DarwinSignalDelegator>();
  G->CTX->SetSignalDelegator(G->Signals.get());
  G->CTX->SetSyscallHandler(G->Syscalls.get());
  if (!G->CTX->InitCore()) {
    fprintf(stderr, "[fexdarwin] FEXCore InitCore failed\n");
    return 70;
  }
  G->Signals->Install();

  // ---- load the program ----
  LoadedElf Elf;
  ElfLoadOptions LoadOpt;
  LoadOpt.DynBase = DynBase;
  LoadOpt.Verbose = Opt.Trace;
  if (!LoadElf(A.ElfPath, G->Mem, LoadOpt, &Elf, &Err)) {
    fprintf(stderr, "[fexdarwin] %s: %s\n", A.ElfPath.c_str(), Err.c_str());
    return 126;
  }

  // brk heap starts after the image, mmap(NULL) allocates top-down below the stack
  G->Mem.SetupBrk(Elf.HighAddr, BrkMaxBytes);
  G->StackTop = WindowBase + WindowSize;
  G->StackSize = StackBytes;
  const uint64_t StackLow = G->StackTop - StackBytes;
  G->Mem.SetMmapTop(StackLow - (1ULL << 20));
  G->Mem.SetMmapFloor(Elf.HighAddr + BrkMaxBytes + (1ULL << 20));
  int64_t R = G->Mem.Mmap(StackLow, StackBytes, Linux::PROT_READ_ | Linux::PROT_WRITE_, Linux::MAP_PRIVATE_ | Linux::MAP_ANONYMOUS_ | Linux::MAP_FIXED_, -1, 0, "[stack]");
  if (R < 0) {
    fprintf(stderr, "[fexdarwin] cannot map the guest stack: errno %lld\n", (long long)-R);
    return 70;
  }

  std::vector<std::string> Envp = A.Env;
  if (A.InheritEnv) {
    extern char** environ;
    for (char** E = environ; E && *E; ++E) Envp.push_back(*E);
  }
  const uint64_t HwCap = G->CTX->RunCPUIDFunction(1, 0).edx;
  const uint64_t Sp = BuildInitialStack(G->StackTop, Elf, A.GuestArgv, Envp, HwCap);

  GuestThread* Main = CreateGuestThread(nullptr);
  if (!Main) { fprintf(stderr, "[fexdarwin] no host memory for the first guest thread\n"); return 70; }
  Main->StartRip = Elf.Entry;
  Main->StartRsp = Sp;
  Main->StartRax = 0;

  if (Opt.Trace) {
    auto Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
    fprintf(stderr, "[fexdarwin] setup done in %.2f ms; entry=%#llx rsp=%#llx\n%s", Ms, (unsigned long long)Elf.Entry, (unsigned long long)Sp,
            G->Mem.Dump().c_str());
  }

  // Run the guest on a thread with a large stack: the JIT compiler recurses deeply.
  pthread_attr_t Attr;
  pthread_attr_init(&Attr);
  pthread_attr_setstacksize(&Attr, 256ULL << 20);
  pthread_t Th;
  if (pthread_create(&Th, &Attr, GuestThreadEntry, Main) != 0) {
    fprintf(stderr, "[fexdarwin] pthread_create failed\n");
    return 70;
  }
  Main->PThread = Th;
  pthread_join(Th, nullptr);
  // Reaching here means the guest main thread stopped without calling exit_group.
  fprintf(stderr, "[fexdarwin] guest thread returned without exit_group\n");
  return 70;
}

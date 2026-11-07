// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct GuestThread;

// Top of the initial stack of every program: 64 MiB under the top of the guest window, above LKD's mmap base (window
// top - 128 MiB), like Linux places the stack above the mmap area. Set once the window is known (main.cpp).
extern uint64_t LkdStackTop;

struct LkdLoadRequest {
  std::string GuestPath;          // path of the executable inside the guest root
  std::string ExecFn;             // AT_EXECFN (the path given to execve); GuestPath when empty
  std::string Cwd;                // initial working directory (first process only); empty keeps the current one
  std::vector<std::string> Argv;
  std::vector<std::string> Envp;
  uint64_t StackTop {};           // guest address of the top of the initial stack
  uint64_t Uid {1000}, Gid {1000};
  uint64_t HwCap {};              // AT_HWCAP (CPUID leaf 1 EDX on x86-64)
};

// Map the program (and its PT_INTERP) through LKD system calls and prepare the initial stack. Fills StartRip/StartRsp of
// the thread. Returns false with Err set on failure.
bool LkdLoadProgram(GuestThread* GT, const LkdLoadRequest& Req, std::string* Err);

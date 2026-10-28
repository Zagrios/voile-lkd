// SPDX-License-Identifier: MIT
#pragma once

#include "Host.h"

#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <optional>
#include <string>

// High-level emulation (HLE) of the x86-64 Linux system calls the fexdarwin witnesses need. Anything not in the table
// returns -ENOSYS and is logged when FEXDARWIN_TRACE=1: no call is ever reported as successful without being done.
class DarwinSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;
  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;
  bool NeedsCodeValidation(uint64_t Start, uint64_t Length) override;
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Address) override;
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState* Thread, uint64_t GuestAddr) override {
    return std::nullopt;
  }
  void SleepThread(FEXCore::Context::Context* CTX, FEXCore::Core::CpuStateFrame* Frame) override;
  void PreCompile() override;
  void PostCompile() override;

  // Statistics for the benchmark and the README.
  std::atomic<uint64_t> SyscallCount {0};

private:
  int64_t Dispatch(GuestThread* T, uint64_t Nr, const uint64_t A[6]);
};

const char* LinuxSyscallName(uint64_t Nr);

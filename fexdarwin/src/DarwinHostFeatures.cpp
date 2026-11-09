// SPDX-License-Identifier: MIT
#include "DarwinHostFeatures.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {
// Reads an integer sysctl of either width (some hw.* values are 32 bit, others such as hw.cachelinesize are 64 bit).
int64_t SysctlInt(const char* Name, int64_t Default = 0, bool* Found = nullptr) {
  int64_t V = 0;
  size_t S = sizeof(V);
  if (sysctlbyname(Name, &V, &S, nullptr, 0) != 0) {
    if (Found) *Found = false;
    return Default;
  }
  if (Found) *Found = true;
  if (S == sizeof(int32_t)) {
    return static_cast<int32_t>(V & 0xffffffffLL);
  }
  return V;
}

uint64_t ReadFPCR() {
  uint64_t V;
  __asm volatile("mrs %0, FPCR" : "=r"(V));
  return V;
}
void WriteFPCR(uint64_t V) {
  __asm volatile("msr FPCR, %0" ::"r"(V));
}
uint64_t ReadDCZID() {
  uint64_t V;
  __asm volatile("mrs %0, DCZID_EL0" : "=r"(V));
  return V;
}

struct Reporter {
  std::string* Out;
  void Feat(const char* Sysctl, int Value, bool Found) {
    if (!Out) return;
    char B[160];
    snprintf(B, sizeof B, "  hw.optional.arm.%-18s %s\n", Sysctl, Found ? (Value ? "1" : "0") : "(absent)");
    *Out += B;
  }
};
} // namespace

FEXCore::HostFeatures GetDarwinHostFeatures(std::string* Report) {
  Reporter R {Report};
  auto Has = [&](const char* Feature) {
    std::string N = std::string("hw.optional.arm.") + Feature;
    bool Found = false;
    int V = static_cast<int>(SysctlInt(N.c_str(), 0, &Found));
    R.Feat(Feature, V, Found);
    return Found && V != 0;
  };

  FEXCore::HostFeatures F {};
  F.SupportsCacheMaintenanceOps = true; // dc cvau / ic ivau are legal at EL0 (sys_icache_invalidate relies on them)

  F.SupportsAES = Has("FEAT_AES");
  F.SupportsPMULL_128Bit = Has("FEAT_PMULL");
  const bool SHA1 = Has("FEAT_SHA1");
  const bool SHA2 = Has("FEAT_SHA256");
  F.SupportsSHA = SHA1 && SHA2;
  F.SupportsCRC = Has("FEAT_CRC32");
  F.SupportsAtomics = Has("FEAT_LSE");
  F.SupportsRCPC = Has("FEAT_LRCPC");
  F.SupportsTSOImm9 = Has("FEAT_LRCPC2");
  F.SupportsCSSC = Has("FEAT_CSSC");
  F.SupportsFCMA = Has("FEAT_FCMA");
  F.SupportsFlagM = Has("FEAT_FlagM");
  F.SupportsFlagM2 = Has("FEAT_FlagM2");
  F.SupportsFRINTTS = Has("FEAT_FRINTTS");
  F.SupportsRPRES = Has("FEAT_RPRES");
  F.SupportsAFP = Has("FEAT_AFP");
  F.SupportsECV = Has("FEAT_ECV");
  F.SupportsWFXT = Has("FEAT_WFxT");
  F.SupportsI8MM = Has("FEAT_I8MM");
  F.SupportsDotProd = Has("FEAT_DotProd");
  F.SupportsMOPS = Has("FEAT_MOPS");
  // RNDR/RNDRRS raise SIGILL at EL0 on macOS (measured), and no FEAT_RNG sysctl exists: keep the software RNG.
  F.SupportsRAND = false;
  // SVE is absent from hw.optional.arm.* on this machine (only SME/SME2 streaming mode exists): no SVE code paths.
  F.ProcessFeatures.SupportsSVE128 = Has("FEAT_SVE");
  F.ProcessFeatures.SupportsSVE256 = false; // FEX also needs a 256-bit vector length, which cannot be probed without SVE
  F.SupportsSVEBitPerm = false;

  // Same convention as Source/Common/HostFeatures.cpp: AVX is always emulated with NEON pairs when no SVE256.
  F.Supports3DNow = true;
  F.ProcessFeatures.SupportsAVX = true;
  F.SupportsAES256 = F.SupportsAVX() && F.SupportsAES;
  F.SupportsPreserveAllABI = FEX_HAS_PRESERVE_ALL_ATTR;
  F.PreferZVAForVZero = false;
  F.SupportsCPUIndexInTPIDRRO = false; // TPIDRRO_EL0 holds the thread pointer on macOS (measured)

  // DC ZVA: DCZID_EL0 is readable at EL0. FEX requires a 64 byte block (CLZERO emulation).
  {
    const uint64_t DCZID = ReadDCZID();
    const bool Permitted = (DCZID & 0x10) == 0;
    const uint32_t Block = (1u << (DCZID & 0xF)) * 4;
    F.SupportsCLZERO = Permitted && Block == 64;
    if (Report) {
      char B[128];
      snprintf(B, sizeof B, "  DCZID_EL0 = %#llx (DC ZVA %s, block %u bytes)\n", (unsigned long long)DCZID, Permitted ? "permitted" : "prohibited", Block);
      *Report += B;
    }
  }

  // D-cache line size: CTR_EL0 is not readable (SIGILL, measured), use the sysctl.
  {
    const int Line = static_cast<int>(SysctlInt("hw.cachelinesize", 64));
    uint32_t Words = Line / 4;
    uint32_t Log2 = 0;
    while ((1u << Log2) < Words) ++Log2;
    F.DCacheLineLog2 = Log2 & 0xF;
    if (Report) {
      char B[96];
      snprintf(B, sizeof B, "  hw.cachelinesize = %d -> DCacheLineLog2 = %u\n", Line, F.DCacheLineLog2);
      *Report += B;
    }
  }

  // Float exception trapping: same probe as FEX (write the trap enable bits, read them back).
  {
    constexpr uint64_t Traps = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 15);
    const uint64_t Old = ReadFPCR();
    WriteFPCR(Old | Traps);
    const uint64_t Now = ReadFPCR();
    F.SupportsFloatExceptions = (Now & Traps) == Traps;
    WriteFPCR(Old);
    if (Report) {
      char B[128];
      snprintf(B, sizeof B, "  FPCR trap-enable probe: wrote %#llx read %#llx -> SupportsFloatExceptions=%d\n",
               (unsigned long long)(Old | Traps), (unsigned long long)Now, (int)F.SupportsFloatExceptions);
      *Report += B;
    }
  }

  // The host type changes code generation: Unknown means no vsyscall page (vgettimeofday, vtime, vgetcpu) and no
  // int 0x80 system call (Decoder::AdjustAddrForSpecialRegion, OpDispatchBuilder::INTOp). RDTSCPOp and RDPIDOp also
  // refuse a host other than Linux without the CPU index in TPIDRRO_EL0, but the Darwin port compiles that check out.
  F.ProcessFeatures.HostType = FEXCore::HostFeatures::HostTypeEnum::Unknown;
  // Is64BitMode only feeds the code cache's hash, as in upstream's FetchHostFeatures.
  F.ProcessFeatures.Is64BitMode = true;

  // Per-core MIDR values feed CPUID (core count, brand string, hybrid detection). MIDR_EL1 is not readable, so use one
  // synthetic Apple value (implementer 0x61, part 0) for every logical core: FEX then reports "Apple Silicon" and a
  // non-hybrid CPU. This is a documented approximation.
  const int Cores = static_cast<int>(SysctlInt("hw.logicalcpu", 1));
  F.CPUMIDRs.assign(Cores > 0 ? Cores : 1, 0x61000000u);
  F.ProcessPID = static_cast<uint32_t>(getpid());

  if (Report) {
    char B[128];
    snprintf(B, sizeof B, "  logical cores = %d, HostType = Unknown, SVE128=%d SVE256=%d AVX=%d LSE=%d RCPC=%d RCPC2=%d CLZERO=%d\n",
             Cores, (int)F.SupportsSVE128(), (int)F.SupportsSVE256(), (int)F.SupportsAVX(), (int)F.SupportsAtomics, (int)F.SupportsRCPC,
             (int)F.SupportsTSOImm9, (int)F.SupportsCLZERO);
    *Report += B;
  }
  return F;
}

// SPDX-License-Identifier: MIT
// Counts of the host signals the fault handler takes, by what they turn out to be (Sampler writes them at each flush).
// They count only while the sampler runs (FEXDARWIN_SAMPLE): otherwise a note is one relaxed load.
#pragma once

#include <atomic>
#include <cstdint>

namespace FaultStats {
enum Kind : unsigned {
  Segv,          // SIGSEGV taken
  Bus,           // SIGBUS taken
  OtherSignal,   // any other host signal taken by the handler
  LkdRecovered,  // an LKD fault-safe copy
  StubFault,     // GuardedLoadPatch stub (step 0)
  Unaligned,     // unaligned atomic back-patched (step 3)
  SmcWrite,      // store to a page with translated code (step 4)
  GuardOpen,     // JIT load/store on a guarded host page, opened (ResolveGuardedFault from JIT)
  GuardStepEnd,  // single-step trap that ends a guard step
  GuestSignal,   // fault delivered to the guest (a guest exception)
  DecoderGuard,  // decoder reading a guarded page (step 6)
  DecoderNoExec, // decoder reading a page that lost its execute permission (step 7)
  GuardFileRead, // JIT load served from the guarded page's file view (GuardedLoadPatch may patch it)
  GuardRace,     // page not guarded any more, the guest allows the access: retried
  GuardStore,    // JIT store to a guarded page: page opened, store retried
  GuardLoadOpen, // JIT load without a file view: page opened, read, closed
  GuardStep,     // JIT instruction stepped with its page open
  Count
};
inline std::atomic<bool> Enabled {false};
inline std::atomic<uint64_t> Counts[Count];
inline void Note(Kind K) {
  if (!Enabled.load(std::memory_order_relaxed)) return;
  Counts[K].fetch_add(1, std::memory_order_relaxed);
}
// The host sites (PC) of guarded faults, by path, open addressing; Sampler writes the busiest.
struct Site {
  std::atomic<uint64_t> Pc;   // 0 = empty
  std::atomic<uint64_t> Addr; // last guest address
  std::atomic<uint32_t> Count;
  std::atomic<uint32_t> Kind;
};
constexpr unsigned SiteSlots = 4096;
inline Site Sites[SiteSlots];
inline void NoteSite(Kind K, uint64_t Pc, uint64_t Addr) {
  if (!Enabled.load(std::memory_order_relaxed)) return;
  unsigned H = static_cast<unsigned>((Pc * 0x9E3779B97F4A7C15ull) >> 52) % SiteSlots;
  for (unsigned P = 0; P < 16; P++, H = (H + 1) % SiteSlots) {
    uint64_t Cur = Sites[H].Pc.load(std::memory_order_relaxed);
    if (Cur == 0 && Sites[H].Pc.compare_exchange_strong(Cur, Pc)) Cur = Pc;
    if (Cur == Pc) {
      Sites[H].Addr.store(Addr, std::memory_order_relaxed);
      Sites[H].Kind.store(K, std::memory_order_relaxed);
      Sites[H].Count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}
inline const char* Name(unsigned K) {
  static const char* const Names[] = {"segv", "bus", "other", "lkd_recovered", "stub", "unaligned", "smc_write", "guard_open",
                                      "guard_step_end", "guest_signal", "decoder_guard", "decoder_noexec", "guard_file_read",
                                      "guard_race", "guard_store", "guard_load_open", "guard_step"};
  return Names[K];
}
} // namespace FaultStats

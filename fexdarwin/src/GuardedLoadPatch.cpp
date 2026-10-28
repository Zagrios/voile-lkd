// SPDX-License-Identifier: MIT
#include "GuardedLoadPatch.h"

#include "DarwinContext.h"
#include "GuardedLoadStub.h"

#include <FEXCore/Core/PatchRegion.h>
#include <FEXCore/Utils/GuestBase.h>
#include <FEXCore/Utils/JITWriteScope.h>
#include <FEXCore/Utils/SpinWaitLock.h>

#include <atomic>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

extern "C" int fexdarwin_lkd_guard_aliases(uint64_t gva, const void* alias[4], const uint64_t** gen, uint64_t* gen_now);

namespace GuardedLoadPatch {
namespace {
using GuardedLoadStub::SlotBytes;

// 32 faults of one host instruction: the SHA-256 loop takes 16 per block; one switch table read of THEIA's interpreter
// loops takes about one per guest instruction it interprets.
constexpr uint32_t PatchAfter = 32;

// Stub slots live in the patch region at the end of each FEX code buffer (FEXCore/Core/PatchRegion.h): every load of
// the buffer reaches them with a branch. Slots are not reused while the buffer lives.
constexpr unsigned MaxPools = 64;

struct Record {
  uint64_t Site;        // host address of the patched load
  uint32_t Insn;        // its original instruction
  GuardedLoadStub::Layout L;
  std::atomic<bool> Dead; // the original instruction is back at Site
};
struct Pool {
  std::atomic<uint64_t> Base; // patch region, 0 when free
  uint64_t CodeBegin;         // the code it serves: [CodeBegin, Base)
  uint64_t Bytes;
  unsigned Used;              // slots handed out (under Lock)
  unsigned Capacity;          // records Recs holds
  Record* Recs;               // kept when the buffer goes, for the next one
};
Pool Pools[MaxPools];
uint32_t Lock {}; // FEXCore::Utils::SpinWaitLock

// Faults counted per host instruction, open addressing. A full neighbourhood gives its first slot to the new
// instruction: hot instructions come back faster than cold ones take their place.
constexpr unsigned HeatSlots = 16384, HeatProbes = 8;
std::atomic<uint64_t> HeatPc[HeatSlots];
std::atomic<uint32_t> HeatCount[HeatSlots];

using Guard = FEXCore::Utils::SpinWaitLock::UniqueSpinMutex<uint32_t>;
// A fork child has only the forking thread: the lock must not be held by one that is gone.
const int AtFork = pthread_atfork([] { FEXCore::Utils::SpinWaitLock::lock(&Lock); },
                                  [] { FEXCore::Utils::SpinWaitLock::unlock(&Lock); },
                                  [] { FEXCore::Utils::SpinWaitLock::unlock(&Lock); });

Record* RecordOfStubPc(uint64_t Pc, uint64_t* SlotBase) {
  for (auto& P : Pools) {
    const uint64_t Base = P.Base.load(std::memory_order_acquire);
    if (!Base || Pc < Base || Pc >= Base + P.Bytes) continue;
    const unsigned I = static_cast<unsigned>((Pc - Base) / SlotBytes);
    *SlotBase = Base + static_cast<uint64_t>(I) * SlotBytes;
    return &P.Recs[I];
  }
  return nullptr;
}

// The record of a patched site: its word is a branch to the slot of a live record of that site.
Record* RecordOfSite(uint64_t Pc, uint64_t* SlotBase) {
  const uint32_t W = *reinterpret_cast<const uint32_t*>(Pc);
  if ((W >> 26) != 0x05) return nullptr;
  const int64_t Off = static_cast<int64_t>(static_cast<int32_t>(W << 6) >> 6) * 4;
  const uint64_t Target = Pc + Off;
  Record* R = RecordOfStubPc(Target, SlotBase);
  if (!R || *SlotBase != Target || R->Site != Pc || R->Dead.load(std::memory_order_acquire)) return nullptr;
  return R;
}

void WriteWord(uint64_t At, uint32_t W) {
  {
    FEXCore::Utils::JITWriteScope Scope;
    __atomic_store_n(reinterpret_cast<uint32_t*>(At), W, __ATOMIC_RELEASE);
  }
  sys_icache_invalidate(reinterpret_cast<void*>(At), 4);
}

// What a stub needs to know about the guarded host page of a guest address (lkd_mm_guard_aliases).
struct Description {
  bool Guarded;
  const void* Alias[4];
  const uint64_t* Gen;
  uint64_t GenNow;
  uint64_t Page; // host address of the guarded host page
};
Description Lookup(uint64_t Addr) {
  Description Desc {};
  Desc.Guarded = fexdarwin_lkd_guard_aliases(Addr, Desc.Alias, &Desc.Gen, &Desc.GenNow) == 1;
  Desc.Page = reinterpret_cast<uint64_t>(FEXCore::G2H(Addr & ~0x3fffull));
  return Desc;
}

// Writes Desc into the stub at Slot: a sequence lock on Seq, as the stub reads it. Lock held.
void Describe(uint64_t Slot, const Description& Desc) {
  auto* D = reinterpret_cast<uint64_t*>(Slot);
  FEXCore::Utils::JITWriteScope Scope;
  __atomic_store_n(&D[GuardedLoadStub::SeqOff / 8], GuardedLoadStub::SeqInvalid, __ATOMIC_RELAXED);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!Desc.Guarded) return; // stays invalid: the original load runs
  __atomic_store_n(&D[GuardedLoadStub::GenPtrOff / 8], reinterpret_cast<uint64_t>(Desc.Gen), __ATOMIC_RELAXED);
  __atomic_store_n(&D[GuardedLoadStub::PageOff / 8], Desc.Page, __ATOMIC_RELAXED);
  for (unsigned I = 0; I < 4; I++) {
    const uint64_t Delta = Desc.Alias[I] ? reinterpret_cast<uint64_t>(Desc.Alias[I]) - (Desc.Page + 4096ull * I) : 0;
    __atomic_store_n(&D[GuardedLoadStub::DeltaOff / 8 + I], Delta, __ATOMIC_RELAXED);
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  __atomic_store_n(&D[GuardedLoadStub::SeqOff / 8], Desc.GenNow, __ATOMIC_RELEASE);
}

// A free slot of the code buffer of Site. Lock held.
bool AllocateSlot(uint64_t Site, uint64_t* Slot, Record** R) {
  for (auto& P : Pools) {
    const uint64_t Base = P.Base.load(std::memory_order_relaxed);
    if (!Base || Site < P.CodeBegin || Site >= Base) continue;
    if (P.Used == P.Bytes / SlotBytes) return false;
    *Slot = Base + static_cast<uint64_t>(P.Used) * SlotBytes;
    *R = &P.Recs[P.Used++];
    return true;
  }
  return false;
}

void RegionAllocated(uint64_t CodeBegin, uint64_t RegionBegin, size_t RegionSize) {
  const unsigned Slots = static_cast<unsigned>(RegionSize / SlotBytes);
  Guard G(&Lock);
  for (auto& P : Pools) {
    if (P.Base.load(std::memory_order_relaxed)) continue;
    if (P.Capacity < Slots) {
      if (P.Recs) munmap(P.Recs, sizeof(Record) * P.Capacity);
      void* M = mmap(nullptr, sizeof(Record) * Slots, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      P.Recs = M == MAP_FAILED ? nullptr : static_cast<Record*>(M);
      P.Capacity = P.Recs ? Slots : 0;
      if (!P.Recs) return;
    }
    memset(static_cast<void*>(P.Recs), 0, sizeof(Record) * Slots); // a reused array: no record of the old buffer
    P.CodeBegin = CodeBegin;
    P.Bytes = static_cast<uint64_t>(Slots) * SlotBytes;
    P.Used = 0;
    P.Base.store(RegionBegin, std::memory_order_release);
    return;
  }
}

void RegionFreed(uint64_t RegionBegin) {
  Guard G(&Lock);
  for (auto& P : Pools) {
    if (P.Base.load(std::memory_order_relaxed) == RegionBegin) P.Base.store(0, std::memory_order_release);
  }
}

const bool Hooked = (FEXCore::CPU::PatchRegion = {RegionAllocated, RegionFreed}, true);

// Lock held.
void Patch(uint64_t Site, uint32_t Insn, const Description& Desc) {
  if (*reinterpret_cast<const uint32_t*>(Site) != Insn) return; // already patched, or the code changed
  uint64_t Slot;
  Record* R;
  if (!AllocateSlot(Site, &Slot, &R)) return;
  uint32_t Code[SlotBytes / 4] {};
  GuardedLoadStub::Layout L {};
  if (!GuardedLoadStub::Emit(Insn, Site, Slot, Code, L)) return; // the slot stays unused
  {
    FEXCore::Utils::JITWriteScope Scope;
    memcpy(reinterpret_cast<void*>(Slot), Code, GuardedLoadStub::GenPtrOff);
  }
  R->Site = Site;
  R->Insn = Insn;
  R->L = L;
  R->Dead.store(false, std::memory_order_release);
  Describe(Slot, Desc);
  sys_icache_invalidate(reinterpret_cast<void*>(Slot), SlotBytes);
  WriteWord(Site, GuardedLoadStub::detail::B(Site, Slot));
}

uint32_t Heat(uint64_t Pc) {
  const unsigned Start = static_cast<unsigned>((Pc >> 2) * 0x9E3779B1u) & (HeatSlots - 1);
  for (unsigned I = 0, H = Start; I < HeatProbes; I++, H = (H + 1) & (HeatSlots - 1)) {
    uint64_t Cur = HeatPc[H].load(std::memory_order_relaxed);
    if (Cur == 0 && HeatPc[H].compare_exchange_strong(Cur, Pc)) Cur = Pc;
    if (Cur == Pc) return HeatCount[H].fetch_add(1, std::memory_order_relaxed) + 1;
  }
  HeatPc[Start].store(Pc, std::memory_order_relaxed);
  HeatCount[Start].store(1, std::memory_order_relaxed);
  return 1;
}
} // namespace

uint32_t InstructionAt(uint64_t Pc) {
  uint64_t Slot;
  if (Record* R = RecordOfSite(Pc, &Slot)) return R->Insn;
  return *reinterpret_cast<const uint32_t*>(Pc);
}

bool OnFault(ucontext_t* Uc) {
  const uint64_t Pc = DarwinCtx::GetPc(Uc);
  uint64_t Slot;
  Record* R = RecordOfStubPc(Pc, &Slot);
  if (!R) return false;
  if (Pc == Slot + R->L.Fallback) {
    DarwinCtx::SetPc(Uc, R->Site);
    return false;
  }
  if (Pc != Slot + R->L.GenLoad && Pc != Slot + R->L.AliasLoad) return false;
  // The scratch registers are on the stack (GuardedLoadStub.h): restore them, put the load back and run it from its site.
  const uint64_t Sp = DarwinCtx::GetSp(Uc);
  const auto* Saved = reinterpret_cast<const uint64_t*>(Sp);
  for (unsigned I = 0; I < 4; I++) DarwinCtx::SetReg(Uc, static_cast<int>(R->L.Scratch[I]), Saved[I]);
  DarwinCtx::SetSp(Uc, Sp + 32);
  DarwinCtx::SetPc(Uc, R->Site);
  Guard G(&Lock);
  uint64_t Same;
  if (RecordOfSite(R->Site, &Same) == R) {
    R->Dead.store(true, std::memory_order_release);
    WriteWord(R->Site, R->Insn);
  }
  return true;
}

void NoteFileRead(uint64_t Pc, uint32_t Insn, uint64_t Addr) {
  uint64_t Slot;
  if (RecordOfSite(Pc, &Slot)) {
    // The stub ran the original load: the mappings changed, or the load moved to another host page.
    const Description Desc = Lookup(Addr);
    Guard G(&Lock);
    if (RecordOfSite(Pc, &Slot)) Describe(Slot, Desc);
    return;
  }
  if (Heat(Pc) != PatchAfter) return;
  const Description Desc = Lookup(Addr);
  Guard G(&Lock);
  Patch(Pc, Insn, Desc);
}

void Reset() {
  for (auto& P : HeatPc) P.store(0, std::memory_order_relaxed);
  for (auto& C : HeatCount) C.store(0, std::memory_order_relaxed);
}
} // namespace GuardedLoadPatch

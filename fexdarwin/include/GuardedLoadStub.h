// The stub that replaces a translated guest load patched by GuardedLoadPatch: its layout and its encoding, no state.
//
// A load that keeps faulting on a guarded host page (LKD mm, mm_page_guarded) branches here instead. The stub computes
// the load's address the way the load does; when it falls in the guarded host page the stub describes, in a 4 KiB page
// the guest allows, and the guest's mappings have not changed since the description was written, it loads from the
// file view of that 4 KiB page (lkd_mm_guard_aliases). Otherwise it runs the original load, which faults as before.
// The stub only uses its own scratch registers, saved on the host stack, and no instruction that sets the flags (FEX
// keeps the guest's flags in NZCV).
#pragma once

#include <cstdint>

namespace GuardedLoadStub {
constexpr unsigned SlotBytes = 256;
// Data at the end of a slot. GuardedLoadPatch rewrites Page and Delta while other threads may run the stub: Seq is a
// sequence lock. The stub uses the description only while Seq equals *GenPtr, before and after it reads it.
constexpr unsigned GenPtrOff = 192; // address of the guest mm's mapping generation
constexpr unsigned SeqOff = 200;    // generation the description was made at, SeqInvalid while it is rewritten
constexpr unsigned PageOff = 208;   // host address of the guarded host page
constexpr unsigned DeltaOff = 216;  // per 4 KiB page: file view minus host address, 0 when the stub must not read it
constexpr uint64_t SeqInvalid = ~0ull;

struct Layout {
  unsigned GenLoad;   // byte offsets of the instructions that can fault with the scratch registers saved
  unsigned AliasLoad;
  unsigned Fallback;  // the original load, run with every register restored
  unsigned Scratch[4];
};

namespace detail {
inline uint32_t Rel(uint64_t From, uint64_t To, unsigned Bits) {
  return static_cast<uint32_t>((static_cast<int64_t>(To - From) >> 2) & ((1ll << Bits) - 1));
}
inline uint32_t AddImm(unsigned D, unsigned N, uint32_t Imm, bool Lsl12 = false) {
  return 0x91000000u | (Lsl12 ? 1u << 22 : 0) | (Imm << 10) | (N << 5) | D;
}
inline uint32_t SubImm(unsigned D, unsigned N, uint32_t Imm, bool Lsl12 = false) {
  return 0xD1000000u | (Lsl12 ? 1u << 22 : 0) | (Imm << 10) | (N << 5) | D;
}
inline uint32_t AddExt(unsigned D, unsigned N, unsigned M, unsigned Option, unsigned Amount) {
  return 0x8B200000u | (M << 16) | (Option << 13) | (Amount << 10) | (N << 5) | D;
}
inline uint32_t AddReg(unsigned D, unsigned N, unsigned M) { return 0x8B000000u | (M << 16) | (N << 5) | D; }
inline uint32_t SubReg(unsigned D, unsigned N, unsigned M) { return 0xCB000000u | (M << 16) | (N << 5) | D; }
inline uint32_t EorReg(unsigned D, unsigned N, unsigned M) { return 0xCA000000u | (M << 16) | (N << 5) | D; }
inline uint32_t LsrImm(unsigned D, unsigned N, unsigned S) { return 0xD340FC00u | (S << 16) | (N << 5) | D; }
inline uint32_t And4K(unsigned D, unsigned N) { return 0x92402C00u | (N << 5) | D; } // and xD, xN, #0xfff
inline uint32_t LdrLit(unsigned T, uint64_t At, uint64_t Lit) { return 0x58000000u | (Rel(At, Lit, 19) << 5) | T; }
inline uint32_t Ldr(unsigned T, unsigned N) { return 0xF9400000u | (N << 5) | T; }
inline uint32_t LdrLsl3(unsigned T, unsigned N, unsigned M) { return 0xF8607800u | (M << 16) | (N << 5) | T; }
inline uint32_t Adr(unsigned D, uint64_t At, uint64_t To) {
  const uint32_t Off = static_cast<uint32_t>(To - At);
  return 0x10000000u | ((Off & 3) << 29) | (((Off >> 2) & 0x7ffff) << 5) | D;
}
inline uint32_t Cbz(unsigned T, uint64_t At, uint64_t To) { return 0xB4000000u | (Rel(At, To, 19) << 5) | T; }
inline uint32_t Cbnz(unsigned T, uint64_t At, uint64_t To) { return 0xB5000000u | (Rel(At, To, 19) << 5) | T; }
inline uint32_t Tbnz(unsigned T, unsigned Bit, uint64_t At, uint64_t To) {
  return 0x37000000u | ((Bit >> 5) << 31) | ((Bit & 31) << 19) | (Rel(At, To, 14) << 5) | T;
}
inline uint32_t B(uint64_t At, uint64_t To) { return 0x14000000u | Rel(At, To, 26); }
inline uint32_t StpPre32(unsigned A, unsigned B) { return 0xA9800000u | (0x7Cu << 15) | (B << 10) | (31u << 5) | A; } // [sp, #-32]!
inline uint32_t Stp16(unsigned A, unsigned B) { return 0xA9000000u | (2u << 15) | (B << 10) | (31u << 5) | A; }     // [sp, #16]
inline uint32_t Ldp16(unsigned A, unsigned B) { return 0xA9400000u | (2u << 15) | (B << 10) | (31u << 5) | A; }     // [sp, #16]
inline uint32_t LdpPost32(unsigned A, unsigned B) { return 0xA8C00000u | (4u << 15) | (B << 10) | (31u << 5) | A; } // [sp], #32
constexpr uint32_t DmbIshld = 0xD50339BFu;
} // namespace detail

// A load FEX emits, decoded: what DarwinSignalDelegator runs in a fault handler and Emit patches into a stub.
struct Load {
  enum Kind { UImm, Unscaled, PreIndex, PostIndex, RegOff, Pair, PairPreIndex, PairPostIndex, Acquire, AcquireUnscaled, Single } Form;
  unsigned Size; // log2 of the bytes of one element
  bool Vec;
  int Sign; // 0 zero-extend, 32 or 64: sign-extend into a W or X register
  unsigned Rt, Rt2, Rn, Rm;
  int64_t Offset;         // immediate byte offset (also the writeback of the pre- and post-indexed forms)
  unsigned Option, Shift; // RegOff: extend of Rm and its left shift
  // Single (LD1 one lane, LD1R): the vector is 128 bits wide (Q) or 64; LD1R fills every lane, LD1 only Lane.
  bool Q, Replicate;
  unsigned Lane;
  bool IsPair() const { return Form == Pair || Form == PairPreIndex || Form == PairPostIndex; }
  unsigned Bytes() const { return (1u << Size) * (IsPair() ? 2 : 1); }
};

// LDR/LDUR/LDAPUR/LDAR/LDAPR, register offset, pre- and post-index, LDP/LDNP; integer and FP/SIMD registers; LD1 of one
// lane and LD1R without writeback (FEX's MOVDDUP, VBROADCASTSS, MOVHPS, PINSR* from memory). False for anything else,
// stores and prefetches included, and for the unpredictable writebacks into a loaded register.
inline bool DecodeLoad(uint32_t I, Load& L) {
  const auto Imm9 = [&] { return static_cast<int64_t>(static_cast<int32_t>(I << 11) >> 23); };
  L = {};
  L.Rt = I & 31;
  L.Rn = (I >> 5) & 31;
  const unsigned SizeBits = I >> 30, Opc = (I >> 22) & 3;
  // The size and sign of an integer LDR form; false for PRFM and unallocated encodings.
  const auto IntegerLoad = [&] {
    L.Size = SizeBits;
    if (Opc < 2) return true;
    if (SizeBits == 3 || (SizeBits == 2 && Opc == 3)) return false;
    L.Sign = Opc == 2 ? 64 : 32;
    return true;
  };

  if ((I & 0x3B000000) == 0x39000000 || (I & 0x3B200400) == 0x38000400 || (I & 0x3B200C00) == 0x38000000 ||
      (I & 0x3B200C00) == 0x38200800) {
    L.Vec = (I >> 26) & 1;
    if (Opc == 0) return false; // a store
    if (L.Vec) {
      if (Opc == 2 || (Opc == 3 && SizeBits != 0)) return false;
      L.Size = Opc == 3 ? 4 : SizeBits;
    } else if (!IntegerLoad()) {
      return false;
    }
    if ((I & 0x3B000000) == 0x39000000) {
      L.Form = Load::UImm;
      L.Offset = static_cast<int64_t>(((I >> 10) & 0xfff) << L.Size);
    } else if ((I & 0x3B200C00) == 0x38000000) {
      L.Form = Load::Unscaled;
      L.Offset = Imm9();
    } else if ((I & 0x3B200400) == 0x38000400) {
      if (L.Rn == L.Rt && L.Rn != 31) return false;
      L.Form = (I >> 11) & 1 ? Load::PreIndex : Load::PostIndex;
      L.Offset = Imm9();
    } else {
      L.Form = Load::RegOff;
      L.Rm = (I >> 16) & 31;
      L.Option = (I >> 13) & 7;
      L.Shift = (I >> 12) & 1 ? L.Size : 0;
      if (L.Option != 2 && L.Option != 3 && L.Option != 6 && L.Option != 7) return false;
    }
    return true;
  }
  const uint32_t PairClass = I & 0x3BC00000;
  if (PairClass == 0x29400000 || PairClass == 0x28400000 || PairClass == 0x28C00000 || PairClass == 0x29C00000) {
    // LDP / LDNP (signed offset), LDP (post- and pre-index)
    L.Vec = (I >> 26) & 1;
    L.Rt2 = (I >> 10) & 31;
    if (L.Vec) {
      if (SizeBits == 3) return false;
      L.Size = 2 + SizeBits;
    } else {
      if (SizeBits == 3 || (SizeBits == 1 && PairClass == 0x28400000)) return false;
      L.Size = SizeBits == 2 ? 3 : 2;
      L.Sign = SizeBits == 1 ? 64 : 0;
    }
    L.Offset = (static_cast<int32_t>(I << 10) >> 25) * (int64_t {1} << L.Size);
    L.Form = PairClass == 0x28C00000 ? Load::PairPostIndex : PairClass == 0x29C00000 ? Load::PairPreIndex : Load::Pair;
    if (L.Form != Load::Pair && !L.Vec && L.Rn != 31 && (L.Rn == L.Rt || L.Rn == L.Rt2)) return false;
    return true;
  }
  if ((I & 0xBFFF0000) == 0x0D400000) {
    // LD1 (single structure) and LD1R, no offset: 0 Q 0011010 1 0 00000 opcode S size Rn Rt
    const unsigned Op = (I >> 13) & 7, S = (I >> 12) & 1, Sz = (I >> 10) & 3, Q = (I >> 30) & 1;
    L.Form = Load::Single;
    L.Vec = true;
    L.Q = Q;
    if (Op == 6 && !S) {
      L.Replicate = true;
      L.Size = Sz;
      return true;
    }
    switch (Op) {
    case 0: L.Size = 0; L.Lane = (Q << 3) | (S << 2) | Sz; return true;
    case 2: if (Sz & 1) return false; L.Size = 1; L.Lane = (Q << 2) | (S << 1) | (Sz >> 1); return true;
    case 4:
      if (Sz == 0) { L.Size = 2; L.Lane = (Q << 1) | S; return true; }
      if (Sz == 1 && !S) { L.Size = 3; L.Lane = Q; return true; }
      return false;
    default: return false;
    }
  }
  if ((I & 0x3FFFFC00) == 0x08DFFC00 || (I & 0x3FFFFC00) == 0x38BFC000) {
    L.Form = Load::Acquire; // LDAR, LDAPR
    L.Size = SizeBits;
    return true;
  }
  if ((I & 0x3F200C00) == 0x19000000) {
    L.Form = Load::AcquireUnscaled; // LDAPUR, LDAPURS
    if (Opc == 0 || !IntegerLoad()) return false; // STLUR
    L.Offset = Imm9();
    return true;
  }
  return false;
}

// Builds the stub of the load Insn at host address Site into Out (SlotBytes, to run at host address Stub, within a
// branch's reach of Site). Handles the loads of DecodeLoad without writeback and with a general base register; the
// acquire loads FEX emits for x86's memory order keep their ordering. False for anything else.
inline bool Emit(uint32_t Insn, uint64_t Site, uint64_t Stub, uint32_t* Out, Layout& L) {
  using namespace detail;
  Load Ld;
  if (!DecodeLoad(Insn, Ld)) return false;
  const auto Form = Ld.Form;
  if (Form == Load::PreIndex || Form == Load::PostIndex || Form == Load::PairPreIndex || Form == Load::PairPostIndex) {
    return false;
  }
  const bool Vec = Ld.Vec;
  const unsigned Rt = Ld.Rt, Rn = Ld.Rn, Rm = Form == Load::RegOff ? Ld.Rm : 32, Rt2 = Form == Load::Pair ? Ld.Rt2 : 32;
  if (Rn == 31) return false; // SP-based: the stub moves SP
  const unsigned Bytes = Ld.Bytes();

  // Four scratch registers the load does not use. Never x18 (the platform's), x29, x30.
  static constexpr unsigned Candidates[] = {16, 17, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7, 8};
  unsigned S[4], NS = 0;
  for (unsigned C : Candidates) {
    if (NS == 4) break;
    if (C == Rn || C == Rm || (!Vec && (C == Rt || C == Rt2))) continue;
    S[NS++] = C;
  }
  const unsigned A = S[0], Bq = S[1], C = S[2], D = S[3];

  unsigned N = 0;
  auto At = [&](unsigned I) { return Stub + 4ull * I; };
  auto Put = [&](uint32_t W) { Out[N++] = W; };
  auto AddOffset = [&](int64_t Off) { // A = Rn + Off, |Off| < 2^24
    const uint64_t Mag = Off < 0 ? -Off : Off;
    if (Off < 0) {
      Put(SubImm(A, Rn, static_cast<uint32_t>(Mag & 0xfff)));
      if (Mag >> 12) Put(SubImm(A, A, static_cast<uint32_t>(Mag >> 12), true));
    } else {
      Put(AddImm(A, Rn, static_cast<uint32_t>(Mag & 0xfff)));
      if (Mag >> 12) Put(AddImm(A, A, static_cast<uint32_t>(Mag >> 12), true));
    }
  };

  Put(StpPre32(A, Bq));
  Put(Stp16(C, D));
  // A = the load's address; Alias = the same load from [A] once A is moved to the file view.
  uint32_t Alias;
  switch (Form) {
  case Load::UImm:
    AddOffset(Ld.Offset);
    Alias = (Insn & ~0x003FFFE0u) | (A << 5);
    break;
  case Load::Unscaled:
  case Load::AcquireUnscaled:
    AddOffset(Ld.Offset);
    Alias = (Insn & ~0x001FF3E0u) | (A << 5);
    break;
  case Load::RegOff:
    Put(AddExt(A, Rn, Rm, Ld.Option, Ld.Shift));
    Alias = (Insn & 0xFCC0001Fu) | (1u << 24) | (A << 5);
    break;
  case Load::Pair:
    AddOffset(Ld.Offset);
    Alias = (Insn & ~0x003F83E0u) | (A << 5);
    break;
  default: // Acquire, Single
    AddOffset(0);
    Alias = (Insn & ~0x3E0u) | (A << 5);
    break;
  }

  unsigned ToFallback[8], NF = 0;
  const uint64_t Lit = Stub;
  // The description is current: Seq == *GenPtr.
  Put(LdrLit(Bq, At(N), Lit + SeqOff));
  Put(DmbIshld);
  Put(LdrLit(C, At(N), Lit + GenPtrOff));
  L.GenLoad = 4 * N;
  Put(Ldr(C, C));
  Put(EorReg(C, C, Bq));
  ToFallback[NF++] = N;
  Put(0); // cbnz C
  // A in the host page.
  Put(LdrLit(C, At(N), Lit + PageOff));
  Put(SubReg(C, A, C));
  Put(LsrImm(D, C, 14));
  ToFallback[NF++] = N;
  Put(0); // cbnz D
  // The 4 KiB page has a file view.
  Put(LsrImm(C, C, 12));
  Put(Adr(D, At(N), Lit + DeltaOff));
  Put(LdrLsl3(D, D, C));
  ToFallback[NF++] = N;
  Put(0); // cbz D
  // The access stays in that 4 KiB page.
  Put(And4K(C, A));
  Put(AddImm(C, C, Bytes - 1));
  ToFallback[NF++] = N;
  Put(0); // tbnz C, #12
  // Nobody rewrote the description meanwhile.
  Put(DmbIshld);
  Put(LdrLit(C, At(N), Lit + SeqOff));
  Put(EorReg(C, C, Bq));
  ToFallback[NF++] = N;
  Put(0); // cbnz C
  Put(AddReg(A, A, D));
  L.AliasLoad = 4 * N;
  Put(Alias);
  Put(Ldp16(C, D));
  Put(LdpPost32(A, Bq));
  Put(B(At(N), Site + 4));

  const unsigned Fb = N;
  Put(Ldp16(C, D));
  Put(LdpPost32(A, Bq));
  L.Fallback = 4 * N;
  Put(Insn);
  Put(B(At(N), Site + 4));
  if (4 * N > GenPtrOff) return false;

  // Branches to the fallback, in the order they were emitted above.
  Out[ToFallback[0]] = Cbnz(C, At(ToFallback[0]), At(Fb));
  Out[ToFallback[1]] = Cbnz(D, At(ToFallback[1]), At(Fb));
  Out[ToFallback[2]] = Cbz(D, At(ToFallback[2]), At(Fb));
  Out[ToFallback[3]] = Tbnz(C, 12, At(ToFallback[3]), At(Fb));
  Out[ToFallback[4]] = Cbnz(C, At(ToFallback[4]), At(Fb));
  for (unsigned I = N; I < GenPtrOff / 4; I++) Out[I] = 0xD4200000u; // brk #0
  for (unsigned I = 0; I < 4; I++) L.Scratch[I] = S[I];
  return true;
}
} // namespace GuardedLoadStub

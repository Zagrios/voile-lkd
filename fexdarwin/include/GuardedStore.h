// SPDX-License-Identifier: MIT
// The stores FEX emits for guest stores, decoded, so that DarwinSignalDelegator can run one that faulted on a guarded
// host page through an alias of the page (GuardAlias) instead of opening the page: an open page lets every other thread
// read the 4 KiB pieces the guest forbids. The mirror of DecodeLoad (GuardedLoadStub.h).
#pragma once

#include <cstdint>

namespace GuardedStore {
struct Store {
  enum Kind { UImm, Unscaled, PreIndex, PostIndex, RegOff, Pair, PairPreIndex, PairPostIndex, Release, ReleaseUnscaled } Form;
  unsigned Size; // log2 of the bytes of one element
  bool Vec;
  unsigned Rt, Rt2, Rn, Rm;
  int64_t Offset;         // immediate byte offset (also the writeback of the pre- and post-indexed forms)
  unsigned Option, Shift; // RegOff: extend of Rm and its left shift
  bool IsPair() const { return Form == Pair || Form == PairPreIndex || Form == PairPostIndex; }
  bool IsRelease() const { return Form == Release || Form == ReleaseUnscaled; }
  unsigned Bytes() const { return (1u << Size) * (IsPair() ? 2 : 1); }
};

// STR/STRB/STRH/STUR, register offset, pre- and post-index, STP/STNP; integer and FP/SIMD registers; STLR/STLUR (the
// release stores FEX emits for x86's store order). False for anything else (stores of one vector lane, exclusive and
// atomic stores included) and for the unpredictable writebacks of a stored register.
inline bool DecodeStore(uint32_t I, Store& S) {
  const auto Imm9 = [&] { return static_cast<int64_t>(static_cast<int32_t>(I << 11) >> 23); };
  S = {};
  S.Rt = I & 31;
  S.Rn = (I >> 5) & 31;
  const unsigned SizeBits = I >> 30, Opc = (I >> 22) & 3;

  if ((I & 0x3B000000) == 0x39000000 || (I & 0x3B200400) == 0x38000400 || (I & 0x3B200C00) == 0x38000000 ||
      (I & 0x3B200C00) == 0x38200800) {
    S.Vec = (I >> 26) & 1;
    if (S.Vec) {
      if (Opc == 0) S.Size = SizeBits;
      else if (Opc == 2 && SizeBits == 0) S.Size = 4; // STR q
      else return false;                              // a load
    } else {
      if (Opc != 0) return false; // a load or a prefetch
      S.Size = SizeBits;
    }
    if ((I & 0x3B000000) == 0x39000000) {
      S.Form = Store::UImm;
      S.Offset = static_cast<int64_t>(((I >> 10) & 0xfff) << S.Size);
    } else if ((I & 0x3B200C00) == 0x38000000) {
      S.Form = Store::Unscaled;
      S.Offset = Imm9();
    } else if ((I & 0x3B200400) == 0x38000400) {
      if (!S.Vec && S.Rn == S.Rt && S.Rn != 31) return false;
      S.Form = (I >> 11) & 1 ? Store::PreIndex : Store::PostIndex;
      S.Offset = Imm9();
    } else {
      S.Form = Store::RegOff;
      S.Rm = (I >> 16) & 31;
      S.Option = (I >> 13) & 7;
      S.Shift = (I >> 12) & 1 ? S.Size : 0;
      if (S.Option != 2 && S.Option != 3 && S.Option != 6 && S.Option != 7) return false;
    }
    return true;
  }
  const uint32_t PairClass = I & 0x3BC00000;
  if (PairClass == 0x29000000 || PairClass == 0x28000000 || PairClass == 0x28800000 || PairClass == 0x29800000) {
    // STP (signed offset), STNP, STP (post- and pre-index)
    S.Vec = (I >> 26) & 1;
    S.Rt2 = (I >> 10) & 31;
    if (S.Vec) {
      if (SizeBits == 3) return false;
      S.Size = 2 + SizeBits;
    } else {
      if (SizeBits == 1 || SizeBits == 3) return false; // STGP, unallocated
      S.Size = SizeBits == 2 ? 3 : 2;
    }
    S.Offset = (static_cast<int32_t>(I << 10) >> 25) * (int64_t {1} << S.Size);
    S.Form = PairClass == 0x28800000 ? Store::PairPostIndex : PairClass == 0x29800000 ? Store::PairPreIndex : Store::Pair;
    if (S.Form != Store::Pair && !S.Vec && S.Rn != 31 && (S.Rn == S.Rt || S.Rn == S.Rt2)) return false;
    return true;
  }
  if ((I & 0x3FFFFC00) == 0x089FFC00) {
    S.Form = Store::Release; // STLR
    S.Size = SizeBits;
    return true;
  }
  if ((I & 0x3F200C00) == 0x19000000 && Opc == 0) {
    S.Form = Store::ReleaseUnscaled; // STLUR
    S.Size = SizeBits;
    S.Offset = Imm9();
    return true;
  }
  return false;
}
} // namespace GuardedStore

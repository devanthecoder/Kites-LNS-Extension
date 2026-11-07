/**
 * @file lns_format.h
 * @brief Single source of truth for the 32-bit LNS word: layout, constants, special values.
 *
 * Layout: [31] sign S | [30:0] signed (two's-complement) fixed-point log2|x|, Q9.22.
 *   value = (-1)^S * 2^(Lq / 2^22),  Lq in [-2^30, 2^30)  ->  L in [-256, 256)
 *
 * Reserved patterns:
 *   zero : log field == kZeroField (most negative); the sign bit is ignored when reading.
 *          Canonical zero is +0 = 0x40000000.
 *   Inf  : integer part of the log field == 255 (L in [255, 256)) and fraction == 0.
 *          +Inf = 0x3FC00000, -Inf = 0xBFC00000 (the sign bit selects).
 *   NaN  : integer part of the log field == 255 and fraction != 0; sign ignored.
 *          Canonical NaN = 0x3FC00001.
 *   Any other pattern is a finite nonzero value. Finite logs lie in
 *   [kLqFiniteMin, kLqFiniteMax]; an arithmetic result at or above kLqInfThreshold becomes Inf,
 *   one below kLqFiniteMin becomes zero.
 */
#ifndef LNS_FORMAT_H
#define LNS_FORMAT_H

#include <cstdint>

namespace Kites
{
namespace lns
{
using LnsWord = uint32_t;

// ---- Layout ---------------------------------------------------------------------------------
constexpr int kPayloadBits = 32;
constexpr int kSignBitIndex = 31;
constexpr int kFieldBits = 31;   ///< width of the log field
constexpr int kFracBits = 22;    ///< fractional bits of the log
constexpr int kIntegerFieldBits = kFieldBits - kFracBits; ///< 9

constexpr uint32_t kSignMask = 1u << kSignBitIndex;
constexpr uint32_t kFieldMask = (1u << kFieldBits) - 1u;
constexpr uint32_t kFieldSignMask = 1u << (kFieldBits - 1); ///< sign bit of the log field
constexpr uint32_t kFractionMask = (1u << kFracBits) - 1u;
constexpr uint32_t kIntegerFieldMask = (1u << kIntegerFieldBits) - 1u;

constexpr int64_t kFractionScale = int64_t{1} << kFracBits; ///< 2^22: one unit of integer log
constexpr double kScale = static_cast<double>(kFractionScale);

// ---- Special values -------------------------------------------------------------------------
/// Integer part (9-bit field) reserved for Inf / NaN.
constexpr uint32_t kSpecialIntegerField = 0x0FFu;
constexpr uint32_t kSpecialFieldBase = kSpecialIntegerField << kFracBits; ///< 0x3FC00000
constexpr uint32_t kZeroField = kFieldSignMask;                           ///< 0x40000000

constexpr LnsWord kPosZero = kZeroField;
constexpr LnsWord kPosInf = kSpecialFieldBase;
constexpr LnsWord kNegInf = kSignMask | kSpecialFieldBase;
constexpr LnsWord kNaN = kSpecialFieldBase | 1u;

// ---- Finite range, in Q22 log units -----------------------------------------------------------
constexpr int32_t kLqMin = -(1 << (kFieldBits - 1));                       ///< reserved: zero
constexpr int32_t kLqFiniteMin = kLqMin + 1;                               ///< smallest finite log
constexpr int32_t kLqInfThreshold = static_cast<int32_t>(kSpecialIntegerField) *
                                    static_cast<int32_t>(kFractionScale); ///< >= this is Inf
constexpr int32_t kLqFiniteMax = kLqInfThreshold - 1;                      ///< largest finite log

// ---- Helpers --------------------------------------------------------------------------------
/// Sign-extend the 31-bit log field of @p w to int32_t.
constexpr int32_t fieldOf(LnsWord w)
{
    uint32_t f = w & kFieldMask;
    return static_cast<int32_t>(f ^ kFieldSignMask) - static_cast<int32_t>(kFieldSignMask);
}

constexpr bool signOf(LnsWord w)
{
    return (w & kSignMask) != 0;
}

/// The 9-bit integer part of the log field (raw, not sign-extended).
constexpr uint32_t integerFieldOf(LnsWord w)
{
    return ((w & kFieldMask) >> kFracBits) & kIntegerFieldMask;
}

constexpr uint32_t fractionOf(LnsWord w)
{
    return w & kFractionMask;
}

constexpr LnsWord pack(bool sign, int32_t lq)
{
    return (sign ? kSignMask : 0u) | (static_cast<uint32_t>(lq) & kFieldMask);
}

constexpr bool isZero(LnsWord w)
{
    return (w & kFieldMask) == kZeroField;
}
constexpr bool isInf(LnsWord w)
{
    return integerFieldOf(w) == kSpecialIntegerField && fractionOf(w) == 0;
}
constexpr bool isNaN(LnsWord w)
{
    return integerFieldOf(w) == kSpecialIntegerField && fractionOf(w) != 0;
}
constexpr bool isFinite(LnsWord w)
{
    return !isZero(w) && integerFieldOf(w) != kSpecialIntegerField;
}

/// Map every non-canonical encoding of zero / NaN onto its canonical word (Inf keeps its sign).
constexpr LnsWord canonicalize(LnsWord w)
{
    if (isZero(w))
    {
        return kPosZero;
    }
    if (isNaN(w))
    {
        return kNaN;
    }
    if (isInf(w))
    {
        return signOf(w) ? kNegInf : kPosInf;
    }
    return w;
}
} // namespace lns
} // namespace Kites

#endif // LNS_FORMAT_H

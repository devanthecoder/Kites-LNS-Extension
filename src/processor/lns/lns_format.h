/**
 * @file lns_format.h
 * @brief Shared bit layout, constants and helpers for the 32-bit LNS format.
 *
 * Layout: [31] sign S | [30:0] signed (two's-complement) fixed-point log2|x|, Q9.22.
 *   value = (-1)^S * 2^(Lq / 2^22),  Lq in [-2^30, 2^30)  ->  L in [-256, 256)
 *
 * Reserved patterns (end points of the log field):
 *   field == kFieldMin, S=0 : +0 (the only zero)
 *   field == kFieldMin, S=1 : NaN (payload ignored)
 *   field == kFieldMax      : +/-Inf (sign selects)
 */
#ifndef LNS_FORMAT_H
#define LNS_FORMAT_H

#include <cstdint>

namespace Kites
{
namespace lns
{
using LnsWord = uint32_t;

constexpr int kFracBits = 22;
constexpr int kFieldBits = 31;
constexpr uint32_t kFieldMask = 0x7FFFFFFFu;
constexpr uint32_t kSignMask = 0x80000000u;

/// Signed field extremes (as signed values).
constexpr int32_t kLqMin = -(1 << 30);     ///< reserved: zero / NaN
constexpr int32_t kLqMax = (1 << 30) - 1;  ///< reserved: infinity
/// Largest / smallest log values usable by finite numbers.
constexpr int32_t kLqFiniteMax = kLqMax - 1;
constexpr int32_t kLqFiniteMin = kLqMin + 1;

constexpr LnsWord kPosZero = 0x40000000u;
constexpr LnsWord kNaN = 0xC0000000u;
constexpr LnsWord kPosInf = 0x3FFFFFFFu;
constexpr LnsWord kNegInf = 0xBFFFFFFFu;

constexpr double kScale = static_cast<double>(1u << kFracBits);

/// Sign-extend the 31-bit log field of @p w to int32_t.
constexpr int32_t fieldOf(LnsWord w)
{
    uint32_t f = w & kFieldMask;
    return static_cast<int32_t>((f ^ 0x40000000u)) - static_cast<int32_t>(0x40000000u);
}

constexpr bool signOf(LnsWord w)
{
    return (w & kSignMask) != 0;
}

constexpr LnsWord pack(bool sign, int32_t lq)
{
    return (sign ? kSignMask : 0u) | (static_cast<uint32_t>(lq) & kFieldMask);
}

constexpr bool isNaN(LnsWord w)
{
    return w == kNaN;
}
constexpr bool isZero(LnsWord w)
{
    return w == kPosZero;
}
constexpr bool isInf(LnsWord w)
{
    return fieldOf(w) == kLqMax;
}
constexpr bool isFinite(LnsWord w)
{
    return !isNaN(w) && !isZero(w) && !isInf(w);
}

/// Any word with the zero/NaN field and the sign set is NaN; normalise so
/// isNaN()/isZero() are exact comparisons.
constexpr LnsWord canonicalize(LnsWord w)
{
    if (fieldOf(w) == kLqMin)
    {
        return signOf(w) ? kNaN : kPosZero;
    }
    return w;
}
} // namespace lns
} // namespace Kites

#endif // LNS_FORMAT_H

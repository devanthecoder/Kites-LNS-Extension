/**
 * @file lns_pow.cpp
 * @brief Exponentiation on 32-bit LNS words. See lns_pow.h.
 */

#include "lns_pow.h"

namespace Kites
{
namespace lns
{
namespace
{
constexpr LnsWord kOne = 0x00000000u; ///< +1.0: sign 0, log 0

/// Packs a result with log @p lq (any width) and the given sign, saturating like LnsAlu.
LnsWord encodeLog(__int128 lq, bool negative)
{
    if (lq >= kLqInfThreshold)
    {
        return negative ? kNegInf : kPosInf;
    }
    if (lq < kLqFiniteMin)
    {
        return kPosZero;
    }
    return pack(negative, static_cast<int32_t>(lq));
}

/// value / 2^shift rounded to nearest, ties away from zero (as LnsAlu rounds).
__int128 roundShiftRight(__int128 value, int shift)
{
    if (shift >= 120)
    {
        return 0;
    }
    const __int128 half = static_cast<__int128>(1) << (shift - 1);
    return value < 0 ? -((-value + half) >> shift) : (value + half) >> shift;
}
} // namespace

LnsWord powerN(LnsWord x, int64_t n)
{
    if (n == 0)
    {
        return kOne;
    }
    if (isNaN(x))
    {
        return kNaN;
    }
    const bool negative = signOf(x) && (n & 1) != 0;
    if (isZero(x))
    {
        return n > 0 ? kPosZero : kPosInf;
    }
    if (isInf(x))
    {
        return n > 0 ? (negative ? kNegInf : kPosInf) : kPosZero;
    }
    return encodeLog(static_cast<__int128>(fieldOf(x)) * n, negative);
}

LnsWord power(LnsWord x, LnsWord y, const LnsConverter &converter)
{
    if (isZero(y) || x == kOne)
    {
        return kOne;
    }
    if (isNaN(x) || isNaN(y))
    {
        return kNaN;
    }
    if (signOf(x) && !isZero(x))
    { // negative base: only y = 1 is defined here; integer powers belong to powerN
        return y == kOne ? canonicalize(x) : kNaN;
    }

    const bool yNegative = signOf(y);
    if (isInf(y))
    { // |x| > 1 grows without bound for y = +Inf, |x| < 1 vanishes (x = 1 handled above)
        const bool xAboveOne = isInf(x) || (!isZero(x) && fieldOf(x) > 0);
        return xAboveOne != yNegative ? kPosInf : kPosZero;
    }
    if (isZero(x))
    {
        return yNegative ? kPosInf : kPosZero;
    }
    if (isInf(x))
    {
        return yNegative ? kPosZero : kPosInf;
    }

    // Both finite, x > 0. |y| = 2^(Ly) with Ly = I + f, f in [0, 1): |y| = 2^f * 2^I, and 2^f
    // comes from the converter's table as m in Q31 (2^31 <= m < 2^32).
    const int32_t ly = fieldOf(y);
    const int32_t integerPart = ly >> kFracBits; // floor
    const uint32_t frac = static_cast<uint32_t>(ly) & kFractionMask;
    const uint64_t m = converter.exp2Frac(frac);

    // log2(x^|y|) in Q22 = Lx * m * 2^(I - 31); |Lx| < 2^30 and m < 2^32, so Lx * m fits.
    const __int128 product = static_cast<__int128>(fieldOf(x)) * static_cast<__int128>(m);
    const int shift = integerPart - 31;
    __int128 lq = 0;
    if (shift >= 0)
    { // |product| >= 2^31 for Lx != 0, so a shift of 32 or more is far outside the range
        lq = shift >= 32 ? (product > 0 ? kLqInfThreshold : kLqMin) * __int128{2}
                         : product * (static_cast<__int128>(1) << shift);
    }
    else
    {
        lq = roundShiftRight(product, -shift);
    }
    if (yNegative)
    {
        lq = -lq;
    }
    return encodeLog(lq, false);
}

LnsWord power(LnsWord x, LnsWord y)
{
    return power(x, y, defaultConverter());
}
} // namespace lns
} // namespace Kites

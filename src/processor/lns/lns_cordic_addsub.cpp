/**
 * @file lns_cordic_addsub.cpp
 * @brief Hyperbolic CORDIC based LNS addition and subtraction.
 *
 * For log magnitudes H >= L, the correction is log2(1 +/- 2^-(H-L)). CORDIC first
 * evaluates the exponential term and then obtains the logarithm through the identity
 * ln(m) = 2 atanh((m - 1) / (m + 1)). Internal values use Q52 fixed point; the
 * result is rounded back to the format's Q22 logarithm field.
 */
#include "lns_cordic_addsub.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Kites
{
namespace lns
{
namespace
{
constexpr int kCordicFractionBits = 52;
constexpr int64_t kCordicScale = int64_t{1} << kCordicFractionBits;
constexpr int kLastIteration = 48;

enum class ValueClass
{
    FINITE,
    ZERO,
    INF,
    NAN_VALUE,
};

struct DecodedValue
{
    ValueClass classification;
    int64_t logMagnitude;
    bool negative;
};

struct CordicConstants
{
    std::array<int64_t, kLastIteration + 1> angles{};
    int64_t inverseRotationGain = kCordicScale;
    int64_t lnTwo = 0;
};

bool isRepeatedIteration(int iteration)
{
    return iteration == 4 || iteration == 13 || iteration == 40;
}

CordicConstants makeCordicConstants()
{
    CordicConstants constants;
    constants.lnTwo = static_cast<int64_t>(std::llround(std::log(2.0L) * kCordicScale));

    long double rotationGain = 1.0L;
    for (int iteration = 1; iteration <= kLastIteration; ++iteration)
    {
        const long double power = std::ldexp(1.0L, -iteration);
        constants.angles[iteration] =
            static_cast<int64_t>(std::llround(std::atanh(power) * kCordicScale));
        rotationGain *= std::sqrt(1.0L - power * power);
        if (isRepeatedIteration(iteration))
        {
            rotationGain *= std::sqrt(1.0L - power * power);
        }
    }
    constants.inverseRotationGain =
        static_cast<int64_t>(std::llround(kCordicScale / rotationGain));
    return constants;
}

const CordicConstants &cordicConstants()
{
    static const CordicConstants constants = makeCordicConstants();
    return constants;
}

DecodedValue decodeWord(LnsWord word)
{
    if (isZero(word))
    {
        return {ValueClass::ZERO, 0, false};
    }
    if (isInf(word))
    {
        return {ValueClass::INF, 0, signOf(word)};
    }
    if (isNaN(word))
    {
        return {ValueClass::NAN_VALUE, 0, false};
    }
    return {ValueClass::FINITE, fieldOf(word), signOf(word)};
}

LnsWord encodeInfinity(bool negative)
{
    return negative ? kNegInf : kPosInf;
}

LnsWord encodeFinite(int64_t logMagnitude, bool negative)
{
    if (logMagnitude < kLqFiniteMin)
    {
        return kPosZero;
    }
    if (logMagnitude >= kLqInfThreshold)
    {
        return encodeInfinity(negative);
    }
    return pack(negative, static_cast<int32_t>(logMagnitude));
}

int64_t roundDivideNearest(int64_t numerator, int64_t denominator)
{
    if (numerator < 0)
    {
        return -((-numerator + denominator / 2) / denominator);
    }
    return (numerator + denominator / 2) / denominator;
}

int64_t roundShiftNearest(int64_t value, int shift)
{
    const int64_t divisor = int64_t{1} << shift;
    return roundDivideNearest(value, divisor);
}

/// Q52 approximation of exp(-fraction * ln(2)), where fraction is Q22 in [0, 1).
int64_t expNegativeFraction(int64_t fractionQ22)
{
    const CordicConstants &constants = cordicConstants();
    const int64_t lnTwoHigh = constants.lnTwo / kFractionScale;
    const int64_t lnTwoLow = constants.lnTwo % kFractionScale;
    const int64_t angle = -(fractionQ22 * lnTwoHigh +
                            roundDivideNearest(fractionQ22 * lnTwoLow, kFractionScale));

    int64_t x = constants.inverseRotationGain;
    int64_t y = 0;
    int64_t residual = angle;
    for (int iteration = 1; iteration <= kLastIteration; ++iteration)
    {
        const int repeats = isRepeatedIteration(iteration) ? 2 : 1;
        for (int repeat = 0; repeat < repeats; ++repeat)
        {
            const int64_t xShift = x >> iteration;
            const int64_t yShift = y >> iteration;
            const int64_t direction = residual >= 0 ? 1 : -1;
            const int64_t nextX = x + direction * yShift;
            y += direction * xShift;
            x = nextX;
            residual -= direction * constants.angles[iteration];
        }
    }

    // x + y is exp(angle); x/y are both bounded by one in magnitude for this range.
    return x + y;
}

/// Computes log2(1 +/- 2^-difference) in Q22. difference is nonnegative Q22.
int64_t cordicCorrection(int64_t difference, bool addition)
{
    if (difference == 0)
    {
        return addition ? kFractionScale : 0; // subtraction's exact cancellation is handled above
    }
    if (difference >= 24LL * kFractionScale)
    {
        return 0;
    }

    const int64_t whole = difference / kFractionScale;
    const int64_t fraction = difference % kFractionScale;
    const int64_t exponential = expNegativeFraction(fraction) >> whole;
    const int64_t argument = addition ? kCordicScale + exponential
                                      : kCordicScale - exponential;

    // Normalize the positive Q52 argument into [1, 2). Subtraction can produce a
    // very small argument when the two magnitudes are close, so normalization may
    // require several left shifts rather than just one.
    int normalizationExponent;
    int64_t normalized;
    if (argument >= kCordicScale)
    {
        normalizationExponent = 0;
        normalized = argument;
    }
    else
    {
        normalizationExponent = 0;
        normalized = argument;
        while (normalized < kCordicScale)
        {
            normalized <<= 1;
            --normalizationExponent;
        }
    }

    // Vectoring CORDIC: z tends to atanh((m - 1) / (m + 1)) for m=normalized.
    int64_t x = normalized + kCordicScale;
    int64_t y = normalized - kCordicScale;
    int64_t z = 0;
    const CordicConstants &constants = cordicConstants();
    for (int iteration = 1; iteration <= kLastIteration; ++iteration)
    {
        const int repeats = isRepeatedIteration(iteration) ? 2 : 1;
        for (int repeat = 0; repeat < repeats; ++repeat)
        {
            const int64_t direction = y >= 0 ? -1 : 1;
            const int64_t xShift = x >> iteration;
            const int64_t yShift = y >> iteration;
            const int64_t nextX = x + direction * yShift;
            y += direction * xShift;
            x = nextX;
            z -= direction * constants.angles[iteration];
        }
    }

    // Convert 2*z from Q52 natural-log units to Q22 log2 units. Reducing z to Q30
    // first keeps the signed 64-bit product within range.
    constexpr int64_t kInvLnTwoQ30 = 1549082005LL;
    const int64_t zQ30 = roundShiftNearest(z, kCordicFractionBits - 30);
    const int64_t twiceZTimesInvLnTwo = (2 * zQ30) * kInvLnTwoQ30;
    const int64_t fractionalLogQ22 = roundShiftNearest(twiceZTimesInvLnTwo, 60 - kFracBits);
    return static_cast<int64_t>(normalizationExponent) * kFractionScale + fractionalLogQ22;
}

LnsWord addFinite(const DecodedValue &a, const DecodedValue &b, bool bNegative)
{
    if (a.classification == ValueClass::ZERO && b.classification == ValueClass::ZERO)
    {
        return kPosZero;
    }
    if (a.classification == ValueClass::ZERO)
    {
        return encodeFinite(b.logMagnitude, bNegative);
    }
    if (b.classification == ValueClass::ZERO)
    {
        return encodeFinite(a.logMagnitude, a.negative);
    }

    const bool sameSign = a.negative == bNegative;
    const int64_t high = std::max(a.logMagnitude, b.logMagnitude);
    const int64_t low = std::min(a.logMagnitude, b.logMagnitude);
    const int64_t difference = high - low;
    if (sameSign)
    {
        return encodeFinite(high + cordicCorrection(difference, true), a.negative);
    }
    if (difference == 0)
    {
        return kPosZero;
    }

    const bool resultNegative = a.logMagnitude > b.logMagnitude ? a.negative : bNegative;
    return encodeFinite(high + cordicCorrection(difference, false), resultNegative);
}

LnsWord addImpl(LnsWord aWord, LnsWord bWord, bool subtractB)
{
    const DecodedValue a = decodeWord(aWord);
    const DecodedValue b = decodeWord(bWord);
    const bool bNegative = b.negative != subtractB;

    if (a.classification == ValueClass::NAN_VALUE || b.classification == ValueClass::NAN_VALUE)
    {
        return kNaN;
    }
    if (a.classification == ValueClass::INF && b.classification == ValueClass::INF)
    {
        return a.negative == bNegative ? encodeInfinity(a.negative) : kNaN;
    }
    if (a.classification == ValueClass::INF)
    {
        return encodeInfinity(a.negative);
    }
    if (b.classification == ValueClass::INF)
    {
        return encodeInfinity(bNegative);
    }
    return addFinite(a, b, bNegative);
}
} // namespace

LnsWord LnsCordicAddSub::add(LnsWord a, LnsWord b)
{
    return addImpl(a, b, false);
}

LnsWord LnsCordicAddSub::sub(LnsWord a, LnsWord b)
{
    return addImpl(a, b, true);
}
} // namespace lns
} // namespace Kites

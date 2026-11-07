/**
 * @file lns_alu.cpp
 * @brief Arithmetic on 32-bit LNS words. See lns_alu.h.
 */

#include "lns_alu.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Kites
{
namespace lns
{
namespace
{
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

DecodedValue decodeWord(LnsWord w)
{
    if (isZero(w))
    {
        return {ValueClass::ZERO, 0, false};
    }
    if (isInf(w))
    {
        return {ValueClass::INF, 0, signOf(w)};
    }
    if (isNaN(w))
    {
        return {ValueClass::NAN_VALUE, 0, false};
    }
    return {ValueClass::FINITE, fieldOf(w), signOf(w)};
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

LnsWord addFinite(const LnsAlu &alu, const DecodedValue &a, const DecodedValue &b, bool bNegative)
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
        const int64_t correction =
            difference == 0 ? kFractionScale : alu.correction(difference, true);
        return encodeFinite(high + correction, a.negative);
    }

    if (difference == 0)
    {
        return kPosZero; // exact cancellation
    }

    const bool resultNegative = a.logMagnitude > b.logMagnitude ? a.negative : bNegative;
    return encodeFinite(high + alu.correction(difference, false), resultNegative);
}

LnsWord addImpl(const LnsAlu &alu, LnsWord aWord, LnsWord bWord, bool subtractB)
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
    return addFinite(alu, a, b, bNegative);
}
} // namespace

LnsAlu::LnsAlu(size_t correctionSamples) : m_requestedSamples(correctionSamples)
{
    if (correctionSamples < kMinCorrectionSamples || correctionSamples > kMaxCorrectionSamples)
    {
        throw std::invalid_argument("LnsAlu: correctionSamples out of range");
    }
    m_table.reserve(correctionSamples + 1);

    const long double minimumDifference = 1.0L;
    const long double maximumDifference = static_cast<long double>(kCorrectionCutoff);
    const long double logMinimum = std::log(minimumDifference);
    const long double logSpan = std::log(maximumDifference) - logMinimum;
    const long double lnTwo = std::log(2.0L);

    for (size_t index = 0; index < correctionSamples; ++index)
    {
        const long double position =
            static_cast<long double>(index) / static_cast<long double>(correctionSamples - 1);
        const long double rawDifference = std::exp(logMinimum + position * logSpan);
        const int64_t difference = static_cast<int64_t>(std::llround(rawDifference));

        if (!m_table.empty() && difference <= m_table.back().difference)
        {
            continue;
        }

        const long double d =
            static_cast<long double>(difference) / static_cast<long double>(kFractionScale);
        const long double addCorrection = std::log2(1.0L + std::exp2(-d));
        const long double subtractTerm = -std::expm1(-d * lnTwo);
        const long double subtractCorrection = std::log2(subtractTerm);

        m_table.push_back({difference,
                           static_cast<int64_t>(std::llround(addCorrection * kFractionScale)),
                           static_cast<int64_t>(std::llround(subtractCorrection * kFractionScale))});
    }

    if (m_table.empty() || m_table.back().difference != kCorrectionCutoff)
    {
        m_table.push_back({kCorrectionCutoff, 0, 0});
    }
}

size_t LnsAlu::correctionBytes() const
{
    return m_table.size() * sizeof(CorrectionEntry);
}

int64_t LnsAlu::correction(int64_t difference, bool addition) const
{
    if (difference >= kCorrectionCutoff)
    {
        return 0;
    }

    const auto upper = std::lower_bound(m_table.begin(), m_table.end(), difference,
                                        [](const CorrectionEntry &entry, int64_t value) {
                                            return entry.difference < value;
                                        });

    if (upper == m_table.begin())
    {
        return addition ? upper->addition : upper->subtraction;
    }
    if (upper == m_table.end())
    {
        return 0;
    }
    if (upper->difference == difference)
    {
        return addition ? upper->addition : upper->subtraction;
    }

    const CorrectionEntry &right = *upper;
    const CorrectionEntry &left = *(upper - 1);
    const int64_t leftValue = addition ? left.addition : left.subtraction;
    const int64_t rightValue = addition ? right.addition : right.subtraction;
    const int64_t span = right.difference - left.difference;
    const int64_t offset = difference - left.difference;
    const int64_t scaledDelta = (rightValue - leftValue) * offset;

    return leftValue + roundDivideNearest(scaledDelta, span);
}

LnsWord LnsAlu::execute(LnsAluOp op, LnsWord a, LnsWord b) const
{
    switch (op)
    {
    case LnsAluOp::ADD:
        return add(a, b);
    case LnsAluOp::SUB:
        return sub(a, b);
    case LnsAluOp::MUL:
        return mul(a, b);
    case LnsAluOp::DIV:
        return div(a, b);
    }
    return kNaN;
}

LnsWord LnsAlu::add(LnsWord a, LnsWord b) const
{
    return addImpl(*this, a, b, false);
}

LnsWord LnsAlu::sub(LnsWord a, LnsWord b) const
{
    return addImpl(*this, a, b, true);
}

LnsWord LnsAlu::mul(LnsWord a, LnsWord b)
{
    const DecodedValue left = decodeWord(a);
    const DecodedValue right = decodeWord(b);
    const bool resultNegative = left.negative != right.negative;

    if (left.classification == ValueClass::NAN_VALUE ||
        right.classification == ValueClass::NAN_VALUE)
    {
        return kNaN;
    }
    if ((left.classification == ValueClass::ZERO && right.classification == ValueClass::INF) ||
        (left.classification == ValueClass::INF && right.classification == ValueClass::ZERO))
    {
        return kNaN;
    }
    if (left.classification == ValueClass::INF || right.classification == ValueClass::INF)
    {
        return encodeInfinity(resultNegative);
    }
    if (left.classification == ValueClass::ZERO || right.classification == ValueClass::ZERO)
    {
        return kPosZero;
    }
    return encodeFinite(left.logMagnitude + right.logMagnitude, resultNegative);
}

LnsWord LnsAlu::div(LnsWord a, LnsWord b)
{
    const DecodedValue numerator = decodeWord(a);
    const DecodedValue denominator = decodeWord(b);
    const bool resultNegative = numerator.negative != denominator.negative;

    if (numerator.classification == ValueClass::NAN_VALUE ||
        denominator.classification == ValueClass::NAN_VALUE)
    {
        return kNaN;
    }
    if ((numerator.classification == ValueClass::ZERO &&
         denominator.classification == ValueClass::ZERO) ||
        (numerator.classification == ValueClass::INF &&
         denominator.classification == ValueClass::INF))
    {
        return kNaN;
    }
    if (numerator.classification == ValueClass::INF ||
        denominator.classification == ValueClass::ZERO)
    {
        return encodeInfinity(resultNegative);
    }
    if (numerator.classification == ValueClass::ZERO ||
        denominator.classification == ValueClass::INF)
    {
        return kPosZero;
    }
    return encodeFinite(numerator.logMagnitude - denominator.logMagnitude, resultNegative);
}

const LnsAlu &defaultAlu()
{
    static const LnsAlu alu(LNS_ALU_CORRECTION_SAMPLES);
    return alu;
}
} // namespace lns
} // namespace Kites

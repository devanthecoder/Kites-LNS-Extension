/**
 * @file lns_alu.cpp
 * @brief Arithmetic operations for the signed logarithmic number representation.
 */

#include "processor/lns_alu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Kites
{
namespace alu
{
namespace
{
constexpr int64_t kFractionScale = LnsAlu::kFractionScale;
constexpr int64_t kSignedFieldModulus = 1LL << LnsAlu::kLogFieldBits;
constexpr int64_t kMinimumFiniteLog = LnsAlu::kMinimumFiniteLog;
constexpr int64_t kInfinityLogThreshold = LnsAlu::kInfinityLogThreshold;
constexpr int64_t kCorrectionCutoff = LnsAlu::kCorrectionCutoff;
constexpr size_t kCorrectionSampleCount = LnsAlu::kCorrectionSampleCount;

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
    int64_t log_magnitude;
    bool negative;
};

struct CorrectionEntry
{
    int64_t difference;
    int64_t addition;
    int64_t subtraction;
};

int64_t decode_log_field(uint32_t payload)
{
    const uint32_t field = payload & LnsAlu::kLogFieldMask;
    if ((field & LnsAlu::kLogFieldSignMask) != 0)
    {
        return static_cast<int64_t>(field) - kSignedFieldModulus;
    }
    return field;
}

DecodedValue decode(uint32_t payload)
{
    const uint32_t field = payload & LnsAlu::kLogFieldMask;
    const bool negative = (payload & LnsAlu::kSignMask) != 0;

    if (field == LnsAlu::kZeroEncoding)
    {
        return {ValueClass::ZERO, 0, false};
    }

    const uint32_t integer_field = (field >> LnsAlu::kFractionBits) & LnsAlu::kIntegerFieldMask;
    if (integer_field == LnsAlu::kSpecialIntegerField)
    {
        const uint32_t fraction = field & LnsAlu::kFractionMask;
        if (fraction == 0)
        {
            return {ValueClass::INF, 0, negative};
        }
        return {ValueClass::NAN_VALUE, 0, false};
    }

    return {ValueClass::FINITE, decode_log_field(payload), negative};
}

uint32_t encode_infinity(bool negative)
{
    return LnsAlu::kPositiveInfinityEncoding | (negative ? LnsAlu::kSignMask : 0U);
}

uint32_t encode_nan()
{
    return LnsAlu::kNaNEncoding;
}

uint32_t encode_zero()
{
    return LnsAlu::kZeroEncoding;
}

uint32_t encode_finite(int64_t log_magnitude, bool negative)
{
    if (log_magnitude < kMinimumFiniteLog)
    {
        return encode_zero();
    }
    if (log_magnitude >= kInfinityLogThreshold)
    {
        return encode_infinity(negative);
    }

    const uint32_t field = static_cast<uint32_t>(log_magnitude) & LnsAlu::kLogFieldMask;
    return field | (negative ? LnsAlu::kSignMask : 0U);
}

int64_t round_divide_nearest(int64_t numerator, int64_t denominator)
{
    if (numerator < 0)
    {
        return -((-numerator + denominator / 2) / denominator);
    }
    return (numerator + denominator / 2) / denominator;
}

std::vector<CorrectionEntry> make_correction_table()
{
    std::vector<CorrectionEntry> entries;
    entries.reserve(kCorrectionSampleCount);

    const long double minimum_difference = 1.0L;
    const long double maximum_difference = static_cast<long double>(kCorrectionCutoff);
    const long double log_minimum = std::log(minimum_difference);
    const long double log_span = std::log(maximum_difference) - log_minimum;
    const long double ln_two = std::log(2.0L);

    for (size_t index = 0; index < kCorrectionSampleCount; ++index)
    {
        const long double position = static_cast<long double>(index) /
                                     static_cast<long double>(kCorrectionSampleCount - 1);
        const long double raw_difference = std::exp(log_minimum + position * log_span);
        const int64_t difference = static_cast<int64_t>(std::llround(raw_difference));

        if (!entries.empty() && difference <= entries.back().difference)
        {
            continue;
        }

        const long double d = static_cast<long double>(difference) /
                              static_cast<long double>(kFractionScale);
        const long double add_correction = std::log2(1.0L + std::exp2(-d));
        const long double subtract_term = -std::expm1(-d * ln_two);
        const long double subtract_correction = std::log2(subtract_term);

        entries.push_back({difference,
                           static_cast<int64_t>(std::llround(add_correction * kFractionScale)),
                           static_cast<int64_t>(std::llround(subtract_correction * kFractionScale))});
    }

    if (entries.empty() || entries.back().difference != kCorrectionCutoff)
    {
        entries.push_back({kCorrectionCutoff, 0, 0});
    }

    return entries;
}

const std::vector<CorrectionEntry> &correction_table()
{
    static const std::vector<CorrectionEntry> table = make_correction_table();
    return table;
}

int64_t interpolate_correction(int64_t difference, bool addition)
{
    if (difference >= kCorrectionCutoff)
    {
        return 0;
    }

    const auto &table = correction_table();
    const auto upper = std::lower_bound(table.begin(), table.end(), difference,
                                        [](const CorrectionEntry &entry, int64_t value) {
                                            return entry.difference < value;
                                        });

    if (upper == table.begin())
    {
        return addition ? upper->addition : upper->subtraction;
    }
    if (upper == table.end())
    {
        return 0;
    }
    if (upper->difference == difference)
    {
        return addition ? upper->addition : upper->subtraction;
    }

    const CorrectionEntry &right = *upper;
    const CorrectionEntry &left = *(upper - 1);
    const int64_t left_value = addition ? left.addition : left.subtraction;
    const int64_t right_value = addition ? right.addition : right.subtraction;
    const int64_t span = right.difference - left.difference;
    const int64_t offset = difference - left.difference;
    const int64_t scaled_delta = (right_value - left_value) * offset;

    return left_value + round_divide_nearest(scaled_delta, span);
}

uint32_t add_finite_values(const DecodedValue &a, const DecodedValue &b, bool b_negative)
{
    if (a.classification == ValueClass::ZERO && b.classification == ValueClass::ZERO)
    {
        return encode_zero();
    }
    if (a.classification == ValueClass::ZERO)
    {
        return encode_finite(b.log_magnitude, b_negative);
    }
    if (b.classification == ValueClass::ZERO)
    {
        return encode_finite(a.log_magnitude, a.negative);
    }

    const bool same_sign = a.negative == b_negative;
    const int64_t high = std::max(a.log_magnitude, b.log_magnitude);
    const int64_t low = std::min(a.log_magnitude, b.log_magnitude);
    const int64_t difference = high - low;

    if (same_sign)
    {
        const int64_t correction = difference == 0
                                       ? kFractionScale
                                       : interpolate_correction(difference, true);
        return encode_finite(high + correction, a.negative);
    }

    if (difference == 0)
    {
        return encode_zero();
    }

    const bool result_negative = a.log_magnitude > b.log_magnitude ? a.negative : b_negative;
    const int64_t correction = interpolate_correction(difference, false);
    return encode_finite(high + correction, result_negative);
}

uint32_t add_impl(uint32_t a_payload, uint32_t b_payload, bool subtract_b)
{
    const DecodedValue a = decode(a_payload);
    const DecodedValue b = decode(b_payload);
    const bool b_negative = b.negative != subtract_b;

    if (a.classification == ValueClass::NAN_VALUE || b.classification == ValueClass::NAN_VALUE)
    {
        return encode_nan();
    }

    if (a.classification == ValueClass::INF && b.classification == ValueClass::INF)
    {
        return a.negative == b_negative ? encode_infinity(a.negative) : encode_nan();
    }
    if (a.classification == ValueClass::INF)
    {
        return encode_infinity(a.negative);
    }
    if (b.classification == ValueClass::INF)
    {
        return encode_infinity(b_negative);
    }

    return add_finite_values(a, b, b_negative);
}

} // namespace

uint32_t LnsAlu::execute(LnsAluOp op, uint64_t a, uint64_t b)
{
    const uint32_t a_payload = static_cast<uint32_t>(a);
    const uint32_t b_payload = static_cast<uint32_t>(b);

    switch (op)
    {
    case LnsAluOp::ADD:
        return add(a_payload, b_payload);
    case LnsAluOp::SUB:
        return sub(a_payload, b_payload);
    case LnsAluOp::MUL:
        return mul(a_payload, b_payload);
    case LnsAluOp::DIV:
        return div(a_payload, b_payload);
    }

    return encode_nan();
}

uint32_t LnsAlu::add(uint32_t a, uint32_t b)
{
    return add_impl(a, b, false);
}

uint32_t LnsAlu::sub(uint32_t a, uint32_t b)
{
    return add_impl(a, b, true);
}

uint32_t LnsAlu::mul(uint32_t a, uint32_t b)
{
    const DecodedValue left = decode(a);
    const DecodedValue right = decode(b);
    const bool result_negative = left.negative != right.negative;

    if (left.classification == ValueClass::NAN_VALUE ||
        right.classification == ValueClass::NAN_VALUE)
    {
        return encode_nan();
    }
    if ((left.classification == ValueClass::ZERO && right.classification == ValueClass::INF) ||
        (left.classification == ValueClass::INF && right.classification == ValueClass::ZERO))
    {
        return encode_nan();
    }
    if (left.classification == ValueClass::INF || right.classification == ValueClass::INF)
    {
        return encode_infinity(result_negative);
    }
    if (left.classification == ValueClass::ZERO || right.classification == ValueClass::ZERO)
    {
        return encode_zero();
    }

    return encode_finite(left.log_magnitude + right.log_magnitude, result_negative);
}

uint32_t LnsAlu::div(uint32_t a, uint32_t b)
{
    const DecodedValue numerator = decode(a);
    const DecodedValue denominator = decode(b);
    const bool result_negative = numerator.negative != denominator.negative;

    if (numerator.classification == ValueClass::NAN_VALUE ||
        denominator.classification == ValueClass::NAN_VALUE)
    {
        return encode_nan();
    }
    if ((numerator.classification == ValueClass::ZERO &&
         denominator.classification == ValueClass::ZERO) ||
        (numerator.classification == ValueClass::INF &&
         denominator.classification == ValueClass::INF))
    {
        return encode_nan();
    }
    if (numerator.classification == ValueClass::INF ||
        denominator.classification == ValueClass::ZERO)
    {
        return encode_infinity(result_negative);
    }
    if (numerator.classification == ValueClass::ZERO ||
        denominator.classification == ValueClass::INF)
    {
        return encode_zero();
    }

    return encode_finite(numerator.log_magnitude - denominator.log_magnitude, result_negative);
}
} // namespace alu
} // namespace Kites

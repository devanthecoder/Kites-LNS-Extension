#include "lns_reference.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Kites
{
namespace lns
{
namespace reference
{
namespace
{
/// Encode a finite, nonzero magnitude (as double) with the given sign.
LnsWord encodeMagnitude(bool sign, double magnitude)
{
    // log2 of a double: absolute error ~1e-13 for the ranges here, far below
    // the 2^-23 rounding step, so quantisation is effectively exact.
    double scaled = std::log2(magnitude) * kScale;
    if (scaled >= static_cast<double>(kLqFiniteMax) + 0.5)
    {
        return sign ? kNegInf : kPosInf;
    }
    if (scaled <= static_cast<double>(kLqFiniteMin) - 0.5)
    {
        return kPosZero;
    }
    return pack(sign, static_cast<int32_t>(std::llround(scaled)));
}

LnsWord encodeDouble(double v)
{
    if (std::isnan(v))
    {
        return kNaN;
    }
    bool sign = std::signbit(v);
    if (std::isinf(v))
    {
        return sign ? kNegInf : kPosInf;
    }
    if (v == 0.0)
    {
        return kPosZero;
    }
    return encodeMagnitude(sign, std::fabs(v));
}

/// Decode a finite LNS word to a double (exact up to exp2 rounding).
double decodeFinite(LnsWord w)
{
    double mag = std::exp2(static_cast<double>(fieldOf(w)) / kScale);
    return signOf(w) ? -mag : mag;
}

double decodeToDouble(LnsWord w)
{
    w = canonicalize(w);
    if (isNaN(w))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (isZero(w))
    {
        return 0.0;
    }
    if (isInf(w))
    {
        return signOf(w) ? -std::numeric_limits<double>::infinity()
                         : std::numeric_limits<double>::infinity();
    }
    return decodeFinite(w);
}

template <typename Int> Int saturatingToInt(LnsWord w)
{
    constexpr Int maxV = std::numeric_limits<Int>::max();
    constexpr Int minV = std::numeric_limits<Int>::min();
    double d = decodeToDouble(w);
    if (std::isnan(d))
    {
        return maxV;
    }
    d = std::nearbyint(d);
    // maxV is not exactly representable for 32/64-bit; compare against 2^bits.
    constexpr double upper = std::ldexp(1.0, std::numeric_limits<Int>::digits);
    if (d >= upper)
    {
        return maxV;
    }
    if (d <= static_cast<double>(minV))
    {
        return minV;
    }
    return static_cast<Int>(d);
}
} // namespace

LnsWord fromFloatBits(uint32_t bits)
{
    // float -> double is exact, including subnormals, so no special-casing.
    return encodeDouble(static_cast<double>(std::bit_cast<float>(bits)));
}

LnsWord fromDoubleBits(uint64_t bits)
{
    return encodeDouble(std::bit_cast<double>(bits));
}

LnsWord fromInt32(int32_t v)
{
    return encodeDouble(static_cast<double>(v));
}

LnsWord fromUInt32(uint32_t v)
{
    return encodeDouble(static_cast<double>(v));
}

LnsWord fromInt64(int64_t v)
{
    // Rounding to double loses < 2^-53 relative; irrelevant at 2^-23 resolution.
    return encodeDouble(static_cast<double>(v));
}

LnsWord fromUInt64(uint64_t v)
{
    return encodeDouble(static_cast<double>(v));
}

uint32_t toFloatBits(LnsWord w)
{
    return std::bit_cast<uint32_t>(static_cast<float>(decodeToDouble(w)));
}

uint64_t toDoubleBits(LnsWord w)
{
    return std::bit_cast<uint64_t>(decodeToDouble(w));
}

int32_t toInt32(LnsWord w)
{
    return saturatingToInt<int32_t>(w);
}

uint32_t toUInt32(LnsWord w)
{
    return saturatingToInt<uint32_t>(w);
}

int64_t toInt64(LnsWord w)
{
    return saturatingToInt<int64_t>(w);
}

uint64_t toUInt64(LnsWord w)
{
    return saturatingToInt<uint64_t>(w);
}
} // namespace reference
} // namespace lns
} // namespace Kites

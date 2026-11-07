#include "lns_conversion.h"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Kites
{
namespace lns
{
namespace
{
constexpr int kLogTableFrac = 32; ///< fractional bits of the log2(1+x) table
constexpr int kExpTableFrac = 31; ///< fractional bits of the 2^f table
constexpr int kLogGuardBits = kLogTableFrac - kFracBits; ///< 10

/// Round-to-nearest-even right shift of an unsigned value.
uint64_t rneShift(uint64_t m, int shift)
{
    if (shift <= 0)
    {
        return m << (-shift);
    }
    if (shift >= 64)
    {
        return 0;
    }
    uint64_t q = m >> shift;
    uint64_t rem = m & ((uint64_t{1} << shift) - 1);
    uint64_t half = uint64_t{1} << (shift - 1);
    if (rem > half || (rem == half && (q & 1)))
    {
        ++q;
    }
    return q;
}

/// Decomposition of a finite LNS word: value = (-1)^sign * 2^I * (m / 2^31), m in [2^31, 2^32].
struct Decoded
{
    bool sign;
    int integerPart; ///< I = floor(L)
    uint64_t mant;   ///< 2^f in Q31
};

enum class Kind
{
    Finite,
    Zero,
    Inf,
    NaN
};

Kind classify(LnsWord &w)
{
    w = canonicalize(w);
    if (isNaN(w))
    {
        return Kind::NaN;
    }
    if (isZero(w))
    {
        return Kind::Zero;
    }
    if (isInf(w))
    {
        return Kind::Inf;
    }
    return Kind::Finite;
}
} // namespace

LnsConverter::LnsConverter(int logLutBits, int expLutBits)
    : m_logBits(logLutBits), m_expBits(expLutBits)
{
    if (logLutBits < 1 || logLutBits > kMaxLogLutBits)
    {
        throw std::invalid_argument("LnsConverter: logLutBits must be in [1, 23]");
    }
    if (expLutBits < 1 || expLutBits > kMaxExpLutBits)
    {
        throw std::invalid_argument("LnsConverter: expLutBits must be in [1, 22]");
    }

    // log2(1+x) table. T[N] = 1.0 exactly, so the last slope reaches 1.0.
    {
        const size_t n = logLutEntries();
        const double invN = 1.0 / static_cast<double>(n); // power of two: exact
        const double scale = std::ldexp(1.0, kLogTableFrac);
        m_logBase.resize(n);
        m_logSlope.resize(n);
        uint64_t cur = 0; // log2(1) = 0
        for (size_t i = 0; i < n; ++i)
        {
            uint64_t next =
                (i + 1 == n)
                    ? (uint64_t{1} << kLogTableFrac)
                    : static_cast<uint64_t>(std::llround(
                          std::log2(1.0 + static_cast<double>(i + 1) * invN) * scale));
            m_logBase[i] = static_cast<uint32_t>(cur);
            m_logSlope[i] = static_cast<uint32_t>(next - cur);
            cur = next;
        }
    }

    // 2^f table. T[N] = 2.0 exactly.
    {
        const size_t n = expLutEntries();
        const double invN = 1.0 / static_cast<double>(n);
        const double scale = std::ldexp(1.0, kExpTableFrac);
        m_expBase.resize(n);
        m_expSlope.resize(n);
        uint64_t cur = uint64_t{1} << kExpTableFrac; // 2^0 = 1.0
        for (size_t i = 0; i < n; ++i)
        {
            uint64_t next =
                (i + 1 == n) ? (uint64_t{1} << (kExpTableFrac + 1))
                             : static_cast<uint64_t>(std::llround(
                                   std::exp2(static_cast<double>(i + 1) * invN) * scale));
            m_expBase[i] = static_cast<uint32_t>(cur);
            m_expSlope[i] = static_cast<uint32_t>(next - cur);
            cur = next;
        }
    }
}

size_t LnsConverter::logLutBytes() const
{
    return logLutEntries() * (sizeof(uint32_t) * 2);
}

size_t LnsConverter::expLutBytes() const
{
    return expLutEntries() * (sizeof(uint32_t) * 2);
}

uint32_t LnsConverter::log2OnePlusX(uint32_t frac32) const
{
    const int r = 32 - m_logBits; // remainder width, 9..31
    const uint64_t i = static_cast<uint64_t>(frac32) >> r;
    const uint64_t rem = static_cast<uint64_t>(frac32) & ((uint64_t{1} << r) - 1);
    const uint64_t y = m_logBase[i] + ((static_cast<uint64_t>(m_logSlope[i]) * rem) >> r);
    // Q32 -> Q22 with round-to-nearest
    return static_cast<uint32_t>((y + (uint64_t{1} << (kLogGuardBits - 1))) >> kLogGuardBits);
}

uint64_t LnsConverter::exp2Frac(uint32_t frac22) const
{
    const int r = kFracBits - m_expBits; // remainder width, 0..21
    const uint64_t i = static_cast<uint64_t>(frac22) >> r;
    const uint64_t rem = static_cast<uint64_t>(frac22) & ((uint64_t{1} << r) - 1);
    return m_expBase[i] + ((static_cast<uint64_t>(m_expSlope[i]) * rem) >> r);
}

LnsWord LnsConverter::encode(bool sign, int exponent, uint32_t frac32) const
{
    const int64_t lq = (static_cast<int64_t>(exponent) << kFracBits) + log2OnePlusX(frac32);
    if (lq > kLqFiniteMax)
    {
        return sign ? kNegInf : kPosInf;
    }
    if (lq < kLqFiniteMin)
    {
        return kPosZero;
    }
    return pack(sign, static_cast<int32_t>(lq));
}

LnsWord LnsConverter::encodeUnsigned(bool sign, uint64_t magnitude) const
{
    if (magnitude == 0)
    {
        return kPosZero;
    }
    const int p = 63 - std::countl_zero(magnitude);
    // Drop the leading one, keep the top 32 bits of the fraction below it.
    const uint64_t shifted = (magnitude << (63 - p)) << 1;
    return encode(sign, p, static_cast<uint32_t>(shifted >> 32));
}

LnsWord LnsConverter::fromFloatBits(uint32_t bits) const
{
    const bool sign = (bits >> 31) != 0;
    const uint32_t e = (bits >> 23) & 0xFF;
    const uint32_t m = bits & 0x7FFFFF;

    if (e == 0xFF)
    {
        if (m != 0)
        {
            return kNaN;
        }
        return sign ? kNegInf : kPosInf;
    }
    if (e == 0)
    {
        if (m == 0)
        {
            return kPosZero; // +0 and -0 share the single LNS zero
        }
        // Subnormal: m * 2^-149 = (1+x) * 2^(p-149), p = position of the leading one.
        const int p = 31 - std::countl_zero(m);
        const uint32_t frac32 = static_cast<uint32_t>(static_cast<uint64_t>(m) << (32 - p));
        return encode(sign, p - 149, frac32);
    }
    return encode(sign, static_cast<int>(e) - 127, m << 9);
}

LnsWord LnsConverter::fromUInt64(uint64_t v) const
{
    return encodeUnsigned(false, v);
}

LnsWord LnsConverter::fromUInt32(uint32_t v) const
{
    return encodeUnsigned(false, v);
}

LnsWord LnsConverter::fromInt64(int64_t v) const
{
    if (v < 0)
    {
        return encodeUnsigned(true, static_cast<uint64_t>(-(v + 1)) + 1);
    }
    return encodeUnsigned(false, static_cast<uint64_t>(v));
}

LnsWord LnsConverter::fromInt32(int32_t v) const
{
    return fromInt64(v);
}

namespace
{
Decoded decodeFinite(const LnsConverter &c, LnsWord w)
{
    const int32_t field = fieldOf(w);
    Decoded d;
    d.sign = signOf(w);
    d.integerPart = field >> kFracBits; // arithmetic shift: floor
    d.mant = c.exp2Frac(static_cast<uint32_t>(field) & ((1u << kFracBits) - 1));
    return d;
}

/// Magnitude (nearest-even) of a finite LNS value as uint64, saturating at UINT64_MAX.
uint64_t magnitudeU64(const Decoded &d)
{
    if (d.integerPart >= 64)
    {
        return std::numeric_limits<uint64_t>::max();
    }
    if (d.integerPart >= 31)
    {
        return d.mant << (d.integerPart - 31); // mant < 2^32+: fits for I <= 63
    }
    return rneShift(d.mant, 31 - d.integerPart);
}
} // namespace

uint32_t LnsConverter::toFloatBits(LnsWord w) const
{
    const Kind kind = classify(w);
    const uint32_t signBit = signOf(w) ? 0x80000000u : 0u;
    switch (kind)
    {
    case Kind::NaN:
        return 0x7FC00000u;
    case Kind::Zero:
        return 0u;
    case Kind::Inf:
        return signBit | 0x7F800000u;
    case Kind::Finite:
        break;
    }

    const Decoded d = decodeFinite(*this, w);
    int be = d.integerPart + 127;
    if (be >= 255)
    {
        return signBit | 0x7F800000u;
    }
    if (be >= 1)
    {
        uint64_t mant24 = rneShift(d.mant, 8); // Q31 -> 24 significant bits
        if (mant24 >= (uint64_t{1} << 24))
        {
            mant24 >>= 1;
            ++be;
            if (be >= 255)
            {
                return signBit | 0x7F800000u;
            }
        }
        return signBit | (static_cast<uint32_t>(be) << 23) |
               (static_cast<uint32_t>(mant24) & 0x7FFFFFu);
    }
    // Subnormal result: value = mant * 2^(I-31), in units of 2^-149.
    const uint64_t q = rneShift(d.mant, -118 - d.integerPart);
    return signBit | static_cast<uint32_t>(q); // q == 2^23 correctly becomes the min normal
}

uint64_t LnsConverter::toUInt64(LnsWord w) const
{
    switch (classify(w))
    {
    case Kind::NaN:
        return std::numeric_limits<uint64_t>::max();
    case Kind::Zero:
        return 0;
    case Kind::Inf:
        return signOf(w) ? 0 : std::numeric_limits<uint64_t>::max();
    case Kind::Finite:
        break;
    }
    const Decoded d = decodeFinite(*this, w);
    return d.sign ? 0 : magnitudeU64(d);
}

uint32_t LnsConverter::toUInt32(LnsWord w) const
{
    const uint64_t v = toUInt64(w);
    return v > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                    : static_cast<uint32_t>(v);
}

int64_t LnsConverter::toInt64(LnsWord w) const
{
    constexpr int64_t maxV = std::numeric_limits<int64_t>::max();
    constexpr int64_t minV = std::numeric_limits<int64_t>::min();
    switch (classify(w))
    {
    case Kind::NaN:
        return maxV;
    case Kind::Zero:
        return 0;
    case Kind::Inf:
        return signOf(w) ? minV : maxV;
    case Kind::Finite:
        break;
    }
    const Decoded d = decodeFinite(*this, w);
    const uint64_t mag = magnitudeU64(d);
    if (!d.sign)
    {
        return mag > static_cast<uint64_t>(maxV) ? maxV : static_cast<int64_t>(mag);
    }
    return mag >= (uint64_t{1} << 63) ? minV : -static_cast<int64_t>(mag);
}

int32_t LnsConverter::toInt32(LnsWord w) const
{
    constexpr int32_t maxV = std::numeric_limits<int32_t>::max();
    constexpr int32_t minV = std::numeric_limits<int32_t>::min();
    const int64_t v = toInt64(w);
    if (v > maxV)
    {
        return maxV;
    }
    if (v < minV)
    {
        return minV;
    }
    return static_cast<int32_t>(v);
}

const LnsConverter &defaultConverter()
{
    static const LnsConverter converter(LNS_LOG_LUT_BITS, LNS_EXP_LUT_BITS);
    return converter;
}

LnsWord fromFloatBits(uint32_t bits)
{
    return defaultConverter().fromFloatBits(bits);
}
LnsWord fromInt32(int32_t v)
{
    return defaultConverter().fromInt32(v);
}
LnsWord fromUInt32(uint32_t v)
{
    return defaultConverter().fromUInt32(v);
}
LnsWord fromInt64(int64_t v)
{
    return defaultConverter().fromInt64(v);
}
LnsWord fromUInt64(uint64_t v)
{
    return defaultConverter().fromUInt64(v);
}
uint32_t toFloatBits(LnsWord w)
{
    return defaultConverter().toFloatBits(w);
}
int32_t toInt32(LnsWord w)
{
    return defaultConverter().toInt32(w);
}
uint32_t toUInt32(LnsWord w)
{
    return defaultConverter().toUInt32(w);
}
int64_t toInt64(LnsWord w)
{
    return defaultConverter().toInt64(w);
}
uint64_t toUInt64(LnsWord w)
{
    return defaultConverter().toUInt64(w);
}
} // namespace lns
} // namespace Kites

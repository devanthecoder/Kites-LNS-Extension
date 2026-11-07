/**
 * @file lns_conversion.h
 * @brief LUT-based conversions between the 32-bit LNS format and FP32 / integers.
 *
 * Forward (to LNS):  value = (-1)^s * (1+x) * 2^E,  x in [0,1)
 *                    L = E + log2(1+x); log2(1+x) comes from a uniform 2^LogBits-entry
 *                    table with linear interpolation.
 * Reverse (from LNS): L = I + f, I = floor(L), f in [0,1)
 *                    2^f comes from a uniform 2^ExpBits-entry table with linear
 *                    interpolation; the result is then scaled by 2^I and rounded.
 *
 * Table sizes are chosen per LnsConverter instance (constructor arguments); the macros
 * below only supply the defaults and may be overridden with -D on the compiler command line.
 *
 * Rules:
 *   - FP/int zero -> +0; FP NaN -> NaN; FP Inf -> +/-Inf.
 *   - Log is rounded to nearest in Q9.22. A nonzero finite input whose log falls outside the
 *     finite range flushes to 0 (below) or saturates to Inf (above); impossible for FP32.
 *   - LNS -> FP32 rounds to nearest-even; overflow gives Inf, tiny values become
 *     subnormal / 0.
 *   - LNS -> integer rounds to nearest-even and follows RISC-V FCVT: NaN -> max positive,
 *     out-of-range saturates, negative -> unsigned gives 0.
 */
#ifndef LNS_CONVERSION_H
#define LNS_CONVERSION_H

#include "lns_format.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/// Default log2(1+x) table size: 2^LNS_LOG_LUT_BITS entries (valid range 1..23).
#ifndef LNS_LOG_LUT_BITS
#define LNS_LOG_LUT_BITS 11
#endif
/// Default 2^f table size: 2^LNS_EXP_LUT_BITS entries (valid range 1..22).
#ifndef LNS_EXP_LUT_BITS
#define LNS_EXP_LUT_BITS 11
#endif

namespace Kites
{
namespace lns
{
constexpr int kMaxLogLutBits = 23; ///< FP32 mantissa width
constexpr int kMaxExpLutBits = 22; ///< LNS fraction width

static_assert(LNS_LOG_LUT_BITS >= 1 && LNS_LOG_LUT_BITS <= kMaxLogLutBits,
              "LNS_LOG_LUT_BITS must be in [1, 23]");
static_assert(LNS_EXP_LUT_BITS >= 1 && LNS_EXP_LUT_BITS <= kMaxExpLutBits,
              "LNS_EXP_LUT_BITS must be in [1, 22]");

class LnsConverter
{
  public:
    /// @throws std::invalid_argument if a size is outside its valid range.
    explicit LnsConverter(int logLutBits = LNS_LOG_LUT_BITS, int expLutBits = LNS_EXP_LUT_BITS);

    // Into LNS
    [[nodiscard]] LnsWord fromFloatBits(uint32_t bits) const;
    [[nodiscard]] LnsWord fromInt32(int32_t v) const;
    [[nodiscard]] LnsWord fromUInt32(uint32_t v) const;
    [[nodiscard]] LnsWord fromInt64(int64_t v) const;
    [[nodiscard]] LnsWord fromUInt64(uint64_t v) const;

    // Out of LNS
    [[nodiscard]] uint32_t toFloatBits(LnsWord w) const;
    [[nodiscard]] int32_t toInt32(LnsWord w) const;
    [[nodiscard]] uint32_t toUInt32(LnsWord w) const;
    [[nodiscard]] int64_t toInt64(LnsWord w) const;
    [[nodiscard]] uint64_t toUInt64(LnsWord w) const;

    [[nodiscard]] int logLutBits() const { return m_logBits; }
    [[nodiscard]] int expLutBits() const { return m_expBits; }
    [[nodiscard]] size_t logLutEntries() const { return size_t{1} << m_logBits; }
    [[nodiscard]] size_t expLutEntries() const { return size_t{1} << m_expBits; }
    /// Bytes of table storage (value + slope per entry; slope could be derived from neighbours).
    [[nodiscard]] size_t logLutBytes() const;
    [[nodiscard]] size_t expLutBytes() const;

    /// log2(1+x) as Q9.22-fraction (0..2^22), x = frac32 / 2^32. Exposed for testing.
    [[nodiscard]] uint32_t log2OnePlusX(uint32_t frac32) const;
    /// 2^f as Q31 (2^31 .. 2^32), f = frac22 / 2^22. Exposed for testing.
    [[nodiscard]] uint64_t exp2Frac(uint32_t frac22) const;

  private:
    LnsWord encode(bool sign, int exponent, uint32_t frac32) const;
    LnsWord encodeUnsigned(bool sign, uint64_t magnitude) const;

    int m_logBits;
    int m_expBits;
    std::vector<uint32_t> m_logBase;  ///< log2(1+i/N) in Q32
    std::vector<uint32_t> m_logSlope; ///< T[i+1]-T[i], Q32
    std::vector<uint32_t> m_expBase;  ///< 2^(i/N) in Q31
    std::vector<uint32_t> m_expSlope; ///< T[i+1]-T[i], Q31
};

/// Converter built with the macro defaults; this is what the simulator uses.
const LnsConverter &defaultConverter();

// Convenience wrappers around defaultConverter().
LnsWord fromFloatBits(uint32_t bits);
LnsWord fromInt32(int32_t v);
LnsWord fromUInt32(uint32_t v);
LnsWord fromInt64(int64_t v);
LnsWord fromUInt64(uint64_t v);
uint32_t toFloatBits(LnsWord w);
int32_t toInt32(LnsWord w);
uint32_t toUInt32(LnsWord w);
int64_t toInt64(LnsWord w);
uint64_t toUInt64(LnsWord w);
} // namespace lns
} // namespace Kites

#endif // LNS_CONVERSION_H

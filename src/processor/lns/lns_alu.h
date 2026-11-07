/**
 * @file lns_alu.h
 * @brief Arithmetic on 32-bit LNS words (add, sub, mul, div).
 *
 * The word layout, special values and limits come from lns_format.h.
 *
 * Multiply / divide add / subtract the logs. Add / subtract use
 *   Lmax + phi(d),  d = |Lx - Ly|,  phi+(d) = log2(1 + 2^-d),  phi-(d) = log2(1 - 2^-d)
 * with phi read from a correction table by linear interpolation. The table has samples spaced
 * logarithmically in d (from 1 LSB up to kCorrectionCutoff); its requested sample count is a
 * constructor argument, so the size / accuracy trade-off can be swept without rebuilding.
 */
#ifndef LNS_ALU_H
#define LNS_ALU_H

#include "lns_format.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/// Default number of requested samples in the add/sub correction table.
#ifndef LNS_ALU_CORRECTION_SAMPLES
#define LNS_ALU_CORRECTION_SAMPLES 4096
#endif

namespace Kites
{
namespace lns
{
/** Operations implemented by the LNS ALU. */
enum class LnsAluOp
{
    ADD,
    SUB,
    MUL,
    DIV,
};

class LnsAlu
{
  public:
    /// Differences d (in Q22 log units) at or beyond this make phi vanish: 24.0.
    static constexpr int64_t kCorrectionCutoff = 24LL * kFractionScale;
    static constexpr size_t kMinCorrectionSamples = 2;
    static constexpr size_t kMaxCorrectionSamples = size_t{1} << 24;

    /// @param correctionSamples requested number of samples (distinct sample points can be
    ///        fewer, because near d = 1 LSB neighbouring samples round to the same integer).
    /// @throws std::invalid_argument if the count is outside [kMinCorrectionSamples,
    ///         kMaxCorrectionSamples].
    explicit LnsAlu(size_t correctionSamples = LNS_ALU_CORRECTION_SAMPLES);

    /// Executes one operation on two LNS words.
    [[nodiscard]] LnsWord execute(LnsAluOp op, LnsWord a, LnsWord b) const;

    [[nodiscard]] LnsWord add(LnsWord a, LnsWord b) const;
    [[nodiscard]] LnsWord sub(LnsWord a, LnsWord b) const;
    [[nodiscard]] static LnsWord mul(LnsWord a, LnsWord b);
    [[nodiscard]] static LnsWord div(LnsWord a, LnsWord b);

    /// phi+(d) (addition = true) or phi-(d) in Q22 log units for d in Q22 log units, d >= 0.
    [[nodiscard]] int64_t correction(int64_t difference, bool addition) const;

    [[nodiscard]] size_t requestedSamples() const { return m_requestedSamples; }
    /// Number of distinct stored sample points.
    [[nodiscard]] size_t correctionEntries() const { return m_table.size(); }
    /// Bytes of table storage (difference + phi+ + phi- per entry).
    [[nodiscard]] size_t correctionBytes() const;

  private:
    struct CorrectionEntry
    {
        int64_t difference;
        int64_t addition;
        int64_t subtraction;
    };

    size_t m_requestedSamples;
    std::vector<CorrectionEntry> m_table;
};

/// ALU built with the macro defaults; this is what the simulator uses.
const LnsAlu &defaultAlu();
} // namespace lns
} // namespace Kites

#endif // LNS_ALU_H

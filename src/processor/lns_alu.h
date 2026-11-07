/**
 * @file lns_alu.h
 * @brief Arithmetic operations for the signed logarithmic number representation.
 */
#ifndef LNS_ALU_H
#define LNS_ALU_H

#include <cstddef>
#include <cstdint>

namespace Kites
{
namespace alu
{
/** Operations implemented by the standalone LNS ALU. */
enum class LnsAluOp
{
    ADD,
    SUB,
    MUL,
    DIV,
};

/**
 * @brief Performs arithmetic on 32-bit LNS values stored in a 64-bit GPR.
 *
 * The payload uses bit 31 for the value sign and bits 30:0 for a signed,
 * two's-complement Q9.22 log2 magnitude. Zero and special values use reserved
 * log-field encodings documented by the constants below.
 */
class LnsAlu
{
  public:
    // Change these format constants together if the payload layout is revised.
    static constexpr uint32_t kPayloadBits = 32;
    static constexpr uint32_t kSignBitIndex = 31;
    static constexpr uint32_t kLogFieldBits = 31;
    static constexpr uint32_t kFractionBits = 22;
    static constexpr uint32_t kIntegerFieldBits = kLogFieldBits - kFractionBits;
    static constexpr int64_t kFractionScale = 1LL << kFractionBits;

    static constexpr uint32_t kSignMask = 1U << kSignBitIndex;
    static constexpr uint32_t kLogFieldMask = (1U << kLogFieldBits) - 1U;
    static constexpr uint32_t kLogFieldSignMask = 1U << (kLogFieldBits - 1);
    static constexpr uint32_t kFractionMask = (1U << kFractionBits) - 1U;
    static constexpr uint32_t kIntegerFieldMask = (1U << kIntegerFieldBits) - 1U;

    // In the reserved integer field, fraction zero means infinity and nonzero means NaN.
    // These codes reuse the original reserved bands after removing the bias.
    static constexpr uint32_t kSpecialIntegerField = 0x0FFU;
    static constexpr uint32_t kSpecialFieldBase = kSpecialIntegerField << kFractionBits;
    static constexpr uint32_t kZeroEncoding = kLogFieldSignMask;
    static constexpr uint32_t kPositiveInfinityEncoding = kSpecialFieldBase;
    static constexpr uint32_t kNegativeInfinityEncoding = kSignMask | kSpecialFieldBase;
    static constexpr uint32_t kNaNEncoding = kSpecialFieldBase | 1U;

    // Arithmetic and lookup-table configuration, expressed in Q22 units.
    static constexpr int64_t kMinimumFiniteLog = -(1LL << (kLogFieldBits - 1)) + 1;
    static constexpr int64_t kInfinityLogThreshold =
        static_cast<int64_t>(kSpecialIntegerField) * kFractionScale;
    static constexpr int64_t kCorrectionCutoff = 24LL * kFractionScale;
    static constexpr size_t kCorrectionSampleCount = 4096;

    /** Executes one LNS operation using the low 32 bits of each input. */
    [[nodiscard]] static uint32_t execute(LnsAluOp op, uint64_t a, uint64_t b);

    [[nodiscard]] static uint32_t add(uint32_t a, uint32_t b);
    [[nodiscard]] static uint32_t sub(uint32_t a, uint32_t b);
    [[nodiscard]] static uint32_t mul(uint32_t a, uint32_t b);
    [[nodiscard]] static uint32_t div(uint32_t a, uint32_t b);
};
} // namespace alu
} // namespace Kites

#endif // LNS_ALU_H

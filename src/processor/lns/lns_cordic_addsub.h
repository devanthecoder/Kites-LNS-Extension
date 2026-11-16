/**
 * @file lns_cordic_addsub.h
 * @brief Standalone CORDIC implementation of LNS addition and subtraction.
 *
 * Multiplication and division remain in LnsAlu. This class provides an alternate
 * implementation of addition and subtraction without changing the ALU interface or ISA.
 */
#ifndef LNS_CORDIC_ADDSUB_H
#define LNS_CORDIC_ADDSUB_H

#include "lns_format.h"

namespace Kites
{
namespace lns
{
class LnsCordicAddSub
{
  public:
    /// Adds two signed LNS values using hyperbolic CORDIC correction calculations.
    [[nodiscard]] static LnsWord add(LnsWord a, LnsWord b);

    /// Subtracts b from a using hyperbolic CORDIC correction calculations.
    [[nodiscard]] static LnsWord sub(LnsWord a, LnsWord b);
};
} // namespace lns
} // namespace Kites

#endif // LNS_CORDIC_ADDSUB_H

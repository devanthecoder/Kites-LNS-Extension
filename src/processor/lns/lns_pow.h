/**
 * @file lns_pow.h
 * @brief Exponentiation on 32-bit LNS words.
 *
 * In LNS, log2|x^y| = y * log2|x|, so a power only scales the stored log field.
 *
 *   powerN(x, n)   integer exponent n: log field * n, an exact integer multiply (lpown).
 *   power(x, y)    LNS exponent y: y is turned back into a fixed-point number with the
 *                  converter's 2^f table (y = 2^(I+f) = 2^f * 2^I), multiplied by the log field
 *                  of x and rounded to nearest (lpow). Exact when y is a power of two, so
 *                  sqrt(x) = power(x, 0.5) is an exact halving of the log (rounded by 1/2 LSB).
 *
 * Results outside the finite range saturate as in LnsAlu: too large -> Inf, too small -> +0.
 *
 * Special values follow C pow / pown, adapted to LNS (zero carries no sign):
 *   x^0 = 1 and 1^y = 1 for any other operand, even NaN; otherwise NaN in -> NaN.
 *   powerN: a negative base gives a negative result for odd n; 0^n = +0 (n > 0) or +Inf (n < 0);
 *           (+/-Inf)^n = Inf (n > 0, sign by parity) or +0 (n < 0).
 *   power:  a negative base gives NaN unless y is exactly 1 (use powerN for integer powers);
 *           0^y = +0 (y > 0) or +Inf (y < 0); (+Inf)^y = +Inf (y > 0) or +0 (y < 0);
 *           x^(+Inf) = +Inf for |x| > 1, +0 for |x| < 1; x^(-Inf) the other way round.
 */
#ifndef LNS_POW_H
#define LNS_POW_H

#include "lns_conversion.h"
#include "lns_format.h"

#include <cstdint>

namespace Kites
{
namespace lns
{
/// x^n for a 64-bit integer exponent; exact apart from saturation.
[[nodiscard]] LnsWord powerN(LnsWord x, int64_t n);

/// x^y for an LNS exponent, using @p converter's 2^f table for the value of y.
[[nodiscard]] LnsWord power(LnsWord x, LnsWord y, const LnsConverter &converter);

/// x^y with defaultConverter(); this is what the simulator uses.
[[nodiscard]] LnsWord power(LnsWord x, LnsWord y);
} // namespace lns
} // namespace Kites

#endif // LNS_POW_H

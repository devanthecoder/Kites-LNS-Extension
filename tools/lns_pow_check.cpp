// Accuracy check for LNS exponentiation (lns_pow) against host std::pow (no Qt).
//
// power(x, y) reads 2^f from the converter's exp table, so its accuracy follows the converter's
// exp LUT size; powerN(x, n) is exact. The error of x^y is reported in units of the result's
// log (LSB = 2^-22), divided by max(1, |y * log2 x|): the log errors of the inputs are scaled by
// that factor in any number format, so this "scaled" error isolates what the table adds.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_pow_check.cpp src/processor/lns/lns_pow.cpp
//       src/processor/lns/lns_conversion.cpp -o lns_pow_check
// Run:  ./lns_pow_check
#include "lns_conversion.h"
#include "lns_pow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace Kites::lns;

namespace
{
/// Exact LNS word for x > 0 (rounded log), independent of any table.
LnsWord exactWord(double x)
{
    return pack(false, static_cast<int32_t>(std::llround(std::log2(x) * kScale)));
}

double logOf(LnsWord w)
{
    return fieldOf(w) / kScale;
}
} // namespace

int main()
{
    // Exact inputs, so only power() itself is measured.
    const double xs[] = {0.013, 0.31, 0.7, 1.9, 3.0, 17.5, 1234.0, 1.0e10};
    const double ys[] = {-7.3, -2.5, -1.0, -0.37, 0.5, 0.9, 1.5, 2.25, 3.0, 6.0, 11.7};

    std::printf("x^y with exact LNS inputs (saturated results skipped); error of log2(result)"
                " in LSB (2^-22)\n");
    std::printf("%-12s %12s %12s %16s\n", "exp LUT bits", "max LSB", "mean LSB",
                "max scaled LSB");
    for (int bits : {6, 8, 10, 11, 12, 14, 16, 22})
    {
        const LnsConverter conv(LNS_LOG_LUT_BITS, bits);
        double maxErr = 0.0, sumErr = 0.0, maxScaled = 0.0;
        int count = 0;
        for (double x : xs)
        {
            for (double y : ys)
            {
                const LnsWord lx = exactWord(x);
                const LnsWord ly = y < 0 ? exactWord(-y) | kSignMask : exactWord(y);
                const LnsWord r = power(lx, ly, conv);
                const double want = logOf(lx) * (y < 0 ? -std::exp2(logOf(ly & ~kSignMask))
                                                       : std::exp2(logOf(ly)));
                if (want * kScale < kLqFiniteMin || want * kScale >= kLqInfThreshold)
                {
                    continue; // saturates to 0 / Inf by design
                }
                const double err = std::fabs(logOf(r) - want) * kScale;
                const double scale = std::max(1.0, std::fabs(y * std::log2(x)));
                maxErr = std::max(maxErr, err);
                maxScaled = std::max(maxScaled, err / scale);
                sumErr += err;
                ++count;
            }
        }
        std::printf("%-12d %12.2f %12.2f %16.3f%s\n", bits, maxErr, sumErr / count, maxScaled,
                    bits == LNS_EXP_LUT_BITS ? "  (default)" : "");
    }

    // powerN is exact: compare with n * log field directly, and power(x, 2^k) with a shift.
    int failures = 0;
    for (double x : xs)
    {
        const LnsWord lx = exactWord(x);
        for (int64_t n : {-5, -2, -1, 0, 1, 2, 3, 7})
        {
            const int64_t want = static_cast<int64_t>(fieldOf(lx)) * n;
            if (want >= kLqFiniteMin && want < kLqInfThreshold &&
                powerN(lx, n) != pack(false, static_cast<int32_t>(want)))
            {
                std::printf("FAIL powerN(%g, %lld)\n", x, static_cast<long long>(n));
                ++failures;
            }
        }
        const LnsWord four = exactWord(4.0);
        if (power(lx, four) != powerN(lx, 4))
        {
            std::printf("FAIL power(%g, 4.0) != powerN(%g, 4)\n", x, x);
            ++failures;
        }
    }
    std::printf("\nexactness checks: %s\n", failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}

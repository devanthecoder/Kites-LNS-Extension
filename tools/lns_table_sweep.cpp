// Table-size sweep for the LNS units on random inputs: accuracy and host time per call (no Qt).
//
// ALU part: ladd/lsub for correction tables of 256 .. 4M requested samples. Inputs have log2|a| in
// [-20, 20] and a log difference d uniform in [0, 24), so lookups spread over the whole table.
// Converter part: fcvt.lns.s, fcvt.s.lns and lpow for LUTs of 4 .. 22 index bits.
// Errors are in LSB of the log (2^-22; multiply by ln2 / 2^22 = 1.6526e-7 for relative error),
// fcvt.s.lns errors in FP32 ulps; lpow errors are divided by max(1, |y * log2 x|) as in
// lns_pow_check. Times are host ns per call (best of 7 runs of 2M calls): they compare LNS
// configurations with each other, not with FP32 hardware.
//
// Build (from the repo root, one command line):
//   g++ -std=c++20 -O2 -I src/processor/lns tools/lns_table_sweep.cpp src/processor/lns/lns_alu.cpp
//       src/processor/lns/lns_conversion.cpp src/processor/lns/lns_pow.cpp -o lns_table_sweep
// Run:  taskset -c 2 ./lns_table_sweep      (CSV on stdout; pinning to one core reduces noise)
#include "lns_alu.h"
#include "lns_conversion.h"
#include "lns_pow.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace Kites::lns;
using Clk = std::chrono::steady_clock;
static volatile uint32_t sink;
constexpr int M = 1 << 16; // inputs per set (fits in L2)

template <class F> double ns(F f, int reps)
{
    double best = 1e30;
    for (int t = 0; t < 7; ++t)
    {
        auto a = Clk::now();
        uint32_t acc = 0;
        for (int i = 0; i < reps; ++i) acc += f(i);
        sink = acc;
        best = std::min(best, std::chrono::duration<double, std::nano>(Clk::now() - a).count() / reps);
    }
    return best;
}
static double logOf(LnsWord w) { return fieldOf(w) / kScale; }

int main()
{
    std::mt19937_64 rng(12345);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int N = 2'000'000;

    // ---- ladd / lsub: a in [-20, 20], d uniform in [0, 24) ----
    std::vector<LnsWord> A(M), B(M);
    for (int i = 0; i < M; ++i)
    {
        const double la = -20 + 40 * U(rng), d = 24 * U(rng);
        A[i] = pack(false, (int32_t)std::llround(la * kScale));
        B[i] = pack(false, (int32_t)std::llround((la - d) * kScale));
    }
    std::printf("ALU samples,table_KiB,add_max_lsb,add_mean_lsb,sub_mean_lsb,ns_per_op\n");
    for (size_t s = 256; s <= (size_t{1} << 22); s *= 2)
    {
        const LnsAlu alu(s);
        double amax = 0, asum = 0, ssum = 0;
        int sc = 0;
        for (int i = 0; i < M; ++i)
        {
            const double la = logOf(A[i]), lb = logOf(B[i]);
            const double ea = std::log2(std::exp2(la) + std::exp2(lb));
            const double e = std::fabs(logOf(alu.add(A[i], B[i])) - ea) * kScale;
            amax = std::max(amax, e);
            asum += e;
            if (la - lb > 1.0 / 64) // skip near-cancellation, handled separately
            {
                const double es = la + std::log2(1 - std::exp2(lb - la));
                ssum += std::fabs(logOf(alu.sub(A[i], B[i])) - es) * kScale;
                ++sc;
            }
        }
        const double t = (ns([&](int i) { return alu.add(A[i & (M - 1)], B[(i * 7 + 13) & (M - 1)]); }, N) +
                          ns([&](int i) { return alu.sub(A[i & (M - 1)], B[(i * 7 + 13) & (M - 1)]); }, N)) /
                         2;
        std::printf("%zu,%.2f,%.2f,%.3f,%.3f,%.1f\n", s, alu.correctionBytes() / 1024.0, amax, asum / M,
                    ssum / sc, t);
    }

    // ---- converter: random positive floats over [2^-60, 2^60]; random LNS words over same range ----
    std::vector<uint32_t> F(M);
    std::vector<LnsWord> W(M), Y(M);
    for (int i = 0; i < M; ++i)
    {
        F[i] = std::bit_cast<uint32_t>((float)std::exp2(-60 + 120 * U(rng)));
        W[i] = pack(false, (int32_t)std::llround((-60 + 120 * U(rng)) * kScale));
        const double y = -8 + 16 * U(rng); // lpow exponent
        Y[i] = pack(y < 0, (int32_t)std::llround(std::log2(std::fabs(y)) * kScale));
    }
    std::printf("\nconv bits,table_KiB,tolns_max_lsb,tolns_mean_lsb,tofloat_max_ulp,tofloat_mean_ulp,"
                "tolns_ns,tofloat_ns,pow_max_lsb,pow_mean_lsb,pow_ns,build_ms\n");
    for (int k = 4; k <= 22; k += (k < 14 ? 1 : 2))
    {
        auto b0 = Clk::now();
        const LnsConverter c(k, k);
        const double build = std::chrono::duration<double, std::milli>(Clk::now() - b0).count();
        double lmax = 0, lsum = 0, fmax = 0, fsum = 0, pmax = 0, psum = 0;
        int pc = 0;
        for (int i = 0; i < M; ++i)
        {
            const float x = std::bit_cast<float>(F[i]);
            const double el = std::fabs(logOf(c.fromFloatBits(F[i])) - std::log2((double)x)) * kScale;
            lmax = std::max(lmax, el);
            lsum += el;
            const double exact = std::exp2(logOf(W[i]));
            const float got = std::bit_cast<float>(c.toFloatBits(W[i]));
            const double ulp = std::ldexp(1.0, std::ilogb(exact) - 23);
            const double ef = std::fabs(got - exact) / ulp;
            fmax = std::max(fmax, ef);
            fsum += ef;
            const double y = (signOf(Y[i]) ? -1 : 1) * std::exp2(logOf(Y[i] & ~kSignMask));
            const double want = logOf(W[i]) * y;
            if (want * kScale > kLqFiniteMin && want * kScale < kLqInfThreshold)
            {
                const double ep = std::fabs(logOf(power(W[i], Y[i], c)) - want) * kScale /
                                  std::max(1.0, std::fabs(want));
                pmax = std::max(pmax, ep);
                psum += ep;
                ++pc;
            }
        }
        const double t1 = ns([&](int i) { return c.fromFloatBits(F[i & (M - 1)]); }, N);
        const double t2 = ns([&](int i) { return c.toFloatBits(W[i & (M - 1)]); }, N);
        const double t3 = ns([&](int i) { return power(W[i & (M - 1)], Y[(i * 7 + 13) & (M - 1)], c); }, N);
        std::printf("%d,%.2f,%.2f,%.3f,%.3f,%.4f,%.1f,%.1f,%.3f,%.4f,%.1f,%.2f\n", k,
                    (c.logLutBytes() + c.expLutBytes()) / 1024.0, lmax, lsum / M, fmax, fsum / M, t1, t2,
                    pmax, psum / pc, t3, build);
    }
}
